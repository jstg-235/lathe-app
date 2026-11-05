// DDP Player: checks and plays a finished DDP exactly as a plant will read it.
#pragma once

#include "AudioEngine.h"
#include "../../core/ddp/Ddp.h"
#include "Theme.h"

namespace lathe {

class DdpPlayer : public juce::Component, private juce::TableListBoxModel, private juce::Timer {
public:
    DdpPlayer(AudioEngine& e, const juce::File& folder, const ddp::ReadResult& r);
    ~DdpPlayer() override;
    std::function<void(const juce::File&)> onOpenAsSession;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    int getNumRows() override { return int(r_.disc.tracks.size()); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override { playTrack(row); }
    void timerCallback() override;
    void playTrack(int t);
    int currentTrack() const;
    juce::Rectangle<int> barArea() const;

    AudioEngine& e_;
    juce::File folder_;
    ddp::ReadResult r_;
    juce::TableListBox table_{"Tracks", this};
    juce::TextButton play_{"Play"}, prev_{"|<"}, next_{">|"}, openSession_{"Open as Session (for revisions)"};
    juce::Label time_, status_;
    bool loaded_ = false;
};

}  // namespace lathe
