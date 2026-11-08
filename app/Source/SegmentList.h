// Text View: every segment (region) as a list, in time order. Select one or many (Command-click,
// Shift-click) for Segment Gain; double-click a row to go to it, double-click its gain to edit.
#pragma once

#include "Session.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe {

class SegmentListPanel : public juce::Component, public juce::TableListBoxModel {
public:
    explicit SegmentListPanel(Session& s);
    std::function<void(juce::Array<int> ids)> onGain;   // open Segment Gain for these regions
    std::function<void(double)> onLocate;
    std::function<void(int id)> onSelect;
    void refresh();
    juce::Array<int> selectedIds() const;

    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }
    int getNumRows() override { return order_.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRow) override;

private:
    const Region* at(int row) const;
    Session& s_;
    juce::Array<int> order_;   // region ids in time order
    juce::TableListBox table_{"Segments", this};
    juce::TextButton gain_{"Segment Gain..."};
    juce::Label hint_;
};

}  // namespace lathe
