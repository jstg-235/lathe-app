#include "ChainPanel.h"

namespace lathe {

void showPluginPicker(PluginHost& host, juce::Component* target, std::function<void(const juce::PluginDescription&)> chosen,
                      std::function<void()> scan) {
    juce::PopupMenu m;
    auto types = host.activeTypes();
    if (types.isEmpty()) m.addItem(1, host.known().getNumTypes() ? "Every plugin is deactivated" : "No plugins found yet; scan first", false);
    juce::KnownPluginList::addToMenu(m, types, juce::KnownPluginList::sortByManufacturer, {});
    m.addSeparator();
    m.addItem(-1, "Plugin Manager (scan, activate / deactivate)...");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [types, chosen, scan](int r) {
        if (r == -1) { if (scan) scan(); return; }
        int idx = juce::KnownPluginList::getIndexChosenByMenu(types, r);
        if (idx >= 0 && chosen) chosen(types.getReference(idx));
    });
}

ChainPanel::ChainPanel(PluginChain& chain, const juce::String& title) : chain_(chain), title_(title) {
    list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    list_.setRowHeight(26);
    addAndMakeVisible(list_);
    for (auto* b : {&add_, &remove_, &bypass_, &up_, &down_, &edit_}) {
        styleButton(*b, b == &add_);
        addAndMakeVisible(*b);
    }
    hint_.setText("Region effects run before the region's gain, envelope and fades. Double-click a plugin to open it.",
                  juce::dontSendNotification);
    hint_.setColour(juce::Label::textColourId, colours::dim);
    hint_.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(hint_);

    add_.onClick = [this] {
        showPluginPicker(chain_.host(), &add_, [this](const juce::PluginDescription& d) {
            juce::String err;
            if (!chain_.add(d, err)) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Couldn't load plugin", err);
                return;
            }
            list_.updateContent();
            list_.selectRow(chain_.size() - 1);
            chain_.showEditor(chain_.size() - 1);
            changed();
        }, onScan);
    };
    remove_.onClick = [this] {
        int i = list_.getSelectedRow();
        if (i >= 0) { chain_.remove(i); list_.updateContent(); changed(); }
    };
    bypass_.onClick = [this] {
        int i = list_.getSelectedRow();
        if (i >= 0) { chain_.setBypassed(i, !chain_.isBypassed(i)); list_.repaint(); changed(); }
    };
    up_.onClick = [this] {
        int i = list_.getSelectedRow();
        if (i > 0) { chain_.move(i, i - 1); list_.selectRow(i - 1); list_.updateContent(); changed(); }
    };
    down_.onClick = [this] {
        int i = list_.getSelectedRow();
        if (i >= 0 && i + 1 < chain_.size()) { chain_.move(i, i + 1); list_.selectRow(i + 1); list_.updateContent(); changed(); }
    };
    edit_.onClick = [this] {
        int i = list_.getSelectedRow();
        if (i >= 0) chain_.showEditor(i);
    };
    setSize(420, 320);
}

void ChainPanel::changed() {
    if (onChange) onChange();
}

void ChainPanel::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText(title_, getLocalBounds().reduced(10, 8).removeFromTop(20), juce::Justification::left);
}

void ChainPanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    g.fillAll(selected ? colours::accent.withAlpha(0.25f) : colours::panel);
    bool byp = chain_.isBypassed(row);
    g.setColour(byp ? colours::dim : colours::text);
    g.setFont(13.0f);
    g.drawText(juce::String(row + 1) + ".  " + chain_.nameAt(row) + (byp ? "   (bypassed)" : ""), 8, 0, w - 16, h,
               juce::Justification::centredLeft);
}

void ChainPanel::resized() {
    auto r = getLocalBounds().reduced(10);
    r.removeFromTop(26);
    hint_.setBounds(r.removeFromBottom(34));
    auto buttons = r.removeFromBottom(26);
    for (auto* b : {&add_, &remove_, &bypass_, &up_, &down_, &edit_}) {
        b->setBounds(buttons.removeFromLeft(b == &add_ ? 70 : 62));
        buttons.removeFromLeft(4);
    }
    r.removeFromBottom(6);
    list_.setBounds(r);
}

CompareBar::CompareBar() {
    for (auto* b : {&match_, &end_}) { styleButton(*b); addAndMakeVisible(*b); }
    match_.setClickingTogglesState(false);
    match_.setTooltip("Turn the processed side down to the loudness of the unprocessed side, so you compare sound, "
                      "not level (Shift-W). It never turns anything up, and never reaches exports.");
    end_.setTooltip("Back to normal playback (Option-W)");
    match_.onClick = [this] { if (onToggleMatch) onToggleMatch(); };
    end_.onClick = [this] { if (onEnd) onEnd(); };
    setTooltip("Click (or press W) to switch between processed and unprocessed");
}

void CompareBar::setState(const State& s) {
    const bool changed = s.hearingUnprocessed != st_.hearingUnprocessed || s.levelMatch != st_.levelMatch || s.measured != st_.measured
                         || std::abs(s.cutDb - st_.cutDb) >= 0.05 || std::abs(s.differenceDb - st_.differenceDb) >= 0.05;
    st_ = s;
    match_.setToggleState(s.levelMatch, juce::dontSendNotification);
    if (changed) repaint();
}

static juce::String dbText(double v) { return (v > 0.05 ? "+" : "") + juce::String(v, 1) + " dB"; }

juce::String CompareBar::describe(const State& s) {
    if (s.hearingUnprocessed) return "B  UNPROCESSED";
    if (!s.levelMatch) return "A  PROCESSED";
    if (!s.measured) return "C  MATCHING... (play)";
    if (s.cutDb > -0.05) return "C  MATCHED (no cut)";
    return "C  MATCHED " + dbText(s.cutDb);
}

void CompareBar::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    const bool lit = st_.hearingUnprocessed || st_.levelMatch;
    const juce::Colour fill = st_.hearingUnprocessed ? colours::accent : (st_.levelMatch ? colours::wave : colours::panel2);
    g.setColour(fill);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(colours::accent);
    g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
    const juce::Colour ink = lit ? colours::bg : colours::text;
    auto t = readout_;
    g.setColour(ink);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(describe(st_), t.removeFromTop(20), juce::Justification::centredLeft, true);
    g.setFont(juce::FontOptions(11.5f));
    g.setColour(ink.withAlpha(0.85f));
    g.drawText(juce::String(st_.levelMatch ? "W switches B / C" : "W switches A / B") + "   meters read A", t.removeFromTop(15),
               juce::Justification::centredLeft, true);
    juce::String level = "Processed level: measuring as it plays";
    if (st_.measured) {
        const double d = st_.differenceDb;
        level = std::abs(d) < 0.05 ? juce::String("Processed is at the same level")
                                   : "Processed is " + juce::String(std::abs(d), 1) + " dB " + (d > 0 ? "louder" : "quieter");
    }
    g.drawText(level, t.removeFromTop(15), juce::Justification::centredLeft, true);
}

void CompareBar::resized() {
    auto r = getLocalBounds().reduced(6, 5);
    auto buttons = r.removeFromRight(86);
    match_.setBounds(buttons.removeFromTop(buttons.getHeight() / 2 - 1));
    buttons.removeFromTop(2);
    end_.setBounds(buttons);
    r.removeFromRight(6);
    readout_ = r;
}

void CompareBar::mouseUp(const juce::MouseEvent& e) {
    if (readout_.contains(e.getPosition()) && onSwitch) onSwitch();
}

}  // namespace lathe
