#include "MarkInfo.h"

namespace lathe {

namespace {
enum Col { cNum = 1, cStart, cLength, cTitle, cPerformer, cIsrc, cOffset, cLock };

class EditCell : public juce::Label {
public:
    explicit EditCell(MarkInfoPanel& o) : owner_(o) {
        setEditable(false, true, false);
        setColour(juce::Label::textColourId, colours::text);
        setColour(juce::Label::textWhenEditingColourId, colours::text);
        setColour(juce::Label::backgroundWhenEditingColourId, colours::bg);
        setFont(juce::FontOptions(13.0f));
    }
    void set(int row, int col) {
        row_ = row;
        col_ = col;
        setText(owner_.text(row, col), juce::dontSendNotification);
    }
    void textWasEdited() override { owner_.setText(row_, col_, getText()); }
    void mouseDown(const juce::MouseEvent& e) override {
        if (auto* t = findParentComponentOfClass<juce::TableListBox>()) t->selectRowsBasedOnModifierKeys(row_, e.mods, false);
        Label::mouseDown(e);
    }
private:
    MarkInfoPanel& owner_;
    int row_ = 0, col_ = 0;
};

void frameSlider(juce::Slider& s, int lo, int hi, const juce::String& lowText = {}) {
    s.setSliderStyle(juce::Slider::IncDecButtons);
    s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 56, 22);
    s.setRange(lo, hi, 1);
    s.setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
    if (lowText.isNotEmpty()) s.textFromValueFunction = [lo, lowText](double v) { return int(v) == lo ? lowText : juce::String(int(v)); };
}
}  // namespace

MarkInfoPanel::MarkInfoPanel(Session& s) : s_(s) {
    auto label = [this](juce::Label& l, const char* t) {
        l.setText(t, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(l);
    };
    label(startL_, "Start offset (fr)");
    label(track1L_, "Track 1 (fr)");
    label(spliceL_, "On an edit (fr)");
    label(endL_, "End offset (fr)");
    label(minIdxL_, "Min index width (fr)");
    label(isrcL_, "First ISRC");
    label(spaceL_, "Track gap (s)");
    label(hint_, "");
    hint_.setText("Offsets apply on delivery (DDP, PQ sheets): starts move earlier, ends later. 75 frames = 1 s. Drag a track by its number to "
                  "reorder it with its audio; double-click to play it.",
                  juce::dontSendNotification);
    hint_.setFont(juce::FontOptions(12.0f));
    frameSlider(start_, 0, 75);
    frameSlider(track1_, -1, 75, "same");
    frameSlider(splice_, -1, 75, "same");
    frameSlider(end_, 0, 150);
    frameSlider(minIdx_, 0, 75 * 60, "off");
    space_.setSliderStyle(juce::Slider::IncDecButtons);
    space_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 56, 22);
    space_.setRange(0.0, 10.0, 0.5);
    for (auto* sl : {&start_, &track1_, &splice_, &end_, &minIdx_, &space_}) {
        addAndMakeVisible(*sl);
        sl->onValueChange = [this] {
            if (loading_) return;
            s_.pqOffsetFrames = int(start_.getValue());
            s_.pqTrack1Frames = int(track1_.getValue());
            s_.pqSpliceFrames = int(splice_.getValue());
            s_.pqEndFrames = int(end_.getValue());
            s_.pqMinIndexFrames = int(minIdx_.getValue());
            s_.autoSpaceGap = space_.getValue();
            commit();
        };
    }
    start_.setTooltip("Track starts move this many CD frames earlier on the disc, so players don't clip the first note");
    track1_.setTooltip("Track 1's offset (\"same\" = the start offset). Never under the 2-second pregap");
    splice_.setTooltip("For a track start that sits on an edit, where earlier audio is a different piece (\"same\" = the start offset)");
    end_.setTooltip("Track ends (pause marks) move this many frames later");
    minIdx_.setTooltip("Index marks closer than this to the previous index are left off the disc");
    for (auto* t : {&offsetsOn_, &lockAll_}) {
        t->setColour(juce::ToggleButton::textColourId, colours::text);
        addAndMakeVisible(*t);
    }
    offsetsOn_.onClick = [this] { if (!loading_) { s_.pqOffsetsOn = offsetsOn_.getToggleState(); commit(); } };
    lockAll_.onClick = [this] {
        if (loading_) return;
        if (onLockAll) { onLockAll(); refresh(); return; }
        s_.lockMarks = lockAll_.getToggleState();
        commit();
    };
    lockAll_.setTooltip("Every mark keeps its time when audio is moved, rippled or edited");
    isrc_.setColour(juce::TextEditor::backgroundColourId, colours::bg);
    isrc_.setColour(juce::TextEditor::textColourId, colours::text);
    isrc_.setColour(juce::TextEditor::outlineColourId, colours::line);
    isrc_.setTextToShowWhenEmpty("CC-XXX-YY-NNNNN", colours::dim);
    addAndMakeVisible(isrc_);
    for (auto* b : {&isrcGo_, &up_, &down_, &segs_, &black_, &analog_, &autoSpace_}) {
        styleButton(*b, false);
        addAndMakeVisible(*b);
    }
    isrcGo_.setTooltip("Give track 1 this ISRC and count up the last five digits for each track after it");
    isrcGo_.onClick = [this] {
        const int n = s_.autoIsrc(isrc_.getText());
        if (n == 0) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Number ISRCs", "Type the first ISRC as 12 characters, e.g. US-ABC-26-00001.");
            return;
        }
        commit();
    };
    auto move = [this](int dir) {
        const int row = table_.getSelectedRow();
        if (row < 0 || !s_.moveTrack(row, row + dir)) return;
        commit();
        table_.selectRow(row + dir);
    };
    up_.onClick = [move] { move(-1); };
    down_.onClick = [move] { move(1); };
    segs_.setTooltip("A track start at the beginning of every region on lane 1 that doesn't have one");
    black_.setTooltip("A track end and the next track's start around every gap between regions");
    analog_.setTooltip("Find silence inside the audio and mark tracks around it");
    autoSpace_.setTooltip("Make every gap between tracks the track gap above (marks move with their audio)");
    segs_.onClick = [this] { if (onSegmentsToMarks) onSegmentsToMarks(); };
    black_.onClick = [this] { if (onEditedBlack) onEditedBlack(); };
    analog_.onClick = [this] { if (onAnalogBlack) onAnalogBlack(); };
    autoSpace_.onClick = [this] { if (onAutoSpace) onAutoSpace(); };

    auto& h = table_.getHeader();
    h.addColumn("#", cNum, 34);
    h.addColumn("Start", cStart, 80);
    h.addColumn("Length", cLength, 74);
    h.addColumn("Title", cTitle, 200);
    h.addColumn("Performer", cPerformer, 150);
    h.addColumn("ISRC", cIsrc, 120);
    h.addColumn("Offset", cOffset, 70);
    h.addColumn("Lock", cLock, 44);
    h.setColour(juce::TableHeaderComponent::backgroundColourId, colours::bg);
    h.setColour(juce::TableHeaderComponent::textColourId, colours::dim);
    table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    table_.setRowHeight(24);
    addAndMakeVisible(table_);
    loadSettings();
    setSize(860, 560);
}

void MarkInfoPanel::loadSettings() {
    loading_ = true;
    start_.setValue(s_.pqOffsetFrames, juce::dontSendNotification);
    track1_.setValue(s_.pqTrack1Frames, juce::dontSendNotification);
    splice_.setValue(s_.pqSpliceFrames, juce::dontSendNotification);
    end_.setValue(s_.pqEndFrames, juce::dontSendNotification);
    minIdx_.setValue(s_.pqMinIndexFrames, juce::dontSendNotification);
    space_.setValue(s_.autoSpaceGap, juce::dontSendNotification);
    offsetsOn_.setToggleState(s_.pqOffsetsOn, juce::dontSendNotification);
    lockAll_.setToggleState(s_.lockMarks, juce::dontSendNotification);
    for (auto* sl : {&start_, &track1_, &splice_, &end_, &minIdx_}) sl->setEnabled(s_.pqOffsetsOn);
    loading_ = false;
}

void MarkInfoPanel::refresh() {
    loadSettings();
    table_.updateContent();
    table_.repaint();
}

void MarkInfoPanel::resized() {
    auto r = getLocalBounds().reduced(10);
    auto row1 = r.removeFromTop(26);
    offsetsOn_.setBounds(row1.removeFromLeft(130));
    auto pair = [&](juce::Rectangle<int>& row, juce::Label& l, juce::Component& c, int lw) {
        l.setBounds(row.removeFromLeft(lw));
        c.setBounds(row.removeFromLeft(96));
        row.removeFromLeft(10);
    };
    pair(row1, startL_, start_, 108);
    pair(row1, track1L_, track1_, 84);
    pair(row1, spliceL_, splice_, 100);
    r.removeFromTop(6);
    auto row2 = r.removeFromTop(26);
    lockAll_.setBounds(row2.removeFromLeft(130));
    pair(row2, endL_, end_, 108);
    pair(row2, minIdxL_, minIdx_, 130);
    r.removeFromTop(6);
    hint_.setBounds(r.removeFromTop(34));
    r.removeFromTop(4);
    auto bottom = r.removeFromBottom(26);
    up_.setBounds(bottom.removeFromLeft(80));
    bottom.removeFromLeft(4);
    down_.setBounds(bottom.removeFromLeft(90));
    bottom.removeFromLeft(16);
    isrcL_.setBounds(bottom.removeFromLeft(70));
    isrc_.setBounds(bottom.removeFromLeft(150));
    bottom.removeFromLeft(4);
    isrcGo_.setBounds(bottom.removeFromLeft(110));
    r.removeFromBottom(6);
    auto auto2 = r.removeFromBottom(26);
    segs_.setBounds(auto2.removeFromLeft(140));
    auto2.removeFromLeft(4);
    black_.setBounds(auto2.removeFromLeft(160));
    auto2.removeFromLeft(4);
    analog_.setBounds(auto2.removeFromLeft(170));
    auto2.removeFromLeft(16);
    spaceL_.setBounds(auto2.removeFromLeft(84));
    space_.setBounds(auto2.removeFromLeft(96));
    auto2.removeFromLeft(4);
    autoSpace_.setBounds(auto2.removeFromLeft(90));
    r.removeFromBottom(6);
    table_.setBounds(r);
}

const Marker* MarkInfoPanel::track(int row) const {
    int n = 0;
    for (auto& m : s_.markers)
        if (m.type == Marker::TrackStart && n++ == row) return &m;
    return nullptr;
}
Marker* MarkInfoPanel::track(int row) { return const_cast<Marker*>(static_cast<const MarkInfoPanel*>(this)->track(row)); }

int MarkInfoPanel::getNumRows() {
    int n = 0;
    for (auto& m : s_.markers) n += m.type == Marker::TrackStart ? 1 : 0;
    return n;
}

juce::String MarkInfoPanel::text(int row, int col) const {
    auto* m = track(row);
    if (!m) return {};
    switch (col) {
        case cTitle: return juce::String::fromUTF8(m->text.title.c_str());
        case cPerformer: return juce::String::fromUTF8(m->text.performer.c_str());
        case cIsrc: return juce::String(m->isrc);
        case cOffset: return m->offsetDefault ? juce::String() : juce::String(m->offsetFrames);
        default: return {};
    }
}

void MarkInfoPanel::setText(int row, int col, const juce::String& t) {
    auto* m = track(row);
    if (!m) return;
    switch (col) {
        case cTitle: m->text.title = t.toStdString(); break;
        case cPerformer: m->text.performer = t.toStdString(); break;
        case cIsrc: m->isrc = cd::normaliseIsrc(t.toStdString()); break;
        case cOffset:
            if (t.trim().isEmpty() || t.trim().equalsIgnoreCase("default")) m->offsetDefault = true;
            else { m->offsetDefault = false; m->offsetFrames = juce::jlimit(0, 150, t.getIntValue()); }
            break;
        default: return;
    }
    commit();
}

void MarkInfoPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    g.fillAll(selected ? colours::selection : (row % 2 ? colours::panel : colours::panel.brighter(0.03f)));
}

void MarkInfoPanel::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    auto* m = track(row);
    if (!m) return;
    g.setFont(13.0f);
    g.setColour(colours::text);
    juce::String t;
    switch (col) {
        case cNum: g.setColour(colours::accent); t = juce::String(row + 1); break;
        case cStart: t = msfString(m->t); break;
        case cLength: {
            auto ranges = s_.trackRanges();
            if (juce::isPositiveAndBelow(row, ranges.size())) t = msfString(ranges[row].getLength());
            break;
        }
        case cOffset:
            if (m->offsetDefault) {
                const int eff = s_.effectiveStartOffset(*m, row == 0);
                g.setColour(colours::dim);
                t = "default (" + juce::String(eff) + ")";
            }
            break;
        case cLock: {
            const bool locked = s_.markLocked(*m);
            auto box = juce::Rectangle<float>(float(w) / 2 - 7, float(h) / 2 - 7, 14, 14);
            g.setColour(locked ? colours::accent : colours::line);
            if (locked) g.fillRoundedRectangle(box, 3); else g.drawRoundedRectangle(box, 3, 1.2f);
            return;
        }
        default: return;
    }
    g.drawText(t, 6, 0, w - 10, h, juce::Justification::centredLeft, true);
}

juce::Component* MarkInfoPanel::refreshComponentForCell(int row, int col, bool, juce::Component* existing) {
    const bool editable = col == cTitle || col == cPerformer || col == cIsrc || (col == cOffset && track(row) && !track(row)->offsetDefault);
    if (!editable) { delete existing; return nullptr; }
    auto* c = dynamic_cast<EditCell*>(existing);
    if (!c) { delete existing; c = new EditCell(*this); }
    c->set(row, col);
    return c;
}

void MarkInfoPanel::cellClicked(int row, int col, const juce::MouseEvent&) {
    auto* m = track(row);
    if (!m) return;
    if (col == cLock) {
        if (s_.lockMarks) return;   // the global lock wins
        m->locked = !m->locked;
        commit();
    } else if (col == cOffset && m->offsetDefault) {
        // Clicking "default" gives the track its own offset, starting from what it gets now.
        m->offsetFrames = s_.effectiveStartOffset(*m, row == 0);
        m->offsetDefault = false;
        commit();
    }
}

void MarkInfoPanel::cellDoubleClicked(int row, int col, const juce::MouseEvent&) {
    if (col == cTitle || col == cPerformer || col == cIsrc || col == cOffset) return;
    if (auto* m = track(row); m && onPlay) onPlay(m->t);
}

juce::var MarkInfoPanel::getDragSourceDescription(const juce::SparseSet<int>& rows) {
    if (rows.size() != 1) return {};
    return "markinfo:" + juce::String(rows[0]);
}

void MarkInfoPanel::itemDragMove(const SourceDetails& d) {
    auto p = table_.getLocalPoint(this, d.localPosition);
    dropRow_ = table_.getInsertionIndexForPosition(p.x, p.y);
    repaint();
}

void MarkInfoPanel::itemDropped(const SourceDetails& d) {
    const int from = d.description.toString().fromFirstOccurrenceOf(":", false, false).getIntValue();
    auto p = table_.getLocalPoint(this, d.localPosition);
    int ins = table_.getInsertionIndexForPosition(p.x, p.y);
    dropRow_ = -1;
    if (ins < 0) ins = getNumRows();
    int to = ins > from ? ins - 1 : ins;   // insertion index -> final position
    to = juce::jlimit(0, getNumRows() - 1, to);
    if (to != from && s_.moveTrack(from, to)) {
        commit();
        table_.selectRow(to);
    }
    repaint();
}

void MarkInfoPanel::paintOverChildren(juce::Graphics& g) {
    if (dropRow_ < 0) return;
    auto rowArea = table_.getRowPosition(juce::jmin(dropRow_, juce::jmax(0, getNumRows() - 1)), true);
    int y = table_.getY() + (dropRow_ >= getNumRows() ? rowArea.getBottom() : rowArea.getY());
    g.setColour(colours::accent);
    g.fillRect(table_.getX(), y - 1, table_.getWidth(), 3);
}

}  // namespace lathe
