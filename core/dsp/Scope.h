// Spectrum, tonal balance and stereo analysis for the meter bridge. Plain C++, no allocation
// in process() after prepare().
//
//  SpectrumAnalyzer  FFT of the mid, side, left and right signals, gathered into fractional-
//                    octave bands. Keeps a fast (about 0.3 s) and a long-term (energy average
//                    since reset) spectrum, the mono-sum loss per band and the left / right
//                    correlation per band.
//  StereoMeter       correlation, width (side against mid), balance, and a ring of recent
//                    sample pairs for a goniometer.
//  BalanceProfile    a spectrum expressed against its own mids (250 Hz - 4 kHz), so two pieces
//                    can be compared at any level. Profiles of reference tracks become a target
//                    zone (mean and spread per band).
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace lathe::dsp {

struct BandLayout {
    std::vector<double> centre, lo, hi;   // Hz
    int size() const { return int(centre.size()); }
    // Bands of 1/`fraction` octave from `fmin` to `fmax`, centred on 1 kHz multiples.
    static BandLayout fractional(int fraction, double fmin = 20.0, double fmax = 20000.0);
};

class SpectrumAnalyzer {
public:
    void prepare(double sampleRate, int fftOrder = 13, int bandsPerOctave = 6);
    void reset();
    void process(const float* left, const float* right, int n);

    const BandLayout& bands() const { return bands_; }
    double sampleRate() const { return sr_; }
    // Band levels in dB (power per band, a full-scale sine reads about 0 dB in its band).
    // `which`: 0 mid (L+R)/2, 1 side (L-R)/2, 2 left, 3 right.
    const std::vector<double>& fastDb(int which = 0) const { return fastDb_[size_t(which)]; }
    const std::vector<double>& averageDb(int which = 0) const { return avgDb_[size_t(which)]; }
    // Long-term band energies (linear power), for profiles.
    const std::vector<double>& averagePower(int which = 0) const { return avgPow_[size_t(which)]; }
    // Mono check: level of the mono sum against the average of the two sides, per band, dB.
    // 0 = nothing lost in mono; strongly negative = cancels in mono (out of phase, comb filtering).
    const std::vector<double>& monoLossDb() const { return monoLoss_; }
    // Left / right correlation per band (-1..1), fast.
    const std::vector<double>& bandCorrelation() const { return bandCorr_; }
    int64_t framesAnalysed() const { return frames_; }

private:
    void analyse();
    double sr_ = 48000;
    int n_ = 8192, hop_ = 4096;
    std::vector<float> ringL_, ringR_;
    int ringPos_ = 0, sinceHop_ = 0;
    int64_t frames_ = 0;
    std::vector<double> window_;
    double windowPower_ = 1;
    std::vector<std::complex<double>> bufL_, bufR_;
    BandLayout bands_;
    std::vector<int> binLo_, binHi_;
    std::vector<double> fastPow_[4], avgPow_[4], fastDb_[4], avgDb_[4];
    std::vector<double> crossRe_, monoLoss_, bandCorr_, fastMono_, fastSides_;
};

class StereoMeter {
public:
    void prepare(double sampleRate);
    void reset();
    void process(const float* left, const float* right, int n);
    double correlation() const;   // -1..1, about 300 ms
    double width() const;         // side / mid energy, 0 = mono, 1 = as much side as mid
    double balanceDb() const;     // left minus right, dB
    double minCorrelation() const { return minCorr_; }   // since reset
    // The most recent sample pairs, oldest first (for a goniometer).
    void recentPairs(std::vector<float>& l, std::vector<float>& r, int count) const;
private:
    double coef_ = 0.999;
    double ll_ = 0, rr_ = 0, lr_ = 0, mm_ = 0, ss_ = 0;
    double minCorr_ = 1.0;
    int settle_ = 0;
    std::vector<float> ringL_, ringR_;
    int ringPos_ = 0;
};

struct BalanceProfile {
    std::vector<double> shareDb;   // per band: dB against the mean band level of the mids
    bool valid() const { return !shareDb.empty(); }
    // From long-term band energies (bands below `fmin` or above `fmax` are ignored in the total).
    static BalanceProfile fromPower(const BandLayout& b, const std::vector<double>& power, double fmin = 30.0, double fmax = 16000.0);
};

// The target zone from one or more reference profiles: mean and half-width per band.
struct BalanceTarget {
    std::vector<double> mean, halfWidth;
    int references = 0;
    bool valid() const { return !mean.empty(); }
    static BalanceTarget fromProfiles(const std::vector<BalanceProfile>& profiles, double minHalfWidth = 1.5);
};

// The broad ranges a mastering engineer talks about, with the programme against the target.
struct BalanceRange {
    const char* name;
    double fLo, fHi;
    double programmeDb = 0, targetDb = 0, halfWidthDb = 0;   // dB against the mids
    double diffDb() const { return programmeDb - targetDb; }
    // How far outside the zone (0 when inside).
    double outsideDb() const;
};
std::vector<BalanceRange> balanceRanges(const BandLayout& b, const std::vector<double>& programmePower, const BalanceTarget& target,
                                        const std::vector<BalanceProfile>& refs);
// Plain-language notes for ranges outside the zone, largest first.
std::vector<std::string> balanceNotes(const std::vector<BalanceRange>& ranges);

// Loudness standards for the compliance readout.
struct LoudnessStandard {
    const char* name;
    double targetLufs;      // integrated target
    double tolerance;       // +/- LU accepted
    double maxTruePeak;     // dBTP
};
const std::vector<LoudnessStandard>& loudnessStandards();

}  // namespace lathe::dsp
