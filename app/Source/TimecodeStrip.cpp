#include "TimecodeStrip.h"

namespace lathe {

namespace {
const juce::Colour kRec(0xffe0504a), kSrc(0xff39c2c9), kDim(0xff7d8794);

juce::String two(juce::int64 v) { return juce::String(v).paddedLeft('0', 2); }

int fpsOf(TimecodeStrip::Units u) { return u == TimecodeStrip::Units::Smpte24 ? 24 : u == TimecodeStrip::Units::Smpte25 ? 25 : 30; }
}  // namespace

juce::String TimecodeStrip::unitsName(Units u) {
    switch (u) {
        case Units::CdFrames: return "CD time (mm:ss:ff, 75 frames)";
        case Units::Hms: return "Hours, minutes, seconds (h:mm:ss.ms)";
        case Units::Samples: return "Samples";
        case Units::Seconds: return "Seconds";
        case Units::Smpte24: return "SMPTE 24 fps (hh:mm:ss:ff)";
        case Units::Smpte25: return "SMPTE 25 fps (hh:mm:ss:ff)";
        case Units::Smpte30: return "SMPTE 30 fps (hh:mm:ss:ff)";
    }
    return {};
}

juce::String TimecodeStrip::format(double sec, Units u, double rate) {
    if (sec < 0) return "-" + format(-sec, u, rate);
    switch (u) {
        case Units::CdFrames: return msfString(sec);
        case Units::Hms: {
            const auto ms = juce::int64(std::llround(sec * 1000.0));
            return juce::String(ms / 3600000) + ":" + two((ms / 60000) % 60) + ":" + two((ms / 1000) % 60) + "." + juce::String(ms % 1000).paddedLeft('0', 3);
        }
        case Units::Samples: return juce::String(juce::int64(std::llround(sec * rate)));
        case Units::Seconds: return juce::String(sec, 3);
        default: {
            const int fps = fpsOf(u);
            const auto fr = juce::int64(std::floor(sec * fps + 1e-6));
            return two(fr / (3600 * fps)) + ":" + two((fr / (60 * fps)) % 60) + ":" + two((fr / fps) % 60) + ":" + two(fr % fps);
        }
    }
}

bool TimecodeStrip::parse(const juce::String& text, Units u, double rate, double& sec) {
    auto t = text.trim();
    if (t.isEmpty()) return false;
    bool neg = t.startsWithChar('-');
    if (neg) t = t.substring(1).trim();
    auto parts = juce::StringArray::fromTokens(t, ":", "");
    for (auto& p : parts) if (!p.trim().containsOnly("0123456789.")) return false;
    double v = 0;
    switch (u) {
        case Units::Samples:
            if (parts.size() != 1 || t.containsChar('.')) return false;
            v = t.getDoubleValue() / rate;
            break;
        case Units::Seconds:
            if (parts.size() != 1) return false;
            v = t.getDoubleValue();
            break;
        case Units::Hms:
            if (parts.size() == 3) v = parts[0].getIntValue() * 3600.0 + parts[1].getIntValue() * 60.0 + parts[2].getDoubleValue();
            else if (parts.size() == 2) v = parts[0].getIntValue() * 60.0 + parts[1].getDoubleValue();
            else v = t.getDoubleValue();
            break;
        case Units::CdFrames:
            if (parts.size() == 3) v = parts[0].getIntValue() * 60.0 + parts[1].getIntValue() + parts[2].getIntValue() / 75.0;
            else if (parts.size() == 2) v = parts[0].getIntValue() * 60.0 + parts[1].getDoubleValue();
            else v = t.getDoubleValue();
            break;
        default: {
            const int fps = fpsOf(u);
            if (parts.size() == 4) v = parts[0].getIntValue() * 3600.0 + parts[1].getIntValue() * 60.0 + parts[2].getIntValue() + parts[3].getIntValue() / double(fps);
            else if (parts.size() == 3) v = parts[0].getIntValue() * 60.0 + parts[1].getIntValue() + parts[2].getIntValue() / double(fps);
            else if (parts.size() == 2) v = parts[0].getIntValue() * 60.0 + parts[1].getDoubleValue();
            else v = t.getDoubleValue();
        }
    }
    sec = neg ? -v : v;
    return true;
}

void TimecodeStrip::setValues(const Values& v) {
    v_ = v;
    repaint();
}

void TimecodeStrip::setUnits(Units u) {
    units_ = u;
    repaint();
}

void TimecodeStrip::resized() {
    auto r = getLocalBounds().reduced(8, 4);
    const int side = juce::jmin(280, (r.getWidth() - 260) / 2);
    leftGroup_ = r.removeFromLeft(side);
    rightGroup_ = r.removeFromRight(side);
    mainArea_ = r;
    fields_[0] = mainArea_.withTrimmedBottom(16).reduced(10, 0);
    fields_[4] = mainArea_.withTop(mainArea_.getBottom() - 16).withSizeKeepingCentre(240, 16);
    const int rowH = rightGroup_.getHeight() / 3;
    for (int k = 0; k < 3; ++k) fields_[1 + k] = rightGroup_.withHeight(rowH).translated(0, k * rowH).withTrimmedLeft(52);
}

void TimecodeStrip::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff0d1117));
    g.setColour(colours::line);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, float(getWidth()));
    const auto mono = juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold));
    auto group = [&](juce::Rectangle<int> area, const char* const names[3], const double vals[3], bool editable, juce::Colour c) {
        const int rowH = area.getHeight() / 3;
        for (int k = 0; k < 3; ++k) {
            auto row = area.withHeight(rowH).translated(0, k * rowH);
            g.setColour(kDim);
            g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
            g.drawText(names[k], row.removeFromLeft(48), juce::Justification::centredRight);
            row.removeFromLeft(4);
            if (editable) {
                g.setColour(juce::Colour(0xff161d26));
                g.fillRoundedRectangle(row.reduced(0, 1).toFloat(), 2.0f);
            }
            g.setColour(vals[k] >= 0 ? c : kDim);
            g.setFont(mono);
            g.drawText(vals[k] >= 0 ? format(vals[k], units_, v_.rate) : juce::String("--"), row.reduced(6, 0), juce::Justification::centredLeft);
        }
    };
    const double inOutDur = v_.in >= 0 && v_.out > v_.in ? v_.out - v_.in : -1;
    const double selDur = v_.left >= 0 && v_.right > v_.left ? v_.right - v_.left : -1;
    static const char* const leftNames[3] = {"LEFT", "RIGHT", "DUR"};
    static const char* const rightNames[3] = {"IN", "OUT", "DUR"};
    const double leftVals[3] = {v_.left, v_.right, selDur};
    const double rightVals[3] = {v_.in, v_.out, inOutDur};
    group(leftGroup_, leftNames, leftVals, false, colours::text);
    group(rightGroup_, rightNames, rightVals, true, juce::Colour(0xffe8c547));
    if (v_.leftWhat.isNotEmpty() && v_.left >= 0) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText(v_.leftWhat, leftGroup_.withTrimmedLeft(leftGroup_.getWidth() - 90).withHeight(leftGroup_.getHeight() / 3), juce::Justification::centredRight);
    }
    // Main counter.
    auto m = mainArea_;
    g.setColour(juce::Colour(0xff161d26));
    g.fillRoundedRectangle(fields_[0].toFloat(), 4.0f);
    const auto c = v_.recording ? kRec : v_.source ? kSrc : colours::text;
    g.setColour(c);
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 30.0f, juce::Font::bold));
    g.drawText(format(v_.main, units_, v_.rate), fields_[0], juce::Justification::centred);
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.setColour(v_.recording ? kRec : kDim);
    const juce::String state = v_.recording ? juce::String::charToString(0x25cf) + " REC" : v_.playing ? "PLAY" : "EDIT POINT";
    g.drawText((v_.source ? "SRC  " : "DEST  ") + state, fields_[0].reduced(8, 3), juce::Justification::topLeft);
    // Units (click to change).
    g.setColour(kDim);
    g.setFont(juce::FontOptions(10.5f));
    g.drawText(unitsName(units_) + "  " + juce::String::charToString(0x25be), fields_[4], juce::Justification::centred);
    juce::ignoreUnused(m);
}

void TimecodeStrip::mouseUp(const juce::MouseEvent& e) {
    const auto p = e.getPosition();
    if (fields_[4].contains(p)) {
        juce::PopupMenu menu;
        for (int k = 0; k < kNumUnits; ++k) menu.addItem(k + 1, unitsName(Units(k)), true, int(units_) == k);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(fields_[4])), [this](int r) {
            if (r <= 0) return;
            setUnits(Units(r - 1));
            if (onUnitsChanged) onUnitsChanged(units_);
        });
        return;
    }
    for (int f : {0, 1, 2, 3})
        if (fields_[f].contains(p)) { startEditing(f); return; }
}

void TimecodeStrip::startEditing(int field) {
    if (field == 3 && v_.in < 0) return;   // a duration needs an In to count from
    editing_ = field;
    editor_ = std::make_unique<juce::TextEditor>();
    auto& ed = *editor_;
    ed.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), field == 0 ? 26.0f : 15.0f, juce::Font::bold));
    ed.setJustification(field == 0 ? juce::Justification::centred : juce::Justification::centredLeft);
    ed.setColour(juce::TextEditor::backgroundColourId, colours::bg);
    ed.setColour(juce::TextEditor::textColourId, colours::text);
    ed.setColour(juce::TextEditor::outlineColourId, colours::accent);
    const double cur = field == 0 ? v_.main : field == 1 ? v_.in : field == 2 ? v_.out : (v_.out > v_.in ? v_.out - v_.in : 0.0);
    ed.setText(cur >= 0 ? format(cur, units_, v_.rate) : juce::String(), false);
    ed.selectAll();
    ed.onReturnKey = [this] { commitEdit(); };
    ed.onEscapeKey = [this] { juce::MessageManager::callAsync([sp = juce::Component::SafePointer<TimecodeStrip>(this)] { if (sp) { sp->editor_.reset(); sp->editing_ = -1; } }); };
    ed.onFocusLost = [this] { commitEdit(); };
    addAndMakeVisible(ed);
    ed.setBounds(fields_[field]);
    ed.grabKeyboardFocus();
}

void TimecodeStrip::commitEdit() {
    if (!editor_ || editing_ < 0) return;
    const int field = editing_;
    const auto text = editor_->getText();
    editing_ = -1;
    juce::MessageManager::callAsync([sp = juce::Component::SafePointer<TimecodeStrip>(this)] { if (sp) sp->editor_.reset(); });
    double t = 0;
    if (!parse(text, units_, v_.rate, t) || t < 0) return;
    if (field == 0 && onSetMain) onSetMain(t);
    if (field == 1 && onSetIn) onSetIn(t);
    if (field == 2 && onSetOut) onSetOut(t);
    if (field == 3 && onSetDuration) onSetDuration(t);
}

}  // namespace lathe
