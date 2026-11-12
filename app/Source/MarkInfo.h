// Mark Info: every CD track in one window. PQ offsets for the disc (start, track 1, splice,
// end, minimum index width), and per track its start, length, title, performer, ISRC, its own
// offset (or the default) and a lock. Drag a track (by its number) to reorder it with its
// audio; double-click a track to play it. Auto marks and ISRC numbering live here too.
#pragma once

#include "Session.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe {

class MarkInfoPanel : public juce::Component,
                      public juce::TableListBoxModel,
                      public juce::DragAndDropContainer,
                      public juce::DragAndDropTarget {
public:
    explicit MarkInfoPanel(Session& s);
    // Close the list while this panel is still whole: a cell being edited at close otherwise
    // reaches back into a half-destroyed list (a crash on macOS).
    ~MarkInfoPanel() override { closeList(table_); }
    std::function<void()> onChange;                 // commit (undo + reload)
    std::function<void(double)> onPlay;             // play from a time
    std::function<void()> onSegmentsToMarks, onEditedBlack, onAnalogBlack, onAutoSpace, onLockAll;
    void refresh();

    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;
    void cellClicked(int row, int col, const juce::MouseEvent&) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;

    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("markinfo:"); }
    void itemDragMove(const SourceDetails& d) override;
    void itemDragExit(const SourceDetails&) override { dropRow_ = -1; repaint(); }
    void itemDropped(const SourceDetails& d) override;
    void paintOverChildren(juce::Graphics& g) override;

    juce::String text(int row, int col) const;
    void setText(int row, int col, const juce::String& t);

private:
    Marker* track(int row);
    const Marker* track(int row) const;
    void loadSettings();
    void commit() { if (onChange) onChange(); refresh(); }

    Session& s_;
    juce::TableListBox table_{"Marks", this};
    juce::ToggleButton offsetsOn_{"PQ offsets on"}, lockAll_{"Lock all marks"};
    juce::Label startL_, track1L_, spliceL_, endL_, minIdxL_, isrcL_, spaceL_, hint_;
    juce::Slider start_, track1_, splice_, end_, minIdx_, space_;
    juce::TextEditor isrc_;
    juce::TextButton isrcGo_{"Number ISRCs"}, up_{"Move Up"}, down_{"Move Down"}, segs_{"Segments to Marks"},
        black_{"Edited Black to Marks"}, analog_{"Analog Black to Marks..."}, autoSpace_{"AutoSpace"};
    int dropRow_ = -1;
    bool loading_ = false;
};

}  // namespace lathe
