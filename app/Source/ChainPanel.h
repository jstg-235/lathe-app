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

}  // namespace lathe
