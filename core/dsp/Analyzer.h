// Programme analyser: one pass over the master produces
//  - a loudness map (momentary and short-term LUFS every 100 ms, plus peak per 100 ms),
//  - per-track statistics (integrated LUFS, LRA, true peak, sample peak, DC, correlation),
//  - a problem list: clipping runs, true-peak overs, DC offset, phase (anti-correlation),
//    and dropouts (digital silence inside a track).
#pragma once

#include "Meters.h"

#include <cmath>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lathe::dsp {

struct LoudnessMap {
    double hopSec = 0.1;
    std::vector<float> momentary, shortTerm, peakDb;   // one value per hop; -inf as -200
    double valueAt(const std::vector<float>& v, double sec) const;
};

struct Problem {
    enum Kind { Clip, TruePeak, DcOffset, Phase, Dropout };
    Kind kind;
    double start = 0, end = 0;   // seconds
    int track = 0;               // 1-based, 0 = outside tracks
    double value = 0;            // kind-specific (samples clipped, dBTP, DC %, correlation, ms)
    bool severe = false;         // error vs warning
    std::string describe() const;
};

struct TrackStats {
    double start = 0, end = 0;
    double lufs = -INFINITY, lra = 0, truePeakDb = -INFINITY, samplePeakDb = -INFINITY;
    double dcPercent = 0, minCorrelation = 1, avgCorrelation = 1;
    int64_t clippedSamples = 0;
};

struct AnalysisResult {
    double rate = 44100, length = 0;
    double lufs = -INFINITY, lra = 0, truePeakDb = -INFINITY, samplePeakDb = -INFINITY, maxShortTerm = -INFINITY;
    LoudnessMap map;
    std::vector<TrackStats> tracks;
    std::vector<Problem> problems;
    std::string summary() const;
};

struct AnalyzerSettings {
    double truePeakWarnDb = -1.0;    // above this: warning
    double truePeakErrorDb = 0.0;    // above this: error
    int clipRun = 3;                 // consecutive full-scale samples counted as a clip
    double dcWarnPercent = 0.1;      // |mean| as % of full scale
    double phaseWarn = -0.2;         // 400 ms correlation below this (on non-quiet audio)
    double dropoutMs = 10.0;         // exact digital silence inside a track
    double trackEdgeIgnoreSec = 0.5; // silence near track edges isn't a dropout
};

class ProgramAnalyzer {
public:
    // trackRanges: [start, end) seconds per CD track.
    void prepare(double rate, const std::vector<std::pair<double, double>>& trackRanges, AnalyzerSettings s = {});
    void process(const double* l, const double* r, int n);
    AnalysisResult finish();

private:
    void endHop();
    void flushPhaseWindow();
    void flushChunk();
    int trackAt(double sec) const;
    void addProblem(Problem p);

    AnalyzerSettings s_;
    double rate_ = 44100;
    int64_t pos_ = 0;
    int hopLen_ = 4410, inHop_ = 0;
    LoudnessMeter whole_;
    std::vector<std::unique_ptr<LoudnessMeter>> perTrack_;
    std::vector<TrackStats> stats_;
    std::vector<double> dcSum_;
    std::vector<int64_t> trackSamples_;
    std::vector<double> corrSum_;
    std::vector<int> corrCount_;
    std::vector<float> hopBufL_, hopBufR_;
    TruePeak tpL_, tpR_;
    float hopPeak_ = 0;
    // clip run tracking per channel
    int run_[2] = {0, 0};
    int64_t runStart_[2] = {0, 0};
    // dropout tracking
    int64_t zeroRun_ = 0, zeroStart_ = 0;
    // phase window (400 ms)
    double pLL_ = 0, pRR_ = 0, pLR_ = 0;
    int pN_ = 0, pLen_ = 17640;
    std::vector<std::pair<double, double>> ranges_;
    size_t curTrack_ = 0;
    int chunkTrack_ = -1;
    std::vector<float> chunkL_, chunkR_;
    AnalysisResult res_;
};

}  // namespace lathe::dsp
