// Loudness (ITU-R BS.1770-4 / EBU R128) and true-peak metering, plus over/clip detection.
// Real-time safe after prepare(): no allocation in process().
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lathe::dsp {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// BS.1770 K-weighting at any sample rate (bilinear-designed from the analog prototypes).
void designKWeighting(double sampleRate, Biquad& shelf, Biquad& highpass);

// 4x oversampling true-peak detector (dBTP), per channel.
class TruePeak {
public:
    void prepare(double sampleRate);
    void reset();
    // Returns the largest absolute sample seen in this block, oversampled.
    float process(const float* x, int n);
    float maxLinear() const { return max_; }
private:
    static constexpr int kTaps = 48;   // per phase: 12
    std::array<float, kTaps> coeffs_{};
    std::array<float, kTaps / 4> hist_{};
    int pos_ = 0;
    float max_ = 0;
    bool needsOversampling_ = true;
};

class LoudnessMeter {
public:
    void prepare(double sampleRate, int channels);
    void reset();
    void process(const float* const* channels, int numSamples);

    double momentaryLufs() const;   // 400 ms
    double shortTermLufs() const;   // 3 s
    double integratedLufs() const;  // gated, whole programme since reset
    double loudnessRange() const;   // LRA in LU (EBU Tech 3342)
    double truePeakDb() const;      // max over all channels, dBTP
    double truePeakDb(int channel) const;
    int64_t clippedSamples() const { return clipped_; }  // samples at or above full scale
    double maxMomentaryLufs() const { return maxM_; }   // since reset
    double maxShortTermLufs() const { return maxS_; }
    // Counts 100 ms steps since reset: a display can take one short-term reading per step.
    int64_t hops() const { return hopsDone_; }
    double durationSeconds() const { return double(hopsDone_) * 0.1; }

private:
    double sr_ = 48000;
    int ch_ = 2;
    std::vector<Biquad> shelf_, hp_;
    std::vector<TruePeak> tp_;
    int blockSize_ = 4800;          // 100 ms hop
    int inBlock_ = 0;
    double blockSum_ = 0;
    std::vector<double> hops_;      // ring of the last 30 hop energies (3 s)
    int hopPos_ = 0, hopCount_ = 0;
    std::vector<double> gatingBlocks_;   // 400 ms block energies, 75 % overlap
    std::vector<double> shortTermBlocks_;// 3 s block energies at 100 ms hop, for LRA
    int64_t clipped_ = 0;
    int64_t hopsDone_ = 0;
    double maxM_ = -INFINITY, maxS_ = -INFINITY;
    // Integrated and LRA are recomputed only when new blocks arrived.
    mutable int64_t cacheHopsI_ = -1, cacheHopsL_ = -1;
    mutable double cacheI_ = -INFINITY, cacheL_ = 0;
    double windowEnergy(int hops) const;
};

double energyToLufs(double meanSquare);

// Loudness match: measures two time-aligned signals together (BS.1770 K-weighting and channel
// weights, 400 ms blocks every 100 ms) and keeps a sliding average of each, over about the last
// `windowSeconds` of programme. Blocks where either signal is silent (below -70 LUFS) or far
// below the running level (-20 LU, like the relative gate) are left out, so pauses and fades
// don't pull the match around. differenceDb() is how much louder `b` is than `a`.
// Real-time safe: no allocation after prepare(). Up to 6 channels (5.1: L R C LFE Ls Rs, the LFE
// is not counted).
class LoudnessMatch {
public:
    void prepare(double sampleRate, int channels, double windowSeconds = 10.0);
    void reset();
    void process(const float* const* a, const float* const* b, int numSamples);
    bool ready() const { return blocks_ >= 20; }   // 2 s of programme measured
    double differenceDb() const;
    double seconds() const { return double(blocks_) * 0.1; }
private:
    static constexpr int kMax = 6;
    double sr_ = 48000;
    int ch_ = 2, hop_ = 4800, inHop_ = 0, hopsSeen_ = 0, window_ = 100;
    Biquad shelf_[2][kMax], hp_[2][kMax];
    double weight_[kMax]{};
    double hopSum_[2]{};
    double hops_[2][4]{};
    int hopPos_ = 0;
    double avg_[2]{};
    int64_t blocks_ = 0;
};

// TPDF dither with optional error-feedback noise shaping, from 64-bit float to integer PCM.
//  None    flat TPDF (the reference: noise is white, signal fully decorrelated from the error)
//  Light   1st-order high-pass shaping
//  Strong  2nd-order high-pass shaping
//  Medium  5-tap Lipshitz minimally-audible shaping (44.1/48 kHz)
//  Psycho  9-tap psychoacoustic F-weighted shaping (Wannamaker), the most audible-band noise removed
// Medium/Psycho are designed for 44.1-48 kHz; at higher rates they fall back to Light,
// because their curves would push noise into the audible band.
class Ditherer {
public:
    enum class Shape { None, Light, Strong, Medium, Psycho };
    void prepare(int channels, int bits, Shape shape, uint32_t seed = 0x5EED1234u, double sampleRate = 44100.0);
    void reset();
    // n frames of float/double [-1,1) to signed integers of `bits` width, interleaved.
    void process(const float* const* in, int n, int32_t* interleavedOut);
    void process(const double* const* in, int n, int32_t* interleavedOut);
    void process16(const float* const* in, int n, int16_t* interleavedOut);
    void process16(const double* const* in, int n, int16_t* interleavedOut);
private:
    template <typename T> void run(const T* const* in, int n, int32_t* out);
    template <typename T> void run16(const T* const* in, int n, int16_t* out);
    static constexpr int kTaps = 9;
    int ch_ = 2, bits_ = 16, taps_ = 0;
    double coef_[kTaps] = {};
    uint32_t rng_ = 1;
    std::vector<std::array<double, kTaps>> err_;
    double nextUniform();
};

}  // namespace lathe::dsp
