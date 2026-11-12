#include "PluginManager.h"

namespace lathe {

namespace {
enum Col { colOn = 1, colName, colMaker, colFormat, colType, colVersion };
}

PluginManagerPanel::PluginManagerPanel(PluginHost& host) : host_(host) {
    search_.setTextToShowWhenEmpty("Search plugins", colours::dim);
    search_.setColour(juce::TextEditor::backgroundColourId, colours::bg);
    search_.setColour(juce::TextEditor::textColourId, colours::text);
    search_.setColour(juce::TextEditor::outlineColourId, colours::line);
    search_.setJustification(juce::Justification::centredLeft);
    search_.onTextChange = [this] { refresh(); };
    addAndMakeVisible(search_);
    for (auto* l : {&summary_, &skippedTitle_, &hint_}) {
        l->setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(*l);
    }
    skippedTitle_.setText("Skipped by the scan (crashed or stopped responding while being examined)", juce::dontSendNotification);
    hint_.setText("Deactivated plugins are hidden from every plugin menu and are never loaded. A session that uses one opens "
                  "without it but keeps its settings, so switching it back on brings it back. Plugins already open in a chain "
                  "stay until you remove them or reopen the session.",
                  juce::dontSendNotification);
    hint_.setFont(juce::FontOptions(12.0f));
    hint_.setJustificationType(juce::Justification::topLeft);
    for (auto* b : {&scan_, &deactivate_, &activate_, &retry_}) {
        styleButton(*b, b == &scan_);
        addAndMakeVisible(*b);
    }
    scan_.onClick = [this] { if (onScan) onScan(); };
    deactivate_.onClick = [this] { setSelectedActive(false); };
    activate_.onClick = [this] { setSelectedActive(true); };
    retry_.onClick = [this] {
        auto sel = skippedList_.getSelectedRows();
        if (sel.isEmpty()) return;
        for (int i = sel.size(); --i >= 0;) host_.known().removeFromBlacklist(skipped_[sel[i]]);
        refresh();
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Try Again",
                                               "Removed from the skipped list. Run Scan for New Plugins to examine "
                                               "it again; if it crashes the scan again, Lathe skips it again.");
    };

    table_.setColour(juce::ListBox::backgroundColourId, colours::bg);
    table_.setMultipleSelectionEnabled(true);
    table_.setRowHeight(24);
    auto& h = table_.getHeader();
    h.setColour(juce::TableHeaderComponent::backgroundColourId, colours::panel2);
    h.setColour(juce::TableHeaderComponent::textColourId, colours::text);
    h.addColumn("On", colOn, 44, 44, 44, juce::TableHeaderComponent::notSortable | juce::TableHeaderComponent::visible);
    h.addColumn("Name", colName, 240);
    h.addColumn("Manufacturer", colMaker, 170);
    h.addColumn("Format", colFormat, 90);
    h.addColumn("Type", colType, 120);
    h.addColumn("Version", colVersion, 80);
    h.setSortColumnId(colMaker, true);
    addAndMakeVisible(table_);
    skippedList_.setColour(juce::ListBox::backgroundColourId, colours::bg);
    skippedList_.setMultipleSelectionEnabled(true);
    skippedList_.setRowHeight(22);
    addAndMakeVisible(skippedList_);

    host_.known().addChangeListener(this);
    refresh();
    setSize(820, 600);
}

PluginManagerPanel::~PluginManagerPanel() {
    host_.known().removeChangeListener(this);
    closeList(table_);
}

void PluginManagerPanel::refresh() {
    const auto q = search_.getText().trim();
    rows_.clearQuick();
    for (auto& d : host_.known().getTypes())
        if (q.isEmpty() || d.name.containsIgnoreCase(q) || d.manufacturerName.containsIgnoreCase(q) || d.pluginFormatName.containsIgnoreCase(q) ||
            d.category.containsIgnoreCase(q))
            rows_.add(d);
    const int col = sortColumn_;
    const bool fwd = sortForwards_;
    auto key = [col](const juce::PluginDescription& d) -> juce::String {
        switch (col) {
            case colName: return d.name + d.manufacturerName;
            case colFormat: return d.pluginFormatName + d.manufacturerName + d.name;
            case colType: return d.category + d.manufacturerName + d.name;
            case colVersion: return d.version + d.name;
            default: return d.manufacturerName + d.name;
        }
    };
    std::stable_sort(rows_.begin(), rows_.end(), [&](const juce::PluginDescription& a, const juce::PluginDescription& b) {
        int c = key(a).compareNatural(key(b));
        return fwd ? c < 0 : c > 0;
    });
    skipped_ = host_.known().getBlacklistedFiles();
    table_.updateContent();
    table_.repaint();
    skippedList_.updateContent();
    skippedList_.repaint();
    retry_.setEnabled(!skipped_.isEmpty());
    updateSummary();
}

void PluginManagerPanel::updateSummary() {
    const int total = host_.known().getNumTypes();
    int off = 0;
    for (auto& d : host_.known().getTypes()) off += host_.isDeactivated(d) ? 1 : 0;
    juce::String s;
    s << total << " plugin" << (total == 1 ? "" : "s") << ", " << (total - off) << " active, " << off << " deactivated";
    if (!skipped_.isEmpty()) s << ", " << skipped_.size() << " skipped by the scan";
    summary_.setText(s, juce::dontSendNotification);
}

void PluginManagerPanel::setSelectedActive(bool active) {
    auto sel = table_.getSelectedRows();
    if (sel.isEmpty()) return;
    for (int i = 0; i < sel.size(); ++i)
        if (juce::isPositiveAndBelow(sel[i], rows_.size())) host_.setDeactivated(rows_.getReference(sel[i]), !active);
    table_.repaint();
    updateSummary();
    if (onActiveChanged) onActiveChanged();
}

void PluginManagerPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    g.fillAll(selected ? colours::selection : (row % 2 ? colours::panel : colours::bg));
}

void PluginManagerPanel::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    if (!juce::isPositiveAndBelow(row, rows_.size())) return;
    const auto& d = rows_.getReference(row);
    const bool off = host_.isDeactivated(d);
    if (col == colOn) {
        // A switch: filled when active, outlined when deactivated.
        auto r = juce::Rectangle<float>(float(w) / 2 - 13, float(h) / 2 - 7, 26, 14);
        g.setColour(off ? colours::panel2 : colours::accent);
        g.fillRoundedRectangle(r, 7);
        g.setColour(off ? colours::dim : colours::bg);
        g.fillEllipse(off ? r.getX() + 2 : r.getRight() - 12, r.getY() + 2, 10, 10);
        return;
    }
    juce::String t;
    switch (col) {
        case colName: t = d.name; break;
        case colMaker: t = d.manufacturerName; break;
        case colFormat: t = d.pluginFormatName; break;
        case colType: t = d.category.isNotEmpty() ? d.category : (d.isInstrument ? "Instrument" : "Effect"); break;
        case colVersion: t = d.version; break;
        default: break;
    }
    g.setColour(off ? colours::dim.withAlpha(0.7f) : colours::text);
    g.setFont(13.0f);
    g.drawText(t, 6, 0, w - 10, h, juce::Justification::centredLeft, true);
}

void PluginManagerPanel::cellClicked(int row, int col, const juce::MouseEvent&) {
    if (col != colOn || !juce::isPositiveAndBelow(row, rows_.size())) return;
    const auto& d = rows_.getReference(row);
    host_.setDeactivated(d, !host_.isDeactivated(d));
    table_.repaintRow(row);
    updateSummary();
    if (onActiveChanged) onActiveChanged();
}

void PluginManagerPanel::returnKeyPressed(int) {
    // Return toggles the selection: deactivate if any selected plugin is active.
    bool anyActive = false;
    const auto sel = table_.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i)
        if (juce::isPositiveAndBelow(sel[i], rows_.size())) anyActive = anyActive || !host_.isDeactivated(rows_.getReference(sel[i]));
    setSelectedActive(!anyActive);
}

void PluginManagerPanel::sortOrderChanged(int columnId, bool forwards) {
    if (columnId == colOn) return;
    sortColumn_ = columnId;
    sortForwards_ = forwards;
    refresh();
}

void PluginManagerPanel::SkippedModel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    g.fillAll(selected ? colours::selection : colours::bg);
    if (!juce::isPositiveAndBelow(row, o.skipped_.size())) return;
    g.setColour(colours::text);
    g.setFont(13.0f);
    g.drawText(o.skipped_[row], 8, 0, w - 16, h, juce::Justification::centredLeft, true);
}

void PluginManagerPanel::resized() {
    auto r = getLocalBounds().reduced(10);
    auto top = r.removeFromTop(28);
    scan_.setBounds(top.removeFromRight(180));
    top.removeFromRight(8);
    search_.setBounds(top.removeFromLeft(240));
    top.removeFromLeft(12);
    summary_.setBounds(top);
    r.removeFromTop(8);
    hint_.setBounds(r.removeFromBottom(48));
    r.removeFromBottom(6);
    auto skippedArea = r.removeFromBottom(130);
    r.removeFromBottom(8);
    auto buttons = r.removeFromBottom(28);
    deactivate_.setBounds(buttons.removeFromLeft(110));
    buttons.removeFromLeft(6);
    activate_.setBounds(buttons.removeFromLeft(110));
    r.removeFromBottom(6);
    table_.setBounds(r);
    auto st = skippedArea.removeFromTop(24);
    retry_.setBounds(st.removeFromRight(110));
    skippedTitle_.setBounds(st);
    skippedArea.removeFromTop(4);
    skippedList_.setBounds(skippedArea);
}

}  // namespace lathe
