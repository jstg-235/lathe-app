// The counters across the top of the main window: LEFT / RIGHT / DUR of what is selected, the main
// counter (playhead, or the Edit Point when stopped), and IN / OUT / DUR of the In-Out. One units
// setting for all of them. Click IN, OUT, DUR or the main counter to type a time.
#pragma once

#include "Theme.h"

#include <functional>

namespace lathe {

class TimecodeStrip : public juce::Component {
public:
    enum class Units { CdFrames = 0, Hms, Samples, Seconds, Smpte24, Smpte25, Smpte30 };
    static constexpr int kNumUnits = 7;
    static juce::String unitsName(Units u);
    // A time (seconds) as text in those units; rate is used for samples.
    static juce::String format(double sec, Units u, double rate);
    // Text typed in those units (or any CD-time / seconds form) back to seconds.
    static bool parse(const juce::String& text, Units u, double rate, double& sec);

    struct Values {
        double main = 0;
        bool playing = false, recording = false, source = false;
        double in = -1, out = -1;       // -1 = not set
        double left = -1, right = -1;   // -1 = nothing selected
        juce::String leftWhat;          // what LEFT / RIGHT describe ("region", "3 segments")
        double rate = 44100;
    };
    void setValues(const Values& v);
    const Values& values() const { return v_; }
    Units units() const { return units_; }
    void setUnits(Units u);

    std::function<void(double)> onSetMain, onSetIn, onSetOut, onSetDuration;
    std::function<void(Units)> onUnitsChanged;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;

    // Field areas (for clicking): 0 main, 1 in, 2 out, 3 in-out duration, 4 units.
    juce::Rectangle<int> fieldArea(int field) const { return fields_[field]; }
    void startEditing(int field);   // opens the typing box on a field

private:
    void commitEdit();
    Values v_;
    Units units_ = Units::CdFrames;
    juce::Rectangle<int> fields_[5], leftGroup_, rightGroup_, mainArea_;
    std::unique_ptr<juce::TextEditor> editor_;
    int editing_ = -1;
};

}  // namespace lathe
