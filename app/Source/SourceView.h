// The source timeline: a second, full timeline holding the material edits are taken from
// (takes, alternate mixes, a client's revisions). It has its own zoom, In / Out and Edit Point,
// plays on its own or linked with the destination, and edits move audio from here into the
// destination with Insert, Replace, Replace Left, Replace Constrained, Fit or Overlay.
#pragma once

#include "Timeline.h"

namespace lathe {

class SourceView : public juce::Component {
public:
    SourceView(Session& source, ThumbPool& pool);

    Timeline& timeline() { return timeline_; }
    void setLinked(bool on) { link_.setToggleState(on, juce::dontSendNotification); }
    bool linked() const { return link_.getToggleState(); }
    void setPlaying(bool playing) { play_.setButtonText(playing ? "Stop" : "Play"); }

    // Toolbar actions, handled by the main window.
    std::function<void()> onOpen, onPlay, onMarkIn, onMarkOut, onInsert, onReplace, onReplaceLeft, onConstrained, onFit,
        onOverlay, onAppend, onLinkChanged, onClear;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    Session& s_;
    Timeline timeline_;
    juce::Label title_;
    juce::TextButton open_{"Open..."}, clear_{"Clear"}, play_{"Play"}, in_{"In"}, out_{"Out"}, insert_{"Insert"}, replace_{"Replace"},
        replaceLeft_{"Replace Left"}, constrained_{"Constrained"}, fit_{"Fit (4-pt)"}, overlay_{"Overlay"}, append_{"Append"},
        link_{"Link"}, zoomIn_{"+"}, zoomOut_{"-"}, fitView_{"Fit"};
};

}  // namespace lathe
