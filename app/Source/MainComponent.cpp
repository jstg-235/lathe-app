#include "MainComponent.h"

#include "../../core/cd/CdTextEncoder.h"

namespace lathe {

// ---------------------------------------------------------------- look

namespace colours {
const juce::Colour bg{0xff16181c}, panel{0xff1f2228}, line{0xff2c3038}, text{0xffd8dbe0}, dim{0xff8a909a},
    accent{0xffe0a43a}, wave{0xff5d9cec}, marker{0xffe0a43a}, pause{0xff7a5c2a}, error{0xffe0605a}, ok{0xff6cc36a};
}

enum Col { cNum = 1, cTitle, cPerformer, cIsrc, cGap, cPause, cGain, cFadeIn, cFadeOut, cStart, cLength, cDcp, cPre };

// ---------------------------------------------------------------- overview

class Overview : public juce::Component, public juce::ChangeListener {
public:
    Overview(MainComponent& o, AudioEngine& e, juce::AudioThumbnailCache& cache) : owner_(o), engine_(e), cache_(cache) {}

    void rebuild(const Session& s) {
        thumbs_.clear();
        layout_ = s.layout();
        for (auto& c : s.clips) {
            auto* t = thumbs_.add(new juce::AudioThumbnail(512, engine_.formats(), cache_));
            t->addChangeListener(this);
            t->setSource(new juce::FileInputSource(c.file));
        }
        clips_ = s.clips;
        repaint();
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    void setPlayhead(double sec) {
        if (std::abs(sec - playhead_) * pxPerSec() >= 1.0) { playhead_ = sec; repaint(); }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(colours::panel);
        auto r = getLocalBounds().toFloat();
        const double pps = pxPerSec();
        g.setFont(11.0f);
        for (int i = 0; i < thumbs_.size() && i < layout_.clips.size(); ++i) {
            const auto& c = clips_.getReference(i);
            const auto& p = layout_.clips.getReference(i);
            float x0 = float(p.start * pps), x1 = float(p.end * pps);
            auto area = juce::Rectangle<float>(x0, 18.0f, x1 - x0, r.getHeight() - 22.0f);
            // pause region before index 01
            double pauseStart = i == 0 ? 0.0 : juce::jmax(layout_.clips[i - 1].end, p.start - c.pauseSec);
            if (pauseStart < p.start) {
                g.setColour(colours::pause.withAlpha(0.35f));
                g.fillRect(juce::Rectangle<float>(float(pauseStart * pps), 18.0f, float((p.start - pauseStart) * pps), area.getHeight()));
            }
            g.setColour(colours::wave.withAlpha(0.85f));
            double t0 = double(c.trimStart) / c.fileRate, t1 = double(c.fileLength - c.trimEnd) / c.fileRate;
            thumbs_[i]->drawChannels(g, area.toNearestInt(), t0, t1, float(juce::Decibels::decibelsToGain(c.gainDb)));
            g.setColour(colours::marker);
            g.drawLine(x0, 14.0f, x0, r.getHeight(), 1.5f);
            g.drawText(juce::String(i + 1), juce::Rectangle<float>(x0 + 3, 1, 40, 14), juce::Justification::left);
            for (double s : c.subIndexSec) {
                float xs = float((p.start + s) * pps);
                g.setColour(colours::marker.withAlpha(0.6f));
                g.drawLine(xs, 18.0f, xs, r.getHeight(), 1.0f);
            }
        }
        g.setColour(colours::text);
        float xp = float(playhead_ * pps);
        g.drawLine(xp, 0, xp, r.getHeight(), 1.5f);
        if (layout_.clips.isEmpty()) {
            g.setColour(colours::dim);
            g.drawText("Drag WAV, AIFF or FLAC files here, or press Add Audio", getLocalBounds(), juce::Justification::centred);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override { seek(e); }
    void mouseDrag(const juce::MouseEvent& e) override { seek(e); }

private:
    double pxPerSec() const { return layout_.total > 0 ? double(getWidth()) / (layout_.total * 1.02) : 1.0; }
    void seek(const juce::MouseEvent& e) {
        engine_.setPositionSeconds(juce::jmax(0.0, double(e.x) / pxPerSec()));
        setPlayhead(engine_.positionSeconds());
    }

    MainComponent& owner_;
    AudioEngine& engine_;
    juce::AudioThumbnailCache& cache_;
    juce::OwnedArray<juce::AudioThumbnail> thumbs_;
    juce::Array<Clip> clips_;
    Layout layout_;
    double playhead_ = 0;
};

// ---------------------------------------------------------------- meters

class LevelMeters : public juce::Component {
public:
    void prepare(double rate) {
        if (rate != rate_) { rate_ = rate; meter_.prepare(rate, 2); }
    }
    void reset() { meter_.reset(); peak_[0] = peak_[1] = 0; hold_[0] = hold_[1] = 0; repaint(); }
    void feed(MeterTap& tap) {
        if (rate_ <= 0) return;
        int n = tap.pull(scratch_);
        if (n > 0) {
            const float* ch[2] = {scratch_.getReadPointer(0), scratch_.getReadPointer(1)};
            meter_.process(ch, n);
            for (int c = 0; c < 2; ++c) {
                float m = scratch_.getMagnitude(c, 0, n);
                peak_[c] = juce::jmax(m, peak_[c] * 0.86f);
                hold_[c] = juce::jmax(hold_[c], m);
            }
        } else {
            for (auto& p : peak_) p *= 0.86f;
        }
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(colours::panel);
        auto r = getLocalBounds().reduced(6);
        auto bars = r.removeFromLeft(44);
        for (int c = 0; c < 2; ++c) {
            auto b = bars.removeFromLeft(18).toFloat();
            bars.removeFromLeft(4);
            g.setColour(colours::line);
            g.fillRect(b);
            auto norm = [](float lin) { float db = juce::Decibels::gainToDecibels(lin, -60.0f); return (db + 60.0f) / 60.0f; };
            float h = b.getHeight() * norm(peak_[c]);
            g.setColour(hold_[c] >= 0.99997f ? colours::error : colours::ok);
            g.fillRect(b.withTop(b.getBottom() - h));
            float hy = b.getBottom() - b.getHeight() * norm(hold_[c]);
            g.setColour(colours::text);
            g.drawHorizontalLine(int(hy), b.getX(), b.getRight());
        }
        r.removeFromLeft(8);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.0f));
        auto line = [&](const juce::String& k, double v, const char* unit) {
            auto row = r.removeFromTop(19);
            g.setColour(colours::dim);
            g.drawText(k, row.removeFromLeft(78), juce::Justification::left);
            g.setColour(colours::text);
            g.drawText(std::isfinite(v) ? juce::String(v, 1) + " " + unit : juce::String("--"), row, juce::Justification::right);
        };
        line("Momentary", meter_.momentaryLufs(), "LUFS");
        line("Short-term", meter_.shortTermLufs(), "LUFS");
        line("Integrated", meter_.integratedLufs(), "LUFS");
        line("Range", meter_.loudnessRange(), "LU");
        line("True peak", meter_.truePeakDb(), "dBTP");
        auto row = r.removeFromTop(19);
        g.setColour(colours::dim);
        g.drawText("Overs", row.removeFromLeft(78), juce::Justification::left);
        g.setColour(meter_.clippedSamples() ? colours::error : colours::text);
        g.drawText(juce::String(meter_.clippedSamples()), row, juce::Justification::right);
    }
    void mouseDown(const juce::MouseEvent&) override { reset(); }   // click to reset

private:
    dsp::LoudnessMeter meter_;
    double rate_ = 0;
    float peak_[2]{}, hold_[2]{};
    juce::AudioBuffer<float> scratch_{2, 1 << 17};
};

// ---------------------------------------------------------------- editable table cell

class Cell : public juce::Label {
public:
    explicit Cell(MainComponent& o) : owner_(o) {
        setEditable(false, true, false);
        setColour(juce::Label::textColourId, colours::text);
        setColour(juce::Label::textWhenEditingColourId, colours::text);
        setColour(juce::Label::backgroundWhenEditingColourId, colours::bg);
        setFont(juce::FontOptions(13.0f));
    }
    void set(int row, int col) {
        row_ = row;
        col_ = col;
        setText(owner_.cellText(row, col), juce::dontSendNotification);
        setJustificationType(col >= cGap ? juce::Justification::centredRight : juce::Justification::centredLeft);
    }
    void textWasEdited() override { owner_.setCellText(row_, col_, getText()); }
    void mouseDown(const juce::MouseEvent& e) override {
        // Single click selects the row; double click edits (handled by Label).
        if (auto* t = findParentComponentOfClass<juce::TableListBox>()) t->selectRowsBasedOnModifierKeys(row_, e.mods, false);
        Label::mouseDown(e);
    }
private:
    MainComponent& owner_;
    int row_ = 0, col_ = 0;
};

// ---------------------------------------------------------------- helpers

static juce::String msf(double sec) { return juce::String(cd::Msf::fromSectors(int64_t(std::llround(sec * 75))).str()); }

static double parseTime(const juce::String& s, double fallback) {
    // Accepts seconds ("2.5") or MM:SS:FF CD time ("00:02:00").
    auto t = s.trim();
    if (t.containsChar(':')) {
        auto parts = juce::StringArray::fromTokens(t, ":", "");
        if (parts.size() == 3) return parts[0].getIntValue() * 60.0 + parts[1].getIntValue() + parts[2].getIntValue() / 75.0;
        if (parts.size() == 2) return parts[0].getIntValue() * 60.0 + parts[1].getDoubleValue();
        return fallback;
    }
    return t.isEmpty() ? fallback : t.getDoubleValue();
}

static void styleButton(juce::TextButton& b) {
    b.setColour(juce::TextButton::buttonColourId, colours::panel);
    b.setColour(juce::TextButton::textColourOffId, colours::text);
}

// ---------------------------------------------------------------- MainComponent

MainComponent::MainComponent() {
    juce::PropertiesFile::Options o;
    o.applicationName = "Lathe";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    o.folderName = "Lathe";
    props_.setStorageParameters(o);

    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, colours::bg);
    overview_ = std::make_unique<Overview>(*this, engine_, thumbCache_);
    meters_ = std::make_unique<LevelMeters>();
    addAndMakeVisible(*overview_);
    addAndMakeVisible(*meters_);

    for (auto* b : {&newBtn_, &openBtn_, &saveBtn_, &addBtn_, &removeBtn_, &upBtn_, &downBtn_, &idxBtn_, &playBtn_, &homeBtn_,
                    &pluginsBtn_, &exportBtn_, &analyseBtn_, &verifyBtn_, &audioBtn_, &chainAdd_, &chainRemove_,
                    &chainBypass_, &chainUp_, &chainDown_}) {
        styleButton(*b);
        addAndMakeVisible(*b);
    }
    exportBtn_.setColour(juce::TextButton::buttonColourId, colours::accent.darker(0.4f));

    newBtn_.onClick = [this] { newSession(); };
    openBtn_.onClick = [this] { openSession(); };
    saveBtn_.onClick = [this] { saveSession(false); };
    addBtn_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Add audio", juce::File(), engine_.formats().getWildcardForAllFormats());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this](const juce::FileChooser& fc) { addFiles(fc.getResults()); });
    };
    removeBtn_.onClick = [this] { removeSelected(); };
    upBtn_.onClick = [this] { moveSelected(-1); };
    downBtn_.onClick = [this] { moveSelected(1); };
    idxBtn_.onClick = [this] { addSubIndexAtPlayhead(); };
    idxBtn_.setTooltip("Add a sub-index (index 02, 03...) at the playhead");
    playBtn_.onClick = [this] { engine_.isPlaying() ? engine_.stop() : engine_.play(); };
    homeBtn_.onClick = [this] { engine_.setPositionSeconds(0); meters_->reset(); };
    pluginsBtn_.onClick = [this] { showPluginMenu(); };
    exportBtn_.onClick = [this] { showExportMenu(); };
    analyseBtn_.onClick = [this] { analyse(); };
    verifyBtn_.onClick = [this] { verifyDdp(); };
    audioBtn_.onClick = [this] { showAudioSettings(); };

    chainAdd_.onClick = [this] { showPluginMenu(); };
    chainRemove_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i >= 0) { engine_.chain().remove(i); session_.pluginChainState = engine_.chain().getState(); sessionChanged(); }
    };
    chainBypass_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i >= 0) { engine_.chain().setBypassed(i, !engine_.chain().isBypassed(i)); chainList_.repaint(); }
    };
    chainUp_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i > 0) { engine_.chain().move(i, i - 1); chainList_.selectRow(i - 1); chainList_.updateContent(); sessionChanged(); }
    };
    chainDown_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i >= 0 && i + 1 < engine_.chain().size()) { engine_.chain().move(i, i + 1); chainList_.selectRow(i + 1); chainList_.updateContent(); sessionChanged(); }
    };

    auto& h = table_.getHeader();
    h.addColumn("#", cNum, 32, 32, 40);
    h.addColumn("Title", cTitle, 200, 80, 600);
    h.addColumn("Performer", cPerformer, 140, 60, 400);
    h.addColumn("ISRC", cIsrc, 120, 80, 160);
    h.addColumn("Gap", cGap, 64);
    h.addColumn("Pause", cPause, 64);
    h.addColumn("Gain dB", cGain, 64);
    h.addColumn("Fade in", cFadeIn, 64);
    h.addColumn("Fade out", cFadeOut, 64);
    h.addColumn("Start", cStart, 84);
    h.addColumn("Length", cLength, 84);
    h.addColumn("DCP", cDcp, 40);
    h.addColumn("Pre", cPre, 40);
    table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    table_.setRowHeight(24);
    table_.setMultipleSelectionEnabled(false);
    h.setColour(juce::TableHeaderComponent::backgroundColourId, colours::bg);
    h.setColour(juce::TableHeaderComponent::textColourId, colours::dim);
    addAndMakeVisible(table_);

    chainList_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    chainList_.setRowHeight(24);
    addAndMakeVisible(chainList_);

    issues_.setMultiLine(true);
    issues_.setReadOnly(true);
    issues_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
    issues_.setColour(juce::TextEditor::textColourId, colours::text);
    issues_.setColour(juce::TextEditor::outlineColourId, colours::line);
    addAndMakeVisible(issues_);

    for (auto* l : {&timeDisplay_, &trackDisplay_, &rateWarning_}) {
        l->setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(*l);
    }
    timeDisplay_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 26.0f, juce::Font::plain));
    rateWarning_.setColour(juce::Label::textColourId, colours::accent);

    auto field = [this](juce::Label& lbl, const char* name, juce::TextEditor& ed, std::function<void(const juce::String&)> set) {
        lbl.setText(name, juce::dontSendNotification);
        lbl.setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(lbl);
        ed.setColour(juce::TextEditor::backgroundColourId, colours::panel);
        ed.setColour(juce::TextEditor::textColourId, colours::text);
        ed.setColour(juce::TextEditor::outlineColourId, colours::line);
        ed.onTextChange = [this, &ed, set] { if (!updatingFields_) { set(ed.getText()); sessionChanged(); } };
        addAndMakeVisible(ed);
    };
    field(titleLbl_, "Album", title_, [this](auto& t) { session_.discTitle = t; });
    field(perfLbl_, "Artist", performer_, [this](auto& t) { session_.discPerformer = t; });
    field(upcLbl_, "UPC/EAN", upc_, [this](auto& t) { session_.upcEan = t; });
    field(clientLbl_, "Client", client_, [this](auto& t) { session_.client = t; });
    field(engLbl_, "Engineer", engineer_, [this](auto& t) { session_.engineer = t; });
    upc_.setInputRestrictions(13, "0123456789");

    ditherLbl_.setText("Dither", juce::dontSendNotification);
    ditherLbl_.setColour(juce::Label::textColourId, colours::dim);
    addAndMakeVisible(ditherLbl_);
    dither_.addItem("TPDF (flat)", 1);
    dither_.addItem("TPDF + light shaping", 2);
    dither_.addItem("TPDF + strong shaping", 3);
    dither_.setSelectedId(props_.getUserSettings()->getIntValue("dither", 1), juce::dontSendNotification);
    dither_.onChange = [this] { props_.getUserSettings()->setValue("dither", dither_.getSelectedId()); };
    addAndMakeVisible(dither_);

    engine_.devices().addChangeListener(this);
    lastDeviceRate_ = engine_.deviceRate();
    setWantsKeyboardFocus(true);
    setSize(1280, 800);
    newSession();
    startTimerHz(30);
}

MainComponent::~MainComponent() {
    engine_.devices().removeChangeListener(this);
    engine_.chain().closeAllEditors();
    table_.setModel(nullptr);
    chainList_.setModel(nullptr);
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(colours::bg); }

void MainComponent::resized() {
    auto r = getLocalBounds().reduced(10);
    auto top = r.removeFromTop(30);
    for (auto* b : {&newBtn_, &openBtn_, &saveBtn_}) { b->setBounds(top.removeFromLeft(64)); top.removeFromLeft(4); }
    top.removeFromLeft(12);
    for (auto* b : {&addBtn_, &removeBtn_, &upBtn_, &downBtn_, &idxBtn_}) { b->setBounds(top.removeFromLeft(b == &addBtn_ ? 90 : 64)); top.removeFromLeft(4); }
    audioBtn_.setBounds(top.removeFromRight(100));
    top.removeFromRight(4);
    exportBtn_.setBounds(top.removeFromRight(90));
    top.removeFromRight(4);
    verifyBtn_.setBounds(top.removeFromRight(90));
    top.removeFromRight(4);
    analyseBtn_.setBounds(top.removeFromRight(80));
    top.removeFromRight(4);
    pluginsBtn_.setBounds(top.removeFromRight(90));

    r.removeFromTop(8);
    auto fields = r.removeFromTop(26);
    auto place = [&](juce::Label& l, juce::Component& c, int lw, int w) {
        l.setBounds(fields.removeFromLeft(lw));
        c.setBounds(fields.removeFromLeft(w));
        fields.removeFromLeft(10);
    };
    place(titleLbl_, title_, 48, 220);
    place(perfLbl_, performer_, 46, 180);
    place(upcLbl_, upc_, 62, 120);
    place(clientLbl_, client_, 46, 130);
    place(engLbl_, engineer_, 60, 120);
    place(ditherLbl_, dither_, 46, juce::jmax(140, fields.getWidth()));

    r.removeFromTop(8);
    auto transport = r.removeFromTop(40);
    homeBtn_.setBounds(transport.removeFromLeft(44).reduced(0, 4));
    transport.removeFromLeft(4);
    playBtn_.setBounds(transport.removeFromLeft(80).reduced(0, 4));
    transport.removeFromLeft(12);
    timeDisplay_.setBounds(transport.removeFromLeft(170));
    trackDisplay_.setBounds(transport.removeFromLeft(220));
    rateWarning_.setBounds(transport);

    r.removeFromTop(6);
    auto right = r.removeFromRight(300);
    r.removeFromRight(10);
    meters_->setBounds(right.removeFromTop(150));
    right.removeFromTop(10);
    auto chainButtons = right.removeFromBottom(28);
    for (auto* b : {&chainAdd_, &chainRemove_, &chainBypass_, &chainUp_, &chainDown_}) {
        b->setBounds(chainButtons.removeFromLeft(56));
        chainButtons.removeFromLeft(4);
    }
    right.removeFromBottom(6);
    chainList_.setBounds(right);

    overview_->setBounds(r.removeFromTop(150));
    r.removeFromTop(8);
    issues_.setBounds(r.removeFromBottom(90));
    r.removeFromBottom(8);
    table_.setBounds(r);
}

bool MainComponent::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::spaceKey) { playBtn_.onClick(); return true; }
    if (k == juce::KeyPress::homeKey || k == juce::KeyPress::returnKey) { homeBtn_.onClick(); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) { undo(); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { redo(); return true; }
    if (k == juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0)) { saveSession(false); return true; }
    if (k == juce::KeyPress('i', 0, 0) || k == juce::KeyPress('m', 0, 0)) { addSubIndexAtPlayhead(); return true; }
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { removeSelected(); return true; }
    return false;
}

// ---------------------------------------------------------------- session lifecycle

void MainComponent::pushUndo() {
    undo_.add(lastState_);
    if (undo_.size() > 200) undo_.remove(0);
    redo_.clear();
}

void MainComponent::sessionChanged(bool undoable) {
    session_.pluginChainState = engine_.chain().getState();
    auto now = juce::JSON::toString(session_.toVar(), true);
    if (now == lastState_) return;
    if (undoable && lastState_.isNotEmpty()) pushUndo();
    lastState_ = now;
    dirty_ = true;
    engine_.loadSession(session_);
    overview_->rebuild(session_);
    table_.updateContent();
    table_.repaint();
    refreshIssues();
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName("Lathe - " + (session_.file.existsAsFile() ? session_.file.getFileNameWithoutExtension() : juce::String("Untitled")) + " *");
}

void MainComponent::undo() {
    if (undo_.isEmpty()) return;
    redo_.add(lastState_);
    auto prev = undo_[undo_.size() - 1];
    undo_.remove(undo_.size() - 1);
    juce::String err;
    auto f = session_.file;
    session_.fromVar(juce::JSON::parse(prev), err);
    session_.file = f;
    lastState_ = prev;
    refreshAll();
}

void MainComponent::redo() {
    if (redo_.isEmpty()) return;
    undo_.add(lastState_);
    auto next = redo_[redo_.size() - 1];
    redo_.remove(redo_.size() - 1);
    juce::String err;
    auto f = session_.file;
    session_.fromVar(juce::JSON::parse(next), err);
    session_.file = f;
    lastState_ = next;
    refreshAll();
}

void MainComponent::refreshAll() {
    updatingFields_ = true;
    title_.setText(session_.discTitle, false);
    performer_.setText(session_.discPerformer, false);
    upc_.setText(session_.upcEan, false);
    client_.setText(session_.client, false);
    engineer_.setText(session_.engineer, false);
    updatingFields_ = false;
    engine_.loadSession(session_);
    overview_->rebuild(session_);
    table_.updateContent();
    table_.repaint();
    chainList_.updateContent();
    chainList_.repaint();
    refreshIssues();
    dirty_ = true;
}

void MainComponent::newSession() {
    engine_.stop();
    session_ = Session();
    juce::StringArray problems;
    engine_.chain().setState({}, problems);
    undo_.clear();
    redo_.clear();
    lastState_ = juce::JSON::toString(session_.toVar(), true);
    refreshAll();
    dirty_ = false;
}

bool MainComponent::loadSessionFile(const juce::File& f) {
    Session s;
    juce::String err;
    if (!s.load(f, err)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Couldn't open session", err);
        return false;
    }
    engine_.stop();
    session_ = s;
    juce::StringArray problems;
    engine_.chain().setState(session_.pluginChainState, problems);
    juce::StringArray missing;
    for (auto& c : session_.clips)
        if (!c.file.existsAsFile()) missing.add(c.file.getFullPathName());
    undo_.clear();
    redo_.clear();
    lastState_ = juce::JSON::toString(session_.toVar(), true);
    refreshAll();
    dirty_ = false;
    if (!problems.isEmpty() || !missing.isEmpty()) {
        juce::String msg;
        if (!missing.isEmpty()) msg << "These audio files are missing:\n" << missing.joinIntoString("\n") << "\n\n";
        if (!problems.isEmpty()) msg << "Plugin problems:\n" << problems.joinIntoString("\n");
        showText("Session opened with problems", msg);
    }
    return true;
}

void MainComponent::openSession() {
    chooser_ = std::make_unique<juce::FileChooser>("Open session", juce::File(), "*.lathe");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              if (fc.getResult().existsAsFile()) loadSessionFile(fc.getResult());
                          });
}

bool MainComponent::saveSession(bool saveAs) {
    session_.pluginChainState = engine_.chain().getState();
    auto doSave = [this](const juce::File& f) {
        juce::String err;
        if (!session_.save(f, err)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Save failed", err);
            return false;
        }
        dirty_ = false;
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setName("Lathe - " + f.getFileNameWithoutExtension());
        return true;
    };
    if (!saveAs && session_.file != juce::File()) return doSave(session_.file);
    chooser_ = std::make_unique<juce::FileChooser>("Save session", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                       .getChildFile((session_.discTitle.isEmpty() ? juce::String("Untitled") : session_.discTitle) + ".lathe"),
                                                   "*.lathe");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [doSave](const juce::FileChooser& fc) {
                              if (fc.getResult() != juce::File()) doSave(fc.getResult().withFileExtension("lathe"));
                          });
    return true;
}

void MainComponent::writeAutosave(const juce::File& f) {
    session_.pluginChainState = engine_.chain().getState();
    juce::String err;
    auto keep = session_.file;
    session_.save(f, err);
    session_.file = keep;   // autosave never becomes "the" file
}

// ---------------------------------------------------------------- editing

void MainComponent::addFiles(const juce::Array<juce::File>& files) {
    juce::StringArray bad;
    for (auto& f : files) {
        std::unique_ptr<juce::AudioFormatReader> r(engine_.formats().createReaderFor(f));
        if (!r) { bad.add(f.getFileName()); continue; }
        Clip c;
        c.file = f;
        c.name = f.getFileNameWithoutExtension();
        c.fileRate = r->sampleRate;
        c.fileLength = r->lengthInSamples;
        c.text.title = c.name.toStdString();
        c.text.performer = session_.discPerformer.toStdString();
        if (session_.clips.isEmpty()) { c.gapBeforeSec = 2.0; c.pauseSec = 2.0; }
        else { c.gapBeforeSec = 2.0; c.pauseSec = 2.0; }
        // Pick up an ISRC from the file's metadata if the mix engineer embedded one.
        auto isrc = r->metadataValues.getValue("ISRC", r->metadataValues.getValue("isrc", ""));
        if (isrc.isNotEmpty()) c.isrc = cd::normaliseIsrc(isrc.toStdString());
        session_.clips.add(c);
    }
    sessionChanged();
    if (!bad.isEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Some files couldn't be read",
                                               bad.joinIntoString("\n"));
}

int MainComponent::selectedRow() const { return table_.getSelectedRow(); }

void MainComponent::removeSelected() {
    int i = selectedRow();
    if (i < 0 || i >= session_.clips.size()) return;
    session_.clips.remove(i);
    sessionChanged();
}

void MainComponent::moveSelected(int d) {
    int i = selectedRow(), j = i + d;
    if (i < 0 || j < 0 || j >= session_.clips.size()) return;
    session_.clips.swap(i, j);
    sessionChanged();
    table_.selectRow(j);
}

void MainComponent::addSubIndexAtPlayhead() {
    auto lay = session_.layout();
    double t = engine_.positionSeconds();
    for (int i = 0; i < lay.clips.size(); ++i) {
        if (t > lay.clips[i].start + 0.05 && t < lay.clips[i].end - 0.05) {
            auto& c = session_.clips.getReference(i);
            c.subIndexSec.add(t - lay.clips[i].start);
            std::sort(c.subIndexSec.begin(), c.subIndexSec.end());
            sessionChanged();
            return;
        }
    }
}

int MainComponent::getNumRows() { return session_.clips.size(); }

void MainComponent::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    g.fillAll(selected ? colours::accent.withAlpha(0.25f) : (row % 2 ? colours::panel : colours::panel.brighter(0.03f)));
}

void MainComponent::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    g.setColour(colours::text);
    g.setFont(13.0f);
    g.drawText(cellText(row, col), 4, 0, w - 8, h, col >= cGap ? juce::Justification::centredRight : juce::Justification::centredLeft);
}

juce::Component* MainComponent::refreshComponentForCell(int row, int col, bool, juce::Component* existing) {
    const bool editable = col == cTitle || col == cPerformer || col == cIsrc || col == cGap || col == cPause ||
                          col == cGain || col == cFadeIn || col == cFadeOut;
    if (!editable || row >= session_.clips.size()) { delete existing; return nullptr; }
    auto* c = dynamic_cast<Cell*>(existing);
    if (!c) { delete existing; c = new Cell(*this); }
    c->set(row, col);
    return c;
}

void MainComponent::cellDoubleClicked(int row, int col, const juce::MouseEvent&) {
    if (row >= session_.clips.size()) return;
    auto& c = session_.clips.getReference(row);
    if (col == cDcp) { c.copyPermitted = !c.copyPermitted; if (c.copyPermitted) c.scms = false; sessionChanged(); }
    if (col == cPre) { c.preEmphasis = !c.preEmphasis; sessionChanged(); }
    if (col == cStart || col == cNum) engine_.setPositionSeconds(juce::jmax(0.0, session_.layout().clips[row].start - 1.0));
}

void MainComponent::selectedRowsChanged(int) {}

juce::String MainComponent::cellText(int row, int col) const {
    if (row < 0 || row >= session_.clips.size()) return {};
    const auto& c = session_.clips.getReference(row);
    auto lay = session_.layout();
    switch (col) {
        case cNum: return juce::String(row + 1);
        case cTitle: return juce::String::fromUTF8(c.text.title.c_str());
        case cPerformer: return juce::String::fromUTF8(c.text.performer.c_str());
        case cIsrc: return juce::String(c.isrc);
        case cGap: return juce::String(c.gapBeforeSec, 2);
        case cPause: return juce::String(c.pauseSec, 2);
        case cGain: return juce::String(c.gainDb, 1);
        case cFadeIn: return juce::String(c.fadeInSec, 2);
        case cFadeOut: return juce::String(c.fadeOutSec, 2);
        case cStart: return msf(lay.clips[row].start);
        case cLength: {
            double next = row + 1 < lay.clips.size() ? lay.clips[row + 1].start : lay.total;
            return msf(next - lay.clips[row].start);
        }
        case cDcp: return c.copyPermitted ? "Yes" : "-";
        case cPre: return c.preEmphasis ? "Yes" : "-";
        default: return {};
    }
}

void MainComponent::setCellText(int row, int col, const juce::String& t) {
    if (row < 0 || row >= session_.clips.size()) return;
    auto& c = session_.clips.getReference(row);
    switch (col) {
        case cTitle: c.text.title = t.toStdString(); c.name = t; break;
        case cPerformer: c.text.performer = t.toStdString(); break;
        case cIsrc: c.isrc = cd::normaliseIsrc(t.toStdString()); break;
        case cGap: c.gapBeforeSec = parseTime(t, c.gapBeforeSec); break;
        case cPause: c.pauseSec = juce::jmax(0.0, parseTime(t, c.pauseSec)); break;
        case cGain: c.gainDb = juce::jlimit(-60.0, 24.0, t.getDoubleValue()); break;
        case cFadeIn: c.fadeInSec = juce::jlimit(0.0, c.durationSec(), parseTime(t, c.fadeInSec)); break;
        case cFadeOut: c.fadeOutSec = juce::jlimit(0.0, c.durationSec(), parseTime(t, c.fadeOutSec)); break;
        default: return;
    }
    sessionChanged();
}

void MainComponent::refreshIssues() {
    if (session_.clips.isEmpty()) { issues_.setText("No tracks yet."); return; }
    juce::StringArray adj;
    auto disc = session_.toDisc(&adj);
    auto issues = cd::validate(disc);
    auto cdt = cd::encodeCdText(disc);
    juce::String s;
    int errors = 0;
    for (auto& i : issues) {
        if (i.level == cd::Issue::Error) ++errors;
        s << (i.level == cd::Issue::Error ? "ERROR    " : "Warning  ") << juce::String::fromUTF8(i.message.c_str()) << "\n";
    }
    if (!cdt.error.empty()) { ++errors; s << "ERROR    " << juce::String::fromUTF8(cdt.error.c_str()) << "\n"; }
    for (auto& a : adj) s << "Note     " << a << "\n";
    for (auto& c : session_.clips)
        if (!c.file.existsAsFile()) { ++errors; s << "ERROR    Missing file: " << c.file.getFullPathName() << "\n"; }
    auto head = juce::String(session_.clips.size()) + " tracks, " + msf(session_.layout().total) + " total. ";
    head << (errors ? juce::String(errors) + " problem(s) to fix before export.\n" : "Ready for DDP export.\n");
    issues_.setText(head + s);
}

// ---------------------------------------------------------------- plugins

int MainComponent::ChainModel::getNumRows() { return o.engine_.chain().size(); }

void MainComponent::ChainModel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    g.fillAll(selected ? colours::accent.withAlpha(0.25f) : colours::panel);
    bool byp = o.engine_.chain().isBypassed(row);
    g.setColour(byp ? colours::dim : colours::text);
    g.setFont(13.0f);
    g.drawText(juce::String(row + 1) + ". " + o.engine_.chain().nameAt(row) + (byp ? "  (bypassed)" : ""), 6, 0, w - 12, h,
               juce::Justification::centredLeft);
}

void MainComponent::ChainModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { o.engine_.chain().showEditor(row); }

void MainComponent::showPluginScanner() {
    auto& ch = engine_.chain();
    auto* list = new juce::PluginListComponent(ch.formats(), ch.known(), ch.deadMansPedal(), props_.getUserSettings(), true);
    list->setSize(700, 500);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(list);
    o.dialogTitle = "Plugins (press Options > Scan to find new ones)";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showPluginMenu() {
    auto& ch = engine_.chain();
    juce::PopupMenu m;
    auto types = ch.known().getTypes();
    if (types.isEmpty()) m.addItem(1, "No plugins found yet; scan first", false);
    juce::KnownPluginList::addToMenu(m, types, juce::KnownPluginList::sortByManufacturer, {});
    m.addSeparator();
    m.addItem(-1, "Scan / manage plugins...");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&pluginsBtn_), [this, types](int r) {
        if (r == -1) { showPluginScanner(); return; }
        int idx = juce::KnownPluginList::getIndexChosenByMenu(types, r);
        if (idx < 0) return;
        juce::String err;
        if (!engine_.chain().add(types[idx], err)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Couldn't load plugin", err);
            return;
        }
        chainList_.updateContent();
        chainList_.selectRow(engine_.chain().size() - 1);
        engine_.chain().showEditor(engine_.chain().size() - 1);
        sessionChanged();
    });
}

// ---------------------------------------------------------------- export / QC

ExportSettings MainComponent::exportSettings() const {
    ExportSettings es;
    es.shape = dither_.getSelectedId() == 2 ? dsp::Ditherer::Shape::Light
             : dither_.getSelectedId() == 3 ? dsp::Ditherer::Shape::Strong
                                            : dsp::Ditherer::Shape::None;
    return es;
}

namespace {
// Runs `work` on a background thread behind a progress window, then calls `done` on the
// message thread. The window deletes itself; shared state lives in the lambdas' captures.
struct Job : juce::ThreadWithProgressWindow {
    std::function<void(Job&)> work;
    std::function<void(bool cancelled)> done;
    explicit Job(const juce::String& t) : ThreadWithProgressWindow(t, true, true) {}
    void run() override { work(*this); }
    void threadComplete(bool cancelled) override {
        auto d = std::move(done);
        delete this;
        if (d) d(cancelled);
    }
    bool keep(double p) { setProgress(p); return !threadShouldExit(); }
};

void runJob(const juce::String& title, std::function<void(Job&)> work, std::function<void(bool)> done) {
    auto* j = new Job(title);
    j->work = std::move(work);
    j->done = std::move(done);
    j->launchThread();
}
}  // namespace

void MainComponent::showExportMenu() {
    juce::PopupMenu m;
    m.addItem(1, "DDP image for replication (CD master)");
    m.addItem(2, "Track WAVs, 16-bit / 44.1 kHz (as on the CD)");
    m.addItem(3, "Programme WAV, 24-bit at project rate");
    m.addItem(4, "Programme WAV, 32-bit float at project rate");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportBtn_), [this](int r) {
        if (r == 1) exportDdp();
        if (r == 2) exportTrackWavs();
        if (r == 3 || r == 4) {
            int bits = r == 3 ? 24 : 32;
            chooser_ = std::make_unique<juce::FileChooser>("Export programme WAV", juce::File(), "*.wav");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, bits](const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f == juce::File()) return;
                f = f.withFileExtension("wav");
                auto es = exportSettings();
                auto snapshot = std::make_shared<Session>(session_);
                auto err = std::make_shared<juce::String>();
                runJob("Exporting programme WAV...",
                       [this, snapshot, err, f, bits, es](Job& j) {
                           *err = exporter_.exportProgramWav(*snapshot, f, bits, es, [&](double p) { return j.keep(p); });
                       },
                       [err, f](bool) {
                           if (err->isNotEmpty()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", *err);
                           else f.revealToUser();
                       });
            });
        }
    });
}

void MainComponent::exportDdp() {
    juce::StringArray adj;
    auto disc = session_.toDisc(&adj);
    if (cd::hasErrors(cd::validate(disc)) || !cd::encodeCdText(disc).error.empty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Not ready",
                                               "Fix the errors listed at the bottom of the window first.");
        return;
    }
    chooser_ = std::make_unique<juce::FileChooser>("Choose where to create the DDP folder",
                                                   juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
        auto parent = fc.getResult();
        if (!parent.isDirectory()) return;
        auto name = juce::File::createLegalFileName((session_.discTitle.isEmpty() ? juce::String("Lathe") : session_.discTitle) + " DDP");
        auto folder = parent.getNonexistentChildFile(name, "", false);
        auto snapshot = std::make_shared<Session>(session_);
        auto es = exportSettings();
        auto res = std::make_shared<ddp::WriteResult>();
        auto qc = std::make_shared<QcReport>();
        runJob("Writing DDP image...",
               [this, snapshot, folder, es, res, qc](Job& j) {
                   *res = exporter_.exportDdp(*snapshot, folder, es, [&](double p) { return j.keep(p); }, *qc);
               },
               [this, res, qc, folder](bool) {
            if (!res->ok) {
                juce::String msg = juce::String::fromUTF8(res->error.c_str());
                for (auto& i : res->issues) msg << "\n" << juce::String::fromUTF8(i.message.c_str());
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "DDP export failed", msg);
                return;
            }
            // Re-read and checksum what was written: catches disk/USB faults now, not at the plant.
            auto check = std::make_shared<ddp::ReadResult>();
            runJob("Verifying DDP...",
                   [check, folder](Job& j) {
                       *check = ddp::readDdp(folder.getFullPathName().toStdString(), [&](double p) { j.setProgress(p); });
                   },
                   [this, check, res, qc, folder](bool) {
                juce::String msg;
                msg << "DDP written to:\n" << folder.getFullPathName() << "\n\nImage MD5: " << res->imageMd5 << "\n";
                msg << (check->ok && check->checksumsOk ? "Verified: every file re-read and matches its checksum.\n"
                                                        : "VERIFY FAILED: the files on disk don't match. Re-export to a different drive.\n");
                msg << "\n" << qc->toText();
                showText("DDP export complete", msg);
                folder.revealToUser();
            });
        });
    });
}

void MainComponent::exportTrackWavs() {
    chooser_ = std::make_unique<juce::FileChooser>("Choose a folder for the track WAVs",
                                                   juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
        auto folder = fc.getResult();
        if (!folder.isDirectory()) return;
        auto snapshot = std::make_shared<Session>(session_);
        auto es = exportSettings();
        auto err = std::make_shared<juce::String>();
        runJob("Exporting track WAVs...",
               [this, snapshot, folder, es, err](Job& j) {
                   *err = exporter_.exportTrackWavs(*snapshot, folder, es, [&](double p) { return j.keep(p); });
               },
               [err, folder](bool) {
                   if (err->isNotEmpty()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", *err);
                   else folder.revealToUser();
               });
    });
}

void MainComponent::analyse() {
    if (session_.clips.isEmpty()) return;
    auto snapshot = std::make_shared<Session>(session_);
    auto es = exportSettings();
    auto qc = std::make_shared<QcReport>();
    auto err = std::make_shared<juce::String>();
    runJob("Analysing the CD master...",
           [this, snapshot, es, qc, err](Job& j) { *err = exporter_.analyse(*snapshot, es, [&](double p) { return j.keep(p); }, *qc); },
           [this, qc, err](bool) {
               if (err->isNotEmpty()) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Analyse", *err); return; }
               showText("Loudness and peak report", qc->toText());
           });
}

void MainComponent::verifyDdp() {
    chooser_ = std::make_unique<juce::FileChooser>("Choose a DDP folder to verify");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
        auto folder = fc.getResult();
        if (!folder.isDirectory()) return;
        auto r = std::make_shared<ddp::ReadResult>();
        runJob("Verifying DDP...",
               [r, folder](Job& j) { *r = ddp::readDdp(folder.getFullPathName().toStdString(), [&](double p) { j.setProgress(p); }); },
               [this, r, folder](bool) {
            juce::String s;
            if (!r->ok) { s << "This folder can't be read as a DDP:\n" << juce::String::fromUTF8(r->error.c_str()); showText("DDP verify", s); return; }
            s << (r->checksumsOk ? "PASS: checksums match.\n" : r->checksumsChecked ? "FAIL: checksum mismatch.\n" : "No checksum file to verify against.\n");
            s << "UPC/EAN: " << (r->disc.upcEan.empty() ? "(none)" : r->disc.upcEan) << "\n";
            if (!r->disc.text.title.empty()) s << "Title: " << juce::String::fromUTF8(r->disc.text.title.c_str()) << "\n";
            s << "Tracks: " << int(r->disc.tracks.size()) << ", total " << cd::Msf::fromSectors(r->disc.leadOutSector).str() << "\n\n";
            s << juce::String::fromUTF8(ddp::makePqLog(r->disc, "").c_str()) << "\n";
            for (auto& n : r->notes) s << juce::String::fromUTF8(n.c_str()) << "\n";
            showText("DDP verify: " + folder.getFileName(), s);
        });
    });
}

void MainComponent::showAudioSettings() {
    auto* sel = new juce::AudioDeviceSelectorComponent(engine_.devices(), 0, 0, 2, 2, false, false, true, false);
    sel->setSize(500, 400);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(sel);
    o.dialogTitle = "Audio Setup";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.launchAsync();
}

void MainComponent::showText(const juce::String& title, const juce::String& text) {
    auto* ed = new juce::TextEditor();
    ed->setMultiLine(true);
    ed->setReadOnly(true);
    ed->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    ed->setText(text);
    ed->setSize(760, 520);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(ed);
    o.dialogTitle = title;
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

// ---------------------------------------------------------------- drag and drop, timer

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files) {
        auto ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".lathe" || engine_.formats().findFormatForFileExtension(ext) != nullptr) return true;
    }
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int) {
    juce::Array<juce::File> audio;
    for (auto& f : files) {
        juce::File file(f);
        if (file.hasFileExtension("lathe")) { loadSessionFile(file); return; }
        audio.add(file);
    }
    audio.sort();
    addFiles(audio);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*) {
    // Device changed (e.g. sample rate): rebuild the streaming source at the new rate.
    if (std::abs(engine_.deviceRate() - lastDeviceRate_) > 0.5) {
        lastDeviceRate_ = engine_.deviceRate();
        engine_.loadSession(session_);
    }
}

void MainComponent::timerCallback() {
    double pos = engine_.positionSeconds();
    overview_->setPlayhead(pos);
    timeDisplay_.setText(msf(pos), juce::dontSendNotification);
    auto lay = session_.layout();
    juce::String tr;
    for (int i = 0; i < lay.clips.size(); ++i) {
        if (pos >= lay.clips[i].start - session_.clips[i].pauseSec && (i + 1 >= lay.clips.size() || pos < lay.clips[i + 1].start - session_.clips[i + 1].pauseSec)) {
            double rel = pos - lay.clips[i].start;
            tr = "Track " + juce::String(i + 1) + "  " + (rel < 0 ? "-" + msf(-rel) : msf(rel));
            break;
        }
    }
    trackDisplay_.setText(tr, juce::dontSendNotification);
    playBtn_.setButtonText(engine_.isPlaying() ? "Stop" : "Play");
    meters_->prepare(engine_.deviceRate());
    meters_->feed(engine_.meterTap());

    double pr = session_.effectiveRate(), dr = engine_.deviceRate();
    rateWarning_.setText(!session_.clips.isEmpty() && std::abs(pr - dr) > 0.5
                             ? "Monitoring at " + juce::String(dr / 1000.0, 1) + " kHz; project is " + juce::String(pr / 1000.0, 1) + " kHz"
                             : juce::String(),
                         juce::dontSendNotification);
}

}  // namespace lathe
