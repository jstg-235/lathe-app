#include "Scope.h"

#include "Restoration.h"   // dsp::fft

#include <algorithm>
#include <cstdio>
#include <numeric>

namespace lathe::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
double toDb(double p) { return p > 1e-30 ? 10.0 * std::log10(p) : -300.0; }
}  // namespace

BandLayout BandLayout::fractional(int fraction, double fmin, double fmax) {
    BandLayout b;
    const double step = std::pow(2.0, 1.0 / fraction), edge = std::pow(2.0, 0.5 / fraction);
    // Centres on 1 kHz * 2^(k/fraction).
    const int kLo = int(std::ceil(std::log2(fmin / 1000.0) * fraction)), kHi = int(std::floor(std::log2(fmax / 1000.0) * fraction));
    for (int k = kLo; k <= kHi; ++k) {
        const double c = 1000.0 * std::pow(step, k);
        b.centre.push_back(c);
        b.lo.push_back(c / edge);
        b.hi.push_back(c * edge);
    }
    return b;
}

// ---------------------------------------------------------------- SpectrumAnalyzer

void SpectrumAnalyzer::prepare(double sampleRate, int fftOrder, int bandsPerOctave) {
    sr_ = sampleRate;
    n_ = 1 << fftOrder;
    hop_ = n_ / 2;
    ringL_.assign(size_t(n_), 0.0f);
    ringR_.assign(size_t(n_), 0.0f);
    window_.resize(size_t(n_));
    double wp = 0;
    for (int i = 0; i < n_; ++i) {
        window_[size_t(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / n_);   // Hann
        wp += window_[size_t(i)] * window_[size_t(i)];
    }
    windowPower_ = wp;
    bufL_.assign(size_t(n_), {});
    bufR_.assign(size_t(n_), {});
    bands_ = BandLayout::fractional(bandsPerOctave, 20.0, 20000.0);   // the same bands at every rate
    binLo_.clear();
    binHi_.clear();
    const double binHz = sr_ / n_;
    for (int b = 0; b < bands_.size(); ++b) {
        int lo = int(std::ceil(bands_.lo[size_t(b)] / binHz)), hi = int(std::floor(bands_.hi[size_t(b)] / binHz));
        // Narrow low bands may hold no bin: take the nearest one.
        if (hi < lo) lo = hi = int(std::lround(bands_.centre[size_t(b)] / binHz));
        binLo_.push_back(std::max(1, lo));
        binHi_.push_back(std::min(n_ / 2 - 1, std::max(1, hi)));
    }
    const size_t nb = size_t(bands_.size());
    for (int w = 0; w < 4; ++w) {
        fastPow_[w].assign(nb, 0.0);
        avgPow_[w].assign(nb, 0.0);
        fastDb_[w].assign(nb, -300.0);
        avgDb_[w].assign(nb, -300.0);
    }
    crossRe_.assign(nb, 0.0);
    monoLoss_.assign(nb, 0.0);
    bandCorr_.assign(nb, 1.0);
    fastMono_.assign(nb, 0.0);
    fastSides_.assign(nb, 0.0);
    reset();
}

void SpectrumAnalyzer::reset() {
    std::fill(ringL_.begin(), ringL_.end(), 0.0f);
    std::fill(ringR_.begin(), ringR_.end(), 0.0f);
    ringPos_ = sinceHop_ = 0;
    frames_ = 0;
    for (int w = 0; w < 4; ++w) {
        std::fill(fastPow_[w].begin(), fastPow_[w].end(), 0.0);
        std::fill(avgPow_[w].begin(), avgPow_[w].end(), 0.0);
        std::fill(fastDb_[w].begin(), fastDb_[w].end(), -300.0);
        std::fill(avgDb_[w].begin(), avgDb_[w].end(), -300.0);
    }
    std::fill(crossRe_.begin(), crossRe_.end(), 0.0);
    std::fill(monoLoss_.begin(), monoLoss_.end(), 0.0);
    std::fill(bandCorr_.begin(), bandCorr_.end(), 1.0);
    std::fill(fastMono_.begin(), fastMono_.end(), 0.0);
    std::fill(fastSides_.begin(), fastSides_.end(), 0.0);
}

void SpectrumAnalyzer::process(const float* l, const float* r, int n) {
    for (int i = 0; i < n; ++i) {
        ringL_[size_t(ringPos_)] = l[i];
        ringR_[size_t(ringPos_)] = r[i];
        ringPos_ = (ringPos_ + 1) % n_;
        if (++sinceHop_ >= hop_) {
            sinceHop_ = 0;
            analyse();
        }
    }
}

void SpectrumAnalyzer::analyse() {
    for (int i = 0; i < n_; ++i) {
        const int k = (ringPos_ + i) % n_;   // oldest first
        bufL_[size_t(i)] = {ringL_[size_t(k)] * window_[size_t(i)], 0.0};
        bufR_[size_t(i)] = {ringR_[size_t(k)] * window_[size_t(i)], 0.0};
    }
    fft(bufL_, false);
    fft(bufR_, false);
    ++frames_;
    // Smoothing: fast follows about 0.3 s; the long-term average is a plain energy mean.
    const double hopSec = double(hop_) / sr_;
    const double a = std::exp(-hopSec / 0.3);
    // Parseval: the positive-frequency bins of a windowed signal hold N/2 * sum (x w)^2 of energy,
    // so sum |X|^2 * 2 / (N sum w^2) is its mean square. Doubled, a full-scale sine reads 0 dB.
    const double scale = 4.0 / (double(n_) * windowPower_);
    for (int b = 0; b < bands_.size(); ++b) {
        double pl = 0, pr = 0, pm = 0, ps = 0, xr = 0;
        for (int k = binLo_[size_t(b)]; k <= binHi_[size_t(b)]; ++k) {
            const auto L = bufL_[size_t(k)], R = bufR_[size_t(k)];
            pl += std::norm(L);
            pr += std::norm(R);
            pm += std::norm((L + R) * 0.5);
            ps += std::norm((L - R) * 0.5);
            xr += (L * std::conj(R)).real();
        }
        const double p[4] = {pm * scale, ps * scale, pl * scale, pr * scale};
        const size_t bi = size_t(b);
        for (int w = 0; w < 4; ++w) {
            fastPow_[w][bi] = a * fastPow_[w][bi] + (1 - a) * p[w];
            avgPow_[w][bi] += (p[w] - avgPow_[w][bi]) / double(frames_);
            fastDb_[w][bi] = toDb(fastPow_[w][bi]);
            avgDb_[w][bi] = toDb(avgPow_[w][bi]);
        }
        crossRe_[bi] = a * crossRe_[bi] + (1 - a) * xr * scale;
        // Mono (L+R)/2 against the average power of the two sides.
        fastMono_[bi] = a * fastMono_[bi] + (1 - a) * pm * scale;
        fastSides_[bi] = a * fastSides_[bi] + (1 - a) * 0.5 * (pl + pr) * scale;
        // 0 dB: fully correlated (nothing lost); -3 dB: unrelated sides; far below: cancels.
        monoLoss_[bi] = fastSides_[bi] > 1e-14 ? std::clamp(toDb(fastMono_[bi] / fastSides_[bi]), -40.0, 0.0) : 0.0;
        const double den = std::sqrt(fastPow_[2][bi] * fastPow_[3][bi]);
        bandCorr_[bi] = den > 1e-14 ? std::clamp(crossRe_[bi] / den, -1.0, 1.0) : 1.0;
    }
}

// ---------------------------------------------------------------- StereoMeter

void StereoMeter::prepare(double sampleRate) {
    coef_ = std::exp(-1.0 / (0.3 * sampleRate));
    ringL_.assign(4096, 0.0f);
    ringR_.assign(4096, 0.0f);
    reset();
}

void StereoMeter::reset() {
    ll_ = rr_ = lr_ = mm_ = ss_ = 0;
    minCorr_ = 1.0;
    settle_ = 0;
    std::fill(ringL_.begin(), ringL_.end(), 0.0f);
    std::fill(ringR_.begin(), ringR_.end(), 0.0f);
    ringPos_ = 0;
}

void StereoMeter::process(const float* l, const float* r, int n) {
    const double c = coef_, d = 1 - coef_;
    for (int i = 0; i < n; ++i) {
        const double L = l[i], R = r[i], M = 0.5 * (L + R), S = 0.5 * (L - R);
        ll_ = c * ll_ + d * L * L;
        rr_ = c * rr_ + d * R * R;
        lr_ = c * lr_ + d * L * R;
        mm_ = c * mm_ + d * M * M;
        ss_ = c * ss_ + d * S * S;
        if (!ringL_.empty()) {
            ringL_[size_t(ringPos_)] = l[i];
            ringR_[size_t(ringPos_)] = r[i];
            ringPos_ = (ringPos_ + 1) % int(ringL_.size());
        }
    }
    settle_ += n;
    // Only count the minimum once the averages have settled and there is signal.
    if (settle_ > int(1.0 / (1.0 - coef_)) && ll_ + rr_ > 1e-7) minCorr_ = std::min(minCorr_, correlation());
}

double StereoMeter::correlation() const {
    const double den = std::sqrt(ll_ * rr_);
    return den > 1e-12 ? std::clamp(lr_ / den, -1.0, 1.0) : 1.0;
}

double StereoMeter::width() const { return mm_ > 1e-12 ? ss_ / mm_ : (ss_ > 1e-12 ? 10.0 : 0.0); }

double StereoMeter::balanceDb() const {
    if (ll_ < 1e-12 && rr_ < 1e-12) return 0.0;
    return 10.0 * std::log10(std::max(ll_, 1e-12) / std::max(rr_, 1e-12));
}

void StereoMeter::recentPairs(std::vector<float>& l, std::vector<float>& r, int count) const {
    const int size = int(ringL_.size());
    count = std::min(count, size);
    l.resize(size_t(count));
    r.resize(size_t(count));
    for (int i = 0; i < count; ++i) {
        const int k = (ringPos_ - count + i + size) % size;
        l[size_t(i)] = ringL_[size_t(k)];
        r[size_t(i)] = ringR_[size_t(k)];
    }
}

// ---------------------------------------------------------------- balance

namespace {
// Mean power per band over [lo, hi): the level of a range, independent of how many bands it has.
double meanPower(const BandLayout& b, const std::vector<double>& power, double lo, double hi) {
    double sum = 0;
    int n = 0;
    for (int i = 0; i < b.size() && i < int(power.size()); ++i)
        if (b.centre[size_t(i)] >= lo && b.centre[size_t(i)] < hi) { sum += power[size_t(i)]; ++n; }
    return n ? sum / n : 0.0;
}
constexpr double kRefLo = 250.0, kRefHi = 4000.0;   // levels are relative to the mids
}  // namespace

BalanceProfile BalanceProfile::fromPower(const BandLayout& b, const std::vector<double>& power, double, double) {
    BalanceProfile p;
    const double ref = meanPower(b, power, kRefLo, kRefHi);
    if (ref <= 1e-20) return p;
    p.shareDb.resize(power.size());
    for (size_t i = 0; i < power.size(); ++i) p.shareDb[i] = std::max(-120.0, toDb(power[i] / ref));
    return p;
}

BalanceTarget BalanceTarget::fromProfiles(const std::vector<BalanceProfile>& profiles, double minHalfWidth) {
    BalanceTarget t;
    std::vector<const BalanceProfile*> ok;
    for (auto& p : profiles)
        if (p.valid()) ok.push_back(&p);
    if (ok.empty()) return t;
    const size_t n = ok.front()->shareDb.size();
    t.mean.assign(n, 0.0);
    t.halfWidth.assign(n, minHalfWidth);
    t.references = int(ok.size());
    for (size_t i = 0; i < n; ++i) {
        double s = 0, s2 = 0;
        for (auto* p : ok) {
            const double v = std::max(-120.0, p->shareDb[i]);
            s += v;
            s2 += v * v;
        }
        const double m = s / double(ok.size());
        const double var = std::max(0.0, s2 / double(ok.size()) - m * m);
        t.mean[i] = m;
        t.halfWidth[i] = std::max(minHalfWidth, std::sqrt(var));
    }
    return t;
}

double BalanceRange::outsideDb() const {
    const double d = diffDb();
    return std::abs(d) <= halfWidthDb ? 0.0 : (d > 0 ? d - halfWidthDb : d + halfWidthDb);
}

std::vector<BalanceRange> balanceRanges(const BandLayout& b, const std::vector<double>& programmePower, const BalanceTarget& target,
                                        const std::vector<BalanceProfile>& refs) {
    static const BalanceRange defs[] = {
        {"Sub", 20, 60}, {"Low", 60, 250}, {"Low mid", 250, 1000}, {"High mid", 1000, 4000}, {"Presence", 4000, 10000}, {"Air", 10000, 20000},
    };
    std::vector<BalanceRange> out;
    // A range's level against the mids (mean power per band, so wide and narrow ranges compare).
    auto share = [&](const std::vector<double>& pow, double lo, double hi) {
        const double ref = meanPower(b, pow, kRefLo, kRefHi), part = meanPower(b, pow, lo, hi);
        return ref > 0 && part > 0 ? 10.0 * std::log10(part / ref) : -120.0;
    };
    for (auto r : defs) {
        if (r.fLo >= b.hi.back()) continue;
        r.programmeDb = share(programmePower, r.fLo, r.fHi);
        if (target.valid()) {
            // The target in a range: the mean of each reference's own share, with their spread.
            std::vector<double> v;
            for (auto& p : refs) {
                if (!p.valid()) continue;
                std::vector<double> pw(p.shareDb.size());
                for (size_t i = 0; i < pw.size(); ++i) pw[i] = std::pow(10.0, p.shareDb[i] / 10.0);
                v.push_back(share(pw, r.fLo, r.fHi));
            }
            if (!v.empty()) {
                const double m = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
                double var = 0;
                for (double x : v) var += (x - m) * (x - m);
                r.targetDb = m;
                r.halfWidthDb = std::max(1.0, std::sqrt(var / double(v.size())));
            }
        }
        out.push_back(r);
    }
    return out;
}

std::vector<std::string> balanceNotes(const std::vector<BalanceRange>& ranges) {
    std::vector<const BalanceRange*> off;
    for (auto& r : ranges)
        if (std::abs(r.outsideDb()) >= 0.5) off.push_back(&r);
    std::sort(off.begin(), off.end(), [](auto* a, auto* b) { return std::abs(a->outsideDb()) > std::abs(b->outsideDb()); });
    std::vector<std::string> notes;
    char buf[200];
    for (auto* r : off) {
        const double d = r->outsideDb();
        std::snprintf(buf, sizeof buf, "%s (%g-%g Hz): %.1f dB %s the reference zone", r->name, r->fLo, r->fHi, std::abs(d),
                      d > 0 ? "above" : "below");
        notes.push_back(buf);
    }
    return notes;
}

const std::vector<LoudnessStandard>& loudnessStandards() {
    static const std::vector<LoudnessStandard> s{
        {"EBU R128 (broadcast)", -23.0, 0.5, -1.0},
        {"ATSC A/85 (US TV)", -24.0, 2.0, -2.0},
        {"ARIB TR-B32 (Japan TV)", -24.0, 1.0, -1.0},
        {"OP-59 (Australian TV)", -24.0, 1.0, -2.0},
        {"Streaming -14 (Spotify, YouTube, Tidal)", -14.0, 1.0, -1.0},
        {"Apple Music / iTunes -16", -16.0, 1.0, -1.0},
        {"Amazon Music -14", -14.0, 1.0, -2.0},
        {"Podcast -16", -16.0, 1.0, -1.0},
        {"CD master (no target)", -9.0, 100.0, -0.3},
    };
    return s;
}

}  // namespace lathe::dsp
