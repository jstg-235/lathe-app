// Streams the assembled programme (all clips, gaps, gains, fades and crossfades) as
// stereo float audio at any sample rate. Source files are resampled with r8brain
// (linear-phase, 24-bit-transparent) when their rate differs from the target.
//
// Not real-time safe (it reads from disk). The engine runs it on a background thread
// and hands audio to the device through a FIFO; exports run it directly.
#pragma once

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
    void setSession(const Session& s, double targetRate);
    void seek(juce::int64 programSample);
    juce::int64 position() const { return pos_; }
    juce::int64 totalSamples() const { return total_; }
    double rate() const { return rate_; }

    // Renders n stereo frames into out (channels 0 and 1) and advances. Past the end
    // it outputs silence. Returns false if a source file couldn't be read.
    bool render(juce::AudioBuffer<float>& out, int startSample, int n);

    juce::String lastError() const { return error_; }

private:
    juce::AudioFormatManager& formats_;
    juce::Array<Region> regions_;
    juce::Array<EffectiveFades> fades_;
    juce::Array<juce::int64> starts_, ends_;
    std::vector<std::unique_ptr<ClipStream>> streams_;
    double rate_ = 44100;
    juce::int64 pos_ = 0, total_ = 0;
    juce::String error_;
};

}  // namespace lathe
