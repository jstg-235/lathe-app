#include "Meters.h"

#include <algorithm>
#include <cmath>

namespace lathe::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAbsGate = -70.0;
}

double energyToLufs(double ms) { return ms > 0 ? -0.691 + 10.0 * std::log10(ms) : -INFINITY; }

void LoudnessMatch::prepare(double fs, int channels, double windowSeconds) {
    sr_ = fs;
    ch_ = std::clamp(channels, 1, kMax);
    hop_ = std::max(1, int(std::lround(fs * 0.1)));
    window_ = std::max(20, int(std::lround(windowSeconds * 10.0)));
    for (int s = 0; s < 2; ++s)
        for (int c = 0; c < kMax; ++c) designKWeighting(fs, shelf_[s][c], hp_[s][c]);
    for (int c = 0; c < kMax; ++c) weight_[c] = 1.0;
    if (ch_ == 6) { weight_[3] = 0.0; weight_[4] = weight_[5] = 1.41; }   // LFE out, surrounds +1.5 dB
    reset();
}

void LoudnessMatch::reset() {
    for (int s = 0; s < 2; ++s) {
        for (int c = 0; c < kMax; ++c) { shelf_[s][c].reset(); hp_[s][c].reset(); }
        hopSum_[s] = 0;
        avg_[s] = 0;
        for (auto& h : hops_[s]) h = 0;
    }
    inHop_ = hopsSeen_ = hopPos_ = 0;
    blocks_ = 0;
}

void LoudnessMatch::process(const float* const* a, const float* const* b, int n) {
    const float* const* sig[2] = {a, b};
    static const double kSilent = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    for (int i = 0; i < n; ++i) {
        for (int s = 0; s < 2; ++s) {
            double e = 0;
            for (int c = 0; c < ch_; ++c) {
                if (weight_[c] == 0.0) continue;
                const double y = hp_[s][c].process(shelf_[s][c].process(double(sig[s][c][i])));
                e += weight_[c] * y * y;
            }
            hopSum_[s] += e;
        }
        if (++inHop_ < hop_) continue;
        for (int s = 0; s < 2; ++s) { hops_[s][hopPos_] = hopSum_[s] / double(hop_); hopSum_[s] = 0; }
        hopPos_ = (hopPos_ + 1) % 4;
        inHop_ = 0;
        if (++hopsSeen_ < 4) continue;
        double blk[2];
        for (int s = 0; s < 2; ++s) blk[s] = (hops_[s][0] + hops_[s][1] + hops_[s][2] + hops_[s][3]) * 0.25;
        if (blk[0] < kSilent || blk[1] < kSilent) continue;
        if (blocks_ >= 20 && (blk[0] < avg_[0] * 0.01 || blk[1] < avg_[1] * 0.01)) continue;
        // A plain mean until the window is full, then a sliding (exponential) average over it.
        const double k = 1.0 / double(std::min<int64_t>(blocks_ + 1, window_));
        for (int s = 0; s < 2; ++s) avg_[s] += (blk[s] - avg_[s]) * k;
        ++blocks_;
    }
}

double LoudnessMatch::differenceDb() const {
    if (blocks_ == 0 || avg_[0] <= 0 || avg_[1] <= 0) return 0.0;
    return 10.0 * std::log10(avg_[1] / avg_[0]);
}

void designKWeighting(double fs, Biquad& shelf, Biquad& hp) {
    // Stage 1: high-shelf (head acoustics). Analog parameters from BS.1770, re-derived for fs.
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    // Stage 2: RLB high-pass.
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0;
        hp.a1 = 2.0 * (K * K - 1.0) / a0;
        hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
    shelf.reset();
    hp.reset();
}

// ---------------------------------------------------------------- True peak

void TruePeak::prepare(double sampleRate) {
    needsOversampling_ = sampleRate < 176400.0;
    // 4x interpolation filter: Kaiser-windowed sinc, cutoff at the original Nyquist (~0.9 for margin).
    const double beta = 7.0;
    auto bessel0 = [](double x) {
        double s = 1, t = 1;
        for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
        return s;
    };
    const double c = 0.5 * (kTaps - 1);
    for (int i = 0; i < kTaps; ++i) {
        double n = i - c;
        double x = n / 4.0;
        double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x * 0.92) / (kPi * x * 0.92) * 0.92;
        double r = n / c;
        double w = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
        coeffs_[size_t(i)] = float(sinc * w);
    }
    // Normalise each polyphase branch to unity DC gain.
    for (int p = 0; p < 4; ++p) {
        double s = 0;
        for (int k = 0; k < kTaps / 4; ++k) s += coeffs_[size_t(k * 4 + p)];
        for (int k = 0; k < kTaps / 4; ++k) coeffs_[size_t(k * 4 + p)] = float(coeffs_[size_t(k * 4 + p)] / s);
    }
    reset();
}

void TruePeak::reset() {
    hist_.fill(0);
    pos_ = 0;
    max_ = 0;
}

float TruePeak::process(const float* x, int n) {
    float blockMax = 0;
    constexpr int L = kTaps / 4;
    for (int i = 0; i < n; ++i) {
        float s = x[i];
        if (!needsOversampling_) { blockMax = std::max(blockMax, std::abs(s)); continue; }
        pos_ = (pos_ + L - 1) % L;
        hist_[size_t(pos_)] = s;
        for (int p = 0; p < 4; ++p) {
            float acc = 0;
            for (int k = 0; k < L; ++k) acc += coeffs_[size_t(k * 4 + p)] * hist_[size_t((pos_ + k) % L)];
            blockMax = std::max(blockMax, std::abs(acc));
        }
        blockMax = std::max(blockMax, std::abs(s));
    }
    max_ = std::max(max_, blockMax);
    return blockMax;
}

// ---------------------------------------------------------------- Loudness

void LoudnessMeter::prepare(double sampleRate, int channels) {
    sr_ = sampleRate;
    ch_ = channels;
    shelf_.assign(size_t(channels), {});
    hp_.assign(size_t(channels), {});
    tp_.assign(size_t(channels), {});
    for (int c = 0; c < channels; ++c) {
        designKWeighting(sampleRate, shelf_[size_t(c)], hp_[size_t(c)]);
        tp_[size_t(c)].prepare(sampleRate);
    }
    blockSize_ = int(std::lround(sampleRate * 0.1));
    hops_.assign(30, 0.0);
    gatingBlocks_.reserve(36000 * 3);       // 3 hours before any reallocation
    shortTermBlocks_.reserve(36000 * 3);
    reset();
}

void LoudnessMeter::reset() {
    for (auto& b : shelf_) b.reset();
    for (auto& b : hp_) b.reset();
    for (auto& t : tp_) t.reset();
    std::fill(hops_.begin(), hops_.end(), 0.0);
    hopPos_ = hopCount_ = 0;
    inBlock_ = 0;
    blockSum_ = 0;
    gatingBlocks_.clear();
    shortTermBlocks_.clear();
    clipped_ = 0;
    hopsDone_ = 0;
    maxM_ = maxS_ = -INFINITY;
    cacheHopsI_ = cacheHopsL_ = -1;
}

double LoudnessMeter::windowEnergy(int hops) const {
    int n = std::min(hops, hopCount_);
    if (n == 0) return 0;
    double s = 0;
    for (int i = 0; i < n; ++i) s += hops_[size_t((hopPos_ - 1 - i + 30) % 30)];
    return s / (double(n) * blockSize_);
}

void LoudnessMeter::process(const float* const* x, int n) {
    for (int c = 0; c < ch_; ++c) {
        tp_[size_t(c)].process(x[c], n);
        for (int i = 0; i < n; ++i)
            if (std::abs(x[c][i]) >= 0.99997f) ++clipped_;   // within 1 LSB of 16-bit full scale
    }
    for (int i = 0; i < n; ++i) {
        double sum = 0;
        for (int c = 0; c < ch_; ++c) {
            double y = hp_[size_t(c)].process(shelf_[size_t(c)].process(x[c][i]));
            sum += y * y;   // L/R/C weight 1.0 (surround weighting not needed for stereo mastering)
        }
        blockSum_ += sum;
        if (++inBlock_ == blockSize_) {
            hops_[size_t(hopPos_)] = blockSum_;
            hopPos_ = (hopPos_ + 1) % 30;
            hopCount_ = std::min(hopCount_ + 1, 30);
            inBlock_ = 0;
            blockSum_ = 0;
            ++hopsDone_;
            if (hopCount_ >= 4) {
                gatingBlocks_.push_back(windowEnergy(4));
                maxM_ = std::max(maxM_, energyToLufs(gatingBlocks_.back()));
            }
            if (hopCount_ >= 30) {
                shortTermBlocks_.push_back(windowEnergy(30));
                maxS_ = std::max(maxS_, energyToLufs(shortTermBlocks_.back()));
            }
        }
    }
}

double LoudnessMeter::momentaryLufs() const { return hopCount_ >= 4 ? energyToLufs(windowEnergy(4)) : -INFINITY; }
double LoudnessMeter::shortTermLufs() const { return hopCount_ >= 30 ? energyToLufs(windowEnergy(30)) : -INFINITY; }

double LoudnessMeter::integratedLufs() const {
    if (cacheHopsI_ == hopsDone_) return cacheI_;
    cacheHopsI_ = hopsDone_;
    double s = 0; int n = 0;
    for (double e : gatingBlocks_) if (energyToLufs(e) > kAbsGate) { s += e; ++n; }
    if (n == 0) return cacheI_ = -INFINITY;
    double rel = energyToLufs(s / n) - 10.0;
    s = 0; n = 0;
    for (double e : gatingBlocks_) {
        double l = energyToLufs(e);
        if (l > kAbsGate && l > rel) { s += e; ++n; }
    }
    return cacheI_ = (n ? energyToLufs(s / n) : -INFINITY);
}

double LoudnessMeter::loudnessRange() const {
    if (cacheHopsL_ == hopsDone_) return cacheL_;
    cacheHopsL_ = hopsDone_;
    cacheL_ = 0;
    std::vector<double> gated;
    double s = 0; int n = 0;
    for (double e : shortTermBlocks_) if (energyToLufs(e) > kAbsGate) { s += e; ++n; }
    if (n == 0) return 0;
    double rel = energyToLufs(s / n) - 20.0;
    for (double e : shortTermBlocks_) {
        double l = energyToLufs(e);
        if (l > kAbsGate && l > rel) gated.push_back(l);
    }
    if (gated.size() < 2) return 0;
    std::sort(gated.begin(), gated.end());
    auto pct = [&](double p) { return gated[size_t(std::lround(p * double(gated.size() - 1)))]; };
    return cacheL_ = pct(0.95) - pct(0.10);
}

double LoudnessMeter::truePeakDb(int channel) const {
    if (channel < 0 || channel >= int(tp_.size())) return -INFINITY;
    const float m = tp_[size_t(channel)].maxLinear();
    return m > 0 ? 20.0 * std::log10(double(m)) : -INFINITY;
}

double LoudnessMeter::truePeakDb() const {
    float m = 0;
    for (auto& t : tp_) m = std::max(m, t.maxLinear());
    return m > 0 ? 20.0 * std::log10(double(m)) : -INFINITY;
}

// ---------------------------------------------------------------- Dither

void Ditherer::prepare(int channels, int bits, Shape shape, uint32_t seed, double sampleRate) {
    ch_ = channels;
    bits_ = bits;
    rng_ = seed ? seed : 1;
    err_.assign(size_t(channels), {});
    std::fill(std::begin(coef_), std::end(coef_), 0.0);
    // Noise transfer function = 1 - sum(c_k z^-k).
    static const double lipshitz[5] = {2.033, -2.165, 1.959, -1.590, 0.6149};
    static const double fweighted[9] = {2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847};
    const bool cdRate = sampleRate < 50000.0;
    if ((shape == Shape::Medium || shape == Shape::Psycho) && !cdRate) shape = Shape::Light;
    switch (shape) {
        case Shape::None: taps_ = 0; break;
        case Shape::Light: taps_ = 1; coef_[0] = 1.0; break;
        case Shape::Strong: taps_ = 2; coef_[0] = 1.5; coef_[1] = -0.6; break;
        case Shape::Medium: taps_ = 5; std::copy(lipshitz, lipshitz + 5, coef_); break;
        case Shape::Psycho: taps_ = 9; std::copy(fweighted, fweighted + 9, coef_); break;
    }
}

void Ditherer::reset() {
    for (auto& e : err_) e.fill(0.0);
}

double Ditherer::nextUniform() {
    // xorshift32: fast, deterministic, good enough for dither.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return double(rng_) / 4294967296.0;
}

template <typename T>
void Ditherer::run(const T* const* in, int n, int32_t* out) {
    const double scale = double(1LL << (bits_ - 1));
    const double maxV = scale - 1, minV = -scale;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < ch_; ++c) {
            auto& e = err_[size_t(c)];
            double x = double(in[c][i]) * scale;
            double fb = 0;
            for (int k = 0; k < taps_; ++k) fb += coef_[k] * e[size_t(k)];
            double v = x - fb;
            // Already exactly at the target word length (untouched 16-bit audio, digital silence):
            // there is no requantisation error to decorrelate, so pass it through bit-for-bit.
            double rv = std::round(v);
            if (std::abs(v - rv) < 1e-9 && rv <= maxV && rv >= minV) {
                for (int k = taps_ - 1; k > 0; --k) e[size_t(k)] = e[size_t(k - 1)];
                if (taps_) e[0] = 0.0;
                nextUniform(); nextUniform();   // keep the noise sequence aligned
                out[i * ch_ + c] = int32_t(rv);
                continue;
            }
            double tpdf = nextUniform() - nextUniform();   // triangular, +/-1 LSB
            double q = std::floor(v + tpdf + 0.5);
            bool clipped = q > maxV || q < minV;
            q = std::clamp(q, minV, maxV);
            for (int k = taps_ - 1; k > 0; --k) e[size_t(k)] = e[size_t(k - 1)];
            // On clipping the shaper would integrate a huge error and ring; reset it instead.
            if (taps_) e[0] = clipped ? 0.0 : std::clamp(q - v, -8.0, 8.0);
            if (clipped) e.fill(0.0);
            out[i * ch_ + c] = int32_t(q);
        }
    }
}

template <typename T>
void Ditherer::run16(const T* const* in, int n, int16_t* out) {
    int32_t tmp[512];
    const int perPass = 512 / std::max(1, ch_);
    const T* ptrs[8] = {};
    for (int done = 0; done < n;) {
        int k = std::min(perPass, n - done);
        for (int c = 0; c < ch_ && c < 8; ++c) ptrs[c] = in[c] + done;
        run(ptrs, k, tmp);
        for (int i = 0; i < k * ch_; ++i) out[done * ch_ + i] = int16_t(tmp[i]);
        done += k;
    }
}

void Ditherer::process(const float* const* in, int n, int32_t* out) { run(in, n, out); }
void Ditherer::process(const double* const* in, int n, int32_t* out) { run(in, n, out); }
void Ditherer::process16(const float* const* in, int n, int16_t* out) { run16(in, n, out); }
void Ditherer::process16(const double* const* in, int n, int16_t* out) { run16(in, n, out); }

}  // namespace lathe::dsp
