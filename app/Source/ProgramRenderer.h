// Streams the assembled programme (all clips, gaps, gains, fades and crossfades) as
// stereo float audio at any sample rate. Source files are resampled with r8brain
// (linear-phase, 24-bit-transparent) when their rate differs from the target.
//
// Not real-time safe (it reads from disk). The engine runs it on a background thread
// and hands audio to the device through a FIFO; exports run it directly.
#pragma once

#include "RegionFx.h"
#include "Session.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>

namespace lathe {

class ClipStream;

class ProgramRenderer {
public:
    ProgramRenderer(juce::AudioFormatManager& formats);
    ~ProgramRenderer();

    // Takes a snapshot of the session; safe to call while nothing else uses this object.
    void setSession(const Session& s, double targetRate, bool offline = false);
    void setRegionFx(RegionFxBank* fx) { fx_ = fx; }
    void seek(juce::int64 programSample);
    juce::int64 position() const { return pos_; }
    juce::int64 totalSamples() const { return total_; }
    double rate() const { return rate_; }
    int channels() const { return channels_; }   // 2, or 6 for a 5.1 programme

    // Renders n frames into out and advances. A 5.1 programme fills six channels when `out`
    // has them, otherwise it is folded down to stereo. Past the end it outputs silence. Returns false if a source file couldn't be read.
    // The double version is the master path: the mix, gains, fades and crossfades are all
    // computed in 64-bit float, and samples at unity gain pass through bit-for-bit.
    bool render(juce::AudioBuffer<float>& out, int startSample, int n);
    bool render(juce::AudioBuffer<double>& out, int startSample, int n);

    juce::String lastError() const { return error_; }

private:
    template <typename T> bool renderT(juce::AudioBuffer<T>& out, int startSample, int n);
    juce::AudioFormatManager& formats_;
    juce::Array<Region> regions_;
    juce::Array<EffectiveFades> fades_;
    juce::Array<juce::int64> starts_, ends_;
    juce::Array<GainNode> gain_;   // timeline gain (empty = none)
    juce::Array<int> laneFormats_;  // per region in regions_
    int channels_ = 2;
    juce::AudioBuffer<double> mix_;
    std::vector<std::unique_ptr<ClipStream>> streams_;
    double rate_ = 44100;
    juce::int64 pos_ = 0, total_ = 0;
    juce::String error_;
    RegionFxBank* fx_ = nullptr;
};

}  // namespace lathe
