// The crossfade editor (both sides of an edit, shapes, length and slip).
#pragma once

#include "AudioEngine.h"
#include "Session.h"
#include "Timeline.h"

namespace lathe {

class CrossfadeEditor : public juce::Component, public juce::ChangeListener {
public:
    CrossfadeEditor(Session& s, ThumbPool& pool, int outgoing, int incoming);
    std::function<void()> onChange;                      // commit (undo + reload)
    std::function<void(double from, double to)> onAudition;
    void setPlayhead(double t) { playhead_ = t; repaint(); }

    void paint(juce::Graphics&) override;
    void resized() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

private:
    void refreshControls();
    double overlap() const;
    void paintSide(juce::Graphics&, juce::Rectangle<int> area, const Region& r, bool outgoing, double t0, double t1);

    Session& s_;
    ThumbPool& pool_;
    int out_, in_;
    double playhead_ = -1;
    juce::Slider length_;
    juce::ComboBox outShape_, inShape_;
    juce::TextButton slipOutL_{"<"}, slipOutR_{">"}, slipInL_{"<"}, slipInR_{">"}, slipStep_{"10 ms"}, audition_{"Audition"},
        pre_{"Pre-roll"}, post_{"Post-roll"};
    juce::Label lenLbl_, outLbl_, inLbl_, slipOutLbl_, slipInLbl_;
    double step_ = 0.010;
    bool updating_ = false;
};

}  // namespace lathe
