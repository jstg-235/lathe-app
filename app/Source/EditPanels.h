// Source window (audition a raw file, mark in/out, edit it into the destination) and the
// crossfade editor (both sides of an edit, shapes, length and slip).
#pragma once

#include "AudioEngine.h"
#include "Session.h"
#include "Timeline.h"

namespace lathe {

class SourcePanel : public juce::Component, public juce::ChangeListener {
public:
    SourcePanel(AudioEngine& e, ThumbPool& pool);

    bool open(const juce::File& f);
    bool hasSource() const { return file_.existsAsFile(); }
    juce::File file() const { return file_; }
    double rate() const { return rate_; }
    juce::int64 lengthSamples() const { return length_; }
    juce::Range<double> inOut() const;     // seconds; whole file if unmarked
    void markIn() { in_ = engine_.sourcePosition(); if (out_ >= 0 && out_ <= in_) out_ = -1; repaint(); }
    void markOut() { out_ = engine_.sourcePosition(); if (in_ >= out_) in_ = -1; repaint(); }
    void togglePlay();
    void playInOut();
    void showOpenDialog();
    void tick();   // called from the UI timer

    std::function<void(Session::EditMode)> onEdit;
    std::function<void()> onAppend;
    std::function<void(const juce::File&)> onOpened;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    bool isInterestedInDrag(const juce::StringArray&) { return true; }

private:
    juce::Rectangle<int> waveArea() const;
    double xToTime(int x) const;
    AudioEngine& engine_;
    ThumbPool& pool_;
    juce::File file_;
    double rate_ = 44100, lenSec_ = 0;
    juce::int64 length_ = 0;
    double in_ = -1, out_ = -1;
    double dragFrom_ = 0;
    juce::TextButton openBtn_{"Open..."}, playBtn_{"Play"}, inBtn_{"In"}, outBtn_{"Out"}, playRange_{"Play In-Out"},
        insertBtn_{"Insert"}, overwriteBtn_{"Overwrite"}, replaceBtn_{"Replace"}, appendBtn_{"Append"};
    juce::Label info_;
    bool stopAtOut_ = false;
    std::unique_ptr<juce::FileChooser> chooser_;
};

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
