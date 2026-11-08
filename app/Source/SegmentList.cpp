#include "SegmentList.h"

namespace lathe {

namespace {
enum Col { cNum = 1, cLane, cName, cStart, cEnd, cLength, cGain, cFadeIn, cFadeOut, cPolarity, cMute };
}

SegmentListPanel::SegmentListPanel(Session& s) : s_(s) {
    auto& h = table_.getHeader();
    h.addColumn("#", cNum, 34);
    h.addColumn("Lane", cLane, 40);
    h.addColumn("Name", cName, 210);
    h.addColumn("Start", cStart, 80);
    h.addColumn("End", cEnd, 80);
    h.addColumn("Length", cLength, 76);
    h.addColumn("Gain", cGain, 70);
    h.addColumn("Fade in", cFadeIn, 64);
    h.addColumn("Fade out", cFadeOut, 64);
    h.addColumn("Polarity", cPolarity, 64);
    h.addColumn("Mute", cMute, 44);
    h.setColour(juce::TableHeaderComponent::backgroundColourId, colours::bg);
    h.setColour(juce::TableHeaderComponent::textColourId, colours::dim);
    table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    table_.setRowHeight(22);
    table_.setMultipleSelectionEnabled(true);
    addAndMakeVisible(table_);
    styleButton(gain_, true);
    gain_.onClick = [this] { if (onGain && !selectedIds().isEmpty()) onGain(selectedIds()); };
    addAndMakeVisible(gain_);
    hint_.setText("Command-click or Shift-click to select several. Double-click a row to go to it; double-click a gain to change it.",
                  juce::dontSendNotification);
    hint_.setColour(juce::Label::textColourId, colours::dim);
    hint_.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(hint_);
    refresh();
    setSize(900, 480);
}

void SegmentListPanel::refresh() {
    auto keep = selectedIds();
    order_.clearQuick();
    for (auto& r : s_.regions) order_.add(r.id);
    std::sort(order_.begin(), order_.end(), [this](int a, int b) {
        auto* x = s_.findRegion(a);
        auto* y = s_.findRegion(b);
        return x->start < y->start || (x->start == y->start && x->lane < y->lane);
    });
    table_.updateContent();
    juce::SparseSet<int> sel;
    for (int i = 0; i < order_.size(); ++i)
        if (keep.contains(order_[i])) sel.addRange({i, i + 1});
    table_.setSelectedRows(sel, juce::dontSendNotification);
    table_.repaint();
}

juce::Array<int> SegmentListPanel::selectedIds() const {
    juce::Array<int> ids;
    auto rows = table_.getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
        if (juce::isPositiveAndBelow(rows[i], order_.size())) ids.add(order_[rows[i]]);
    return ids;
}

const Region* SegmentListPanel::at(int row) const { return juce::isPositiveAndBelow(row, order_.size()) ? s_.findRegion(order_[row]) : nullptr; }

void SegmentListPanel::resized() {
    auto r = getLocalBounds().reduced(8);
    auto bottom = r.removeFromBottom(26);
    gain_.setBounds(bottom.removeFromRight(130));
    hint_.setBounds(bottom);
    r.removeFromBottom(6);
    table_.setBounds(r);
}

void SegmentListPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    g.fillAll(selected ? colours::selection : (row % 2 ? colours::panel : colours::panel.brighter(0.03f)));
}

void SegmentListPanel::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    auto* r = at(row);
    if (!r) return;
    const int idx = s_.indexOfRegion(r->id);
    auto f = s_.fadesFor(idx);
    juce::String t;
    g.setColour(r->mute ? colours::dim : colours::text);
    switch (col) {
        case cNum: t = juce::String(row + 1); break;
        case cLane: t = juce::String(r->lane + 1); break;
        case cName: t = r->name; break;
        case cStart: t = msfString(r->start); break;
        case cEnd: t = msfString(r->end()); break;
        case cLength: t = msfString(r->length()); break;
        case cGain: t = (r->gainDb > 0.005 ? "+" : "") + juce::String(r->gainDb, 2) + " dB"; break;
        case cFadeIn: t = juce::String(f.in * 1000.0, 0) + " ms"; break;
        case cFadeOut: t = juce::String(f.out * 1000.0, 0) + " ms"; break;
        case cPolarity: t = r->invert ? "inverted" : ""; if (r->invert) g.setColour(colours::accent); break;
        case cMute: t = r->mute ? "muted" : ""; break;
        default: break;
    }
    g.setFont(13.0f);
    g.drawText(t, 6, 0, w - 10, h, juce::Justification::centredLeft, true);
}

void SegmentListPanel::cellDoubleClicked(int row, int col, const juce::MouseEvent&) {
    auto* r = at(row);
    if (!r) return;
    if (col == cGain) {
        auto ids = selectedIds();
        if (!ids.contains(r->id)) ids = {r->id};
        if (onGain) onGain(ids);
        return;
    }
    if (onLocate) onLocate(r->start);
}

void SegmentListPanel::selectedRowsChanged(int lastRow) {
    if (auto* r = at(lastRow); r && onSelect) onSelect(r->id);
}

}  // namespace lathe
