// Metering: one hub that measures what you hear (after the master chain), and the views that
// show it.
//
//  MeterHub      loudness (momentary, short-term, integrated, range, maxima), sample and true
//                peak per channel with holds, PLR / PSR, overs, short-term history for the
//                radar, spectrum and tonal balance against reference tracks, stereo
//                correlation / width / balance and the goniometer feed.
//  MeterStrip    the compact meter in the main window.
//  MeterBridge   the full meter in its own window (put it on a second screen): Radar,
//                Loudness (bars, history, statistics, compliance), Balance (spectrum against
//                the reference zone, ranges, notes, mono check) and Stereo (goniometer,
//                correlation, width, balance, correlation per band).
#pragma once

#include "AudioEngine.h"
#include "Theme.h"

#include "../../core/dsp/Meters.h"
#include "../../core/dsp/Scope.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe {

class MeterHub {
public:
    void prepare(double rate);
    void reset();
    // Pulls what the engine played since the last call and measures it. Returns frames read.
    int feed(MeterTap& tap);
    bool paused = false;   // freeze the readings (the audio still plays)

    // Loudness target: a standard, or the session's own target (index -1).
    int standard = -1;
    double sessionTarget = -14.0;
    double targetLufs() const;
    double tolerance() const;
    double maxTruePeak() const;
    juce::String standardName() const;

    const dsp::LoudnessMeter& loudness() const { return loud_; }
    const dsp::SpectrumAnalyzer& spectrum() const { return spec_; }
    const dsp::StereoMeter& stereo() const { return stereo_; }

    // Peak bars: decaying sample peak, held peak (3 s), max since reset; dBFS per channel.
    float peakDb(int ch) const;
    float holdDb(int ch) const;
    float maxSampleDb(int ch) const;
    double plr() const;   // max true peak minus integrated
    double psr() const;   // true peak over the last 3 s minus short-term
    // Short-term loudness every 100 ms since reset (LUFS; -inf in silence), and momentary.
    const std::vector<float>& shortTermHistory() const { return histS_; }
    const std::vector<float>& momentaryHistory() const { return histM_; }
    const std::vector<float>& correlationHistory() const { return histC_; }

    // Tonal balance target from reference tracks.
    void setReferenceProfiles(std::vector<dsp::BalanceProfile> p, juce::StringArray names);
    const std::vector<dsp::BalanceProfile>& referenceProfiles() const { return refs_; }
    const juce::StringArray& referenceNames() const { return refNames_; }
    const dsp::BalanceTarget& target() const { return target_; }
    dsp::BalanceProfile programmeProfile() const;
    std::vector<dsp::BalanceRange> ranges() const;

    // Long-term profile of an audio file (for references). Blocking; run it off the message thread.
    static dsp::BalanceProfile profileOfFile(juce::AudioFormatManager& fm, const juce::File& f, std::function<bool(double)> keepGoing = {});

private:
    double rate_ = 0;
    dsp::LoudnessMeter loud_;
    dsp::SpectrumAnalyzer spec_;
    dsp::StereoMeter stereo_;
    dsp::TruePeak blockTp_[2];
    juce::AudioBuffer<float> scratch_{2, 1 << 17};
    float peak_[2]{}, hold_[2]{}, max_[2]{};
    double holdAge_[2]{};
    std::vector<std::pair<double, float>> recentTp_;   // (time, linear) for PSR
    double clock_ = 0;
    int64_t lastHop_ = 0;
    std::vector<float> histS_, histM_, histC_;
    std::vector<dsp::BalanceProfile> refs_;
    juce::StringArray refNames_;
    dsp::BalanceTarget target_;
};

class MeterStrip : public juce::Component {
public:
    explicit MeterStrip(MeterHub& out, MeterHub* in = nullptr) : hub_(out), in_(in) {}
    std::function<void()> onOpenBridge, onReset, onResetInput;
    // While the inputs are armed the strip splits in two: OUT (what is played) and IN (what
    // comes back). The owner sets these each tick.
    bool showInput = false;
    int inputOvers = 0;          // samples at full scale on the armed inputs; stays until clicked
    float inputMaxDb = -200.0f;  // highest sample on any armed input (dBFS)
    juce::String inputName;      // e.g. "3-4"
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
private:
    void paintSplit(juce::Graphics&);
    MeterHub& hub_;
    MeterHub* in_;
    juce::Rectangle<int> bridgeBtn_, inHalf_;
};

class MeterBridge : public juce::Component, private juce::Timer {
public:
    explicit MeterBridge(MeterHub& out, MeterHub* in = nullptr);
    std::function<void()> onReset, onStandardChanged, onRefreshReferences;
    // What the views show: what is played, what comes back on the armed inputs, or both (the
    // Loudness view then shows the two side by side and the gain through the outboard).
    enum class Source { Output = 0, Input, Both };
    void setSource(Source s);
    Source source() const { return source_; }
    std::function<void()> onSourceChanged;
    bool inputsLive = false;    // set by the owner: the inputs are armed
    void refresh() { repaint(); }
    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

    enum class View { Radar = 0, Loudness, Balance, Stereo };
    void setView(View v);
    double radarMinutes = 4.0;
    bool tilt = false;          // display the balance with a 3 dB / octave tilt
    bool showFast = true;       // also draw the fast (live) spectrum

private:
    void timerCallback() override {}
    void paintRadar(juce::Graphics&, juce::Rectangle<int>);
    void paintLoudness(juce::Graphics&, juce::Rectangle<int>);
    void paintBalance(juce::Graphics&, juce::Rectangle<int>);
    void paintStereo(juce::Graphics&, juce::Rectangle<int>);
    void paintBigNumbers(juce::Graphics&, juce::Rectangle<int>);
    void paintLoudnessBoth(juce::Graphics&, juce::Rectangle<int>);
    MeterHub& hub() const { return source_ == Source::Input && in_ ? *in_ : out_; }

    MeterHub& out_;
    MeterHub* in_;
    Source source_ = Source::Output;
    juce::ComboBox sourceBox_;
    View view_ = View::Radar;
    juce::TextButton tabs_[4] = {juce::TextButton{"Radar"}, juce::TextButton{"Loudness"}, juce::TextButton{"Balance"}, juce::TextButton{"Stereo"}};
    juce::ComboBox standard_, speed_;
    juce::TextButton reset_{"Reset"}, pause_{"Pause"}, refs_{"References..."}, tiltBtn_{"Tilt"}, fastBtn_{"Live"};
    juce::Rectangle<int> content_;
};

}  // namespace lathe
