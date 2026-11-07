// Offline renders: DDP 2.0 image, per-track CD WAVs, and a hi-res programme WAV.
// Signal flow: clips → plugin chain (at project rate, latency-compensated)
//              → r8brain SRC to 44.1 kHz → TPDF dither → 16-bit.
#pragma once

#include "AudioEngine.h"
#include "Products.h"
#include "Session.h"

#include "../../core/ddp/Ddp.h"
#include "../../core/dsp/Analyzer.h"
#include "../../core/dsp/Meters.h"

namespace lathe {

juce::String ditherName(dsp::Ditherer::Shape s);

struct QcReport {
    double integratedLufs = -INFINITY, truePeakDb = -INFINITY, lra = 0;
    juce::int64 clippedSamples = 0;
    struct TrackQc { double lufs = -INFINITY, truePeakDb = -INFINITY; };
    juce::Array<TrackQc> tracks;
    juce::String toText() const;
};

struct ExportSettings;

// Batch render ("Render Album"): every deliverable in one go.
struct BatchSettings {
    bool ddp = true;
    bool wav16 = true;      // per-track 16-bit / 44.1 kHz WAV, bit-identical to the CD
    bool flac16 = false;    // per-track 16-bit / 44.1 kHz FLAC, tagged
    bool wavHi = true;      // per-track hi-res WAV at the project rate
    bool flacHi = false;    // per-track hi-res FLAC at the project rate, tagged
    bool programme = false; // one hi-res WAV of the whole programme
    bool products = true;   // each product's units: a DDP per CD, a hi-res file per vinyl/tape side
    bool paperwork = true;  // PQ sheets, cutting sheets, label copy, manifest (PDF + CSV + JSON)
    int hiBits = 24;
};

// One rendered unit (a CD of a multi-disc set, an LP side), for the paperwork.
struct UnitRender {
    int product = 0, unit = 0;          // indexes into Session::products / Product::units
    juce::String label, longLabel;      // "Side C", "Record 2, Side C"
    Unit window;
    cd::Disc disc;                      // CD units: the disc as written
    double lead = 0;                    // CD units: seconds of pregap added in front
    juce::StringArray notes;
    juce::String path;                  // relative: DDP folder or side file
    double rate = 44100;
    int bits = 16;
    double lufs = -INFINITY, truePeakDb = -INFINITY;
    bool verified = false;
    juce::String imageMd5;
};

// Everything the paperwork needs from a render.
struct DeliveryData {
    cd::Disc disc;                      // the full-programme disc
    juce::StringArray discNotes;
    bool fullDdp = false, ddpVerified = false;
    QcReport qc;
    std::vector<UnitRender> units;
    juce::StringArray files;            // every deliverable, relative to the output folder
    juce::String settingsText, dither;
    double hiRate = 0;
    int hiBits = 24;
};

struct BatchResult {
    bool ok = false;
    juce::String error;
    juce::StringArray files;     // relative to the output folder
    juce::String report;         // also written as Render Report.txt
    bool ddpVerified = false;
    DeliveryData delivery;
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
    // Renders all selected deliverables into `folder` (created), with a QC + checksum report.
    BatchResult batchRender(const Session& s, const BatchSettings& bs, const juce::File& folder, const ExportSettings& es,
                            const Keep& keepGoing);
    // Full programme analysis of the CD master (44.1 kHz, before dither): loudness map,
    // per-track stats and problems. With `raw`, also analyses the unprocessed mix (no plugins).
    juce::String analyseProgram(const Session& s, const ExportSettings& es, const Keep& keepGoing, dsp::AnalysisResult& processed,
                                dsp::AnalysisResult* raw);
    // Loudness / true peak / overs for the CD master, without writing anything.
    juce::String analyse(const Session& s, const ExportSettings& es, const Keep& keepGoing, QcReport& qc);

private:
    AudioEngine& engine_;
};

}  // namespace lathe
