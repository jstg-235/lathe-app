#include "Meters.h"

#include <algorithm>
#include <cmath>

namespace lathe::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAbsGate = -70.0;
}

double energyToLufs(double ms) { return ms > 0 ? -0.691 + 10.0 * std::log10(ms) : -INFINITY; }

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
            if (hopCount_ >= 4) gatingBlocks_.push_back(windowEnergy(4));
            if (hopCount_ >= 30) shortTermBlocks_.push_back(windowEnergy(30));
        }
    }
}

double LoudnessMeter::momentaryLufs() const { return hopCount_ >= 4 ? energyToLufs(windowEnergy(4)) : -INFINITY; }
double LoudnessMeter::shortTermLufs() const { return hopCount_ >= 30 ? energyToLufs(windowEnergy(30)) : -INFINITY; }

double LoudnessMeter::integratedLufs() const {
    double s = 0; int n = 0;
    for (double e : gatingBlocks_) if (energyToLufs(e) > kAbsGate) { s += e; ++n; }
    if (n == 0) return -INFINITY;
    double rel = energyToLufs(s / n) - 10.0;
    s = 0; n = 0;
    for (double e : gatingBlocks_) {
        double l = energyToLufs(e);
        if (l > kAbsGate && l > rel) { s += e; ++n; }
    }
    return n ? energyToLufs(s / n) : -INFINITY;
}

double LoudnessMeter::loudnessRange() const {
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
    return pct(0.95) - pct(0.10);
}

double LoudnessMeter::truePeakDb() const {
    float m = 0;
    for (auto& t : tp_) m = std::max(m, t.maxLinear());
    return m > 0 ? 20.0 * std::log10(double(m)) : -INFINITY;
}

// ---------------------------------------------------------------- Dither

void Ditherer::prepare(int channels, int bits, Shape shape, uint32_t seed) {
    ch_ = channels;
    bits_ = bits;
    shape_ = shape;
    rng_ = seed ? seed : 1;
    err_.assign(size_t(channels), {0.0, 0.0});
}

void Ditherer::reset() {
    for (auto& e : err_) e = {0.0, 0.0};
}

double Ditherer::nextUniform() {
    // xorshift32: fast, deterministic, good enough for dither.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return double(rng_) / 4294967296.0;
}

void Ditherer::process(const float* const* in, int n, int32_t* out) {
    const double scale = double(1LL << (bits_ - 1));
    const double maxV = scale - 1, minV = -scale;
    double c1 = 0, c2 = 0;
    if (shape_ == Shape::Light) c1 = 1.0;
    if (shape_ == Shape::Strong) { c1 = 1.5; c2 = -0.6; }
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < ch_; ++c) {
            auto& e = err_[size_t(c)];
            double x = double(in[c][i]) * scale;
            double v = x - c1 * e[0] - c2 * e[1];
            double tpdf = nextUniform() - nextUniform();   // triangular, ±1 LSB
            double q = std::floor(v + tpdf + 0.5);
            q = std::clamp(q, minV, maxV);
            e[1] = e[0];
            e[0] = std::clamp(q - v, -4.0, 4.0);   // bounded so clipping can't make the shaper ring
            out[i * ch_ + c] = int32_t(q);
        }
    }
}

void Ditherer::process16(const float* const* in, int n, int16_t* out) {
    // Small fixed buffer avoids allocation.
    int32_t tmp[512];
    const int perPass = 512 / std::max(1, ch_);
    const float* ptrs[8] = {};
    for (int done = 0; done < n;) {
        int k = std::min(perPass, n - done);
        for (int c = 0; c < ch_ && c < 8; ++c) ptrs[c] = in[c] + done;
        process(ptrs, k, tmp);
        for (int i = 0; i < k * ch_; ++i) out[done * ch_ + i] = int16_t(tmp[i]);
        done += k;
    }
}

}  // namespace lathe::dsp
