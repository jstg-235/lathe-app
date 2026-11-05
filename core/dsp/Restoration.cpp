#include "Restoration.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lathe::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cplx = std::complex<double>;

size_t frames(const Channels& ch) { return ch.empty() ? 0 : ch[0].size(); }

// ---------------------------------------------------------------- STFT helper
// sqrt-Hann analysis and synthesis at 75 % overlap: perfect reconstruction when unmodified.
struct Stft {
    int n, hop;
    std::vector<double> win;
    explicit Stft(int size) : n(size), hop(size / 4), win(size_t(size)) {
        for (int i = 0; i < n; ++i) win[size_t(i)] = std::sqrt(0.5 - 0.5 * std::cos(2 * kPi * i / n));
    }
    int numFrames(size_t len) const { return int((len + size_t(n)) / size_t(hop)) + 1; }
    // Frame f starts at sample f*hop - n (so the first frames cover the start fully).
    void analyse(const std::vector<double>& x, int f, std::vector<cplx>& out) const {
        out.assign(size_t(n), 0.0);
        long start = long(f) * hop - n;
        for (int i = 0; i < n; ++i) {
            long k = start + i;
            if (k >= 0 && k < long(x.size())) out[size_t(i)] = x[size_t(k)] * win[size_t(i)];
        }
        fft(out, false);
    }
    void synthAdd(std::vector<cplx>& spec, int f, std::vector<double>& acc) const {
        fft(spec, true);
        long start = long(f) * hop - n;
        for (int i = 0; i < n; ++i) {
            long k = start + i;
            if (k >= 0 && k < long(acc.size())) acc[size_t(k)] += spec[size_t(i)].real() * win[size_t(i)] * 0.5;   // hann sums to 2 at 75 %
        }
    }
};

// ---------------------------------------------------------------- zero-phase biquad
struct Biq {
    double b0, b1, b2, a1, a2;
    void run(std::vector<double>& x) const {
        double z1 = 0, z2 = 0;
        for (auto& v : x) {
            double y = b0 * v + z1;
            z1 = b1 * v - a1 * y + z2;
            z2 = b2 * v - a2 * y;
            v = y;
        }
    }
    void filtfilt(std::vector<double>& x) const {
        run(x);
        std::reverse(x.begin(), x.end());
        run(x);
        std::reverse(x.begin(), x.end());
    }
};

Biq notch(double f, double rate, double q) {
    double w = 2 * kPi * f / rate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
    return {1 / a0, -2 * c / a0, 1 / a0, -2 * c / a0, (1 - alpha) / a0};
}

// ---------------------------------------------------------------- AR modelling
// Autocorrelation-method LPC via Levinson-Durbin. a[0] = 1.
std::vector<double> lpc(const double* x, int n, int order) {
    std::vector<double> r(size_t(order + 1), 0.0);
    for (int k = 0; k <= order; ++k)
        for (int i = k; i < n; ++i) r[size_t(k)] += x[i] * x[i - k];
    std::vector<double> a(size_t(order + 1), 0.0);
    a[0] = 1;
    if (r[0] <= 1e-20) return a;
    r[0] *= 1.0 + 1e-9;   // slight regularisation
    double err = r[0];
    for (int i = 1; i <= order; ++i) {
        double acc = r[size_t(i)];
        for (int j = 1; j < i; ++j) acc += a[size_t(j)] * r[size_t(i - j)];
        double k = -acc / err;
        std::vector<double> prev(a);
        for (int j = 1; j < i; ++j) a[size_t(j)] = prev[size_t(j)] + k * prev[size_t(i - j)];
        a[size_t(i)] = k;
        err *= (1 - k * k);
        if (err <= 1e-20) break;
    }
    return a;
}

// Solve the symmetric positive system M x = b (Cholesky); M is n x n row-major.
bool cholSolve(std::vector<double>& M, std::vector<double>& b, int n) {
    for (int j = 0; j < n; ++j) {
        double s = M[size_t(j * n + j)];
        for (int k = 0; k < j; ++k) s -= M[size_t(j * n + k)] * M[size_t(j * n + k)];
        if (s <= 1e-18) return false;
        double d = std::sqrt(s);
        M[size_t(j * n + j)] = d;
        for (int i = j + 1; i < n; ++i) {
            double t = M[size_t(i * n + j)];
            for (int k = 0; k < j; ++k) t -= M[size_t(i * n + k)] * M[size_t(j * n + k)];
            M[size_t(i * n + j)] = t / d;
        }
    }
    for (int i = 0; i < n; ++i) {
        double t = b[size_t(i)];
        for (int k = 0; k < i; ++k) t -= M[size_t(i * n + k)] * b[size_t(k)];
        b[size_t(i)] = t / M[size_t(i * n + i)];
    }
    for (int i = n - 1; i >= 0; --i) {
        double t = b[size_t(i)];
        for (int k = i + 1; k < n; ++k) t -= M[size_t(k * n + i)] * b[size_t(k)];
        b[size_t(i)] = t / M[size_t(i * n + i)];
    }
    return true;
}

// Least-squares AR interpolation of x[g0, g1) from the surrounding context.
void lsarInterpolate(std::vector<double>& x, long g0, long g1, int order) {
    const long L = g1 - g0;
    const long ctx = std::max<long>(order * 4, L * 3);
    long c0 = std::max<long>(0, g0 - ctx), c1 = std::min<long>(long(x.size()), g1 + ctx);
    if (g0 - c0 < order || c1 - g1 < order) return;   // not enough context
    // AR model from the clean context either side (gap zeroed so it doesn't bias the estimate).
    std::vector<double> seg(x.begin() + c0, x.begin() + c1);
    for (long i = g0; i < g1; ++i) seg[size_t(i - c0)] = 0;
    auto a = lpc(seg.data(), int(seg.size()), order);
    // Minimise sum_n (sum_k a_k x[n-k])^2 over n in [c0+order, c1) w.r.t. the gap samples.
    const int n = int(L);
    std::vector<double> M(size_t(n * n), 0.0), rhs(size_t(n), 0.0);
    for (long t = c0 + order; t < c1; ++t) {
        // e(t) = sum_k a_k x[t-k] = known part + sum over unknown u: a_{t-u} x_u
        double known = 0;
        int ui[64];
        double uc[64];
        int nu = 0;
        for (int k = 0; k <= order; ++k) {
            long s = t - k;
            if (s >= g0 && s < g1) {
                if (nu < 64) { ui[nu] = int(s - g0); uc[nu] = a[size_t(k)]; ++nu; }
            } else {
                known += a[size_t(k)] * x[size_t(s)];
            }
        }
        for (int p = 0; p < nu; ++p) {
            rhs[size_t(ui[p])] -= uc[p] * known;
            for (int q2 = 0; q2 < nu; ++q2) M[size_t(ui[p] * n + ui[q2])] += uc[p] * uc[q2];
        }
    }
    for (int i = 0; i < n; ++i) M[size_t(i * n + i)] += 1e-9;
    if (cholSolve(M, rhs, n))
        for (int i = 0; i < n; ++i) x[size_t(g0 + i)] = rhs[size_t(i)];
}

ClickStats detectAndRepair(Channels& ch, double rate, double threshold, int maxLen, int order, int passes) {
    ClickStats st;
    const size_t len = frames(ch);
    if (len < size_t(order * 8)) return st;
    const int block = int(std::max(2048.0, rate * 0.05));
    for (int pass = 0; pass < passes; ++pass) {
        // Joint detector: residual energy summed over channels.
        std::vector<double> resid(len, 0.0);
        for (auto& x : ch) {
            for (size_t b0 = 0; b0 < len; b0 += size_t(block)) {
                size_t b1 = std::min(len, b0 + size_t(block));
                size_t w0 = b0 >= size_t(order) ? b0 - size_t(order) : 0;
                auto a = lpc(x.data() + w0, int(b1 - w0), order);
                for (size_t t = std::max(b0, size_t(order)); t < b1; ++t) {
                    double e = 0;
                    for (int k = 0; k <= order; ++k) e += a[size_t(k)] * x[t - size_t(k)];
                    resid[t] += e * e;
                }
            }
        }
        // Robust local scale: median of |e| over a sliding window, sampled per block.
        std::vector<double> scale((len + size_t(block) - 1) / size_t(block), 0.0);
        for (size_t b = 0; b < scale.size(); ++b) {
            size_t b0 = b * size_t(block), b1 = std::min(len, b0 + size_t(block));
            std::vector<double> v(resid.begin() + long(b0), resid.begin() + long(b1));
            if (v.empty()) continue;
            std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
            scale[b] = std::sqrt(std::max(1e-24, v[v.size() / 2]));
        }
        // Mark samples whose residual exceeds threshold x local scale, widen, merge.
        std::vector<std::pair<long, long>> gaps;
        long cur0 = -1, last = -1;
        for (size_t t = 0; t < len; ++t) {
            if (std::sqrt(resid[t]) > threshold * scale[t / size_t(block)] * 1.4826) {
                if (cur0 < 0) cur0 = long(t);
                last = long(t);
            } else if (cur0 >= 0 && long(t) - last > 16) {
                // A click's tail keeps exciting the predictor a few samples after the spike:
                // repair a little beyond the last flagged sample, and merge flags < 16 apart.
                gaps.push_back({cur0 - 3, last + 6});
                cur0 = -1;
            }
        }
        if (cur0 >= 0) gaps.push_back({cur0 - 3, last + 6});
        int fixedThisPass = 0;
        for (auto& g : gaps) {
            long g0 = std::max<long>(0, g.first), g1 = std::min<long>(long(len), g.second);
            if (g1 - g0 > maxLen || g1 <= g0) continue;
            for (auto& x : ch) lsarInterpolate(x, g0, g1, order);
            ++fixedThisPass;
            st.samplesRepaired += g1 - g0;
        }
        st.found += fixedThisPass;
        if (fixedThisPass == 0) break;
    }
    return st;
}
}  // namespace

// ---------------------------------------------------------------- FFT

void fft(std::vector<cplx>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * kPi / double(len) * (inverse ? 1 : -1);
        cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                cplx u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= double(n);
}

int stftSizeFor(double rate) {
    int n = 4096;
    while (rate > 50000.0 * double(n) / 4096.0 * 1.0 && n < 16384) n *= 2;
    return n;
}

// ---------------------------------------------------------------- DC / hum

void removeDc(Channels& ch) {
    for (auto& x : ch) {
        if (x.empty()) continue;
        double m = std::accumulate(x.begin(), x.end(), 0.0) / double(x.size());
        for (auto& v : x) v -= m;
    }
}

double removeHum(Channels& ch, double rate, double f0, int harmonics, double q) {
    if (f0 <= 0) {
        // Detect: which of 50 / 60 Hz (and their 2nd harmonic) stands out more from the spectrum?
        const int n = 1 << 16;
        std::vector<cplx> s(size_t(n), 0.0);
        size_t len = frames(ch);
        for (size_t i = 0; i < std::min(len, size_t(n)); ++i) {
            double v = 0;
            for (auto& x : ch) v += x[i];
            s[i] = v * (0.5 - 0.5 * std::cos(2 * kPi * double(i) / n));
        }
        fft(s, false);
        auto peakness = [&](double f) {
            double binHz = rate / n, total = 0;
            for (int h = 1; h <= 2; ++h) {
                int b = int(std::round(f * h / binHz));
                double at = std::abs(s[size_t(b)]);
                double around = 0;
                for (int k = 6; k <= 20; ++k) around += std::abs(s[size_t(b - k)]) + std::abs(s[size_t(b + k)]);
                around /= 30.0;
                total += at / std::max(1e-12, around);
            }
            return total;
        };
        double p50 = peakness(50), p60 = peakness(60);
        if (std::max(p50, p60) < 6.0) return 0;   // no clear hum
        f0 = p60 > p50 ? 60.0 : 50.0;
    }
    for (auto& x : ch)
        for (int h = 1; h <= harmonics; ++h) {
            double f = f0 * h;
            if (f >= rate * 0.45) break;
            notch(f, rate, q * h).filtfilt(x);   // narrower notches for higher harmonics
        }
    return f0;
}

// ---------------------------------------------------------------- clicks

ClickStats declick(Channels& ch, double rate, double sensitivity, double maxClickMs) {
    double threshold = 14.0 - std::clamp(sensitivity, 1.0, 10.0);   // 13 (gentle) .. 4 (sensitive)
    int maxLen = int(std::clamp(maxClickMs, 0.1, 5.0) * rate / 1000.0);
    int order = int(std::clamp(rate / 1500.0, 20.0, 64.0));
    return detectAndRepair(ch, rate, threshold, std::min(maxLen, 200), order, 2);
}

ClickStats decrackle(Channels& ch, double rate, double amount) {
    double threshold = 7.0 - std::clamp(amount, 0.0, 10.0) * 0.35;   // 7 .. 3.5
    int order = int(std::clamp(rate / 2000.0, 16.0, 48.0));
    return detectAndRepair(ch, rate, threshold, int(rate * 0.0005) + 4, order, 3);
}

// ---------------------------------------------------------------- noise

NoiseProfile learnNoise(const Channels& ch, double rate) {
    NoiseProfile np;
    np.fftSize = stftSizeFor(rate);
    np.rate = rate;
    Stft st(np.fftSize);
    const size_t len = frames(ch);
    if (len < size_t(np.fftSize)) return np;
    np.power.assign(size_t(np.fftSize / 2 + 1), 0.0);
    int count = 0;
    std::vector<cplx> spec;
    // Use only frames fully inside the noise selection.
    for (int f = 4; long(f) * st.hop <= long(len); ++f) {
        for (auto& x : ch) {
            st.analyse(x, f, spec);
            for (size_t b = 0; b < np.power.size(); ++b) np.power[b] += std::norm(spec[b]);
        }
        ++count;
    }
    if (count == 0) { np.power.clear(); return np; }
    for (auto& p : np.power) p /= double(count) * double(ch.size());
    return np;
}

void denoise(Channels& ch, double rate, const NoiseProfile& np, double reductionDb, double sensitivity) {
    if (!np.valid() || ch.empty()) return;
    const int n = np.fftSize;
    Stft st(n);
    const size_t len = frames(ch), bins = size_t(n / 2 + 1);
    const double floorGain = std::pow(10.0, -std::abs(reductionDb) / 20.0);
    const double alpha = std::clamp(sensitivity, 0.3, 4.0);
    // Release over ~100 ms, attack fast: avoids the warbling "musical noise".
    const double framesPerSec = rate / st.hop;
    const double rel = std::exp(-1.0 / (0.10 * framesPerSec)), att = std::exp(-1.0 / (0.01 * framesPerSec));
    std::vector<double> gain(bins, 1.0), prevGain(bins, 1.0);
    std::vector<std::vector<double>> out(ch.size(), std::vector<double>(len, 0.0));
    std::vector<std::vector<cplx>> spec(ch.size());
    const int nf = st.numFrames(len);
    for (int f = 0; f < nf; ++f) {
        for (size_t c = 0; c < ch.size(); ++c) st.analyse(ch[c], f, spec[c]);
        for (size_t b = 0; b < bins; ++b) {
            double p = 0;
            for (size_t c = 0; c < ch.size(); ++c) p += std::norm(spec[c][b]);
            p /= double(ch.size());
            double noise = alpha * np.power[b];
            double g = p > 0 ? std::max(floorGain, 1.0 - noise / p) : floorGain;   // power-domain Wiener-like gain
            g = std::sqrt(std::max(g, floorGain * floorGain));
            gain[b] = g;
        }
        // Smooth across frequency (3 bins) then in time (attack/release).
        for (size_t b = 0; b < bins; ++b) {
            double s = gain[b] * 2;
            int k = 2;
            if (b > 0) { s += gain[b - 1]; ++k; }
            if (b + 1 < bins) { s += gain[b + 1]; ++k; }
            double g = s / k;
            double coeff = g > prevGain[b] ? att : rel;
            prevGain[b] = g + coeff * (prevGain[b] - g);
        }
        for (size_t c = 0; c < ch.size(); ++c) {
            for (size_t b = 0; b < bins; ++b) {
                spec[c][b] *= prevGain[b];
                if (b > 0 && b < size_t(n / 2)) spec[c][size_t(n) - b] = std::conj(spec[c][b]);
            }
            st.synthAdd(spec[c], f, out[c]);
        }
    }
    for (size_t c = 0; c < ch.size(); ++c) ch[c].swap(out[c]);
}

// ---------------------------------------------------------------- spectral editing

void spectralEdit(Channels& ch, double rate, double t0, double t1, double f0, double f1, SpectralMode mode, double gainDb) {
    if (ch.empty() || t1 <= t0 || f1 <= f0) return;
    const int n = stftSizeFor(rate);
    Stft st(n);
    const size_t len = frames(ch), bins = size_t(n / 2 + 1);
    const double binHz = rate / n;
    const size_t b0 = size_t(std::clamp(std::floor(f0 / binHz), 0.0, double(bins - 1)));
    const size_t b1 = size_t(std::clamp(std::ceil(f1 / binHz), double(b0 + 1), double(bins)));
    // Frames whose centre lies in [t0, t1). Frame f is centred at f*hop - n/2.
    auto frameAt = [&](double t) { return int(std::round((t * rate + n / 2.0) / st.hop)); };
    const int fa = std::max(0, frameAt(t0)), fb = std::max(fa + 1, frameAt(t1));
    // Only re-synthesise the affected span (plus overlap) so everything else stays bit-exact.
    const long s0 = std::max<long>(0, long(fa - 4) * st.hop - n), s1 = std::min<long>(long(len), long(fb + 4) * st.hop);
    const double g = std::pow(10.0, gainDb / 20.0);
    for (auto& x : ch) {
        std::vector<double> acc(len, 0.0);
        std::vector<cplx> spec, before, after;
        if (mode == SpectralMode::Heal) {
            st.analyse(x, std::max(0, fa - 2), before);
            st.analyse(x, fb + 1, after);
        }
        for (int f = std::max(0, fa - 4); f <= fb + 4; ++f) {
            st.analyse(x, f, spec);
            if (f >= fa && f < fb) {
                double w = fb > fa ? double(f - fa + 0.5) / double(fb - fa) : 0.5;
                for (size_t b = b0; b < b1; ++b) {
                    // Soft edges in frequency: 2-bin taper.
                    double edge = 1.0;
                    if (b < b0 + 2) edge = 0.5 + 0.25 * double(b - b0);
                    if (b + 2 >= b1) edge = std::min(edge, 0.5 + 0.25 * double(b1 - 1 - b));
                    if (mode == SpectralMode::Attenuate) {
                        spec[b] *= 1.0 + (g - 1.0) * edge;
                    } else {
                        double mag = std::abs(spec[b]);
                        double target = std::exp((1 - w) * std::log(std::abs(before[b]) + 1e-12) + w * std::log(std::abs(after[b]) + 1e-12));
                        double newMag = mag + (std::min(mag, target) - mag) * edge;
                        if (mag > 0) spec[b] *= newMag / mag;
                    }
                }
                for (size_t b = 1; b < size_t(n / 2); ++b) spec[size_t(n) - b] = std::conj(spec[b]);
            }
            st.synthAdd(spec, f, acc);
        }
        // Splice: inside the affected frames use the re-synthesis; crossfade over one hop at each end.
        const long e0 = std::max<long>(0, long(fa) * st.hop - n), e1 = std::min<long>(long(len), long(fb) * st.hop);
        for (long k = s0; k < s1; ++k) {
            double w = 1.0;
            if (k < e0) w = std::clamp(double(k - (e0 - st.hop)) / st.hop, 0.0, 1.0);
            else if (k >= e1) w = std::clamp(1.0 - double(k - e1) / st.hop, 0.0, 1.0);
            x[size_t(k)] = x[size_t(k)] * (1 - w) + acc[size_t(k)] * w;
        }
    }
}

Spectrogram spectrogram(const Channels& ch, double rate, int maxFrames) {
    Spectrogram sg;
    sg.rate = rate;
    sg.fftSize = std::max(1024, stftSizeFor(rate) / 2);
    const size_t len = frames(ch);
    if (len == 0) return sg;
    sg.hop = std::max(sg.fftSize / 4, int(len / size_t(std::max(1, maxFrames))) + 1);
    std::vector<double> win(size_t(sg.fftSize));
    for (int i = 0; i < sg.fftSize; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / sg.fftSize);
    std::vector<cplx> s;
    const double norm = 4.0 / sg.fftSize;
    for (size_t start = 0; start + size_t(sg.fftSize) <= len + size_t(sg.fftSize / 2); start += size_t(sg.hop)) {
        s.assign(size_t(sg.fftSize), 0.0);
        for (int i = 0; i < sg.fftSize; ++i) {
            size_t k = start + size_t(i);
            if (k >= len) break;
            double v = 0;
            for (auto& x : ch) v += x[k];
            s[size_t(i)] = v / double(ch.size()) * win[size_t(i)];
        }
        fft(s, false);
        std::vector<float> col(size_t(sg.fftSize / 2 + 1));
        for (size_t b = 0; b < col.size(); ++b) col[b] = float(20 * std::log10(std::abs(s[b]) * norm + 1e-10));
        sg.db.push_back(std::move(col));
    }
    return sg;
}

}  // namespace lathe::dsp
