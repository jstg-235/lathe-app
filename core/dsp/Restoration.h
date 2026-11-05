// Offline restoration and spectral repair. All functions work on whole channel buffers
// (64-bit) at any sample rate; they're run on a copy of a region's audio and the result
// is written to a new file, so the original is never touched.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace lathe::dsp {

using Channels = std::vector<std::vector<double>>;

// In-place radix-2 FFT (size must be a power of two).
void fft(std::vector<std::complex<double>>& a, bool inverse);

// ---- DC and hum -----------------------------------------------------------------
void removeDc(Channels& ch);
// Zero-phase notches at f0 and its harmonics. f0 <= 0: detect 50 or 60 Hz automatically.
// Returns the fundamental used (0 if none was found when detecting).
double removeHum(Channels& ch, double rate, double f0, int harmonics, double q);

// ---- Clicks and crackle -----------------------------------------------------------
struct ClickStats {
    int found = 0;
    int64_t samplesRepaired = 0;
};
// sensitivity: 1 (only big clicks) .. 10 (very sensitive). maxClickMs: longest event repaired.
// Detection uses the residual of a short-term linear predictor; repair is least-squares
// autoregressive interpolation from the surrounding audio (both channels detected jointly).
ClickStats declick(Channels& ch, double rate, double sensitivity, double maxClickMs);
// Crackle: many tiny impulses. Several low-threshold, short-gap passes.
ClickStats decrackle(Channels& ch, double rate, double amount);

// ---- Broadband noise --------------------------------------------------------------
struct NoiseProfile {
    int fftSize = 0;
    double rate = 0;
    std::vector<double> power;   // mean power per bin (fftSize/2 + 1)
    bool valid() const { return !power.empty(); }
};
NoiseProfile learnNoise(const Channels& noiseOnly, double rate);
// reductionDb: maximum attenuation of noise-dominated bins (e.g. 6..24).
// sensitivity: 0.5 (gentle) .. 3 (aggressive) multiplier on the noise estimate.
// Gains are computed once from both channels and applied to each, preserving the image.
void denoise(Channels& ch, double rate, const NoiseProfile& np, double reductionDb, double sensitivity);

// ---- Spectral editing ------------------------------------------------------------------
enum class SpectralMode { Attenuate, Heal };
// Edits the time-frequency rectangle [t0,t1) x [f0,f1). Attenuate: scale by gainDb.
// Heal: replace the magnitudes inside with an interpolation of the frames just before and
// after (removes coughs, chair squeaks, phone rings), keeping a smooth transition.
void spectralEdit(Channels& ch, double rate, double t0, double t1, double f0, double f1, SpectralMode mode, double gainDb);

// Spectrogram for display: frames x bins of dB values (mono sum), fftSize chosen from rate.
struct Spectrogram {
    int fftSize = 0, hop = 0;
    double rate = 0;
    std::vector<std::vector<float>> db;   // [frame][bin]
};
Spectrogram spectrogram(const Channels& ch, double rate, int maxFrames = 4000);

int stftSizeFor(double rate);   // 4096 at 44.1/48 kHz, scaled with the rate

}  // namespace lathe::dsp
