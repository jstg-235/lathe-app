// A plugin chain editor: list of inserts with Add / Remove / Bypass / Up / Down / Edit.
// Used for region effects windows (the master chain lives in the main window).
#pragma once

#include "PluginChain.h"
#include "Theme.h"

namespace lathe {

// Shows the scanned plugins as a menu; calls `chosen` with the selected description.
void showPluginPicker(PluginHost& host, juce::Component* target, std::function<void(const juce::PluginDescription&)> chosen,
                      std::function<void()> scan);

class ChainPanel : public juce::Component, private juce::ListBoxModel {
public:
    ChainPanel(PluginChain& chain, const juce::String& title);
    std::function<void()> onChange;   // chain edited: commit to the session
    std::function<void()> onScan;     // open the plugin manager

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return chain_.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { chain_.showEditor(row); }
    void changed();

    PluginChain& chain_;
    juce::String title_;
    juce::ListBox list_{"Inserts", this};
    juce::TextButton add_{"Add..."}, remove_{"Remove"}, bypass_{"Bypass"}, up_{"Up"}, down_{"Down"}, edit_{"Edit"};
    juce::Label hint_;
};

// The compare banner across the top of the master plugins while Compare Unprocessed is on.
//  A processed, B unprocessed, C processed turned down to the unprocessed loudness.
// Clicking the readout switches sides (as W does); Level Match turns C on and off; End
// returns to normal playback.
class CompareBar : public juce::Component, public juce::SettableTooltipClient {
public:
    CompareBar();
    struct State {
        bool hearingUnprocessed = false, levelMatch = false, measured = false;
        double cutDb = 0.0, differenceDb = 0.0;
    };
    void setState(const State& s);
    std::function<void()> onSwitch, onToggleMatch, onEnd;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    static juce::String describe(const State& s);   // one line, also for the transport readout
private:
    State st_;
    juce::TextButton match_{"Level Match"}, end_{"End"};
    juce::Rectangle<int> readout_;
};

}  // namespace lathe
