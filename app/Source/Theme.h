#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe::colours {
inline const juce::Colour bg{0xff16181c}, panel{0xff1f2228}, panel2{0xff262a31}, line{0xff2c3038}, text{0xffd8dbe0},
    dim{0xff8a909a}, accent{0xffe0a43a}, wave{0xff5d9cec}, waveSel{0xff8cc0ff}, marker{0xffe0a43a}, index{0xffc9b27a},
    pause{0xff7a5c2a}, end{0xffe0605a}, error{0xffe0605a}, ok{0xff6cc36a}, selection{0x335d9cec}, envelope{0xfff2d16b};
}

namespace lathe {
inline juce::String msfString(double sec) {
    if (sec < 0) return "-" + msfString(-sec);
    auto sectors = juce::int64(std::llround(sec * 75));
    int f = int(sectors % 75), s = int((sectors / 75) % 60), m = int(sectors / 4500);
    return juce::String(m).paddedLeft('0', 2) + ":" + juce::String(s).paddedLeft('0', 2) + ":" + juce::String(f).paddedLeft('0', 2);
}

// Accepts seconds ("2.5"), MM:SS ("1:02.5") or CD time MM:SS:FF ("00:02:00").
inline double parseTimeString(const juce::String& s, double fallback) {
    auto t = s.trim();
    if (t.isEmpty()) return fallback;
    if (t.containsChar(':')) {
        auto parts = juce::StringArray::fromTokens(t, ":", "");
        if (parts.size() == 3) return parts[0].getIntValue() * 60.0 + parts[1].getIntValue() + parts[2].getIntValue() / 75.0;
        if (parts.size() == 2) return parts[0].getIntValue() * 60.0 + parts[1].getDoubleValue();
        return fallback;
    }
    return t.getDoubleValue();
}

inline void styleButton(juce::Button& b, bool accent = false) {
    b.setColour(juce::TextButton::buttonColourId, accent ? colours::accent.darker(0.4f) : colours::panel2);
    b.setColour(juce::TextButton::buttonOnColourId, colours::accent.darker(0.2f));
    b.setColour(juce::TextButton::textColourOffId, colours::text);
    b.setColour(juce::TextButton::textColourOnId, colours::bg);
}
// Tears a list down safely before its owner (usually also its model) is destroyed: finishes any
// cell being edited, gives up keyboard focus, then removes the rows while everything is intact.
inline void giveUpListFocus(juce::ListBox& list) {
    if (auto* f = juce::Component::getCurrentlyFocusedComponent(); f && (f == &list || list.isParentOf(f))) f->giveAwayKeyboardFocus();
}
inline void closeList(juce::ListBox& list) {
    giveUpListFocus(list);
    list.setModel(nullptr);
}
inline void closeList(juce::TableListBox& table) {
    giveUpListFocus(table);
    table.setModel(nullptr);
}

}  // namespace lathe
