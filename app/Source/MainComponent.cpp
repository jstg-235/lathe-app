#include "MainComponent.h"

#include "../../core/cd/CdTextEncoder.h"
#include "NullTest.h"
#include "Theme.h"

namespace lathe {

enum PqCol { pTrack = 1, pType, pTime, pLength, pTitle, pPerformer, pIsrc, pDcp, pPre };

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
        auto norm = [](float lin) { float db = juce::Decibels::gainToDecibels(lin, -60.0f); return (db + 60.0f) / 60.0f; };
        for (int c = 0; c < 2; ++c) {
            auto b = bars.removeFromLeft(18).toFloat();
            bars.removeFromLeft(4);
            g.setColour(colours::line);
            g.fillRect(b);
            g.setColour(hold_[c] >= 0.99997f ? colours::error : colours::ok);
            g.fillRect(b.withTop(b.getBottom() - b.getHeight() * norm(peak_[c])));
            g.setColour(colours::text);
            g.drawHorizontalLine(int(b.getBottom() - b.getHeight() * norm(hold_[c])), b.getX(), b.getRight());
        }
        r.removeFromLeft(8);
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
    void mouseDown(const juce::MouseEvent&) override { reset(); }

private:
    dsp::LoudnessMeter meter_;
    double rate_ = 0;
    float peak_[2]{}, hold_[2]{};
    juce::AudioBuffer<float> scratch_{2, 1 << 17};
};

// ---------------------------------------------------------------- editable cell

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
    }
    void textWasEdited() override { owner_.setCellText(row_, col_, getText()); }
    void mouseDown(const juce::MouseEvent& e) override {
        if (auto* t = findParentComponentOfClass<juce::TableListBox>()) {
            t->selectRowsBasedOnModifierKeys(row_, e.mods, false);
            if (auto* m = t->getTableListBoxModel()) m->cellClicked(row_, col_, e);
        }
        Label::mouseDown(e);
    }
private:
    MainComponent& owner_;
    int row_ = 0, col_ = 0;
};

// ---------------------------------------------------------------- background jobs

namespace {
struct Job : juce::ThreadWithProgressWindow {
    std::function<void(Job&)> work;
    std::function<void(bool)> done;
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

class XfadeWindow : public juce::DocumentWindow {
public:
    XfadeWindow(juce::Component* c) : DocumentWindow("Crossfade Editor", colours::bg, DocumentWindow::closeButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(c, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setAlwaysOnTop(true);
        setVisible(true);
    }
    std::function<void()> onClose;
    void closeButtonPressed() override { if (onClose) onClose(); }
};
}  // namespace

// ---------------------------------------------------------------- construction

MainComponent::MainComponent() {
    juce::PropertiesFile::Options o;
    o.applicationName = "Lathe";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    o.folderName = "Lathe";
    props_.setStorageParameters(o);

    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, colours::bg);
    meters_ = std::make_unique<LevelMeters>();
    addAndMakeVisible(*meters_);
    addAndMakeVisible(source_);
    addAndMakeVisible(timeline_);

    for (auto* b : {&newBtn_, &openBtn_, &saveBtn_, &importBtn_, &envBtn_, &snapBtn_, &lanesBtn_, &xfadeBtn_, &trackMk_, &indexMk_,
                    &pauseMk_, &splitBtn_, &playBtn_, &homeBtn_, &pluginsBtn_, &exportBtn_, &analyseBtn_, &verifyBtn_, &audioBtn_,
                    &chainAdd_, &chainRemove_, &chainBypass_, &chainUp_, &chainDown_, &zoomIn_, &zoomOut_, &zoomFit_}) {
        styleButton(*b, b == &exportBtn_);
        addAndMakeVisible(*b);
    }
    envBtn_.setClickingTogglesState(true);
    snapBtn_.setClickingTogglesState(true);
    snapBtn_.setToggleState(true, juce::dontSendNotification);

    newBtn_.onClick = [this] { newSession(); };
    openBtn_.onClick = [this] { openSession(); };
    saveBtn_.onClick = [this] { saveSession(false); };
    importBtn_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Import audio as tracks", juce::File(), engine_.formats().getWildcardForAllFormats());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this](const juce::FileChooser& fc) { importFiles(fc.getResults()); });
    };
    envBtn_.onClick = [this] { timeline_.setEnvelopeMode(envBtn_.getToggleState()); };
    snapBtn_.onClick = [this] { timeline_.setSnap(snapBtn_.getToggleState()); };
    lanesBtn_.onClick = [this] {
        juce::PopupMenu m;
        for (int n : {1, 2, 4, 8, 16}) m.addItem(n, juce::String(n) + (n == 1 ? " track" : " tracks"), true, session_.lanes == n);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&lanesBtn_), [this](int n) {
            if (n <= 0) return;
            int maxLane = 0;
            for (auto& r : session_.regions) maxLane = juce::jmax(maxLane, r.lane);
            session_.lanes = juce::jmax(n, maxLane + 1);
            sessionChanged();
            timeline_.resized();
        });
    };
    xfadeBtn_.onClick = [this] {
        auto p = timeline_.crossfadeNear(engine_.positionSeconds());
        if (p.first) openCrossfade(p.first, p.second);
    };
    trackMk_.onClick = [this] { addMarkerAtPlayhead(Marker::TrackStart); };
    indexMk_.onClick = [this] { addMarkerAtPlayhead(Marker::Index); };
    pauseMk_.onClick = [this] { addMarkerAtPlayhead(Marker::PauseStart); };
    splitBtn_.onClick = [this] { splitAtPlayhead(); };
    playBtn_.onClick = [this] { auditionEnd_ = -1; engine_.isPlaying() ? engine_.stop() : engine_.play(); };
    homeBtn_.onClick = [this] { engine_.setPositionSeconds(0); meters_->reset(); };
    pluginsBtn_.onClick = [this] { showPluginMenu(); };
    exportBtn_.onClick = [this] { showExportMenu(); };
    analyseBtn_.onClick = [this] { analyse(); };
    verifyBtn_.onClick = [this] { verifyDdp(); };
    audioBtn_.onClick = [this] { showAudioSettings(); };
    zoomIn_.onClick = [this] { timeline_.zoomBy(1.5, engine_.positionSeconds()); };
    zoomOut_.onClick = [this] { timeline_.zoomBy(1 / 1.5, engine_.positionSeconds()); };
    zoomFit_.onClick = [this] { timeline_.zoomToFit(); };

    chainAdd_.onClick = [this] { showPluginMenu(); };
    chainRemove_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i >= 0) { engine_.chain().remove(i); chainList_.updateContent(); sessionChanged(); }
    };
    chainBypass_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i >= 0) { engine_.chain().setBypassed(i, !engine_.chain().isBypassed(i)); chainList_.repaint(); sessionChanged(); }
    };
    chainUp_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i > 0) { engine_.chain().move(i, i - 1); chainList_.selectRow(i - 1); chainList_.updateContent(); sessionChanged(); }
    };
    chainDown_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i >= 0 && i + 1 < engine_.chain().size()) { engine_.chain().move(i, i + 1); chainList_.selectRow(i + 1); chainList_.updateContent(); sessionChanged(); }
    };

    source_.onEdit = [this](Session::EditMode m) { sourceEdit(m); };
    source_.onAppend = [this] { appendSource(); };

    timeline_.onEdit = [this] { sessionChanged(); };
    timeline_.onSeek = [this](double t) { auditionEnd_ = -1; engine_.setPositionSeconds(t); timeline_.setPlayhead(t, false); };
    timeline_.onCrossfade = [this](int a, int b) { openCrossfade(a, b); };
    timeline_.onMarkerSelected = [this](int id) {
        for (int i = 0; i < session_.markers.size(); ++i)
            if (session_.markers[i].id == id) { pqList_.selectRow(i); break; }
    };
    timeline_.onMarkerDoubleClicked = [this](int id) {
        if (auto* m = session_.findMarker(id)) engine_.setPositionSeconds(m->t);
    };

    auto& h = pqList_.getHeader();
    h.addColumn("Trk", pTrack, 36);
    h.addColumn("Type", pType, 70);
    h.addColumn("Time", pTime, 78);
    h.addColumn("Length", pLength, 74);
    h.addColumn("Title", pTitle, 200, 80, 600);
    h.addColumn("Performer", pPerformer, 150, 60, 400);
    h.addColumn("ISRC", pIsrc, 118);
    h.addColumn("DCP", pDcp, 40);
    h.addColumn("Pre", pPre, 40);
    pqList_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    pqList_.setRowHeight(22);
    h.setColour(juce::TableHeaderComponent::backgroundColourId, colours::bg);
    h.setColour(juce::TableHeaderComponent::textColourId, colours::dim);
    addAndMakeVisible(pqList_);

    chainList_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    chainList_.setRowHeight(24);
    addAndMakeVisible(chainList_);

    issues_.setMultiLine(true);
    issues_.setReadOnly(true);
    issues_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
    issues_.setColour(juce::TextEditor::textColourId, colours::text);
    issues_.setColour(juce::TextEditor::outlineColourId, colours::line);
    addAndMakeVisible(issues_);

    for (auto* l : {&timeDisplay_, &trackDisplay_, &rateWarning_, &selDisplay_}) {
        l->setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(*l);
    }
    timeDisplay_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 26.0f, juce::Font::plain));
    rateWarning_.setColour(juce::Label::textColourId, colours::accent);
    selDisplay_.setColour(juce::Label::textColourId, colours::dim);

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
    dither_.addItem("TPDF flat", 1);
    dither_.addItem("TPDF + 1st-order shaping", 2);
    dither_.addItem("TPDF + 2nd-order shaping", 3);
    dither_.addItem("TPDF + Lipshitz 5th-order", 4);
    dither_.addItem("TPDF + psychoacoustic 9th-order", 5);
    dither_.setSelectedId(props_.getUserSettings()->getIntValue("dither", 1), juce::dontSendNotification);
    dither_.onChange = [this] { props_.getUserSettings()->setValue("dither", dither_.getSelectedId()); };
    addAndMakeVisible(dither_);

    engine_.devices().addChangeListener(this);
    lastDeviceRate_ = engine_.deviceRate();
    followRate_ = props_.getUserSettings()->getBoolValue("followProjectRate", true);

    commands_.registerAllCommandsForTarget(this);
    commands_.setFirstCommandTarget(this);
    if (auto xml = props_.getUserSettings()->getXmlValue("keyMappings")) commands_.getKeyMappings()->restoreFromXml(*xml);
    commands_.getKeyMappings()->addChangeListener(this);
    addKeyListener(commands_.getKeyMappings());
    setApplicationCommandManagerToWatch(&commands_);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#endif
    setWantsKeyboardFocus(true);
    setSize(1440, 900);
    newSession();
    startTimerHz(30);
}

MainComponent::~MainComponent() {
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    commands_.getKeyMappings()->removeChangeListener(this);
    removeKeyListener(commands_.getKeyMappings());
    xfadeWindow_.reset();
    engine_.devices().removeChangeListener(this);
    engine_.chain().closeAllEditors();
    pqList_.setModel(nullptr);
    chainList_.setModel(nullptr);
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(colours::bg); }

void MainComponent::resized() {
    auto r = getLocalBounds().reduced(8);
    auto top = r.removeFromTop(28);
    for (auto* b : {&newBtn_, &openBtn_, &saveBtn_, &importBtn_}) { b->setBounds(top.removeFromLeft(62)); top.removeFromLeft(4); }
    audioBtn_.setBounds(top.removeFromRight(96));
    top.removeFromRight(4);
    exportBtn_.setBounds(top.removeFromRight(84));
    top.removeFromRight(4);
    verifyBtn_.setBounds(top.removeFromRight(86));
    top.removeFromRight(4);
    analyseBtn_.setBounds(top.removeFromRight(74));
    top.removeFromRight(4);
    pluginsBtn_.setBounds(top.removeFromRight(84));

    r.removeFromTop(6);
    auto fields = r.removeFromTop(24);
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

    r.removeFromTop(6);
    auto right = r.removeFromRight(300);
    r.removeFromRight(8);
    meters_->setBounds(right.removeFromTop(150));
    right.removeFromTop(8);
    auto chainButtons = right.removeFromBottom(26);
    for (auto* b : {&chainAdd_, &chainRemove_, &chainBypass_, &chainUp_, &chainDown_}) {
        b->setBounds(chainButtons.removeFromLeft(56));
        chainButtons.removeFromLeft(4);
    }
    right.removeFromBottom(6);
    issues_.setBounds(right.removeFromBottom(160));
    right.removeFromBottom(6);
    chainList_.setBounds(right);

    source_.setBounds(r.removeFromTop(150));
    r.removeFromTop(6);

    auto transport = r.removeFromTop(34);
    homeBtn_.setBounds(transport.removeFromLeft(40).reduced(0, 3));
    transport.removeFromLeft(4);
    playBtn_.setBounds(transport.removeFromLeft(70).reduced(0, 3));
    transport.removeFromLeft(10);
    timeDisplay_.setBounds(transport.removeFromLeft(150));
    trackDisplay_.setBounds(transport.removeFromLeft(170));
    for (auto* b : {&zoomFit_, &zoomIn_, &zoomOut_}) { b->setBounds(transport.removeFromRight(b == &zoomFit_ ? 40 : 28).reduced(0, 3)); transport.removeFromRight(3); }
    transport.removeFromRight(6);
    for (auto* b : {&xfadeBtn_, &lanesBtn_, &snapBtn_, &envBtn_, &splitBtn_, &pauseMk_, &indexMk_, &trackMk_}) {
        b->setBounds(transport.removeFromRight(b == &xfadeBtn_ ? 86 : 64).reduced(0, 3));
        transport.removeFromRight(3);
    }
    selDisplay_.setBounds(transport.removeFromLeft(transport.getWidth() / 2));
    rateWarning_.setBounds(transport);

    pqList_.setBounds(r.removeFromBottom(200));
    r.removeFromBottom(6);
    timeline_.setBounds(r);
}

// ---------------------------------------------------------------- commands

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& ids) {
    for (auto& d : allCommands()) ids.add(d.id);
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) {
    auto* d = findCommand(id);
    if (!d) return;
    info.setInfo(d->name, d->description, d->category, 0);
    for (auto& k : juce::StringArray::fromTokens(d->keys, ";", ""))
        if (k.trim().isNotEmpty()) info.addDefaultKeypress(juce::KeyPress::createFromDescription(k.trim()).getKeyCode(),
                                                           juce::KeyPress::createFromDescription(k.trim()).getModifiers());
    switch (id) {
        case cmd::undo: info.setActive(!undo_.isEmpty()); break;
        case cmd::redo: info.setActive(!redo_.isEmpty()); break;
        case cmd::toggleEnvelope: info.setTicked(timeline_.envelopeMode()); break;
        case cmd::toggleSnap: info.setTicked(snapBtn_.getToggleState()); break;
        case cmd::followProjectRate: info.setTicked(followRate_); break;
        case cmd::muteRegion:
        case cmd::nudgeLeft: case cmd::nudgeRight: case cmd::nudgeLeftBig: case cmd::nudgeRightBig:
        case cmd::selectRegionRange: info.setActive(timeline_.selectedRegion() != 0); break;
        default: break;
    }
}

bool MainComponent::perform(const InvocationInfo& info) {
    const double pos = engine_.positionSeconds();
    switch (info.commandID) {
        case cmd::newSession: newSession(); break;
        case cmd::openSession: openSession(); break;
        case cmd::saveSession: saveSession(false); break;
        case cmd::saveSessionAs: saveSession(true); break;
        case cmd::importAudio: importBtn_.onClick(); break;
        case cmd::openSource: source_.showOpenDialog(); break;
        case cmd::exportDdp: exportDdp(); break;
        case cmd::exportTrackWavs: exportTrackWavs(); break;
        case cmd::exportProgram24:
        case cmd::exportProgram32: showExportMenu(); break;
        case cmd::undo: undo(); break;
        case cmd::redo: redo(); break;
        case cmd::splitAtPlayhead: splitAtPlayhead(); break;
        case cmd::deleteClose: deleteSelection(true); break;
        case cmd::deleteLeave: deleteSelection(false); break;
        case cmd::muteRegion:
            if (auto* r = session_.findRegion(timeline_.selectedRegion())) { r->mute = !r->mute; sessionChanged(); }
            break;
        case cmd::nudgeLeft: nudgeSelectedRegion(-1.0 / 75); break;
        case cmd::nudgeRight: nudgeSelectedRegion(1.0 / 75); break;
        case cmd::nudgeLeftBig: nudgeSelectedRegion(-0.1); break;
        case cmd::nudgeRightBig: nudgeSelectedRegion(0.1); break;
        case cmd::clearSelection: timeline_.setSelection({}); timeline_.selectRegion(0); timeline_.selectMarker(0); break;
        case cmd::selectRegionRange:
            if (auto* r = session_.findRegion(timeline_.selectedRegion())) timeline_.setSelection({r->start, r->end()});
            break;
        case cmd::sourceIn: source_.markIn(); break;
        case cmd::sourceOut: source_.markOut(); break;
        case cmd::destIn: { auto s = timeline_.selection(); timeline_.setSelection({pos, juce::jmax(pos, s.getEnd())}); break; }
        case cmd::destOut: { auto s = timeline_.selection(); timeline_.setSelection({juce::jmin(pos, s.isEmpty() ? pos : s.getStart()), pos}); break; }
        case cmd::insertEdit: sourceEdit(Session::EditMode::Insert); break;
        case cmd::overwriteEdit: sourceEdit(Session::EditMode::Overwrite); break;
        case cmd::replaceEdit: sourceEdit(Session::EditMode::Replace); break;
        case cmd::appendEdit: appendSource(); break;
        case cmd::playSourceInOut: source_.playInOut(); break;
        case cmd::playStop: playBtn_.onClick(); break;
        case cmd::playSource: source_.togglePlay(); break;
        case cmd::returnToStart: homeBtn_.onClick(); break;
        case cmd::locateBackFrame: engine_.setPositionSeconds(juce::jmax(0.0, pos - 1.0 / 75)); break;
        case cmd::locateFwdFrame: engine_.setPositionSeconds(pos + 1.0 / 75); break;
        case cmd::locateBackSecond: engine_.setPositionSeconds(juce::jmax(0.0, pos - 1.0)); break;
        case cmd::locateFwdSecond: engine_.setPositionSeconds(pos + 1.0); break;
        case cmd::prevMarker: jumpToMarker(-1); break;
        case cmd::nextMarker: jumpToMarker(1); break;
        case cmd::playSelection: {
            auto s = timeline_.selection();
            if (!s.isEmpty()) auditionRange(s.getStart(), s.getEnd());
            break;
        }
        case cmd::addTrackMarker: addMarkerAtPlayhead(Marker::TrackStart); break;
        case cmd::addIndexMarker: addMarkerAtPlayhead(Marker::Index); break;
        case cmd::addPauseMarker: addMarkerAtPlayhead(Marker::PauseStart); break;
        case cmd::addEndMarker: addMarkerAtPlayhead(Marker::EndOfDisc); break;
        case cmd::zoomIn: zoomIn_.onClick(); break;
        case cmd::zoomOut: zoomOut_.onClick(); break;
        case cmd::zoomFit: timeline_.zoomToFit(); break;
        case cmd::toggleEnvelope: envBtn_.triggerClick(); break;
        case cmd::toggleSnap: snapBtn_.triggerClick(); break;
        case cmd::crossfadeEditor: xfadeBtn_.onClick(); break;
        case cmd::analyse: analyse(); break;
        case cmd::verifyDdp: verifyDdp(); break;
        case cmd::nullTest: runNullTest(); break;
        case cmd::pluginManager: showPluginScanner(); break;
        case cmd::audioSetup: showAudioSettings(); break;
        case cmd::followProjectRate:
            followRate_ = !followRate_;
            props_.getUserSettings()->setValue("followProjectRate", followRate_);
            applyFollowRate();
            break;
        case cmd::keyGuide: showKeyGuide(); break;
        case cmd::editKeyCommands: showKeyEditor(); break;
        default: return false;
    }
    commands_.commandStatusChanged();
    return true;
}

juce::StringArray MainComponent::getMenuBarNames() { return {"File", "Edit", "Transport", "Markers", "View", "Tools", "Help"}; }

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&) {
    juce::PopupMenu m;
    auto add = [&](std::initializer_list<juce::CommandID> ids) { for (auto id : ids) m.addCommandItem(&commands_, id); };
    switch (index) {
        case 0:
            add({cmd::newSession, cmd::openSession, cmd::saveSession, cmd::saveSessionAs});
            m.addSeparator();
            add({cmd::importAudio, cmd::openSource});
            m.addSeparator();
            add({cmd::exportDdp, cmd::exportTrackWavs, cmd::exportProgram24});
            break;
        case 1:
            add({cmd::undo, cmd::redo});
            m.addSeparator();
            add({cmd::sourceIn, cmd::sourceOut, cmd::destIn, cmd::destOut});
            m.addSeparator();
            add({cmd::insertEdit, cmd::overwriteEdit, cmd::replaceEdit, cmd::appendEdit});
            m.addSeparator();
            add({cmd::splitAtPlayhead, cmd::deleteClose, cmd::deleteLeave, cmd::muteRegion, cmd::selectRegionRange, cmd::clearSelection});
            m.addSeparator();
            add({cmd::nudgeLeft, cmd::nudgeRight, cmd::nudgeLeftBig, cmd::nudgeRightBig});
            break;
        case 2:
            add({cmd::playStop, cmd::playSource, cmd::playSourceInOut, cmd::playSelection, cmd::returnToStart});
            m.addSeparator();
            add({cmd::locateBackFrame, cmd::locateFwdFrame, cmd::locateBackSecond, cmd::locateFwdSecond, cmd::prevMarker, cmd::nextMarker});
            break;
        case 3: add({cmd::addTrackMarker, cmd::addIndexMarker, cmd::addPauseMarker, cmd::addEndMarker}); break;
        case 4:
            add({cmd::zoomIn, cmd::zoomOut, cmd::zoomFit});
            m.addSeparator();
            add({cmd::toggleEnvelope, cmd::toggleSnap, cmd::crossfadeEditor});
            break;
        case 5:
            add({cmd::analyse, cmd::verifyDdp, cmd::nullTest});
            m.addSeparator();
            add({cmd::pluginManager, cmd::audioSetup, cmd::followProjectRate});
            break;
        case 6: add({cmd::keyGuide, cmd::editKeyCommands}); break;
        default: break;
    }
    return m;
}

void MainComponent::saveKeyMappings() {
    if (auto xml = commands_.getKeyMappings()->createXml(true)) props_.getUserSettings()->setValue("keyMappings", xml.get());
    props_.saveIfNeeded();
}

namespace {
class KeyGuideView : public juce::Component {
public:
    KeyGuideView(const juce::KeyPressMappingSet& keys) : keys_(keys) {
        text_.setMultiLine(true);
        text_.setReadOnly(true);
        text_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        text_.setText(keyGuideText(&keys_));
        addAndMakeVisible(text_);
        for (auto* b : {&saveBtn_, &editBtn_}) { styleButton(*b); addAndMakeVisible(*b); }
        saveBtn_.onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Save printable key guide",
                                                           juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("Lathe Key Commands.html"), "*.html");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (f == juce::File()) return;
                f = f.withFileExtension("html");
                if (f.replaceWithText(keyGuideHtml(&keys_))) f.startAsProcess();
            });
        };
        setSize(900, 640);
    }
    std::function<void()> onEdit;
    void resized() override {
        auto r = getLocalBounds().reduced(8);
        auto row = r.removeFromBottom(28);
        saveBtn_.setBounds(row.removeFromLeft(220));
        row.removeFromLeft(6);
        editBtn_.setBounds(row.removeFromLeft(160));
        r.removeFromBottom(6);
        text_.setBounds(r);
        editBtn_.onClick = [this] { if (onEdit) onEdit(); };
    }
private:
    const juce::KeyPressMappingSet& keys_;
    juce::TextEditor text_;
    juce::TextButton saveBtn_{"Save Printable Guide (HTML)..."}, editBtn_{"Edit Key Commands..."};
    std::unique_ptr<juce::FileChooser> chooser_;
};
}  // namespace

void MainComponent::showKeyGuide() {
    auto* v = new KeyGuideView(*commands_.getKeyMappings());
    v->onEdit = [this] { showKeyEditor(); };
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(v);
    o.dialogTitle = "Key Command Guide";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showKeyEditor() {
    auto* ed = new juce::KeyMappingEditorComponent(*commands_.getKeyMappings(), true);
    ed->setColours(colours::panel2, colours::text);
    ed->setSize(620, 640);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(ed);
    o.dialogTitle = "Edit Key Commands (click + to add a key, click a key to change or remove it)";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::jumpToMarker(int dir) {
    double pos = engine_.positionSeconds();
    double best = dir < 0 ? -1.0 : 1e12;
    for (auto& m : session_.markers) {
        if (dir < 0 && m.t < pos - 0.02 && m.t > best) best = m.t;
        if (dir > 0 && m.t > pos + 0.02 && m.t < best) best = m.t;
    }
    if (dir < 0 && best < 0) best = 0;
    if (dir > 0 && best > 1e11) return;
    engine_.setPositionSeconds(best);
}

void MainComponent::applyFollowRate() {
    rateMessage_ = {};
    if (followRate_ && !session_.regions.isEmpty()) rateMessage_ = engine_.matchDeviceRate(session_.effectiveRate());
}

void MainComponent::runNullTest() {
    auto pickB = [this](juce::File a) {
        chooser_ = std::make_unique<juce::FileChooser>("Null test: choose file B (e.g. the Lathe render)", a.getParentDirectory(),
                                                       engine_.formats().getWildcardForAllFormats());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, a](const juce::FileChooser& fc) {
            auto b = fc.getResult();
            if (!b.existsAsFile()) return;
            auto diff = a.getSiblingFile(a.getFileNameWithoutExtension() + " minus " + b.getFileNameWithoutExtension() + ".wav");
            auto res = std::make_shared<NullResult>();
            runJob("Comparing files...",
                   [this, a, b, diff, res](Job& j) { *res = nullTest(engine_.formats(), a, b, diff, [&](double p) { return j.keep(p); }); },
                   [this, a, b, diff, res](bool) {
                       auto text = res->toText(a.getFileName(), b.getFileName());
                       if (res->ok) text << "\nDifference file (A minus B, 32-bit float):\n  " << diff.getFullPathName() << "\n";
                       showText("Null test", text);
                   });
        });
    };
    chooser_ = std::make_unique<juce::FileChooser>("Null test: choose file A (e.g. the reference render)", juce::File(),
                                                   engine_.formats().getWildcardForAllFormats());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [pickB](const juce::FileChooser& fc) {
        if (fc.getResult().existsAsFile()) pickB(fc.getResult());
    });
}

// ---------------------------------------------------------------- session lifecycle

void MainComponent::sessionChanged(bool undoable) {
    session_.pluginChainState = engine_.chain().getState();
    auto now = juce::JSON::toString(session_.toVar(), true);
    if (now == lastState_) { timeline_.repaint(); return; }
    if (undoable && lastState_.isNotEmpty()) {
        undo_.add(lastState_);
        if (undo_.size() > 300) undo_.remove(0);
        redo_.clear();
    }
    lastState_ = now;
    dirty_ = true;
    applyFollowRate();
    engine_.loadSession(session_);
    timeline_.sessionUpdated();
    pqList_.updateContent();
    pqList_.repaint();
    refreshIssues();
    if (xfadeEditor_) xfadeEditor_->repaint();
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
    xfadeWindow_.reset();
    applyFollowRate();
    engine_.loadSession(session_);
    timeline_.sessionUpdated();
    timeline_.resized();
    pqList_.updateContent();
    pqList_.repaint();
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
    timeline_.zoomToFit();
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
    for (auto& r : session_.regions)
        if (!r.file.existsAsFile()) missing.addIfNotAlreadyThere(r.file.getFullPathName());
    undo_.clear();
    redo_.clear();
    lastState_ = juce::JSON::toString(session_.toVar(), true);
    refreshAll();
    timeline_.zoomToFit();
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
                          [this](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadSessionFile(fc.getResult()); });
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
                          [doSave](const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) doSave(fc.getResult().withFileExtension("lathe")); });
    return true;
}

void MainComponent::writeAutosave(const juce::File& f) {
    session_.pluginChainState = engine_.chain().getState();
    juce::String err;
    auto keep = session_.file;
    session_.save(f, err);
    session_.file = keep;
}

// ---------------------------------------------------------------- editing

void MainComponent::importFiles(const juce::Array<juce::File>& files) {
    juce::StringArray bad;
    for (auto& f : files) {
        std::unique_ptr<juce::AudioFormatReader> r(engine_.formats().createReaderFor(f));
        if (!r) { bad.add(f.getFileName()); continue; }
        Region reg = session_.makeRegion(f, r->sampleRate, r->lengthInSamples);
        reg.lane = 0;
        int id = session_.append(reg, 2.0);
        // ISRC embedded by the mix engineer is carried onto the track marker.
        auto isrc = r->metadataValues.getValue("ISRC", r->metadataValues.getValue("isrc", ""));
        if (isrc.isNotEmpty())
            for (auto& m : session_.markers)
                if (m.attachedRegion == id && m.type == Marker::TrackStart) m.isrc = cd::normaliseIsrc(isrc.toStdString());
    }
    sessionChanged();
    timeline_.zoomToFit();
    if (!bad.isEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Some files couldn't be read", bad.joinIntoString("\n"));
}

void MainComponent::appendSource() {
    if (!source_.hasSource()) return;
    auto io = source_.inOut();
    Region reg = session_.makeRegion(source_.file(), source_.rate(), source_.lengthSamples());
    reg.srcIn = juce::int64(io.getStart() * source_.rate());
    reg.srcOut = juce::jmin(source_.lengthSamples(), juce::int64(io.getEnd() * source_.rate()));
    session_.append(reg, 2.0);
    sessionChanged();
}

void MainComponent::sourceEdit(Session::EditMode mode) {
    if (!source_.hasSource()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "No source", "Open a file in the Source window first.");
        return;
    }
    auto io = source_.inOut();
    auto sel = timeline_.selection();
    double dIn = sel.isEmpty() ? engine_.positionSeconds() : sel.getStart();
    double dOut = sel.isEmpty() ? -1.0 : sel.getEnd();
    if (mode == Session::EditMode::Replace && sel.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Replace needs a destination range",
                                               "Drag across the timeline (or use Shift+I / Shift+O) to mark the destination In and Out, "
                                               "and mark the source In and Out, then Replace.");
        return;
    }
    const double rate = source_.rate();
    auto sIn = juce::int64(io.getStart() * rate), sOut = juce::jmin(source_.lengthSamples(), juce::int64(io.getEnd() * rate));
    int id = session_.sourceEdit(source_.file(), rate, source_.lengthSamples(), sIn, sOut, timeline_.selectedLane(), dIn, dOut, mode);
    timeline_.selectRegion(id);
    timeline_.setSelection({});
    sessionChanged();
}

void MainComponent::addMarkerAtPlayhead(Marker::Type t) {
    int id = session_.addMarker(t, engine_.positionSeconds());
    timeline_.selectMarker(id);
    sessionChanged();
}

void MainComponent::splitAtPlayhead() {
    double t = engine_.positionSeconds();
    int sel = timeline_.selectedRegion();
    bool any = false;
    if (sel) any = session_.split(sel, t) != 0;
    if (!any) {
        juce::Array<int> ids;
        for (auto& r : session_.regions)
            if (t > r.start && t < r.end()) ids.add(r.id);
        for (int id : ids) any |= session_.split(id, t) != 0;
    }
    if (any) sessionChanged();
}

void MainComponent::deleteSelection(bool ripple) {
    if (int m = timeline_.selectedMarker(); m && session_.findMarker(m)) {
        session_.removeMarker(m);
        timeline_.selectMarker(0);
        sessionChanged();
        return;
    }
    if (int id = timeline_.selectedRegion()) {
        session_.deleteRegion(id, ripple);
        timeline_.selectRegion(0);
        sessionChanged();
    }
}

void MainComponent::nudgeSelectedRegion(double dt) {
    if (auto* r = session_.findRegion(timeline_.selectedRegion())) {
        session_.moveRegion(r->id, r->start + dt, r->lane);
        sessionChanged();
    }
}

void MainComponent::openCrossfade(int out, int in) {
    xfadeWindow_.reset();
    auto* ed = new CrossfadeEditor(session_, thumbs_, out, in);
    ed->onChange = [this] { sessionChanged(); };
    ed->onAudition = [this](double a, double b) { auditionRange(a, b); };
    xfadeEditor_ = ed;
    auto* w = new XfadeWindow(ed);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { xfadeWindow_.reset(); }); };
    xfadeWindow_.reset(w);
}

void MainComponent::auditionRange(double from, double to) {
    engine_.setPositionSeconds(juce::jmax(0.0, from));
    auditionEnd_ = to;
    engine_.play();
}

// ---------------------------------------------------------------- PQ list

int MainComponent::getNumRows() { return session_.markers.size(); }

int MainComponent::markerRowToTrackNumber(int row) const {
    int n = 0;
    for (int i = 0; i <= row && i < session_.markers.size(); ++i)
        if (session_.markers[i].type == Marker::TrackStart) ++n;
    return n;
}

void MainComponent::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
    bool track = row < session_.markers.size() && session_.markers[row].type == Marker::TrackStart;
    g.fillAll(selected ? colours::accent.withAlpha(0.25f) : track ? colours::panel2 : colours::panel);
}

void MainComponent::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) {
    g.setColour(col == pType || col == pTrack ? colours::dim : colours::text);
    g.setFont(13.0f);
    g.drawText(cellText(row, col), 4, 0, w - 8, h, juce::Justification::centredLeft);
}

juce::Component* MainComponent::refreshComponentForCell(int row, int col, bool, juce::Component* existing) {
    bool isTrack = row < session_.markers.size() && session_.markers[row].type == Marker::TrackStart;
    bool editable = col == pTime || (isTrack && (col == pTitle || col == pPerformer || col == pIsrc));
    if (!editable) { delete existing; return nullptr; }
    auto* c = dynamic_cast<Cell*>(existing);
    if (!c) { delete existing; c = new Cell(*this); }
    c->set(row, col);
    return c;
}

void MainComponent::cellClicked(int row, int, const juce::MouseEvent&) {
    if (row >= session_.markers.size()) return;
    timeline_.selectMarker(session_.markers[row].id);
    engine_.setPositionSeconds(session_.markers[row].t);
}

void MainComponent::cellDoubleClicked(int row, int col, const juce::MouseEvent&) {
    if (row >= session_.markers.size()) return;
    auto& m = session_.markers.getReference(row);
    if (m.type != Marker::TrackStart) return;
    if (col == pDcp) { m.copyPermitted = !m.copyPermitted; if (m.copyPermitted) m.scms = false; sessionChanged(); }
    if (col == pPre) { m.preEmphasis = !m.preEmphasis; sessionChanged(); }
}

void MainComponent::deleteKeyPressed(int row) {
    if (row < session_.markers.size()) { session_.removeMarker(session_.markers[row].id); sessionChanged(); }
}

juce::String MainComponent::cellText(int row, int col) const {
    if (row < 0 || row >= session_.markers.size()) return {};
    const auto& m = session_.markers.getReference(row);
    const bool track = m.type == Marker::TrackStart;
    switch (col) {
        case pTrack: return track ? juce::String(markerRowToTrackNumber(row)) : juce::String();
        case pType: return track ? "Track" : m.type == Marker::Index ? "Index" : m.type == Marker::PauseStart ? "Pause" : "End";
        case pTime: return msfString(m.t);
        case pLength: {
            if (!track) return {};
            double next = session_.end();
            for (int i = row + 1; i < session_.markers.size(); ++i)
                if (session_.markers[i].type == Marker::TrackStart) { next = session_.markers[i].t; break; }
            return msfString(next - m.t);
        }
        case pTitle: return track ? juce::String::fromUTF8(m.text.title.c_str()) : juce::String();
        case pPerformer: return track ? juce::String::fromUTF8(m.text.performer.c_str()) : juce::String();
        case pIsrc: return track ? juce::String(m.isrc) : juce::String();
        case pDcp: return track ? (m.copyPermitted ? "Yes" : "-") : juce::String();
        case pPre: return track ? (m.preEmphasis ? "Yes" : "-") : juce::String();
        default: return {};
    }
}

void MainComponent::setCellText(int row, int col, const juce::String& t) {
    if (row < 0 || row >= session_.markers.size()) return;
    auto& m = session_.markers.getReference(row);
    switch (col) {
        case pTime: session_.moveMarker(m.id, parseTimeString(t, m.t)); break;
        case pTitle: m.text.title = t.toStdString(); break;
        case pPerformer: m.text.performer = t.toStdString(); break;
        case pIsrc: m.isrc = cd::normaliseIsrc(t.toStdString()); break;
        default: return;
    }
    sessionChanged();
}

void MainComponent::refreshIssues() {
    if (session_.regions.isEmpty()) { issues_.setText("No audio yet."); return; }
    juce::StringArray adj;
    auto disc = session_.toDisc(&adj);
    auto issues = cd::validate(disc);
    auto cdt = cd::encodeCdText(disc);
    juce::String s;
    int errors = 0;
    for (auto& i : issues) {
        if (i.level == cd::Issue::Error) ++errors;
        s << (i.level == cd::Issue::Error ? "ERROR  " : "Warn   ") << juce::String::fromUTF8(i.message.c_str()) << "\n";
    }
    if (!cdt.error.empty()) { ++errors; s << "ERROR  " << juce::String::fromUTF8(cdt.error.c_str()) << "\n"; }
    for (auto& a : adj) s << "Note   " << a << "\n";
    juce::StringArray missing;
    for (auto& r : session_.regions)
        if (!r.file.existsAsFile()) missing.addIfNotAlreadyThere(r.file.getFileName());
    for (auto& m : missing) { ++errors; s << "ERROR  Missing file: " << m << "\n"; }
    juce::String head = juce::String(int(disc.tracks.size())) + " tracks, " + msfString(session_.end()) + ". ";
    head << (errors ? juce::String(errors) + " problem(s) to fix before DDP.\n" : "Ready for DDP.\n");
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
    o.dialogTitle = "Plugins (Options > Scan to find new ones)";
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
    switch (dither_.getSelectedId()) {
        case 2: es.shape = dsp::Ditherer::Shape::Light; break;
        case 3: es.shape = dsp::Ditherer::Shape::Strong; break;
        case 4: es.shape = dsp::Ditherer::Shape::Medium; break;
        case 5: es.shape = dsp::Ditherer::Shape::Psycho; break;
        default: es.shape = dsp::Ditherer::Shape::None; break;
    }
    return es;
}

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
                       [this, snapshot, err, f, bits, es](Job& j) { *err = exporter_.exportProgramWav(*snapshot, f, bits, es, [&](double p) { return j.keep(p); }); },
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
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Not ready", "Fix the errors listed in the issues panel first.");
        return;
    }
    chooser_ = std::make_unique<juce::FileChooser>("Choose where to create the DDP folder", juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
        auto parent = fc.getResult();
        if (!parent.isDirectory()) return;
        auto name = juce::File::createLegalFileName((session_.discTitle.isEmpty() ? juce::String("Lathe") : session_.discTitle) + " DDP");
        auto folder = parent.getNonexistentChildFile(name, "", false);
        auto snapshot = std::make_shared<Session>(session_);
        auto es = exportSettings();
        auto res = std::make_shared<ddp::WriteResult>();
        auto qc = std::make_shared<QcReport>();
        runJob("Writing DDP image...",
               [this, snapshot, folder, es, res, qc](Job& j) { *res = exporter_.exportDdp(*snapshot, folder, es, [&](double p) { return j.keep(p); }, *qc); },
               [this, res, qc, folder](bool) {
            if (!res->ok) {
                juce::String msg = juce::String::fromUTF8(res->error.c_str());
                for (auto& i : res->issues) msg << "\n" << juce::String::fromUTF8(i.message.c_str());
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "DDP export failed", msg);
                return;
            }
            auto check = std::make_shared<ddp::ReadResult>();
            runJob("Verifying DDP...",
                   [check, folder](Job& j) { *check = ddp::readDdp(folder.getFullPathName().toStdString(), [&](double p) { j.setProgress(p); }); },
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
    chooser_ = std::make_unique<juce::FileChooser>("Choose a folder for the track WAVs", juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
        auto folder = fc.getResult();
        if (!folder.isDirectory()) return;
        auto snapshot = std::make_shared<Session>(session_);
        auto es = exportSettings();
        auto err = std::make_shared<juce::String>();
        runJob("Exporting track WAVs...",
               [this, snapshot, folder, es, err](Job& j) { *err = exporter_.exportTrackWavs(*snapshot, folder, es, [&](double p) { return j.keep(p); }); },
               [err, folder](bool) {
                   if (err->isNotEmpty()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", *err);
                   else folder.revealToUser();
               });
    });
}

void MainComponent::analyse() {
    if (session_.regions.isEmpty()) return;
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
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
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

void MainComponent::filesDropped(const juce::StringArray& files, int x, int y) {
    juce::Array<juce::File> audio;
    for (auto& f : files) {
        juce::File file(f);
        if (file.hasFileExtension("lathe")) { loadSessionFile(file); return; }
        audio.add(file);
    }
    audio.sort();
    // Dropped on the source window: open the first file there. Anywhere else: import as tracks.
    if (source_.getBounds().contains(x, y) && !audio.isEmpty()) { source_.open(audio.getFirst()); return; }
    importFiles(audio);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* src) {
    if (src == commands_.getKeyMappings()) { saveKeyMappings(); return; }
    if (std::abs(engine_.deviceRate() - lastDeviceRate_) > 0.5) {
        lastDeviceRate_ = engine_.deviceRate();
        engine_.loadSession(session_);
    }
}

void MainComponent::timerCallback() {
    double pos = engine_.positionSeconds();
    if (auditionEnd_ >= 0 && engine_.isPlaying() && pos >= auditionEnd_) { engine_.stop(); auditionEnd_ = -1; }
    timeline_.setPlayhead(pos, engine_.isPlaying());
    if (xfadeEditor_) xfadeEditor_->setPlayhead(pos);
    source_.tick();
    timeDisplay_.setText(msfString(pos), juce::dontSendNotification);

    juce::String tr;
    auto ranges = session_.trackRanges();
    for (int i = 0; i < ranges.size(); ++i)
        if (pos >= ranges[i].getStart() && pos < ranges[i].getEnd()) { tr = "Track " + juce::String(i + 1) + "  " + msfString(pos - ranges[i].getStart()); break; }
    if (tr.isEmpty() && !ranges.isEmpty() && pos < ranges[0].getStart()) tr = "Pregap  -" + msfString(ranges[0].getStart() - pos);
    trackDisplay_.setText(tr, juce::dontSendNotification);

    auto sel = timeline_.selection();
    selDisplay_.setText(sel.isEmpty() ? juce::String() : "Dest In " + msfString(sel.getStart()) + "  Out " + msfString(sel.getEnd()) +
                                                              "  (" + msfString(sel.getLength()) + ")",
                        juce::dontSendNotification);
    playBtn_.setButtonText(engine_.isPlaying() ? "Stop" : "Play");
    meters_->prepare(engine_.deviceRate());
    meters_->feed(engine_.meterTap());

    double pr = session_.effectiveRate(), dr = engine_.deviceRate();
    juce::String rw;
    if (!session_.regions.isEmpty() && std::abs(pr - dr) > 0.5)
        rw = rateMessage_.isNotEmpty() ? rateMessage_
                                       : "Monitoring at " + juce::String(dr / 1000.0, 1) + " kHz (resampled); project " + juce::String(pr / 1000.0, 1) + " kHz";
    else if (!session_.regions.isEmpty())
        rw = "Playing at the project rate, " + juce::String(dr / 1000.0, 1) + " kHz (no resampling)";
    rateWarning_.setText(rw, juce::dontSendNotification);
    rateWarning_.setColour(juce::Label::textColourId, std::abs(pr - dr) > 0.5 ? colours::accent : colours::ok);
}

}  // namespace lathe
