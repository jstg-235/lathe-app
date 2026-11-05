// Offline renders: DDP 2.0 image, per-track CD WAVs, and a hi-res programme WAV.
// Signal flow: clips → plugin chain (at project rate, latency-compensated)
//              → r8brain SRC to 44.1 kHz → TPDF dither → 16-bit.
#pragma once

#include "AudioEngine.h"
#include "Session.h"

#include "../../core/ddp/Ddp.h"
#include "../../core/dsp/Meters.h"

namespace lathe {

struct QcReport {
    double integratedLufs = -INFINITY, truePeakDb = -INFINITY, lra = 0;
    juce::int64 clippedSamples = 0;
    struct TrackQc { double lufs = -INFINITY, truePeakDb = -INFINITY; };
    juce::Array<TrackQc> tracks;
    juce::String toText() const;
};

struct ExportSettings {
    dsp::Ditherer::Shape shape = dsp::Ditherer::Shape::None;
    bool runPlugins = true;
};

class Exporter {
public:
    Exporter(AudioEngine& e) : engine_(e) {}

    // Each call blocks; run from a background thread. `progress` gets 0..1; return false
    // from `keepGoing` to cancel.
    using Keep = std::function<bool(double)>;

    ddp::WriteResult exportDdp(const Session& s, const juce::File& folder, const ExportSettings& es,
                               const Keep& keepGoing, QcReport& qc);
    juce::String exportTrackWavs(const Session& s, const juce::File& folder, const ExportSettings& es,
                                 const Keep& keepGoing);
    juce::String exportProgramWav(const Session& s, const juce::File& file, int bits, const ExportSettings& es,
                                  const Keep& keepGoing);
    // Loudness / true peak / overs for the CD master, without writing anything.
    juce::String analyse(const Session& s, const ExportSettings& es, const Keep& keepGoing, QcReport& qc);

private:
    AudioEngine& engine_;
};

}  // namespace lathe
