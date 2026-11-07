// Plugin Manager: every scanned plugin with an on/off switch, the plugins the scanner had to
// skip (crashed or hung while being examined), and the button that opens the scanner.
#pragma once

#include "PluginChain.h"
#include "Theme.h"

namespace lathe {

class PluginManagerPanel : public juce::Component, public juce::TableListBoxModel, private juce::ChangeListener {
public:
    explicit PluginManagerPanel(PluginHost& host);
    ~PluginManagerPanel() override;
    std::function<void()> onScan;            // open the scanner window
    std::function<void()> onActiveChanged;   // menus may need rebuilding
    void refresh();

    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

    int getNumRows() override { return rows_.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellClicked(int row, int col, const juce::MouseEvent&) override;
    void sortOrderChanged(int columnId, bool forwards) override;
    void returnKeyPressed(int lastRow) override;

private:
    struct SkippedModel : juce::ListBoxModel {
        PluginManagerPanel& o;
        explicit SkippedModel(PluginManagerPanel& p) : o(p) {}
        int getNumRows() override { return o.skipped_.size(); }
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    };
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void setSelectedActive(bool active);
    void updateSummary();

    PluginHost& host_;
    juce::Array<juce::PluginDescription> rows_;
    juce::StringArray skipped_;
    int sortColumn_ = 3;
    bool sortForwards_ = true;
    juce::TextEditor search_;
    juce::Label summary_, skippedTitle_, hint_;
    juce::TextButton scan_{"Scan for New Plugins..."}, deactivate_{"Deactivate"}, activate_{"Activate"}, retry_{"Try Again"};
    juce::TableListBox table_{"Plugins", this};
    SkippedModel skippedModel_{*this};
    juce::ListBox skippedList_{"Skipped", &skippedModel_};
};

}  // namespace lathe
