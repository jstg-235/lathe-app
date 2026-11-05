#include "Analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lathe::dsp {

namespace {
constexpr float kFullScale = 0.99997f;   // within 1 LSB of 16-bit full scale
inline float toDb(double lin) { return lin > 0 ? float(20.0 * std::log10(lin)) : -200.0f; }
inline float lufsOrFloor(double v) { return std::isfinite(v) ? float(v) : -200.0f; }

std::string timeStr(double sec) {
    char b[32];
    int m = int(sec / 60);
    std::snprintf(b, sizeof b, "%02d:%05.2f", m, sec - m * 60);
    return b;
}
}  // namespace

double LoudnessMap::valueAt(const std::vector<float>& v, double sec) const {
    if (v.empty()) return -200;
    auto i = size_t(std::clamp(sec / hopSec, 0.0, double(v.size() - 1)));
    return v[i];
}

std::string Problem::describe() const {
    char b[160];
    switch (kind) {
        case Clip: std::snprintf(b, sizeof b, "Clipping: %d consecutive full-scale samples", int(value)); break;
        case TruePeak: std::snprintf(b, sizeof b, "True peak %+.2f dBTP", value); break;
        case DcOffset: std::snprintf(b, sizeof b, "DC offset %.2f %% of full scale", value); break;
        case Phase: std::snprintf(b, sizeof b, "Out of phase: L/R correlation %.2f", value); break;
        case Dropout: std::snprintf(b, sizeof b, "Dropout: %.0f ms of digital silence inside the track", value); break;
    }
    return b;
}

void ProgramAnalyzer::prepare(double rate, const std::vector<std::pair<double, double>>& ranges, AnalyzerSettings s) {
    s_ = s;
    rate_ = rate;
    ranges_ = ranges;
    pos_ = 0;
    hopLen_ = std::max(1, int(std::lround(rate * 0.1)));
    pLen_ = std::max(1, int(std::lround(rate * 0.4)));
    inHop_ = 0;
    whole_.prepare(rate, 2);
    perTrack_.clear();
    stats_.assign(ranges.size(), {});
    for (size_t i = 0; i < ranges.size(); ++i) {
        perTrack_.push_back(std::make_unique<LoudnessMeter>());
        perTrack_.back()->prepare(rate, 2);
        stats_[i].start = ranges[i].first;
        stats_[i].end = ranges[i].second;
    }
    dcSum_.assign(ranges.size() * 2, 0.0);
    trackSamples_.assign(ranges.size(), 0);
    corrSum_.assign(ranges.size(), 0.0);
    corrCount_.assign(ranges.size(), 0);
    hopBufL_.assign(size_t(hopLen_), 0.f);
    hopBufR_.assign(size_t(hopLen_), 0.f);
    tpL_.prepare(rate);
    tpR_.prepare(rate);
    run_[0] = run_[1] = 0;
    zeroRun_ = 0;
    pLL_ = pRR_ = pLR_ = 0;
    pN_ = 0;
    res_ = {};
    res_.rate = rate;
    res_.map.hopSec = double(hopLen_) / rate;
    curTrack_ = 0;
    chunkTrack_ = -1;
    chunkL_.clear();
    chunkR_.clear();
    chunkL_.reserve(4096);
    chunkR_.reserve(4096);
}

int ProgramAnalyzer::trackAt(double sec) const {
    for (size_t i = 0; i < ranges_.size(); ++i)
        if (sec >= ranges_[i].first && sec < ranges_[i].second) return int(i) + 1;
    return 0;
}

void ProgramAnalyzer::addProblem(Problem p) {
    p.track = trackAt(p.start);
    // Merge with the previous problem of the same kind if they touch.
    if (!res_.problems.empty()) {
        auto& last = res_.problems.back();
        if (p.kind == Problem::Clip && last.kind == Problem::Clip && std::abs(p.start - last.start) < 0.01) {
            last.value = std::max(last.value, p.value);   // same clip on both channels: one entry
            last.end = std::max(last.end, p.end);
            last.severe = last.severe || p.severe;
            return;
        }
        if (last.kind == p.kind && p.start - last.end < 0.5 && (p.kind == Problem::TruePeak || p.kind == Problem::Phase)) {
            last.end = p.end;
            last.value = p.kind == Problem::TruePeak ? std::max(last.value, p.value) : std::min(last.value, p.value);
            last.severe = last.severe || p.severe;
            return;
        }
    }
    res_.problems.push_back(p);
}

void ProgramAnalyzer::flushChunk() {
    if (chunkTrack_ >= 0 && !chunkL_.empty()) {
        const float* ch[2] = {chunkL_.data(), chunkR_.data()};
        perTrack_[size_t(chunkTrack_)]->process(ch, int(chunkL_.size()));
    }
    chunkL_.clear();
    chunkR_.clear();
}

void ProgramAnalyzer::endHop() {
    const float* ch[2] = {hopBufL_.data(), hopBufR_.data()};
    whole_.process(ch, inHop_);
    float tp = std::max(tpL_.process(hopBufL_.data(), inHop_), tpR_.process(hopBufR_.data(), inHop_));
    float tpDb = toDb(tp);
    res_.map.momentary.push_back(lufsOrFloor(whole_.momentaryLufs()));
    res_.map.shortTerm.push_back(lufsOrFloor(whole_.shortTermLufs()));
    res_.map.peakDb.push_back(tpDb);
    double hopEnd = double(pos_) / rate_, hopStart = hopEnd - double(inHop_) / rate_;
    if (tpDb > s_.truePeakWarnDb) addProblem({Problem::TruePeak, hopStart, hopEnd, 0, double(tpDb), tpDb > s_.truePeakErrorDb});
    inHop_ = 0;
}

void ProgramAnalyzer::flushPhaseWindow() {
    if (pN_ == 0) return;
    double ms = (pLL_ + pRR_) / (2.0 * pN_);
    if (ms > 1e-4 && pLL_ > 0 && pRR_ > 0) {   // only judge phase on audio above about -40 dBFS RMS
        double corr = pLR_ / std::sqrt(pLL_ * pRR_);
        double end = double(pos_) / rate_, start = end - double(pN_) / rate_;
        int t = trackAt(start);
        if (t > 0) {
            corrSum_[size_t(t - 1)] += corr;
            corrCount_[size_t(t - 1)]++;
            auto& st = stats_[size_t(t - 1)];
            st.minCorrelation = std::min(st.minCorrelation, corr);
        }
        if (corr < s_.phaseWarn) addProblem({Problem::Phase, start, end, 0, corr, corr < -0.5});
    }
    pLL_ = pRR_ = pLR_ = 0;
    pN_ = 0;
}

void ProgramAnalyzer::process(const double* l, const double* r, int n) {
    const int64_t minZeros = int64_t(s_.dropoutMs * rate_ / 1000.0);
    for (int i = 0; i < n; ++i) {
        const double x[2] = {l[i], r[i]};
        const double sec = double(pos_) / rate_;
        while (curTrack_ < ranges_.size() && sec >= ranges_[curTrack_].second) ++curTrack_;
        const int tIdx = (curTrack_ < ranges_.size() && sec >= ranges_[curTrack_].first) ? int(curTrack_) : -1;

        // Clipping runs.
        for (int c = 0; c < 2; ++c) {
            if (std::abs(x[c]) >= kFullScale) {
                if (run_[c]++ == 0) runStart_[c] = pos_;
            } else {
                if (run_[c] >= s_.clipRun)
                    addProblem({Problem::Clip, double(runStart_[c]) / rate_, sec, 0, double(run_[c]), run_[c] >= 8});
                run_[c] = 0;
            }
        }
        // Dropouts: exact digital silence on both channels, away from track edges.
        if (x[0] == 0.0 && x[1] == 0.0) {
            if (zeroRun_++ == 0) zeroStart_ = pos_;
        } else {
            if (zeroRun_ >= minZeros) {
                double a = double(zeroStart_) / rate_, b = sec;
                for (auto& rg : ranges_)
                    if (a > rg.first + s_.trackEdgeIgnoreSec && b < rg.second - s_.trackEdgeIgnoreSec) {
                        addProblem({Problem::Dropout, a, b, 0, (b - a) * 1000.0, (b - a) > 0.1});
                        break;
                    }
            }
            zeroRun_ = 0;
        }
        // Per-track accumulators.
        if (tIdx >= 0) {
            auto& st = stats_[size_t(tIdx)];
            dcSum_[size_t(tIdx) * 2] += x[0];
            dcSum_[size_t(tIdx) * 2 + 1] += x[1];
            trackSamples_[size_t(tIdx)]++;
            double pk = std::max(std::abs(x[0]), std::abs(x[1]));
            st.samplePeakDb = std::max(st.samplePeakDb, double(toDb(pk)));
            if (std::abs(x[0]) >= kFullScale || std::abs(x[1]) >= kFullScale) st.clippedSamples++;
            if (tIdx != chunkTrack_ || chunkL_.size() >= 4096) {
                flushChunk();
                chunkTrack_ = tIdx;
            }
            chunkL_.push_back(float(x[0]));
            chunkR_.push_back(float(x[1]));
        } else if (chunkTrack_ >= 0) {
            flushChunk();
            chunkTrack_ = -1;
        }
        res_.samplePeakDb = std::max(res_.samplePeakDb, double(toDb(std::max(std::abs(x[0]), std::abs(x[1])))));

        // Phase window.
        pLL_ += x[0] * x[0];
        pRR_ += x[1] * x[1];
        pLR_ += x[0] * x[1];
        ++pN_;
        ++pos_;
        if (pN_ == pLen_) flushPhaseWindow();

        hopBufL_[size_t(inHop_)] = float(x[0]);
        hopBufR_[size_t(inHop_)] = float(x[1]);
        if (++inHop_ == hopLen_) endHop();
    }
}

AnalysisResult ProgramAnalyzer::finish() {
    if (inHop_ > 0) endHop();
    flushPhaseWindow();
    flushChunk();
    const double end = double(pos_) / rate_;
    for (int c = 0; c < 2; ++c)
        if (run_[c] >= s_.clipRun) addProblem({Problem::Clip, double(runStart_[c]) / rate_, end, 0, double(run_[c]), run_[c] >= 8});
    res_.length = end;
    res_.lufs = whole_.integratedLufs();
    res_.lra = whole_.loudnessRange();
    res_.truePeakDb = whole_.truePeakDb();
    for (float v : res_.map.shortTerm) res_.maxShortTerm = std::max(res_.maxShortTerm, double(v));
    for (size_t i = 0; i < stats_.size(); ++i) {
        auto& st = stats_[i];
        st.lufs = perTrack_[i]->integratedLufs();
        st.lra = perTrack_[i]->loudnessRange();
        st.truePeakDb = perTrack_[i]->truePeakDb();
        if (trackSamples_[i] > 0) {
            double dl = dcSum_[i * 2] / double(trackSamples_[i]), dr = dcSum_[i * 2 + 1] / double(trackSamples_[i]);
            st.dcPercent = 100.0 * std::max(std::abs(dl), std::abs(dr));
            if (st.dcPercent > s_.dcWarnPercent)
                res_.problems.push_back({Problem::DcOffset, st.start, st.end, int(i) + 1, st.dcPercent, st.dcPercent > 1.0});
        }
        st.avgCorrelation = corrCount_[i] ? corrSum_[i] / corrCount_[i] : 1.0;
    }
    std::stable_sort(res_.problems.begin(), res_.problems.end(), [](const Problem& a, const Problem& b) { return a.start < b.start; });
    res_.tracks = stats_;
    return res_;
}

std::string AnalysisResult::summary() const {
    std::string s;
    char b[256];
    auto f = [](double v, const char* unit) {
        char t[32];
        if (std::isfinite(v) && v > -199) std::snprintf(t, sizeof t, "%.1f %s", v, unit);
        else std::snprintf(t, sizeof t, "--");
        return std::string(t);
    };
    s += "PROGRAMME\n";
    s += "  Integrated " + f(lufs, "LUFS") + "   LRA " + f(lra, "LU") + "   Max short-term " + f(maxShortTerm, "LUFS") + "\n";
    s += "  True peak " + f(truePeakDb, "dBTP") + "   Sample peak " + f(samplePeakDb, "dBFS");
    if (std::isfinite(truePeakDb) && std::isfinite(lufs)) { std::snprintf(b, sizeof b, "   PLR %.1f dB", truePeakDb - lufs); s += b; }
    s += "\n\nTRACKS\n";
    s += "  #   Start     LUFS     LRA   TruePk  SmplPk   DC%    MinCorr\n";
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& t = tracks[i];
        auto num = [](double v, int dp) {
            char t[16];
            if (std::isfinite(v) && v > -199) std::snprintf(t, sizeof t, "%.*f", dp, v);
            else std::snprintf(t, sizeof t, "--");
            return std::string(t);
        };
        std::snprintf(b, sizeof b, "  %-2d  %s  %7s  %5s  %7s  %7s  %5.2f  %6.2f\n", int(i + 1), timeStr(t.start).c_str(),
                      num(t.lufs, 1).c_str(), num(t.lra, 1).c_str(), num(t.truePeakDb, 2).c_str(), num(t.samplePeakDb, 2).c_str(),
                      t.dcPercent, t.minCorrelation);
        s += b;
    }
    int errors = 0;
    for (auto& p : problems) errors += p.severe;
    std::snprintf(b, sizeof b, "\nPROBLEMS: %d (%d serious)\n", int(problems.size()), errors);
    s += b;
    for (auto& p : problems) {
        std::snprintf(b, sizeof b, "  %s %s  track %d  %s\n", p.severe ? "!!" : " -", timeStr(p.start).c_str(), p.track, p.describe().c_str());
        s += b;
    }
    return s;
}

}  // namespace lathe::dsp
