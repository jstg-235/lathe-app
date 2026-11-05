#include "ChainPanel.h"

namespace lathe {

void showPluginPicker(PluginHost& host, juce::Component* target, std::function<void(const juce::PluginDescription&)> chosen,
                      std::function<void()> scan) {
    juce::PopupMenu m;
    auto types = host.known().getTypes();
    if (types.isEmpty()) m.addItem(1, "No plugins found yet; scan first", false);
    juce::KnownPluginList::addToMenu(m, types, juce::KnownPluginList::sortByManufacturer, {});
    m.addSeparator();
    m.addItem(-1, "Scan / manage plugins...");
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

}  // namespace lathe
