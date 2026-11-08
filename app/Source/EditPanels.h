// Edit Fade: the editor for one edit (crossfade) between an outgoing and an incoming region.
// Shows both sides with their hidden handles and the summed result, and edits the crossfade
// length (with Lock Sound or ripple), each fade's shape, length, offset, dB down and alpha,
// each side's edit point and slip, with nudges that audition automatically. Fades can be saved
// to and recalled from a ten-slot library (slot 0 is the default crossfade for new edits).
#pragma once

#include "AudioEngine.h"
#include "Session.h"
#include "Timeline.h"

#include <array>

namespace lathe {

// One library fade: a crossfade length and both fades' settings.
struct FadePreset {
    juce::String name;
    double length = 0.005;
    FadeShape outShape = FadeShape::EqualPower, inShape = FadeShape::EqualPower;
    double outDb = 3.0, inDb = 3.0, outAlpha = 0, inAlpha = 0;
    bool used = false;   // slots 1-9 start empty
    juce::var toVar() const;
    static FadePreset fromVar(const juce::var& v);
};
using FadeLibrary = std::array<FadePreset, 10>;
FadeLibrary defaultFadeLibrary();
// Applies a preset to the edit between `outgoing` and `incoming`. With lockSound the edit is
// re-centred by trimming (nothing moves); otherwise the incoming side moves per `ripple`.
void applyFadePreset(Session& s, int outgoing, int incoming, const FadePreset& p, bool lockSound, Session::Ripple ripple);

// Editor options, kept between sessions by the main window.
struct FadeEditorPrefs {
    bool lockSound = true, powerLock = true, autoZoom = true, autoAudition = true;
    Session::Ripple ripple = Session::Ripple::UntilBlack;
    double nudge[3] = {1.0 / 75, 0.010, 0.001};
    int nudgeSize = 0;
    bool defaultOnEdits = true;   // put the default crossfade (slot 0) on new edits
    juce::var toVar() const;
    void fromVar(const juce::var& v);
};

class CrossfadeEditor : public juce::Component, public juce::ChangeListener, private juce::Timer {
public:
    CrossfadeEditor(Session& s, ThumbPool& pool, juce::AudioFormatManager& formats, FadeEditorPrefs& prefs, FadeLibrary& lib,
                    int outgoing, int incoming);
    std::function<void()> onChange;                      // commit (undo + reload)
    std::function<void(double from, double to)> onAudition;
    std::function<void()> onPrefsChanged, onLibraryChanged;
    void setPlayhead(double t) { playhead_ = t; repaint(); }
    int outgoing() const { return out_; }
    int incoming() const { return in_; }
    void recall(int slot);   // apply a library slot to this edit
    void refresh() { refreshControls(); resultDirty_ = true; startTimer(60); repaint(); }

    void paint(juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Side {
        juce::Label title, shapeLbl, lenLbl, offLbl, dbLbl, alphaLbl, editLbl, slipLbl;
        juce::ComboBox shape;
        juce::Slider len, off, db, alpha;
        juce::TextButton editL{"<"}, editR{">"}, slipL{"<"}, slipR{">"}, toEdit;
    };
    void timerCallback() override;
    void refreshControls();
    void changed(bool audition, double auditionAt = -1);
    void setupSide(Side& sd, bool outgoing);
    void layoutSide(Side& sd, juce::Rectangle<int> r);
    void sideParamChanged(bool outgoing);
    void nudgeEdit(bool outgoing, int dir);
    void nudgeSlip(bool outgoing, int dir);
    void renderResult();
    double overlap() const;
    juce::Range<double> viewRange() const;
    void paintSide(juce::Graphics&, juce::Rectangle<int> area, const Region& r, bool outgoing, double t0, double t1);
    void paintResult(juce::Graphics&, juce::Rectangle<int> area, double t0, double t1);
    juce::Rectangle<int> waveArea() const;

    Session& s_;
    ThumbPool& pool_;
    juce::AudioFormatManager& formats_;
    FadeEditorPrefs& prefs_;
    FadeLibrary& lib_;
    int out_, in_;
    double playhead_ = -1;
    double zoomSpan_ = 2.0;   // seconds shown when Auto Zoom is off
    Side o_, i_;
    juce::Slider length_;
    juce::Label lenLbl_, rippleLbl_, nudgeLbl_, libLbl_, info_;
    juce::ComboBox ripple_, libSlot_;
    juce::TextButton lockSound_{"Lock Sound"}, powerLock_{"Power Lock"}, autoZoom_{"Auto Zoom"}, zoomIn_{"+"}, zoomOut_{"-"},
        align_{"Align Fades"}, nA_{"A"}, nB_{"B"}, nC_{"C"}, autoAud_{"Auto-Audition"}, audition_{"Audition"}, pre_{"Pre-roll"},
        post_{"Post-roll"}, libApply_{"Apply"}, libSave_{"Save to Slot..."};
    bool updating_ = false;
    // Result: min / max of the summed audio per display column.
    std::vector<float> resMin_, resMax_;
    juce::Range<double> resRange_;
    bool resultDirty_ = true;
    double dipDb_ = 0;
};

}  // namespace lathe
