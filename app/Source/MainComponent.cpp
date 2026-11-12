#include "MainComponent.h"

#include "../../core/cd/CdTextEncoder.h"
#include "NullTest.h"
#include "Theme.h"

namespace lathe {

enum PqCol { pTrack = 1, pType, pTime, pLength, pTitle, pPerformer, pIsrc, pDcp, pPre };

// ---------------------------------------------------------------- meters

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
    XfadeWindow(juce::Component* c) : DocumentWindow("Edit Fade", colours::bg, DocumentWindow::closeButton) {
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
    studio_.fromVar(juce::JSON::parse(props_.getUserSettings()->getValue("studioProfile")));

    getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId, colours::bg);
    meters_ = std::make_unique<MeterStrip>(meterHub_, &inputHub_);
    meters_->onOpenBridge = [this] { showMeterBridge(); };
    meters_->onReset = [this] { meterHub_.reset(); };
    meters_->onResetInput = [this] { inputHub_.reset(); engine_.resetInputOvers(); };
    addAndMakeVisible(*meters_);
    meterHub_.standard = props_.getUserSettings()->getIntValue("meterStandard", -1);
    srcSession_.lanes = 1;
    session_.source = &srcSession_;
    addAndMakeVisible(sourceView_);
    addAndMakeVisible(timeline_);
    timeline_.setActive(true);
    {
        auto* u = props_.getUserSettings();
        preRoll_ = u->getDoubleValue("preRoll", 3.0);
        postRoll_ = u->getDoubleValue("postRoll", 2.0);
        nudgeSizes_[0] = u->getDoubleValue("nudgeA", 1.0 / 75);
        nudgeSizes_[1] = u->getDoubleValue("nudgeB", 0.1);
        nudgeSizes_[2] = u->getDoubleValue("nudgeC", 1.0);
        nudgeSize_ = juce::jlimit(0, 2, u->getIntValue("nudgeSize", 0));
        editFollowsStop_ = u->getBoolValue("editPointFollowsStop", false);
        fadePrefs_.fromVar(juce::JSON::parse(u->getValue("fadeEditor")));
        if (auto* arr = juce::JSON::parse(u->getValue("fadeLibrary")).getArray())
            for (int k = 0; k < juce::jmin(10, arr->size()); ++k) fadeLib_[size_t(k)] = FadePreset::fromVar(arr->getReference(k));
        fadeLib_[0].used = true;
        if (fadeLib_[0].name.isEmpty()) fadeLib_[0].name = "Default crossfade";
    }

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
    envBtn_.onClick = [this] { timeline_.setEnvelopeMode(envBtn_.getToggleState()); commands_.commandStatusChanged(); };
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
    xfadeBtn_.onClick = [this] { openFadeNearEditPoint(); };
    xfadeBtn_.setTooltip("Edit Fade (X) for the edit, or the region fade-in / fade-out, nearest the Edit Point. Or double-click a join or a region's start or end.");
    trackMk_.onClick = [this] { addMarkerAtPlayhead(Marker::TrackStart); };
    indexMk_.onClick = [this] { addMarkerAtPlayhead(Marker::Index); };
    pauseMk_.onClick = [this] { addMarkerAtPlayhead(Marker::PauseStart); };
    splitBtn_.onClick = [this] { splitAtPlayhead(); };
    playBtn_.onClick = [this] { setActiveTimeline(false); togglePlay(); };
    homeBtn_.onClick = [this] { locateDest(0); meterHub_.reset(); };
    pluginsBtn_.onClick = [this] { showPluginMenu(); };
    exportBtn_.onClick = [this] { showExportMenu(); };
    analyseBtn_.onClick = [this] { runAnalysis(); };
    verifyBtn_.onClick = [this] { verifyDdp(); };
    audioBtn_.onClick = [this] { showAudioSettings(); };
    zoomIn_.onClick = [this] { timeline_.zoomBy(1.5, engine_.isPlaying() ? engine_.positionSeconds() : timeline_.editPoint()); };
    zoomOut_.onClick = [this] { timeline_.zoomBy(1 / 1.5, engine_.isPlaying() ? engine_.positionSeconds() : timeline_.editPoint()); };
    zoomFit_.onClick = [this] { timeline_.zoomToFit(); };

    chainAdd_.onClick = [this] { showPluginMenu(); };
    chainRemove_.onClick = [this] {
        int i = chainList_.getSelectedRow();
        if (i < 0) return;
        auto* gear = engine_.chain().builtinAt(i);
        const int prints = gear ? printCount(gear->anchorId()) : 0;
        if (gear) gearHear(gear->anchorId(), 0);
        engine_.chain().remove(i);
        chainList_.updateContent();
        sessionChanged();
        if (prints > 0)
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "External Gear removed",
                                                   "There " + juce::String(prints == 1 ? "is a print" : "are prints") +
                                                       " made through that insert. With it gone, they play through the whole master chain, including the "
                                                       "plugins that were above it (so those are applied twice). Undo brings the insert back.");
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

    sourceView_.onOpen = [this] { showSourceOpenDialog(); };
    sourceView_.onClear = [this] { commands_.invokeDirectly(cmd::clearSource, true); };
    sourceView_.onPlay = [this] {
        if (engine_.isSourcePlaying()) { stopAll(); return; }
        setActiveTimeline(true);
        togglePlay();
    };
    sourceView_.onMarkIn = [this] { setActiveTimeline(true); sourceView_.timeline().setInPoint(markTime()); };
    sourceView_.onMarkOut = [this] { setActiveTimeline(true); sourceView_.timeline().setOutPoint(markTime()); };
    sourceView_.onInsert = [this] { sourceEdit(EditKind::Insert); };
    sourceView_.onReplace = [this] { sourceEdit(EditKind::Replace); };
    sourceView_.onReplaceLeft = [this] { sourceEdit(EditKind::ReplaceLeft); };
    sourceView_.onConstrained = [this] { sourceEdit(EditKind::Constrained); };
    sourceView_.onFit = [this] { sourceEdit(EditKind::Fit); };
    sourceView_.onOverlay = [this] { sourceEdit(EditKind::Overlay); };
    sourceView_.onAppend = [this] { appendSource(); };
    sourceView_.onLinkChanged = [this] {
        if (!sourceView_.linked() && engine_.isLinkedPlaying()) stopAll();
    };
    {
        auto& st = sourceView_.timeline();
        st.onFocus = [this] { setActiveTimeline(true); };
        st.onEdit = [this] { sessionChanged(); };
        st.onSeek = [this](double t) { auditionEnd_ = -1; engine_.setSourcePosition(t); sourceView_.timeline().setPlayhead(t, false); };
        st.onSrpEdit = [this](int id) { editSrpText(true, id); };
        st.onSegmentGain = [this](int id) { setActiveTimeline(true); showSegmentGain({id}); };
        st.onLaneMenu = [this](int lane) { showLaneMenu(true, lane); };
        st.onCrossfade = [this](int a, int b) { setActiveTimeline(true); openCrossfade(a, b, true); };
    }
    timeline_.onFocus = [this] { setActiveTimeline(false); };
    timeline_.onSrpEdit = [this](int id) { editSrpText(false, id); };
    timeline_.onSegmentGain = [this](int id) { setActiveTimeline(false); showSegmentGain({id}); };
    timeline_.onLaneMenu = [this](int lane) { showLaneMenu(false, lane); };

    timeline_.onEdit = [this] { sessionChanged(); };
    timeline_.onSeek = [this](double t) { auditionEnd_ = -1; engine_.setPositionSeconds(t); timeline_.setPlayhead(t, false); };
    timeline_.onCrossfade = [this](int a, int b) { setActiveTimeline(false); openCrossfade(a, b); };
    timeline_.onBreakEdit = [this](int pid, int k) { openBreakEditor(pid, k); };
    timeline_.onProductsEdited = [this] { if (productsPanel_) productsPanel_->refresh(); };
    timeline_.onProductMenu = [this](int pid, double t, int k) { showProductStripMenu(pid, t, k); };
    timeline_.onRegionFx = [this](int id) { openRegionFx(id); };
    timeline_.onRegionRestore = [this](int id) { showRestore(id); };
    timeline_.onRegionSpectral = [this](int id) { showSpectral(id); };
    timeline_.onProblemClicked = [this](double t) { locateDest(juce::jmax(0.0, t - 1.0)); };
    recordBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff7a2622));
    recordBtn_.setColour(juce::TextButton::buttonOnColourId, colours::error);
    recordBtn_.setColour(juce::TextButton::textColourOffId, colours::text);
    recordBtn_.onClick = [this] { commands_.invokeDirectly(cmd::record, true); };
    addAndMakeVisible(recordBtn_);
    {
        // Audio Setup's settings. The first time, the recording format carries over from the Record window.
        const auto saved = props_.getUserSettings()->getValue("ioSettings");
        if (saved.isNotEmpty()) io_.fromVar(juce::JSON::parse(saved));
        else io_.bits = props_.getUserSettings()->getIntValue("recBits", 24) == 32 ? 32 : 24;
        applyIo();
    }
    {
        // What an External Gear insert's window asks for.
        auto& a = *engine_.gearActions;
        a.print = [this](int gear) { beginPrint(gear); };
        a.printStatus = [this](int gear) { return printStatus(gear); };
        a.printing = [this](int gear) { return print_.printing && print_.gear == gear; };
        a.printCount = [this](int gear) { return printCount(gear); };
        a.usingPrints = [this](int gear) {
            for (auto& r : session_.regions)
                if (r.afterGear == gear && !r.mute) return true;
            return false;
        };
        a.use = [this](int gear, bool usePrint) {
            gearHear(gear, 0);
            if (session_.usePrints(gear, usePrint) > 0) { sessionChanged(); timeline_.repaint(); }
        };
        a.hear = [this](int gear, int which) { gearHear(gear, which); };
        a.hearing = [this](int gear) { return gearAb_ == gear ? (engine_.chain().hearingPrint() ? 2 : 1) : 0; };
        a.changed = [this] { sessionChanged(); };
    }
    rec_.firstInput = props_.getUserSettings()->getIntValue("recInput", io_.defaultInput);
    rec_.bits = io_.bits;
    rec_.extraLatency = props_.getUserSettings()->getIntValue("recExtraLatency", 0);
    rec_.measured = props_.getUserSettings()->getIntValue("recMeasuredLatency", -1);
    rec_.print = props_.getUserSettings()->getBoolValue("recPrint", true);
    rec_.muteSources = props_.getUserSettings()->getBoolValue("recMute", true);
    abDisplay_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    abDisplay_.setColour(juce::Label::textColourId, colours::accent);
    addAndMakeVisible(abDisplay_);
    timeline_.onMarkerSelected = [this](int id) {
        for (int i = 0; i < session_.markers.size(); ++i)
            if (session_.markers[i].id == id) { pqList_.selectRow(i); break; }
    };
    timeline_.onMarkerDoubleClicked = [this](int id) {
        if (auto* m = session_.findMarker(id)) locateDest(m->t);
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
    addChildComponent(compareBar_);
    compareBar_.onSwitch = [this] { toggleCompareUnprocessed(); };
    compareBar_.onToggleMatch = [this] { setLevelMatch(!engine_.levelMatch()); };
    compareBar_.onEnd = [this] { endCompare(); };

    issues_.setMultiLine(true);
    issues_.setReadOnly(true);
    issues_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
    issues_.setColour(juce::TextEditor::textColourId, colours::text);
    issues_.setColour(juce::TextEditor::outlineColourId, colours::line);
    addAndMakeVisible(issues_);
    addAndMakeVisible(tcStrip_);
    tcStrip_.setUnits(TimecodeStrip::Units(juce::jlimit(0, TimecodeStrip::kNumUnits - 1, props_.getUserSettings()->getIntValue("timecodeUnits", 0))));
    tcStrip_.onUnitsChanged = [this](TimecodeStrip::Units u) { props_.getUserSettings()->setValue("timecodeUnits", int(u)); };
    tcStrip_.onSetMain = [this](double t) {
        if (takeInProgress()) return;
        if (sourceActive_) {
            sourceView_.timeline().setEditPoint(t, true);
            engine_.setSourcePosition(t);
            sourceView_.timeline().setPlayhead(t, false);
        } else {
            locateDest(t);
        }
    };
    tcStrip_.onSetIn = [this](double t) { activeTimeline().setInPoint(t); };
    tcStrip_.onSetOut = [this](double t) { activeTimeline().setOutPoint(t); };
    tcStrip_.onSetDuration = [this](double d) {
        auto& tl = activeTimeline();
        if (tl.inPoint() >= 0 && d > 0) tl.setOutPoint(tl.inPoint() + d);
    };
    timeDisplay_.setVisible(false);   // (the timecode strip shows it now)
    selDisplay_.setVisible(false);
    issuesLbl_.setText("PROBLEMS & NOTES", juce::dontSendNotification);
    issuesLbl_.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    issuesLbl_.setColour(juce::Label::textColourId, colours::dim);
    issuesLbl_.setBorderSize({0, 2, 0, 2});
    addAndMakeVisible(issuesLbl_);

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
    engine_.chain().windowKeys = commands_.getKeyMappings();
    setApplicationCommandManagerToWatch(&commands_);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#endif
    surface_ = std::make_unique<ControlSurface>(commands_, *props_.getUserSettings());
    surface_->position = [this] { return engine_.positionSeconds(); };
    surface_->isPlaying = [this] { return engine_.isPlaying(); };
    surface_->isRecording = [this] { return engine_.isRecording(); };
    surface_->jog = [this](double dt) {
        if (engine_.isPlaying()) engine_.setPositionSeconds(juce::jmax(0.0, engine_.positionSeconds() + dt));
        else locateDest(juce::jmax(0.0, timeline_.editPoint() + dt));
    };
    setWantsKeyboardFocus(true);
    setSize(1440, 900);
    newSession();
    startTimerHz(30);
}

MainComponent::~MainComponent() {
    surfaceWindow_.reset();
    surface_.reset();
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    commands_.getKeyMappings()->removeChangeListener(this);
    removeKeyListener(commands_.getKeyMappings());
    engine_.chain().closeAllEditors();
    engine_.chain().windowKeys = nullptr;
    xfadeWindow_.reset();
    engine_.devices().removeChangeListener(this);
    engine_.chain().closeAllEditors();
    closeList(pqList_);
    closeList(chainList_);
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
    meters_->setBounds(right.removeFromTop(176));
    right.removeFromTop(8);
    // Top to bottom: meters, (compare banner), the plugin list with its buttons right under it,
    // then the problems and notes.
    issues_.setBounds(right.removeFromBottom(160));
    issuesLbl_.setBounds(right.removeFromBottom(18));
    right.removeFromBottom(6);
    auto chainButtons = right.removeFromBottom(26);
    for (auto* b : {&chainAdd_, &chainRemove_, &chainBypass_, &chainUp_, &chainDown_}) {
        b->setBounds(chainButtons.removeFromLeft(56));
        chainButtons.removeFromLeft(4);
    }
    right.removeFromBottom(4);
    if (compare_) {
        compareBar_.setBounds(right.removeFromTop(62));
        right.removeFromTop(4);
    }
    chainList_.setBounds(right);

    tcStrip_.setBounds(r.removeFromTop(64));
    r.removeFromTop(6);
    sourceView_.setBounds(r.removeFromTop(juce::jlimit(150, 260, r.getHeight() / 4)));
    r.removeFromTop(6);

    auto transport = r.removeFromTop(34);
    homeBtn_.setBounds(transport.removeFromLeft(40).reduced(0, 3));
    transport.removeFromLeft(4);
    playBtn_.setBounds(transport.removeFromLeft(70).reduced(0, 3));
    transport.removeFromLeft(4);
    recordBtn_.setBounds(transport.removeFromLeft(50).reduced(0, 3));
    transport.removeFromLeft(10);
    timeDisplay_.setBounds({});
    // Buttons first; the track readout takes what is left.
    for (auto* b : {&zoomFit_, &zoomIn_, &zoomOut_}) { b->setBounds(transport.removeFromRight(b == &zoomFit_ ? 40 : 28).reduced(0, 3)); transport.removeFromRight(3); }
    transport.removeFromRight(6);
    for (auto* b : {&xfadeBtn_, &lanesBtn_, &snapBtn_, &envBtn_, &splitBtn_, &pauseMk_, &indexMk_, &trackMk_}) {
        b->setBounds(transport.removeFromRight(b == &xfadeBtn_ ? 80 : 60).reduced(0, 3));
        transport.removeFromRight(3);
    }
    trackDisplay_.setBounds(transport.removeFromLeft(juce::jmin(170, transport.getWidth())));
    abDisplay_.setBounds(transport.removeFromLeft(juce::jmin(260, transport.getWidth() / 2)));
    selDisplay_.setBounds({});
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
        case cmd::repeatPlay: info.setTicked(repeat_); break;
        case cmd::armInputs: info.setTicked(armInputs_); break;
        case cmd::renameRegion: info.setActive(activeTimeline().selectedRegion() != 0); break;
        case cmd::lockAllMarks: info.setTicked(session_.lockMarks); break;
        case cmd::gainOverlay: info.setTicked(activeTimeline().gainOverlay()); break;
        case cmd::gainBypass: info.setTicked(activeSession().gainBypass); break;
        case cmd::lockAllGain: info.setTicked(session_.lockGain); break;
        case cmd::defaultFadeOnEdits: info.setTicked(fadePrefs_.defaultOnEdits); break;
        case cmd::fadeRecall0: case cmd::fadeRecall1: case cmd::fadeRecall2: case cmd::fadeRecall3: case cmd::fadeRecall4:
        case cmd::fadeRecall5: case cmd::fadeRecall6: case cmd::fadeRecall7: case cmd::fadeRecall8: case cmd::fadeRecall9: {
            const auto& p = fadeLib_[size_t(id - cmd::fadeRecall0)];
            info.setActive(p.used);
            if (p.used && p.name.isNotEmpty()) info.shortName << ": " << p.name;
            break;
        }
        case cmd::keyPresetLathe: info.setTicked(props_.getUserSettings()->getIntValue("keyPreset", 0) == 0); break;
        case cmd::keyPresetLegacy: info.setTicked(props_.getUserSettings()->getIntValue("keyPreset", 0) == 1); break;
        case cmd::linkPlay: info.setTicked(sourceView_.linked()); break;
        case cmd::editPointFollowsStop: info.setTicked(editFollowsStop_); break;
        case cmd::nudgeSizeA: case cmd::nudgeSizeB: case cmd::nudgeSizeC: {
            const int k = int(id - cmd::nudgeSizeA);
            info.setTicked(nudgeSize_ == k);
            const double v = nudgeSizes_[k];
            info.shortName << "  (" << (std::abs(v * 75 - std::round(v * 75)) < 1e-6 && v < 1 ? juce::String(juce::roundToInt(v * 75)) + " frame" + (juce::roundToInt(v * 75) == 1 ? "" : "s")
                                                                                              : juce::String(v * 1000, 0) + " ms") << ")";
            break;
        }
        case cmd::pasteInsert: case cmd::pasteReplace: case cmd::pasteReplaceLeft: case cmd::pasteConstrained: case cmd::pasteOverlay:
            info.setActive(!clip_.isEmpty());
            break;
        case cmd::muteRegion:
        case cmd::nudgeLeft: case cmd::nudgeRight: case cmd::nudgeLeftBig: case cmd::nudgeRightBig:
        case cmd::selectRegionRange:
        case cmd::regionEffects: info.setActive(timeline_.selectedRegion() != 0); break;
        case cmd::bypassRegionFx: info.setTicked(engine_.regionFx().bypassAll()); break;
        case cmd::listenReference: info.setTicked(engine_.listeningToReference()); info.setActive(engine_.hasReference()); break;
        case cmd::compareUnprocessed: info.setTicked(compare_); break;
        case cmd::compareLevelMatch: info.setTicked(engine_.levelMatch()); info.setActive(compare_); break;
        case cmd::compareEnd: info.setActive(compare_); break;
        case cmd::toggleLoudness: info.setTicked(timeline_.showLoudness()); break;
        case cmd::showAnalysis: info.setActive(analysis_ != nullptr); break;
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
        case cmd::openSource: showSourceOpenDialog(); break;
        case cmd::exportDdp: exportDdp(); break;
        case cmd::exportTrackWavs: exportTrackWavs(); break;
        case cmd::exportProgram24:
        case cmd::exportProgram32: showExportMenu(); break;
        case cmd::exportInOut: exportInOut(); break;
        case cmd::exportTracksAs: deliver("every track", trackItems(session_), false); break;
        case cmd::exportProgrammeAs: deliver("the whole programme", {programmeItem(session_)}, false); break;
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
        case cmd::regionEffects: openRegionFx(timeline_.selectedRegion()); break;
        case cmd::bypassRegionFx:
            engine_.regionFx().setBypassAll(!engine_.regionFx().bypassAll());
            engine_.loadSession(session_);   // re-time the regions (latency changes when bypassed)
            break;
        case cmd::batchRender: showBatchDialog(); break;
        case cmd::locateMissingAudio: locateMissingAudio(true); break;
        case cmd::sourceIn: activeTimeline().setInPoint(markTime()); break;
        case cmd::sourceOut: activeTimeline().setOutPoint(markTime()); break;
        case cmd::destIn: timeline_.setInPoint(engine_.isPlaying() ? pos : timeline_.editPoint()); break;
        case cmd::destOut: timeline_.setOutPoint(engine_.isPlaying() ? pos : timeline_.editPoint()); break;
        case cmd::goToIn: if (activeTimeline().inPoint() >= 0) locate(activeTimeline().inPoint()); break;
        case cmd::goToOut: if (activeTimeline().outPoint() >= 0) locate(activeTimeline().outPoint()); break;
        case cmd::clearIn: { auto& t = activeTimeline(); const double o = t.outPoint(); t.clearInOut(); if (o >= 0) t.setOutPoint(o); break; }
        case cmd::clearOut: { auto& t = activeTimeline(); const double i = t.inPoint(); t.clearInOut(); if (i >= 0) t.setInPoint(i); break; }
        case cmd::moveInToOut: {
            auto& t = activeTimeline();
            if (t.inPoint() >= 0 && t.outPoint() > t.inPoint()) {
                const double len = t.outPoint() - t.inPoint(), o = t.outPoint();
                t.setSelection({o, o + len});
                t.reveal(o + len);
            }
            break;
        }
        case cmd::nudgeInLeft: nudgePoint(true, -1); break;
        case cmd::nudgeInRight: nudgePoint(true, 1); break;
        case cmd::nudgeOutLeft: nudgePoint(false, -1); break;
        case cmd::nudgeOutRight: nudgePoint(false, 1); break;
        case cmd::nudgeSizeA: case cmd::nudgeSizeB: case cmd::nudgeSizeC:
            nudgeSize_ = int(info.commandID - cmd::nudgeSizeA);
            props_.getUserSettings()->setValue("nudgeSize", nudgeSize_);
            commands_.commandStatusChanged();
            break;
        case cmd::movePoint: showMovePoint(0); break;
        case cmd::editPointToPlayhead: {
            const double t = activePlaying() ? activePosition() : (sourceActive_ ? lastStopSource_ : lastStopDest_);
            activeTimeline().setEditPoint(t, true);
            break;
        }
        case cmd::insertEdit: sourceEdit(EditKind::Insert); break;
        case cmd::overwriteEdit: sourceEdit(EditKind::Replace); break;
        case cmd::replaceLeftEdit: sourceEdit(EditKind::ReplaceLeft); break;
        case cmd::constrainedEdit: sourceEdit(EditKind::Constrained); break;
        case cmd::replaceEdit: sourceEdit(EditKind::Fit); break;
        case cmd::overlayEdit: sourceEdit(EditKind::Overlay); break;
        case cmd::appendEdit: appendSource(); break;
        case cmd::playSourceInOut: {
            auto r = sourceView_.timeline().selection();
            setActiveTimeline(true);
            if (!r.isEmpty()) playRange(true, r.getStart(), r.getEnd());
            break;
        }
        case cmd::switchTimeline: setActiveTimeline(!sourceActive_); break;
        case cmd::linkPlay:
            sourceView_.setLinked(!sourceView_.linked());
            sourceView_.onLinkChanged();
            commands_.commandStatusChanged();
            break;
        case cmd::clearSource:
            if (engine_.isSourcePlaying()) stopAll();
            srcSession_.regions.clear();
            srcSession_.srps.clear();
            srcSession_.markers.clear();
            sourceView_.timeline().clearInOut();
            sourceView_.timeline().setEditPoint(0);
            sessionChanged();
            break;
        case cmd::cutRange: copyActive(true); break;
        case cmd::copyRange: copyActive(false); break;
        case cmd::pasteInsert: pasteActive(PasteMode::Insert); break;
        case cmd::pasteReplace: pasteActive(PasteMode::Replace); break;
        case cmd::pasteReplaceLeft: pasteActive(PasteMode::ReplaceLeft); break;
        case cmd::pasteConstrained: pasteActive(PasteMode::Constrained); break;
        case cmd::pasteOverlay: pasteActive(PasteMode::Overlay); break;
        case cmd::eraseRange: eraseActive(); break;
        case cmd::playStop: togglePlay(); break;
        case cmd::playSource:
            if (engine_.isSourcePlaying() && !engine_.isPlaying()) { stopAll(); break; }
            setActiveTimeline(true);
            if (!activePlaying()) togglePlay();
            break;
        case cmd::returnToStart: if (sourceActive_) locate(0); else homeBtn_.onClick(); break;
        case cmd::locateBackFrame: locate(juce::jmax(0.0, markTime() - 1.0 / 75)); break;
        case cmd::locateFwdFrame: locate(markTime() + 1.0 / 75); break;
        case cmd::locateBackSecond: locate(juce::jmax(0.0, markTime() - 1.0)); break;
        case cmd::locateFwdSecond: locate(markTime() + 1.0); break;
        case cmd::prevMarker: jumpToMarker(-1); break;
        case cmd::nextMarker: jumpToMarker(1); break;
        case cmd::playSelection: {
            auto s = activeTimeline().selection();
            if (!s.isEmpty()) playRange(sourceActive_, s.getStart(), s.getEnd());
            break;
        }
        case cmd::playToIn: playRelative(0); break;
        case cmd::playFromIn: playRelative(1); break;
        case cmd::playToOut: playRelative(2); break;
        case cmd::playFromOut: playRelative(3); break;
        case cmd::playAroundEdit: playRelative(4); break;
        case cmd::repeatPlay: repeat_ = !repeat_; commands_.commandStatusChanged(); break;
        case cmd::editPointFollowsStop:
            editFollowsStop_ = !editFollowsStop_;
            props_.getUserSettings()->setValue("editPointFollowsStop", editFollowsStop_);
            commands_.commandStatusChanged();
            break;
        case cmd::playbackSettings: showPlaybackSettings(); break;
        case cmd::markInfo: showMarkInfo(); break;
        case cmd::meterBridge: showMeterBridge(); break;
        case cmd::resetMeters: meterHub_.reset(); break;
        case cmd::exportSegments: exportSelectedSegments(); break;
        case cmd::gainOverlay: {
            const bool on = !activeTimeline().gainOverlay();
            activeTimeline().setGainOverlay(on);
            if (on) envBtn_.setToggleState(false, juce::dontSendNotification);
            commands_.commandStatusChanged();
            break;
        }
        case cmd::gainBypass: activeSession().gainBypass = !activeSession().gainBypass; sessionChanged(); commands_.commandStatusChanged(); break;
        case cmd::lockAllGain:
            session_.lockGain = srcSession_.lockGain = !session_.lockGain;
            props_.getUserSettings()->setValue("lockGain", session_.lockGain);   // a global setting: every session
            sessionChanged();
            commands_.commandStatusChanged();
            break;
        case cmd::clearGain: {
            auto& ss = activeSession();
            for (int i = ss.gainNodes.size(); --i >= 0;)
                if (!ss.gainLocked(ss.gainNodes.getReference(i))) ss.gainNodes.remove(i);
            sessionChanged();
            break;
        }
        case cmd::textView: showTextView(); break;
        case cmd::segmentGain: {
            juce::Array<int> ids;
            if (textViewPanel_ && textViewWindow_ && textViewWindow_->isActiveWindow()) ids = textViewPanel_->selectedIds();
            if (ids.isEmpty() && activeSession().findRegion(activeTimeline().selectedRegion())) ids.add(activeTimeline().selectedRegion());
            if (!ids.isEmpty()) showSegmentGain(ids);
            break;
        }
        case cmd::segmentsToMarks: if (session_.segmentsToStartMarks(0) > 0) sessionChanged(); break;
        case cmd::editedBlackToMarks: if (session_.editedBlackToMarks() > 0) sessionChanged(); break;
        case cmd::analogBlackToMarks: runAnalogBlack(); break;
        case cmd::autoSpace: session_.autoSpace(session_.autoSpaceGap); sessionChanged(); break;
        case cmd::lockAllMarks:
            session_.lockMarks = !session_.lockMarks;
            props_.getUserSettings()->setValue("lockMarks", session_.lockMarks);   // a global setting: every session
            sessionChanged();
            commands_.commandStatusChanged();
            break;
        case cmd::fadeRecall0: case cmd::fadeRecall1: case cmd::fadeRecall2: case cmd::fadeRecall3: case cmd::fadeRecall4:
        case cmd::fadeRecall5: case cmd::fadeRecall6: case cmd::fadeRecall7: case cmd::fadeRecall8: case cmd::fadeRecall9: {
            const int slot = int(info.commandID - cmd::fadeRecall0);
            if (!fadeLib_[size_t(slot)].used) break;
            if (xfadeEditor_ && xfadeWindow_) { xfadeEditor_->recall(slot); break; }
            auto& tl = activeTimeline();
            auto p = tl.fadeNear(tl.editPoint());
            if (!p.first && !p.second) break;
            if (p.first && p.second) applyFadePreset(activeSession(), p.first, p.second, fadeLib_[size_t(slot)], fadePrefs_.lockSound, fadePrefs_.ripple);
            else applySingleFadePreset(activeSession(), p.first ? p.first : p.second, p.second != 0, fadeLib_[size_t(slot)]);
            sessionChanged();
            break;
        }
        case cmd::defaultFadeOnEdits:
            fadePrefs_.defaultOnEdits = !fadePrefs_.defaultOnEdits;
            saveFadeSettings();
            commands_.commandStatusChanged();
            break;
        case cmd::playFromPlayhead: {
            if (engine_.isPlaying() || engine_.isSourcePlaying()) { stopAll(); break; }
            const double from = sourceActive_ ? lastStopSource_ : lastStopDest_;
            if (sourceActive_) { engine_.setSourcePosition(from); engine_.playSource(); }
            else { engine_.setPositionSeconds(from); engine_.play(); }
            break;
        }
        case cmd::playRepeatInOut: {
            auto r = activeTimeline().selection();
            if (r.isEmpty()) break;
            repeat_ = true;
            commands_.commandStatusChanged();
            playRange(sourceActive_, r.getStart(), r.getEnd());
            break;
        }
        case cmd::movePointIn: showMovePoint(1); break;
        case cmd::movePointOut: showMovePoint(2); break;
        case cmd::nextEdge: case cmd::prevEdge: {
            const double p = markTime();
            const bool fwd = info.commandID == cmd::nextEdge;
            double best = fwd ? 1e12 : -1.0;
            for (auto& r : activeSession().regions)
                for (double e : {r.start, r.end()}) {
                    if (fwd && e > p + 0.001 && e < best) best = e;
                    if (!fwd && e < p - 0.001 && e > best) best = e;
                }
            if (best >= 0 && best < 1e11) locate(best);
            break;
        }
        case cmd::scrollLeft: activeTimeline().scrollBy(-0.25); break;
        case cmd::scrollRight: activeTimeline().scrollBy(0.25); break;
        case cmd::zoomSelection: {
            auto& t = activeTimeline();
            auto r = t.selection();
            if (r.isEmpty())
                if (auto* reg = activeSession().findRegion(t.selectedRegion())) r = {reg->start, reg->end()};
            if (!r.isEmpty()) t.zoomToRange(r);
            break;
        }
        case cmd::keyPresetLathe: case cmd::keyPresetLegacy: {
            const int preset = info.commandID == cmd::keyPresetLegacy ? 1 : 0;
            applyKeyPreset(*commands_.getKeyMappings(), preset);
            props_.getUserSettings()->setValue("keyPreset", preset);
            saveKeyMappings();
            commands_.commandStatusChanged();
            break;
        }
        case cmd::addSrp: addSrp(false); break;
        case cmd::addSrpText: addSrp(true); break;
        case cmd::srpsFromSelection: {
            auto& t = activeTimeline();
            auto& ss = activeSession();
            auto r = t.selection();
            if (r.isEmpty())
                if (auto* reg = ss.findRegion(t.selectedRegion())) r = {reg->start, reg->end()};
            if (r.isEmpty()) break;
            ss.addSrp(r.getStart(), "In");
            ss.addSrp(r.getEnd(), "Out");
            sessionChanged();
            break;
        }
        case cmd::prevSrp: jumpToSrp(-1); break;
        case cmd::nextSrp: jumpToSrp(1); break;
        case cmd::lockSrp:
            if (auto* p = activeSession().findSrp(activeTimeline().selectedSrp())) { p->locked = !p->locked; sessionChanged(); }
            break;
        case cmd::deleteSrp:
            if (activeSession().findSrp(activeTimeline().selectedSrp())) {
                activeSession().removeSrp(activeTimeline().selectedSrp());
                activeTimeline().selectSrp(0);
                sessionChanged();
            }
            break;
        case cmd::deleteAllSrps:
            if (!activeSession().srps.isEmpty()) { activeSession().srps.clear(); sessionChanged(); }
            break;
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
        case cmd::analyse: runAnalysis(); break;
        case cmd::showAnalysis: showAnalysisWindow(); break;
        case cmd::ddpPlayer: openDdpPlayer(); break;
        case cmd::controlSurfaces: showControlSurfaces(); break;
        case cmd::restoreRegion: showRestore(targetRegion()); break;
        case cmd::spectralEditor: showSpectral(targetRegion()); break;
        case cmd::record:
            if (print_.printing) finishPrint();
            else if (cap_.capturing) finishCapture();
            else if (engine_.isRecording()) finishRecording();
            else showRecordDialog();
            break;
        case cmd::capture: showCapture(); break;
        case cmd::renameRegion: renameRegion(); break;
        case cmd::armInputs:
            armInputs_ = !armInputs_;
            commands_.commandStatusChanged();
            break;
        case cmd::openDdpSession: {
            chooser_ = std::make_unique<juce::FileChooser>("Open a DDP folder as a session");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this](const juce::FileChooser& fc) { if (fc.getResult().isDirectory()) openDdpAsSession(fc.getResult()); });
            break;
        }
        case cmd::references: showReferences(); break;
        case cmd::listenReference:
            if (engine_.hasReference()) engine_.setListenToReference(!engine_.listeningToReference());
            else showReferences();
            break;
        case cmd::compareUnprocessed: toggleCompareUnprocessed(); break;
        case cmd::compareLevelMatch: if (compare_) setLevelMatch(!engine_.levelMatch()); break;
        case cmd::compareEnd: endCompare(); break;
        case cmd::toggleLoudness: timeline_.setShowLoudness(!timeline_.showLoudness()); break;
        case cmd::verifyDdp: verifyDdp(); break;
        case cmd::nullTest: runNullTest(); break;
        case cmd::pluginManager: showPluginManager(); break;
        case cmd::audioSetup: showAudioSettings(); break;
        case cmd::followProjectRate:
            followRate_ = !followRate_;
            props_.getUserSettings()->setValue("followProjectRate", followRate_);
            applyFollowRate();
            break;
        case cmd::keyGuide: showKeyGuide(); break;
        case cmd::editKeyCommands: showKeyEditor(); break;
        case cmd::products: showProducts(); break;
        case cmd::breakEditor: breakNearPlayhead(); break;
        case cmd::releaseInfo: showReleaseInfo(); break;
        case cmd::trackCredits: showTrackCredits(); break;
        case cmd::studioProfile: showStudioProfile(); break;
        case cmd::exportPaperwork: exportPaperworkOnly(); break;
        default: return false;
    }
    commands_.commandStatusChanged();
    return true;
}

juce::StringArray MainComponent::getMenuBarNames() { return {"File", "Edit", "Transport", "Markers", "View", "Delivery", "Tools", "Help"}; }

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&) {
    juce::PopupMenu m;
    auto add = [&](std::initializer_list<juce::CommandID> ids) { for (auto id : ids) m.addCommandItem(&commands_, id); };
    switch (index) {
        case 0:
            add({cmd::newSession, cmd::openSession, cmd::saveSession, cmd::saveSessionAs});
            m.addSeparator();
            add({cmd::importAudio, cmd::openSource, cmd::openDdpSession, cmd::locateMissingAudio});
            m.addSeparator();
            add({cmd::batchRender});
            m.addSeparator();
            add({cmd::exportDdp, cmd::exportTrackWavs, cmd::exportProgram24, cmd::exportTracksAs, cmd::exportProgrammeAs, cmd::exportInOut, cmd::exportSegments});
            break;
        case 1:
            add({cmd::undo, cmd::redo});
            m.addSeparator();
            add({cmd::cutRange, cmd::copyRange, cmd::pasteInsert, cmd::pasteReplace, cmd::pasteReplaceLeft, cmd::pasteConstrained, cmd::pasteOverlay,
                 cmd::eraseRange});
            m.addSeparator();
            {
                juce::PopupMenu sd;
                auto addTo = [&](juce::PopupMenu& mm, std::initializer_list<juce::CommandID> ids) { for (auto id : ids) mm.addCommandItem(&commands_, id); };
                addTo(sd, {cmd::switchTimeline, cmd::linkPlay});
                sd.addSeparator();
                addTo(sd, {cmd::sourceIn, cmd::sourceOut, cmd::destIn, cmd::destOut, cmd::goToIn, cmd::goToOut, cmd::clearIn, cmd::clearOut, cmd::moveInToOut});
                sd.addSeparator();
                addTo(sd, {cmd::nudgeInLeft, cmd::nudgeInRight, cmd::nudgeOutLeft, cmd::nudgeOutRight, cmd::nudgeSizeA, cmd::nudgeSizeB, cmd::nudgeSizeC,
                           cmd::movePoint, cmd::movePointIn, cmd::movePointOut, cmd::editPointToPlayhead});
                sd.addSeparator();
                addTo(sd, {cmd::insertEdit, cmd::overwriteEdit, cmd::replaceLeftEdit, cmd::constrainedEdit, cmd::replaceEdit, cmd::overlayEdit, cmd::appendEdit});
                sd.addSeparator();
                addTo(sd, {cmd::openSource, cmd::clearSource});
                m.addSubMenu("Source / Destination", sd);
            }
            m.addSeparator();
            add({cmd::splitAtPlayhead, cmd::deleteClose, cmd::deleteLeave, cmd::muteRegion, cmd::renameRegion, cmd::selectRegionRange, cmd::clearSelection});
            m.addSeparator();
            add({cmd::textView, cmd::segmentGain});
            m.addSeparator();
            {
                juce::PopupMenu ef;
                ef.addCommandItem(&commands_, cmd::crossfadeEditor);
                ef.addCommandItem(&commands_, cmd::defaultFadeOnEdits);
                ef.addSeparator();
                for (int k = 0; k < 10; ++k) ef.addCommandItem(&commands_, cmd::fadeRecall0 + k);
                m.addSubMenu("Edit Fade", ef);
                m.addSeparator();
            }
            add({cmd::regionEffects, cmd::bypassRegionFx});
            m.addSeparator();
            add({cmd::restoreRegion, cmd::spectralEditor});
            m.addSeparator();
            add({cmd::nudgeLeft, cmd::nudgeRight, cmd::nudgeLeftBig, cmd::nudgeRightBig});
            break;
        case 2:
            add({cmd::playStop, cmd::playSource, cmd::playSourceInOut, cmd::playSelection, cmd::returnToStart});
            m.addSeparator();
            add({cmd::playToIn, cmd::playFromIn, cmd::playToOut, cmd::playFromOut, cmd::playAroundEdit, cmd::repeatPlay});
            m.addSeparator();
            add({cmd::editPointFollowsStop, cmd::playbackSettings});
            m.addSeparator();
            add({cmd::listenReference, cmd::compareUnprocessed, cmd::compareLevelMatch, cmd::compareEnd});
            m.addSeparator();
            add({cmd::record, cmd::capture, cmd::armInputs});
            m.addSeparator();
            add({cmd::playFromPlayhead, cmd::playRepeatInOut});
            m.addSeparator();
            add({cmd::locateBackFrame, cmd::locateFwdFrame, cmd::locateBackSecond, cmd::locateFwdSecond, cmd::prevMarker, cmd::nextMarker,
                 cmd::prevEdge, cmd::nextEdge});
            break;
        case 3:
            add({cmd::addTrackMarker, cmd::addIndexMarker, cmd::addPauseMarker, cmd::addEndMarker});
            m.addSeparator();
            add({cmd::markInfo, cmd::lockAllMarks});
            m.addSeparator();
            add({cmd::segmentsToMarks, cmd::editedBlackToMarks, cmd::analogBlackToMarks, cmd::autoSpace});
            m.addSeparator();
            add({cmd::addSrp, cmd::addSrpText, cmd::srpsFromSelection, cmd::prevSrp, cmd::nextSrp, cmd::lockSrp, cmd::deleteSrp, cmd::deleteAllSrps});
            break;
        case 4:
            add({cmd::zoomIn, cmd::zoomOut, cmd::zoomFit, cmd::zoomSelection, cmd::scrollLeft, cmd::scrollRight});
            m.addSeparator();
            add({cmd::toggleEnvelope, cmd::toggleSnap, cmd::crossfadeEditor, cmd::toggleLoudness});
            m.addSeparator();
            add({cmd::gainOverlay, cmd::gainBypass, cmd::lockAllGain, cmd::clearGain});
            break;
        case 5:
            add({cmd::releaseInfo, cmd::trackCredits});
            m.addSeparator();
            add({cmd::products, cmd::breakEditor});
            m.addSeparator();
            add({cmd::batchRender, cmd::exportPaperwork});
            m.addSeparator();
            add({cmd::studioProfile});
            break;
        case 6:
            add({cmd::meterBridge, cmd::resetMeters});
            m.addSeparator();
            add({cmd::analyse, cmd::showAnalysis, cmd::references});
            m.addSeparator();
            add({cmd::ddpPlayer, cmd::verifyDdp, cmd::nullTest});
            m.addSeparator();
            add({cmd::pluginManager, cmd::audioSetup, cmd::controlSurfaces, cmd::followProjectRate});
            break;
        case 7:
            add({cmd::keyGuide, cmd::editKeyCommands});
            m.addSeparator();
            add({cmd::keyPresetLathe, cmd::keyPresetLegacy});
            break;
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
    // The standard editor plus a preset chooser above it.
    struct KeyEditor : juce::Component {
        juce::KeyMappingEditorComponent ed;
        juce::Label lbl;
        juce::ComboBox preset;
        KeyEditor(juce::KeyPressMappingSet& k) : ed(k, true) {
            ed.setColours(colours::panel2, colours::text);
            lbl.setText("Preset:", juce::dontSendNotification);
            lbl.setColour(juce::Label::textColourId, colours::text);
            addAndMakeVisible(lbl);
            addAndMakeVisible(preset);
            addAndMakeVisible(ed);
        }
        void paint(juce::Graphics& g) override { g.fillAll(colours::bg); }
        void resized() override {
            auto r = getLocalBounds().reduced(6);
            auto top = r.removeFromTop(26);
            lbl.setBounds(top.removeFromLeft(56));
            preset.setBounds(top.removeFromLeft(220));
            r.removeFromTop(6);
            ed.setBounds(r);
        }
    };
    auto* ed = new KeyEditor(*commands_.getKeyMappings());
    auto names = keyPresetNames();
    for (int i = 0; i < names.size(); ++i) ed->preset.addItem(names[i], i + 1);
    ed->preset.setText("Load a preset...", juce::dontSendNotification);
    ed->preset.onChange = [this, ed] {
        const int id = ed->preset.getSelectedId();
        if (id <= 0) return;
        commands_.invokeDirectly(id == 2 ? cmd::keyPresetLegacy : cmd::keyPresetLathe, false);
    };
    ed->setSize(640, 680);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(ed);
    o.dialogTitle = "Edit Key Commands (click + to add a key, click a key to change or remove it)";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

namespace {
class FxWindow : public juce::DocumentWindow {
public:
    FxWindow(juce::Component* c, const juce::String& title) : DocumentWindow(title, colours::bg, DocumentWindow::closeButton) {
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

class BatchPanel : public juce::Component {
public:
    BatchPanel(double projectRate, int productCount, bool fullFitsCd) {
        auto hi = juce::String(projectRate / 1000.0, projectRate == std::round(projectRate / 1000.0) * 1000.0 ? 0 : 1) + " kHz";
        ddp_.setButtonText("DDP image for replication (verified after writing)");
        wav16_.setButtonText("WAV 16-bit / 44.1 kHz per track (bit-identical to the CD)");
        flac16_.setButtonText("FLAC 16-bit / 44.1 kHz per track (tagged)");
        wavHi_.setButtonText("WAV 24-bit / " + hi + " per track (hi-res / streaming masters)");
        flacHi_.setButtonText("FLAC 24-bit / " + hi + " per track (tagged)");
        prog_.setButtonText("Whole programme as one WAV, 24-bit / " + hi);
        products_.setButtonText(productCount > 0 ? "Products: " + juce::String(productCount) + " product(s), a DDP per CD and a file per vinyl / tape side"
                                                 : juce::String("Products (none set up: Delivery > Products & Splitting)"));
        paperwork_.setButtonText("Paperwork: PQ / cutting sheets, label copy, manifest (PDF, CSV, JSON)");
        for (auto* t : {&ddp_, &wav16_, &flac16_, &wavHi_, &flacHi_, &prog_, &products_, &paperwork_}) {
            t->setColour(juce::ToggleButton::textColourId, colours::text);
            t->setColour(juce::ToggleButton::tickColourId, colours::accent);
            addAndMakeVisible(*t);
        }
        ddp_.setToggleState(fullFitsCd, juce::dontSendNotification);
        if (!fullFitsCd) ddp_.setButtonText("DDP image of the full programme (longer than a CD: use a CD product instead)");
        wav16_.setToggleState(fullFitsCd, juce::dontSendNotification);
        wavHi_.setToggleState(true, juce::dontSendNotification);
        products_.setToggleState(productCount > 0, juce::dontSendNotification);
        products_.setEnabled(productCount > 0);
        paperwork_.setToggleState(true, juce::dontSendNotification);
        if (!fullFitsCd) prog_.setToggleState(true, juce::dontSendNotification);
        note_.setText("One render pass feeds the DDP and the 16-bit files, a second (at the project rate, no "
                      "sample-rate conversion) feeds the hi-res files. A Render Report with QC and an MD5 for "
                      "every file is written alongside. MP3/AAC aren't included (encoder licensing).",
                      juce::dontSendNotification);
        note_.setColour(juce::Label::textColourId, colours::dim);
        note_.setFont(juce::FontOptions(12.0f));
        note_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(note_);
        styleButton(render_, true);
        styleButton(cancel_);
        addAndMakeVisible(render_);
        addAndMakeVisible(cancel_);
        render_.onClick = [this] { if (onRender) onRender(settings()); };
        cancel_.onClick = [this] { if (onCancel) onCancel(); };
        setSize(600, 400);
    }
    BatchSettings settings() const {
        BatchSettings b;
        b.ddp = ddp_.getToggleState();
        b.wav16 = wav16_.getToggleState();
        b.flac16 = flac16_.getToggleState();
        b.wavHi = wavHi_.getToggleState();
        b.flacHi = flacHi_.getToggleState();
        b.programme = prog_.getToggleState();
        b.products = products_.getToggleState();
        b.paperwork = paperwork_.getToggleState();
        return b;
    }
    std::function<void(BatchSettings)> onRender;
    std::function<void()> onCancel;
    void paint(juce::Graphics& g) override { g.fillAll(colours::bg); }
    void resized() override {
        auto r = getLocalBounds().reduced(14);
        for (auto* t : {&ddp_, &wav16_, &flac16_, &wavHi_, &flacHi_, &prog_, &products_, &paperwork_}) { t->setBounds(r.removeFromTop(26)); r.removeFromTop(2); }
        auto buttons = r.removeFromBottom(28);
        render_.setBounds(buttons.removeFromRight(140));
        buttons.removeFromRight(8);
        cancel_.setBounds(buttons.removeFromRight(90));
        r.removeFromBottom(8);
        note_.setBounds(r);
    }
private:
    juce::ToggleButton ddp_, wav16_, flac16_, wavHi_, flacHi_, prog_, products_, paperwork_;
    juce::Label note_;
    juce::TextButton render_{"Choose Folder & Render"}, cancel_{"Cancel"};
};
}  // namespace

void MainComponent::openRegionFx(int regionId) {
    auto* r = session_.findRegion(regionId);
    if (!r) return;
    fxWindow_.reset();
    auto* chain = engine_.regionFx().chainFor(regionId, true, r->name);
    auto* panel = new ChainPanel(*chain, "Region effects: " + r->name);
    panel->onChange = [this] {
        sessionChanged();
        engine_.loadSession(session_);   // pick up the new latency/plugins in playback
    };
    panel->onScan = [this] { showPluginManager(); };
    auto* w = new FxWindow(panel, "Region Effects - " + r->name);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { fxWindow_.reset(); }); };
    fxWindow_.reset(w);
    fxWindowRegion_ = regionId;
}

void MainComponent::showBatchDialog() {
    if (session_.programmeChannels() > 2) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Stereo only",
                                               "CD, DDP and the per-track deliverables are stereo, and this programme is 5.1 (a lane is set to 5.1). "
                                               "Use Export Programme WAV for the 5.1 master, or set the lanes to stereo for the CD.");
        return;
    }
    if (session_.regions.isEmpty()) return;
    auto* panel = new BatchPanel(session_.effectiveRate(), session_.products.size(),
                                 session_.end() <= productSpec(ProductKind::CD).hardMax);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(panel);
    o.dialogTitle = "Render Album";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    auto* dw = o.launchAsync();
    juce::Component::SafePointer<juce::DialogWindow> safe(dw);
    panel->onCancel = [safe] { if (safe) safe->exitModalState(0); };
    panel->onRender = [this, safe](BatchSettings bs) {
        if (safe) safe->exitModalState(0);
        chooser_ = std::make_unique<juce::FileChooser>("Choose where to create the album folder",
                                                       juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, bs](const juce::FileChooser& fc) {
            auto parent = fc.getResult();
            if (!parent.isDirectory()) return;
            auto name = juce::File::createLegalFileName((session_.discTitle.isEmpty() ? juce::String("Lathe") : session_.discTitle) + " Masters");
            auto folder = parent.getNonexistentChildFile(name, "", false);
            engine_.regionFx().syncAndCapture(session_);
            auto snapshot = std::make_shared<Session>(session_);
            auto es = exportSettings();
            auto res = std::make_shared<BatchResult>();
            auto studio = studio_;
            runJob("Rendering album...",
                   [this, snapshot, bs, folder, es, res, studio](Job& j) {
                       *res = exporter_.batchRender(*snapshot, bs, folder, es, [&](double p) { return j.keep(p); });
                       if (bs.paperwork && !res->report.isEmpty()) {
                           auto pw = writePaperwork(*snapshot, res->delivery, studio, folder);
                           if (pw.error.isNotEmpty()) res->report << "\nPAPERWORK FAILED: " << pw.error << "\n";
                           else res->report << "\nPAPERWORK\n" << "  " << pw.files.joinIntoString("\n  ") << "\n";
                       }
                   },
                   [this, res, folder](bool) {
                       if (!res->ok && res->report.isEmpty()) {
                           juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Render failed", res->error);
                           return;
                       }
                       juce::String text;
                       if (!res->ok) text << "WARNING: " << res->error << "\n\n";
                       text << res->report;
                       showText("Render complete", text);
                       folder.revealToUser();
                   });
        });
    };
}

// ---------------------------------------------------------------- analysis, references, A/B

namespace {
class AnalysisView : public juce::Component, private juce::ListBoxModel {
public:
    // Close the list while this panel is still whole: a cell being edited at close otherwise
    // reaches back into a half-destroyed list (a crash on macOS).
    ~AnalysisView() override { closeList(list_); }
    AnalysisView(std::shared_ptr<dsp::AnalysisResult> a, std::function<void(double)> locate) : a_(std::move(a)), locate_(std::move(locate)) {
        summary_.setMultiLine(true);
        summary_.setReadOnly(true);
        summary_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        auto s = juce::String(a_->summary());
        summary_.setText(s.upToFirstOccurrenceOf("PROBLEMS:", false, false).trimEnd());
        addAndMakeVisible(summary_);
        list_.setRowHeight(22);
        list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        addAndMakeVisible(list_);
        header_.setText(juce::String(int(a_->problems.size())) + " problem(s). Double-click one to go there.", juce::dontSendNotification);
        header_.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(header_);
        setSize(820, 600);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(8);
        summary_.setBounds(r.removeFromTop(r.getHeight() / 2));
        r.removeFromTop(6);
        header_.setBounds(r.removeFromTop(22));
        list_.setBounds(r);
    }
private:
    int getNumRows() override { return int(a_->problems.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        auto& p = a_->problems[size_t(row)];
        g.fillAll(selected ? colours::accent.withAlpha(0.25f) : colours::panel);
        g.setColour(p.severe ? colours::error : colours::accent);
        g.fillEllipse(8, float(h) / 2 - 4, 8, 8);
        g.setColour(colours::text);
        g.setFont(13.0f);
        g.drawText(msfString(p.start), 24, 0, 80, h, juce::Justification::centredLeft);
        g.drawText(p.track ? "Track " + juce::String(p.track) : juce::String("-"), 110, 0, 70, h, juce::Justification::centredLeft);
        g.drawText(juce::String(p.describe()), 186, 0, w - 190, h, juce::Justification::centredLeft);
    }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { if (locate_) locate_(a_->problems[size_t(row)].start); }

    std::shared_ptr<dsp::AnalysisResult> a_;
    std::function<void(double)> locate_;
    juce::TextEditor summary_;
    juce::Label header_;
    juce::ListBox list_{"Problems", this};
};

class ReferencePanel : public juce::Component, private juce::ListBoxModel, private juce::Timer {
public:
    // Close the list while this panel is still whole: a cell being edited at close otherwise
    // reaches back into a half-destroyed list (a crash on macOS).
    ~ReferencePanel() override { closeList(list_); }
    ReferencePanel(Session& s, AudioEngine& e, std::function<void(int)> select, std::function<void()> add, std::function<void(int)> remove,
                   std::function<void()> changed, std::function<juce::String()> status)
        : s_(s), e_(e), select_(std::move(select)), add_(std::move(add)), remove_(std::move(remove)), changed_(std::move(changed)),
          status_(std::move(status)) {
        list_.setRowHeight(24);
        list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        addAndMakeVisible(list_);
        for (auto* b : {&addBtn_, &removeBtn_, &abBtn_}) { styleButton(*b, b == &abBtn_); addAndMakeVisible(*b); }
        match_.setButtonText("Level-match to the master (integrated loudness)");
        match_.setColour(juce::ToggleButton::textColourId, colours::text);
        match_.setToggleState(s_.levelMatchReference, juce::dontSendNotification);
        addAndMakeVisible(match_);
        trim_.setSliderStyle(juce::Slider::LinearHorizontal);
        trim_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
        trim_.setRange(-12.0, 12.0, 0.1);
        trim_.setTextValueSuffix(" dB");
        addAndMakeVisible(trim_);
        trimLbl_.setText("Trim", juce::dontSendNotification);
        trimLbl_.setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(trimLbl_);
        info_.setColour(juce::Label::textColourId, colours::text);
        info_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(info_);
        addBtn_.onClick = [this] { if (add_) add_(); };
        removeBtn_.onClick = [this] { int r = list_.getSelectedRow(); if (r >= 0 && remove_) remove_(r); };
        abBtn_.onClick = [this] { e_.setListenToReference(!e_.listeningToReference()); };
        match_.onClick = [this] { s_.levelMatchReference = match_.getToggleState(); if (changed_) changed_(); };
        trim_.onValueChange = [this] {
            if (juce::isPositiveAndBelow(s_.activeReference, s_.references.size())) {
                s_.references.getReference(s_.activeReference).trimDb = trim_.getValue();
                if (changed_) changed_();
            }
        };
        refresh();
        startTimerHz(10);
        setSize(560, 400);
    }
    void refresh() {
        list_.updateContent();
        list_.selectRow(s_.activeReference, true, true);
        if (juce::isPositiveAndBelow(s_.activeReference, s_.references.size()))
            trim_.setValue(s_.references[s_.activeReference].trimDb, juce::dontSendNotification);
        repaint();
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(colours::bg);
        // Reference position bar: click to move the reference playhead.
        auto bar = posBar();
        g.setColour(colours::panel2);
        g.fillRoundedRectangle(bar.toFloat(), 3);
        if (e_.hasReference() && e_.referenceLength() > 0) {
            float f = float(e_.referencePosition() / e_.referenceLength());
            g.setColour(colours::accent);
            g.fillRoundedRectangle(bar.toFloat().withWidth(bar.getWidth() * f), 3);
            g.setColour(colours::text);
            g.setFont(11.0f);
            g.drawText(msfString(e_.referencePosition()) + " / " + msfString(e_.referenceLength()), bar, juce::Justification::centred);
        }
    }
    void mouseDown(const juce::MouseEvent& ev) override {
        auto bar = posBar();
        if (bar.contains(ev.getPosition()) && e_.hasReference())
            e_.setReferencePosition(e_.referenceLength() * double(ev.x - bar.getX()) / bar.getWidth());
    }
    void resized() override {
        auto r = getLocalBounds().reduced(10);
        auto top = r.removeFromTop(28);
        addBtn_.setBounds(top.removeFromLeft(90));
        top.removeFromLeft(6);
        removeBtn_.setBounds(top.removeFromLeft(80));
        abBtn_.setBounds(top.removeFromRight(170));
        r.removeFromTop(8);
        list_.setBounds(r.removeFromTop(150));
        r.removeFromTop(8);
        match_.setBounds(r.removeFromTop(24));
        auto t = r.removeFromTop(26);
        trimLbl_.setBounds(t.removeFromLeft(40));
        trim_.setBounds(t);
        r.removeFromTop(4);
        bar_ = r.removeFromTop(18);
        r.removeFromTop(8);
        info_.setBounds(r);
    }
private:
    juce::Rectangle<int> posBar() const { return bar_; }
    int getNumRows() override { return s_.references.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        auto& r = s_.references.getReference(row);
        g.fillAll(selected ? colours::accent.withAlpha(0.25f) : colours::panel);
        g.setColour(r.file.existsAsFile() ? colours::text : colours::error);
        g.setFont(13.0f);
        g.drawText(r.name, 8, 0, w - 120, h, juce::Justification::centredLeft);
        g.setColour(colours::dim);
        g.drawText(r.lufs > -999 ? juce::String(r.lufs, 1) + " LUFS" : juce::String("measuring..."), w - 110, 0, 100, h, juce::Justification::centredRight);
    }
    void selectedRowsChanged(int row) override { if (row >= 0 && row != s_.activeReference && select_) select_(row); }
    void timerCallback() override {
        abBtn_.setButtonText(e_.listeningToReference() ? "Hearing REFERENCE (Q)" : "Hearing MASTER (Q)");
        abBtn_.setToggleState(e_.listeningToReference(), juce::dontSendNotification);
        if (status_) info_.setText(status_(), juce::dontSendNotification);
        list_.repaint();
        repaint(bar_);
    }
    Session& s_;
    AudioEngine& e_;
    std::function<void(int)> select_;
    std::function<void()> add_;
    std::function<void(int)> remove_;
    std::function<void()> changed_;
    std::function<juce::String()> status_;
    juce::ListBox list_{"References", this};
    juce::TextButton addBtn_{"Add..."}, removeBtn_{"Remove"}, abBtn_{"Hearing MASTER (Q)"};
    juce::ToggleButton match_;
    juce::Slider trim_;
    juce::Label trimLbl_, info_;
    juce::Rectangle<int> bar_;
};

class PlainWindow : public juce::DocumentWindow {
public:
    PlainWindow(juce::Component* c, const juce::String& title, bool onTop) : DocumentWindow(title, colours::bg, DocumentWindow::closeButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(c, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setAlwaysOnTop(onTop);
        setVisible(true);
    }
    std::function<void()> onClose;
    void closeButtonPressed() override { if (onClose) onClose(); }
};
}  // namespace

double MainComponent::masterLoudnessAt(double t) const {
    if (!analysis_) return -INFINITY;
    for (auto& tr : analysis_->tracks)
        if (t >= tr.start && t < tr.end && std::isfinite(tr.lufs)) return tr.lufs;
    return analysis_->lufs;
}

double MainComponent::referenceGainDb(juce::String* explain) const {
    if (!juce::isPositiveAndBelow(session_.activeReference, session_.references.size())) return 0;
    auto& r = session_.references.getReference(session_.activeReference);
    double g = r.trimDb;
    if (!session_.levelMatchReference) { if (explain) *explain = "Level match off"; return g; }
    double m = masterLoudnessAt(engine_.positionSeconds());
    if (!std::isfinite(m) || r.lufs < -999) {
        if (explain) *explain = r.lufs < -999 ? "Measuring the reference..." : "Analyse the master (Cmd+Shift+A) to level-match";
        return g;
    }
    if (explain) *explain = "Reference " + juce::String(r.lufs, 1) + " LUFS, master here " + juce::String(m, 1) + " LUFS";
    return g + (m - r.lufs);
}

void MainComponent::runAnalysis() {
    if (session_.regions.isEmpty()) return;
    engine_.regionFx().syncAndCapture(session_);
    auto snapshot = std::make_shared<Session>(session_);
    auto es = exportSettings();
    auto proc = std::make_shared<dsp::AnalysisResult>();
    auto raw = std::make_shared<dsp::AnalysisResult>();
    auto err = std::make_shared<juce::String>();
    runJob("Analysing the master and the unprocessed mix...",
           [this, snapshot, es, proc, raw, err](Job& j) { *err = exporter_.analyseProgram(*snapshot, es, [&](double p) { return j.keep(p); }, *proc, raw.get()); },
           [this, proc, raw, err](bool) {
               if (err->isNotEmpty()) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Analyse", *err); return; }
               analysis_ = proc;
               rawAnalysis_ = raw;
               analysisStale_ = false;
               timeline_.setAnalysis(proc, raw);
               timeline_.setShowLoudness(true);
               commands_.commandStatusChanged();
               showAnalysisWindow();
           });
}

void MainComponent::openDdpPlayer() {
    chooser_ = std::make_unique<juce::FileChooser>("Choose a DDP folder to play");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
        auto folder = fc.getResult();
        if (!folder.isDirectory()) return;
        auto r = std::make_shared<ddp::ReadResult>();
        runJob("Reading and verifying DDP...",
               [r, folder](Job& j) { *r = ddp::readDdp(folder.getFullPathName().toStdString(), [&](double p) { j.setProgress(p); }); },
               [this, r, folder](bool) {
                   if (!r->ok) {
                       juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "DDP Player",
                                                              "This folder can't be read as a DDP:\n" + juce::String::fromUTF8(r->error.c_str()));
                       return;
                   }
                   engine_.stop();
                   playerWindow_.reset();
                   auto* p = new DdpPlayer(engine_, folder, *r);
                   p->onOpenAsSession = [this](const juce::File& f) {
                       juce::MessageManager::callAsync([this, f] { playerWindow_.reset(); openDdpAsSession(f); });
                   };
                   auto* w = new PlainWindow(p, "DDP Player - " + folder.getFileName(), false);
                   w->onClose = [this] { juce::MessageManager::callAsync([this] { playerWindow_.reset(); }); };
                   playerWindow_.reset(w);
               });
    });
}

void MainComponent::openDdpAsSession(const juce::File& folder) {
    Session s;
    juce::String err;
    juce::StringArray notes;
    if (!sessionFromDdp(folder, s, err, notes)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Open DDP", err);
        return;
    }
    newSession();
    session_ = s;
    attachSource();
    lastState_ = juce::JSON::toString(session_.toVar(), true);
    refreshAll();
    timeline_.zoomToFit();
    dirty_ = true;
    juce::String msg = "Opened " + folder.getFileName() + " as a session: " + juce::String(int(s.toDisc().tracks.size())) +
                       " tracks with their PQ, CD-Text and ISRCs.\n\nThe audio is the DDP image itself (16-bit). Edit markers or metadata, "
                       "or Replace sections from new mixes, then export a new DDP. Save the session to keep it.";
    if (!notes.isEmpty()) msg << "\n\nNotes:\n" << notes.joinIntoString("\n");
    showText("DDP opened as session", msg);
}

void MainComponent::showAnalysisWindow() {
    if (!analysis_) return;
    auto* v = new AnalysisView(analysis_, [this](double t) { locateDest(juce::jmax(0.0, t - 1.0)); });
    auto title = juce::String("Analysis") + (analysisStale_ ? " (out of date: the session changed since)" : "");
    auto* w = new PlainWindow(v, title, false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { analysisWindow_.reset(); }); };
    analysisWindow_.reset(w);
}

void MainComponent::measureReference(int index) {
    if (!juce::isPositiveAndBelow(index, session_.references.size())) return;
    auto file = session_.references[index].file;
    auto result = std::make_shared<double>(-1000.0);
    juce::Component::SafePointer<MainComponent> safe(this);
    juce::Thread::launch([safe, file, index, result] {
        juce::AudioFormatManager fm;   // own manager: this thread may outlive a window close
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(file));
        if (r) {
            dsp::LoudnessMeter m;
            m.prepare(r->sampleRate, 2);
            juce::AudioBuffer<float> b(2, 65536);
            for (juce::int64 p = 0; p < r->lengthInSamples; p += 65536) {
                int n = int(juce::jmin<juce::int64>(65536, r->lengthInSamples - p));
                r->read(&b, 0, n, p, true, true);
                if (r->numChannels == 1) b.copyFrom(1, 0, b, 0, 0, n);
                const float* ch[2] = {b.getReadPointer(0), b.getReadPointer(1)};
                m.process(ch, n);
            }
            *result = m.integratedLufs();
        }
        juce::MessageManager::callAsync([safe, index, file, result] {
            if (!safe) return;
            auto& refs = safe->session_.references;
            if (juce::isPositiveAndBelow(index, refs.size()) && refs[index].file == file) {
                refs.getReference(index).lufs = std::isfinite(*result) ? *result : -100.0;
                safe->sessionChanged(false);
            }
        });
    });
}

void MainComponent::selectReference(int index) {
    if (!juce::isPositiveAndBelow(index, session_.references.size())) return;
    session_.activeReference = index;
    if (!engine_.loadReference(session_.references[index].file))
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Reference", "Couldn't open " + session_.references[index].file.getFullPathName());
    sessionChanged(false);
}

void MainComponent::showReferences() {
    juce::Component::SafePointer<MainComponent> safe(this);
    auto* panel = new ReferencePanel(
        session_, engine_, [this](int i) { selectReference(i); },
        [this, safe] {
            chooser_ = std::make_unique<juce::FileChooser>("Add reference tracks", juce::File(), engine_.formats().getWildcardForAllFormats());
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems, [this, safe](const juce::FileChooser& fc) {
                if (!safe) return;
                for (auto& f : fc.getResults()) {
                    Reference r;
                    r.file = f;
                    r.name = f.getFileNameWithoutExtension();
                    session_.references.add(r);
                    measureReference(session_.references.size() - 1);
                }
                if (session_.activeReference < 0 && !session_.references.isEmpty()) selectReference(0);
                sessionChanged(false);
                if (refWindow_) if (auto* p = dynamic_cast<ReferencePanel*>(refWindow_->getContentComponent())) p->refresh();
            });
        },
        [this](int i) {
            session_.references.remove(i);
            if (session_.activeReference == i) { session_.activeReference = -1; engine_.clearReference(); }
            else if (session_.activeReference > i) --session_.activeReference;
            sessionChanged(false);
            if (refWindow_) if (auto* p = dynamic_cast<ReferencePanel*>(refWindow_->getContentComponent())) p->refresh();
        },
        [this] { sessionChanged(false); },
        [this] {
            juce::String why;
            if (compare_) updateCompareBar();
        double g = referenceGainDb(&why);
            juce::String s;
            if (session_.activeReference < 0) return juce::String("Add a reference and select it. Press Q (or the button) to switch between it and the master; both keep playing so you hear the same moment.");
            s << why << "\nApplied to the reference: " << (g >= 0 ? "+" : "") << juce::String(g, 1) << " dB";
            s << "\n\nThe reference plays without your master chain. Use W to hear your own mix unprocessed, level-matched.";
            return s;
        });
    auto* w = new PlainWindow(panel, "References", true);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { refWindow_.reset(); }); };
    refWindow_.reset(w);
    if (session_.activeReference >= 0 && !engine_.hasReference()) selectReference(session_.activeReference);
}

void MainComponent::toggleCompareUnprocessed() {
    // First press: Compare Unprocessed comes on and you hear B. After that, W switches sides
    // (A / B, or B / C with Level Match); End (Option-W) goes back to normal playback.
    if (!compare_) {
        endCaptureCheck();
        gearHear(gearAb_, 0);
        compare_ = true;
        engine_.setHearUnprocessed(true);
    } else {
        engine_.setHearUnprocessed(!engine_.hearingUnprocessed());
    }
    updateCompareBar();
    resized();
    commands_.commandStatusChanged();
}

void MainComponent::setLevelMatch(bool on) {
    engine_.setLevelMatch(on);
    if (on) engine_.setHearUnprocessed(false);   // straight to C, so you hear the match
    updateCompareBar();
    commands_.commandStatusChanged();
}

void MainComponent::endCompare() {
    if (!compare_) return;
    compare_ = false;
    engine_.setHearUnprocessed(false);
    engine_.setLevelMatch(false);
    updateCompareBar();
    resized();
    commands_.commandStatusChanged();
}

void MainComponent::updateCompareBar() {
    CompareBar::State st;
    st.hearingUnprocessed = engine_.hearingUnprocessed();
    st.levelMatch = engine_.levelMatch();
    st.measured = engine_.matchMeasured();
    st.cutDb = engine_.matchCutDb();
    st.differenceDb = engine_.matchDifferenceDb();
    compareBar_.setState(st);
    compareBar_.setVisible(compare_);
}

// ---------------------------------------------------------------- recording

namespace {
class RecordPanel : public juce::Component, private juce::Timer {
public:
    RecordPanel(AudioEngine& e, bool haveSession, bool haveSelection, int firstInput, int bits, bool print, bool punch, bool mute, int extra, int measured)
        : e_(e), measured_(measured) {
        auto pairs = e_.inputPairNames();
        for (int i = 0; i < pairs.size(); ++i) input_.addItem(pairs[i], i + 1);
        input_.setSelectedId(juce::jlimit(1, juce::jmax(1, pairs.size()), firstInput / 2 + 1), juce::dontSendNotification);
        bitsValue_ = bits == 32 ? 32 : 24;
        bitsInfo_.setText(juce::String(bitsValue_ == 32 ? "32-bit float" : "24-bit") + " WAV  (the format and the folder are in Audio Setup)", juce::dontSendNotification);
        input_.onChange = [this] { e_.setInputSelection(this->firstInput(), 2); };
        e_.setInputSelection(this->firstInput(), 2);
        print_.setButtonText("Play the session while recording (print through outboard)");
        print_.setToggleState(print && haveSession, juce::dontSendNotification);
        print_.setEnabled(haveSession);
        punch_.setButtonText("Auto-punch: keep only the destination In-Out (with pre-roll)");
        punch_.setToggleState(punch && haveSelection, juce::dontSendNotification);
        punch_.setEnabled(haveSelection);
        mute_.setButtonText("Mute the source regions after printing");
        mute_.setToggleState(mute, juce::dontSendNotification);
        for (auto* t : {&print_, &punch_, &mute_}) { t->setColour(juce::ToggleButton::textColourId, colours::text); addAndMakeVisible(*t); }
        for (auto* l : {&inLbl_, &bitsLbl_, &bitsInfo_, &latLbl_, &latInfo_}) { l->setColour(juce::Label::textColourId, colours::dim); addAndMakeVisible(*l); }
        inLbl_.setText("Inputs", juce::dontSendNotification);
        bitsLbl_.setText("Format", juce::dontSendNotification);
        latLbl_.setText("Extra offset (samples)", juce::dontSendNotification);
        extra_.setText(juce::String(extra), false);
        extra_.setInputRestrictions(7, "-0123456789");
        extra_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
        extra_.setColour(juce::TextEditor::textColourId, colours::text);
        addAndMakeVisible(input_);
        addAndMakeVisible(extra_);
        for (auto* b : {&measure_, &record_, &cancel_}) { styleButton(*b, b == &record_); addAndMakeVisible(*b); }
        record_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff9a2a24));
        measure_.onClick = [this] {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Latency test",
                "Patch the selected outputs straight back into the selected inputs (or through your outboard at unity), "
                "then press OK. A single click will be played.",
                "OK", this, juce::ModalCallbackFunction::create([this](int) {
                    e_.startLatencyTest((input_.getSelectedId() - 1) * 2);
                    testing_ = true;
                }));
        };
        record_.onClick = [this] { if (onRecord) onRecord(); };
        cancel_.onClick = [this] { if (onCancel) onCancel(); };
        updateInfo();
        startTimerHz(10);
        setSize(560, 330);
    }
    std::function<void()> onRecord, onCancel;
    int firstInput() const { return (input_.getSelectedId() - 1) * 2; }
    int bits() const { return bitsValue_; }
    bool print() const { return print_.getToggleState(); }
    bool punch() const { return punch_.getToggleState(); }
    bool mute() const { return mute_.getToggleState(); }
    int extra() const { return extra_.getText().getIntValue(); }
    int measured() const { return measured_; }
    void paint(juce::Graphics& g) override { g.fillAll(colours::bg); }
    void resized() override {
        auto r = getLocalBounds().reduced(12);
        auto row = [&](juce::Label& l, juce::Component& c) { auto x = r.removeFromTop(26); l.setBounds(x.removeFromLeft(150)); c.setBounds(x); r.removeFromTop(6); };
        row(inLbl_, input_);
        row(bitsLbl_, bitsInfo_);
        for (auto* t : {&print_, &punch_, &mute_}) { t->setBounds(r.removeFromTop(24)); r.removeFromTop(2); }
        r.removeFromTop(6);
        auto x = r.removeFromTop(26);
        latLbl_.setBounds(x.removeFromLeft(150));
        extra_.setBounds(x.removeFromLeft(80));
        x.removeFromLeft(8);
        measure_.setBounds(x.removeFromLeft(150));
        latInfo_.setBounds(r.removeFromTop(40));
        auto b = r.removeFromBottom(28);
        record_.setBounds(b.removeFromRight(130));
        b.removeFromRight(8);
        cancel_.setBounds(b.removeFromRight(90));
    }
private:
    void updateInfo() {
        juce::String s = "Interface reports " + juce::String(e_.reportedRoundTrip()) + " samples round trip.";
        if (measured_ > 0) s << " Measured loopback: " << measured_ << " samples (used instead).";
        latInfo_.setText(s + "\nPrints are moved earlier by the round trip so they line up with the session.", juce::dontSendNotification);
    }
    void timerCallback() override {
        if (testing_ && e_.latencyTestResult() >= 0) {
            testing_ = false;
            int v = e_.latencyTestResult();
            if (v > 0) measured_ = v;
            else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Latency test", "No click came back on the selected inputs. Check the loopback patch and levels.");
            updateInfo();
        }
    }
    AudioEngine& e_;
    int measured_;
    bool testing_ = false;
    juce::ComboBox input_;
    int bitsValue_ = 24;
    juce::ToggleButton print_, punch_, mute_;
    juce::Label inLbl_, bitsLbl_, bitsInfo_, latLbl_, latInfo_;
    juce::TextEditor extra_;
    juce::TextButton measure_{"Measure (loopback)"}, record_{"Record"}, cancel_{"Cancel"};
};
}  // namespace

juce::File MainComponent::recordingsFolder() const { return lathe::recordingsFolder(io_, session_.file); }

void MainComponent::showRecordDialog() {
    recWindow_.reset();
    auto sel = timeline_.selection();
    auto* p = new RecordPanel(engine_, !session_.regions.isEmpty(), !sel.isEmpty(), rec_.firstInput, io_.bits, rec_.print, rec_.autoPunch,
                              rec_.muteSources, rec_.extraLatency, rec_.measured);
    juce::Component::SafePointer<RecordPanel> safe(p);
    p->onCancel = [this] { juce::MessageManager::callAsync([this] { recWindow_.reset(); }); };
    p->onRecord = [this, safe] {
        if (!safe) return;
        rec_.firstInput = safe->firstInput();
        rec_.bits = io_.bits;
        rec_.print = safe->print();
        rec_.autoPunch = safe->punch();
        rec_.muteSources = safe->mute();
        rec_.extraLatency = safe->extra();
        rec_.measured = safe->measured();
        auto* u = props_.getUserSettings();
        u->setValue("recInput", rec_.firstInput);
        u->setValue("recExtraLatency", rec_.extraLatency);
        u->setValue("recMeasuredLatency", rec_.measured);
        u->setValue("recPrint", rec_.print);
        u->setValue("recMute", rec_.muteSources);
        juce::MessageManager::callAsync([this] { recWindow_.reset(); beginRecording(); });
    };
    auto* w = new PlainWindow(p, "Record", true);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { recWindow_.reset(); }); };
    recWindow_.reset(w);
}

void MainComponent::beginRecording() {
    auto folder = recordingsFolder();
    if (cap_.capturing || cap_.levelCheck) return;
    endCaptureCheck();
    recFile_ = folder.getNonexistentChildFile(recordingName(session_.file, rec_.print ? "Print" : "Recording", juce::Time::getCurrentTime()), ".wav", false);
    recPunch_ = rec_.autoPunch ? timeline_.selection() : juce::Range<double>();
    if (rec_.print && !recPunch_.isEmpty()) engine_.setPositionSeconds(juce::jmax(0.0, recPunch_.getStart() - 2.0));   // 2 s pre-roll
    juce::String err;
    recMarks_.clear();
    if (!engine_.startRecording(recFile_, rec_.firstInput, io_.bits, rec_.print && !session_.regions.isEmpty(), err)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Couldn't record", err);
        return;
    }
    commands_.commandStatusChanged();
}

void MainComponent::finishRecording() {
    if (!engine_.isRecording()) return;
    const bool printed = rec_.print && !session_.regions.isEmpty();
    const double start = engine_.recordStartPosition();   // (to the sample when the session was playing)
    const int chainLatency = engine_.masterLatency();     // the print left the master plugins this late
    engine_.stopRecording();
    std::unique_ptr<juce::AudioFormatReader> r(engine_.formats().createReaderFor(recFile_));
    if (!r || r->lengthInSamples < 16) {
        recMarks_.clear();
        timeline_.clearLiveTake();
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recording", "Nothing was recorded.");
        return;
    }
    Region reg = session_.makeRegion(recFile_, r->sampleRate, r->lengthInSamples);
    reg.start = start;
    if (printed) {
        // The return arrives one round trip late: drop those samples so the print lines up.
        int lat = (rec_.measured > 0 ? rec_.measured : engine_.reportedRoundTrip()) + rec_.extraLatency + chainLatency;
        reg.srcIn = juce::jlimit<juce::int64>(0, reg.srcOut - 1, lat);
        if (!recPunch_.isEmpty()) {
            // Auto-punch: keep only the destination In-Out of the print.
            auto in = juce::jlimit<juce::int64>(reg.srcIn, reg.srcOut - 1, reg.srcIn + juce::int64((recPunch_.getStart() - start) * r->sampleRate));
            auto out = juce::jlimit<juce::int64>(in + 1, reg.srcOut, reg.srcIn + juce::int64((recPunch_.getEnd() - start) * r->sampleRate));
            reg.start = start + double(in - reg.srcIn) / r->sampleRate;
            reg.srcIn = in;
            reg.srcOut = out;
        }
        // New track (lane) below everything else.
        int maxLane = 0;
        for (auto& x : session_.regions) maxLane = juce::jmax(maxLane, x.lane);
        reg.lane = maxLane + 1;
        session_.lanes = juce::jmax(session_.lanes, reg.lane + 1);
        if (rec_.muteSources)
            for (auto& x : session_.regions)
                if (recPunch_.isEmpty() || (x.end() > recPunch_.getStart() && x.start < recPunch_.getEnd())) x.mute = true;
    } else {
        reg.lane = timeline_.selectedLane();
    }
    reg.name = recFile_.getFileNameWithoutExtension();
    session_.regions.add(reg);
    bool markedStart = false;
    for (auto& m : recMarks_) markedStart |= m.type == Marker::TrackStart && std::abs(m.t - reg.start) < 1.0;
    if (session_.toDisc().tracks.empty() && !printed && !markedStart) session_.addMarker(Marker::TrackStart, reg.start, reg.id);
    placeRecMarks(reg.id);
    sessionChanged();
    timeline_.resized();
    timeline_.selectRegion(reg.id);
    commands_.commandStatusChanged();
}

// ---------------------------------------------------------------- audio setup settings, capture

void MainComponent::saveIo() { props_.getUserSettings()->setValue("ioSettings", juce::JSON::toString(io_.toVar(), true)); }

void MainComponent::applyIo() {
    const int was = engine_.playbackChannels();
    if (io_.monitorOut >= 0) engine_.setMonitorOutput(io_.monitorOut, io_.monitorSix);
    engine_.setInputMonitoring(io_.inputMonitoring);
    // A 5.1 programme changes between six outputs and the stereo fold-down with the setting.
    if (session_.programmeChannels() == 6 && !cap_.capturing && !cap_.levelCheck && cap_.hearing == 0) {
        engine_.loadSession(session_);
        juce::ignoreUnused(was);
    }
}

int MainComponent::captureRoundTrip() const {
    const int saved = io_.delay(IoSettings::delayKey(engine_.outputDeviceName(), engine_.inputDeviceName(), io_.captureOut, io_.captureIn,
                                                     engine_.deviceRate(), engine_.bufferSize()));
    return saved > 0 ? saved : engine_.reportedRoundTrip();
}

CapturePanel::Status MainComponent::captureStatus() const {
    CapturePanel::Status st;
    st.checkingLevels = cap_.levelCheck;
    st.capturing = cap_.capturing;
    st.progress = engine_.captureProgress();
    st.haveTake = cap_.haveTake && session_.findRegion(cap_.regionId) != nullptr;
    st.hearing = cap_.hearing;
    st.levelMatch = engine_.levelMatch() && cap_.hearing != 0;
    st.matchMeasured = cap_.hearing != 0 && engine_.matchMeasured();
    st.matchDifferenceDb = engine_.matchDifferenceDb();
    st.message = cap_.message;
    return st;
}

void MainComponent::showCapture() {
    if (captureWindow_) { captureWindow_->setVisible(true); captureWindow_->toFront(true); return; }
    CapturePanel::Host h;
    h.plan = [this] { return planCapture(session_, timeline_.selection()); };
    h.status = [this] { return captureStatus(); };
    h.levelCheck = [this](bool on) { captureLevelCheck(on); };
    h.capture = [this] { beginCapture(); };
    h.stop = [this] { finishCapture(); };
    h.hear = [this](int which) { captureHear(which); };
    h.levelMatch = [this](bool on) { if (cap_.hearing != 0) engine_.setLevelMatch(on); };
    h.nullTest = [this] { captureNullTest(); };
    h.settingsChanged = [this] { saveIo(); };
    h.audioSetup = [this] { showAudioSettings(); };
    h.folder = [this] { return recordingsFolder(); };
    auto* p = new CapturePanel(engine_, io_, meterHub_, inputHub_, std::move(h));
    auto* w = new PlainWindow(p, "Capture Through Outboard", true);
    w->addKeyListener(commands_.getKeyMappings());   // M drops a marker, Space stops, while this window has the keys
    w->onClose = [this] {
        juce::MessageManager::callAsync([this] {
            if (cap_.levelCheck) captureLevelCheck(false);
            endCaptureCheck();
            if (!cap_.capturing) captureWindow_.reset();   // (a capture in progress keeps its window)
        });
    };
    captureWindow_.reset(w);
    engine_.setInputSelection(io_.captureIn, planCapture(session_, timeline_.selection()).channels);
}

void MainComponent::captureLevelCheck(bool on) {
    if (cap_.capturing) return;
    if (!on) {
        if (!cap_.levelCheck) return;
        cap_.levelCheck = false;
        engine_.stopCapture();
        engine_.loadSession(session_);
        return;
    }
    endCaptureCheck();
    endCompare();
    auto plan = planCapture(session_, timeline_.selection());
    if (!plan.valid()) return;
    AudioEngine::CaptureRequest rq;
    rq.levelCheck = true;
    rq.firstInput = io_.captureIn;
    rq.firstOutput = io_.captureOut;
    rq.channels = plan.channels;
    rq.from = plan.range.getStart();
    rq.to = plan.range.getEnd();
    rq.preRoll = 0.0;
    juce::String err;
    if (!engine_.startCapture(rq, session_, err)) {
        engine_.loadSession(session_);
        cap_.message = "!" + err;
        return;
    }
    cap_.plan = plan;
    cap_.levelCheck = true;
    cap_.message = {};
    meterHub_.reset();
    inputHub_.reset();
    engine_.resetInputOvers();
}

void MainComponent::beginCapture() {
    if (cap_.capturing || engine_.isRecording()) return;
    if (cap_.levelCheck) captureLevelCheck(false);
    endCaptureCheck();
    endCompare();
    gearHear(gearAb_, 0);
    auto plan = planCapture(session_, timeline_.selection());
    if (!plan.valid()) return;
    const auto folder = recordingsFolder();
    folder.createDirectory();
    // Room on the disk for the whole take, with some to spare.
    const auto need = recordingBytes(plan.range.getLength() + 2.0 + io_.capturePostRoll, engine_.deviceRate(), plan.channels, io_.bits);
    const auto freeBytes = folder.getBytesFreeOnVolume();
    if (!folder.isDirectory()) { cap_.message = "!Couldn't create " + folder.getFullPathName(); return; }
    if (freeBytes > 0 && freeBytes < need + (juce::int64(64) << 20)) {
        cap_.message = "!Not enough room: the capture needs " + byteSize(need) + " and " + byteSize(freeBytes) + " is free in " + folder.getFullPathName() +
                       ". Choose another folder in Audio Setup.";
        return;
    }
    cap_.file = folder.getNonexistentChildFile(recordingName(session_.file, "Capture", juce::Time::getCurrentTime()), ".wav", false);
    cap_.plan = plan;
    cap_.before = session_;
    cap_.before.source = nullptr;
    cap_.roundTrip = captureRoundTrip();
    cap_.haveTake = false;
    cap_.regionId = 0;
    AudioEngine::CaptureRequest rq;
    rq.file = cap_.file;
    rq.firstInput = io_.captureIn;
    rq.firstOutput = io_.captureOut;
    rq.channels = plan.channels;
    rq.bits = io_.bits;
    rq.from = plan.range.getStart();
    rq.to = plan.range.getEnd();
    rq.preRoll = 2.0;
    rq.postRoll = io_.capturePostRoll;
    juce::String err;
    if (!engine_.startCapture(rq, session_, err)) {
        engine_.loadSession(session_);
        cap_.message = "!" + err;
        return;
    }
    cap_.capturing = true;
    recMarks_.clear();
    cap_.message = {};
    meterHub_.reset();
    inputHub_.reset();
    engine_.resetInputOvers();
    commands_.commandStatusChanged();
}

void MainComponent::finishCapture() {
    if (!cap_.capturing) return;
    cap_.capturing = false;
    const bool complete = engine_.captureComplete();
    const juce::int64 startSample = engine_.recordStartSample();
    const int overs = engine_.inputOvers();
    const int chainLatency = engine_.masterLatency();   // the send left the master plugins this late
    engine_.stopCapture();   // closes the file: whatever was recorded is kept
    lastStopDest_ = engine_.positionSeconds();
    wasPlaying_ = false;
    CaptureTake take;
    take.file = cap_.file;
    take.startSample = startSample;
    take.roundTrip = cap_.roundTrip + chainLatency;
    if (std::unique_ptr<juce::AudioFormatReader> r{engine_.formats().createReaderFor(cap_.file)}) {
        take.rate = r->sampleRate;
        take.length = r->lengthInSamples;
    }
    const int id = placeCapture(session_, cap_.plan, take, io_.captureMute, cap_.file.getFileNameWithoutExtension());
    if (id == 0) {
        engine_.loadSession(session_);
        cap_.message = "!Stopped before the range began, so there is nothing to put on the timeline.";
        recMarks_.clear();
        timeline_.clearLiveTake();
        commands_.commandStatusChanged();
        return;
    }
    cap_.regionId = id;
    cap_.haveTake = true;
    placeRecMarks(id);
    const auto* reg = session_.findRegion(id);
    const double got = reg ? reg->length() : 0.0, want = cap_.plan.range.getLength();
    sessionChanged();
    timeline_.resized();
    timeline_.selectRegion(id);
    engine_.setPositionSeconds(cap_.plan.range.getStart());
    timeline_.setPlayhead(cap_.plan.range.getStart(), false);
    cap_.message = complete || got >= want - 0.01 ? "Captured " + msfString(got) + " onto lane " + juce::String(cap_.plan.underLane + 2) + ", in sync." +
                                                        (io_.captureMute ? " The original is muted." : " The original still plays too: mute one of them (U).")
                                                  : "!Stopped early: kept the first " + msfString(got) + " of " + msfString(want) + ".";
    if (overs > 0) cap_.message = "!" + cap_.message.trimCharactersAtStart("!") + " The return clipped (" + juce::String(overs) + " samples): turn the gear down and capture again.";
    commands_.commandStatusChanged();
}

void MainComponent::captureHear(int which) {
    if (which == 0) { endCaptureCheck(); return; }
    if (cap_.capturing || cap_.levelCheck || !cap_.haveTake || !session_.findRegion(cap_.regionId)) return;
    gearHear(gearAb_, 0);
    if (cap_.hearing == 0) {
        // The engine plays the session as it was sent (through the master plugins) with the
        // capture alongside, in step and untouched; switching between them is instant.
        endCompare();
        engine_.loadSessionWithAlternate(cap_.before, captureAlone(session_, cap_.regionId));
        engine_.resetMatch();
    }
    cap_.hearing = which;
    engine_.setHearUnprocessed(which == 2);
}

void MainComponent::endCaptureCheck() {
    if (cap_.hearing == 0) return;
    cap_.hearing = 0;
    engine_.setHearUnprocessed(false);
    engine_.setLevelMatch(false);
    engine_.loadSession(session_);
}

void MainComponent::captureNullTest() {
    const auto* reg = session_.findRegion(cap_.regionId);
    if (!cap_.haveTake || !reg || cap_.capturing) return;
    if (std::abs(cap_.before.effectiveRate() - reg->fileRate) > 0.5) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Null test",
                                               "The capture was made at " + juce::String(reg->fileRate / 1000.0, 1) + " kHz and the session is " +
                                                   juce::String(cap_.before.effectiveRate() / 1000.0, 1) +
                                                   " kHz, so they can't be compared sample for sample. Capture with the interface at the session's rate.");
        return;
    }
    endCaptureCheck();
    const Region capture = *reg;
    const Session before = cap_.before;
    const juce::Range<double> range(capture.start, capture.end());
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Lathe null test");
    dir.createDirectory();
    const auto sent = dir.getChildFile("sent.wav"), back = dir.getChildFile("capture.wav");
    auto es = exportSettings();
    auto res = std::make_shared<NullResult>();
    auto err = std::make_shared<juce::String>();
    runJob("Null test: the capture against what was sent...",
           [this, capture, before, range, sent, back, es, res, err](Job& j) {
               *err = exporter_.exportRangeWav(before, sent, 32, es, range, [&](double p) { return j.keep(p * 0.5); });
               if (err->isNotEmpty()) return;
               // The captured stretch on its own, as the region plays it.
               std::unique_ptr<juce::AudioFormatReader> r(engine_.formats().createReaderFor(capture.file));
               if (!r) { *err = "Couldn't read " + capture.file.getFileName(); return; }
               back.deleteFile();
               juce::WavAudioFormat wav;
               std::unique_ptr<juce::OutputStream> os(back.createOutputStream().release());
               const int ch = int(r->numChannels);
               const auto layout = ch == 6 ? juce::AudioChannelSet::create5point1() : ch == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
               std::unique_ptr<juce::AudioFormatWriter> w(os ? wav.createWriterFor(os.get(), r->sampleRate, layout, 32, {}, 0) : nullptr);
               if (!w) { *err = "Couldn't write " + back.getFullPathName(); return; }
               os.release();
               w->writeFromAudioReader(*r, capture.srcIn, capture.srcOut - capture.srcIn);
               w.reset();
               *res = nullTest(engine_.formats(), sent, back, juce::File(), [&](double p) { return j.keep(0.5 + p * 0.5); });
           },
           [this, res, err, sent, back](bool cancelled) {
               sent.deleteFile();
               back.deleteFile();
               if (cancelled) return;
               if (err->isNotEmpty() || !res->ok) {
                   juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Null test", err->isNotEmpty() ? *err : res->error);
                   return;
               }
               const auto& r = *res;
               const double left = r.signalRms > 0 && r.residualRmsGainMatched > 0 ? 20.0 * std::log10(r.residualRmsGainMatched / r.signalRms) : -200.0;
               juce::String t;
               t << "THE CAPTURE AGAINST WHAT WAS SENT\n\n";
               t << "Timing     " << (r.offset == 0 ? juce::String("lines up to the sample")
                                                     : "the capture is " + juce::String(std::abs(int(r.offset))) + " samples " + (r.offset > 0 ? "late" : "early") +
                                                           " (" + juce::String(std::abs(double(r.offset)) * 1000.0 / r.rateA, 2) + " ms)")
                 << "\n";
               t << "Polarity   " << (r.inverted ? "INVERTED: a cable or the gear flips the polarity" : "the same") << "\n";
               t << "Level      the capture is " << juce::String(std::abs(r.gainDiffDb), 2) << " dB " << (r.gainDiffDb > 0 ? "quieter" : "louder") << " than what was sent\n";
               t << "Left over  " << (left <= -150.0 ? juce::String("nothing") : juce::String(left, 1) + " dB below the music (after matching the level)") << "\n\n";
               if (r.offset != 0)
                   t << "The capture isn't lined up. Measure the round trip again in the Capture window (with the gear set as it was for the capture) and capture "
                        "again, or nudge the region by the amount above.\n";
               else if (left < -60.0)
                   t << "Patch and alignment are right, and the path is transparent: with the level matched, what came back is what was sent.\n";
               else
                   t << "Patch and alignment are right. What is left over is what the gear (and its converters) did to the sound.\n";
               showText("Null test", t);
           });
}

// ---------------------------------------------------------------- rename

void MainComponent::renameRegion() {
    const bool onSource = sourceActive_;
    const int id = activeTimeline().selectedRegion();
    auto* reg = activeSession().findRegion(id);
    if (!reg) return;
    if (engine_.isRecording() || cap_.capturing || print_.printing) return;
    const auto file = reg->file;
    int uses = 0;
    for (auto& r : session_.regions) uses += r.file == file ? 1 : 0;
    for (auto& r : srcSession_.regions) uses += r.file == file ? 1 : 0;
    auto* w = new juce::AlertWindow("Rename", "The name shown on the region:", juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", reg->name);
    auto tick = std::make_shared<juce::ToggleButton>("Rename the audio file on disk too  (" + file.getFileName() + ")");
    tick->setSize(440, 24);
    tick->setEnabled(file.existsAsFile());
    w->addCustomComponent(tick.get());
    w->addTextBlock(uses > 1 ? juce::String(uses) + " regions use this file; they all follow it. Renaming the file can't be undone with Undo (rename it back instead)."
                             : juce::String("Renaming the file can't be undone with Undo (rename it back instead)."));
    w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, tick, id, onSource, file](int result) {
        if (result != 1) return;
        const auto name = w->getTextEditorContents("name").trim();
        auto& ses = onSource ? srcSession_ : session_;
        auto* r = ses.findRegion(id);
        if (!r || name.isEmpty()) return;
        if (tick->getToggleState()) {
            // The engine has the file open: let go of it, rename, then load the session again.
            engine_.stop();
            endCaptureCheck();
            gearHear(gearAb_, 0);
            juce::File now;
            auto err = renameAudioFile(session_, file, name, now);
            if (err.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Rename", err);
                return;
            }
            if (auto* again = ses.findRegion(id)) again->name = now.getFileNameWithoutExtension();
            // Earlier undo steps still name the old file: point them at the new one, so Undo keeps working.
            const auto was = juce::JSON::toString(juce::var(file.getFullPathName())), is = juce::JSON::toString(juce::var(now.getFullPathName()));
            for (auto* list : {&undo_, &redo_})
                for (auto& st : *list) st = st.replace(was, is);
            lastState_ = lastState_.replace(was, is);
            if (cap_.file == file) cap_.file = now;
            if (print_.file == file) print_.file = now;
        } else {
            r->name = name;
        }
        sessionChanged();
        timeline_.repaint();
        sourceView_.timeline().repaint();
    }), true);
}

// ---------------------------------------------------------------- external gear, print

void MainComponent::addExternalGear() {
    if (session_.programmeChannels() == 6) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "External Gear",
                                               "External Gear is stereo, and a 5.1 programme doesn't use the master chain.");
        return;
    }
    auto err = engine_.addExternalGear();
    if (err.isNotEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "External Gear", err);
        return;
    }
    chainList_.updateContent();
    chainList_.selectRow(engine_.chain().size() - 1);
    engine_.chain().showEditor(engine_.chain().size() - 1);
    sessionChanged();
}

int MainComponent::printCount(int gear) const {
    int n = 0;
    for (auto& r : session_.regions) n += r.afterGear == gear && gear != 0 ? 1 : 0;
    return n;
}

juce::String MainComponent::printStatus(int gear) const {
    if (print_.printing && print_.gear == gear) {
        const double p = engine_.captureProgress();
        return p >= 1.0 ? juce::String("Printing: recording the tail...") : "Printing in real time: " + juce::String(int(p * 100)) + " %. Stop keeps what is recorded.";
    }
    auto& chain = const_cast<AudioEngine&>(engine_).chain();
    if (gearAb_ == gear) {
        juce::String t = chain.hearingPrint() ? "Hearing the print." : "Hearing the live gear.";
        if (chain.compareMeasured()) {
            const double d = -chain.compareDifferenceDb();   // print against live
            t << " The print is " << juce::String(std::abs(d), 1) << " LU " << (d >= 0 ? "louder" : "quieter") << " than the live gear; the louder side is turned down to match.";
        } else {
            t << " Play a few seconds over the print to measure the level difference.";
        }
        return t;
    }
    if (print_.gear == gear && print_.message.isNotEmpty()) return print_.message;
    const int n = printCount(gear);
    if (n == 0)
        return "Print plays the In-Out (or the whole programme) through the gear in real time and records the result onto a new lane, so exports no "
               "longer need the hardware. Until then, exports are refused rather than leaving the gear out.";
    bool usingPrints = false;
    for (auto& r : session_.regions) usingPrints |= r.afterGear == gear && !r.mute;
    const bool stillLive = needsHardware(session_, chain.lastLiveHardwareSlot(), [&chain](int a) { return chain.slotOfAnchor(a); });
    return juce::String(n) + (n == 1 ? " print" : " prints") + " through this insert. " +
           (usingPrints ? "The prints are playing (the material under them is muted)." : "The live gear is playing (the prints are muted).") +
           (stillLive ? " Some material still passes through hardware, so exports are refused until it is printed too." : " Nothing needs the hardware now: exports run at full speed.");
}

void MainComponent::beginPrint(int gear) {
    if (print_.printing) { finishPrint(); return; }   // the button is Stop while printing
    if (cap_.capturing || cap_.levelCheck || engine_.isRecording()) return;
    endCaptureCheck();
    endCompare();
    gearHear(gearAb_, 0);
    auto& chain = engine_.chain();
    const int slot = chain.slotOfAnchor(gear);
    if (slot < 0) return;
    print_.gear = gear;
    print_.message = {};
    if (chain.isBypassed(slot)) { print_.message = "The insert is bypassed. Switch it in to print through it."; return; }
    auto plan = planCapture(session_, timeline_.selection());
    plan.sources = sourcesThrough(session_, plan, slot, [&chain](int a) { return chain.slotOfAnchor(a); });
    if (plan.sources.isEmpty()) {
        print_.message = plan.whole ? "Nothing passes through this insert: it is all printed already." : "Nothing between In and Out passes through this insert (it may be printed already).";
        return;
    }
    plan.channels = 2;
    plan.underLane = 0;
    for (int id : plan.sources)
        if (auto* r = session_.findRegion(id)) plan.underLane = juce::jmax(plan.underLane, r->lane);
    const auto folder = recordingsFolder();
    folder.createDirectory();
    if (!folder.isDirectory()) { print_.message = "Couldn't create " + folder.getFullPathName(); return; }
    const auto need = recordingBytes(plan.range.getLength() + 2.0 + io_.capturePostRoll + 3.0, engine_.deviceRate(), 2, io_.bits);
    const auto freeBytes = folder.getBytesFreeOnVolume();
    if (freeBytes > 0 && freeBytes < need + (juce::int64(64) << 20)) {
        print_.message = "Not enough room: the print needs " + byteSize(need) + " and " + byteSize(freeBytes) + " is free in " + folder.getFullPathName() + ".";
        return;
    }
    print_.file = folder.getNonexistentChildFile(recordingName(session_.file, "Print", juce::Time::getCurrentTime()), ".wav", false);
    print_.plan = plan;
    AudioEngine::PrintRequest rq;
    rq.file = print_.file;
    rq.gear = gear;
    rq.bits = io_.bits;
    rq.from = plan.range.getStart();
    rq.to = plan.range.getEnd();
    rq.preRoll = 2.0;
    rq.postRoll = io_.capturePostRoll;
    juce::String err;
    if (!engine_.startPrint(rq, session_, err)) {
        engine_.loadSession(session_);
        print_.message = err;
        return;
    }
    print_.printing = true;
    recMarks_.clear();
    commands_.commandStatusChanged();
}

void MainComponent::finishPrint() {
    if (!print_.printing) return;
    print_.printing = false;
    const bool complete = engine_.captureComplete();
    CaptureTake take;
    take.file = print_.file;
    take.startSample = engine_.recordStartSample();
    take.roundTrip = engine_.chain().latencyThrough(print_.gear);   // what left the insert was this late
    engine_.stopCapture();
    lastStopDest_ = engine_.positionSeconds();
    wasPlaying_ = false;
    if (std::unique_ptr<juce::AudioFormatReader> r{engine_.formats().createReaderFor(print_.file)}) {
        take.rate = r->sampleRate;
        take.length = r->lengthInSamples;
    }
    juce::Array<int> muted;
    const int id = placeCapture(session_, print_.plan, take, true, print_.file.getFileNameWithoutExtension(), &muted);
    if (id == 0) {
        engine_.loadSession(session_);
        print_.message = "Stopped before the range began, so there is nothing to put on the timeline.";
        recMarks_.clear();
        timeline_.clearLiveTake();
        commands_.commandStatusChanged();
        return;
    }
    double got = 0;
    if (auto* reg = session_.findRegion(id)) {
        reg->afterGear = print_.gear;
        reg->printOf = muted;
        got = reg->length();
    }
    placeRecMarks(id);
    sessionChanged();
    timeline_.resized();
    timeline_.selectRegion(id);
    engine_.setPositionSeconds(print_.plan.range.getStart());
    timeline_.setPlayhead(print_.plan.range.getStart(), false);
    const double want = print_.plan.range.getLength();
    print_.message = (complete || got >= want - 0.01 ? "Printed " + msfString(got) : "Stopped early: kept the first " + msfString(got) + " of " + msfString(want)) +
                     " onto lane " + juce::String(print_.plan.underLane + 2) + ". The print is playing; Use live gear switches back, and Undo removes it.";
    commands_.commandStatusChanged();
}

void MainComponent::gearHear(int gear, int which) {
    auto& chain = engine_.chain();
    if (which == 0 || gear == 0) {
        if (gearAb_ == 0) return;
        gearAb_ = 0;
        chain.setCompareAt(0);
        chain.setHearPrint(false);
        engine_.loadSession(session_);
        return;
    }
    if (print_.printing || cap_.capturing || cap_.levelCheck || printCount(gear) == 0) return;
    if (gearAb_ != gear) {
        // Both at once: the material goes through the gear live while its print runs beside it,
        // in step. At the insert you hear one or the other, level-matched.
        endCompare();
        endCaptureCheck();
        if (gearAb_ != 0) gearHear(gearAb_, 0);
        Session both = session_;
        both.source = nullptr;
        both.usePrints(gear, true);
        for (auto& r : session_.regions)
            if (r.afterGear == gear)
                for (int src : r.printOf)
                    if (auto* x = both.findRegion(src)) x->mute = false;
        engine_.loadSession(both);
        chain.setCompareLevelMatch(true);
        chain.setCompareAt(gear);
        gearAb_ = gear;
    }
    chain.setHearPrint(which == 2);
}

// ---------------------------------------------------------------- control surfaces

namespace {
class SurfacePanel : public juce::Component, private juce::TableListBoxModel, private juce::Timer {
public:
    SurfacePanel(ControlSurface& s) : s_(s) {
        ins_ = juce::MidiInput::getAvailableDevices();
        outs_ = juce::MidiOutput::getAvailableDevices();
        in_.addItem("(none)", 1);
        for (int i = 0; i < ins_.size(); ++i) { in_.addItem(ins_[i].name, i + 2); if (ins_[i].identifier == s_.inputId()) in_.setSelectedId(i + 2, juce::dontSendNotification); }
        if (in_.getSelectedId() == 0) in_.setSelectedId(1, juce::dontSendNotification);
        out_.addItem("(none)", 1);
        for (int i = 0; i < outs_.size(); ++i) { out_.addItem(outs_[i].name, i + 2); if (outs_[i].identifier == s_.outputId()) out_.setSelectedId(i + 2, juce::dontSendNotification); }
        if (out_.getSelectedId() == 0) out_.setSelectedId(1, juce::dontSendNotification);
        proto_.addItem("Mackie Control (MCU and compatibles)", 1);
        proto_.addItem("HUI (keep-alive; assign buttons with Learn)", 2);
        proto_.addItem("MIDI learn only", 3);
        proto_.setSelectedId(int(s_.protocol()) + 1, juce::dontSendNotification);
        auto apply = [this] {
            s_.configure(in_.getSelectedId() > 1 ? ins_[in_.getSelectedId() - 2].identifier : juce::String(),
                         out_.getSelectedId() > 1 ? outs_[out_.getSelectedId() - 2].identifier : juce::String(),
                         ControlSurface::Protocol(proto_.getSelectedId() - 1));
            status_.setText(s_.status(), juce::dontSendNotification);
        };
        in_.onChange = apply;
        out_.onChange = apply;
        proto_.onChange = apply;
        for (auto* c : {&in_, &out_, &proto_}) addAndMakeVisible(*c);
        for (auto* l : {&inL_, &outL_, &protoL_, &status_, &hint_}) { l->setColour(juce::Label::textColourId, colours::dim); addAndMakeVisible(*l); }
        inL_.setText("MIDI in", juce::dontSendNotification);
        outL_.setText("MIDI out", juce::dontSendNotification);
        protoL_.setText("Protocol", juce::dontSendNotification);
        status_.setText(s_.status(), juce::dontSendNotification);
        hint_.setText("Mackie Control works out of the box (transport, jog, cursor/zoom, Marker, F1-F8, Save, Undo, timecode display). "
                      "To assign anything else, select a command and press Learn, then press a button or move a control.",
                      juce::dontSendNotification);
        hint_.setFont(juce::FontOptions(12.0f));
        hint_.setJustificationType(juce::Justification::topLeft);
        table_.getHeader().addColumn("Command", 1, 300);
        table_.getHeader().addColumn("Assigned to", 2, 200);
        table_.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, colours::bg);
        table_.getHeader().setColour(juce::TableHeaderComponent::textColourId, colours::dim);
        table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        table_.setRowHeight(22);
        addAndMakeVisible(table_);
        for (auto* b : {&learn_, &clear_}) { styleButton(*b, b == &learn_); addAndMakeVisible(*b); }
        learn_.onClick = [this] {
            int r = table_.getSelectedRow();
            if (r < 0) return;
            s_.learn(allCommands()[size_t(r)].id);
            learn_.setButtonText("Waiting for MIDI...");
        };
        clear_.onClick = [this] { int r = table_.getSelectedRow(); if (r >= 0) { s_.clearMapping(allCommands()[size_t(r)].id); table_.repaint(); } };
        s_.onLearned = [this] { learn_.setButtonText("Learn"); table_.repaint(); };
        startTimerHz(5);
        setSize(620, 560);
    }
    ~SurfacePanel() override {
        s_.onLearned = nullptr;
        closeList(table_);
    }
    void paint(juce::Graphics& g) override { g.fillAll(colours::bg); }
    void resized() override {
        auto r = getLocalBounds().reduced(10);
        auto row = [&](juce::Label& l, juce::Component& c) { auto x = r.removeFromTop(26); l.setBounds(x.removeFromLeft(80)); c.setBounds(x); r.removeFromTop(4); };
        row(inL_, in_);
        row(outL_, out_);
        row(protoL_, proto_);
        status_.setBounds(r.removeFromTop(20));
        hint_.setBounds(r.removeFromTop(40));
        auto b = r.removeFromBottom(28);
        learn_.setBounds(b.removeFromLeft(160));
        b.removeFromLeft(6);
        clear_.setBounds(b.removeFromLeft(90));
        r.removeFromBottom(6);
        table_.setBounds(r);
    }
private:
    int getNumRows() override { return int(allCommands().size()); }
    void paintRowBackground(juce::Graphics& g, int, int, int, bool sel) override { g.fillAll(sel ? colours::accent.withAlpha(0.25f) : colours::panel); }
    void paintCell(juce::Graphics& g, int row, int col, int w, int h, bool) override {
        auto& d = allCommands()[size_t(row)];
        g.setColour(col == 1 ? colours::text : colours::accent);
        g.setFont(13.0f);
        g.drawText(col == 1 ? juce::String(d.category) + " - " + d.name : s_.mappingFor(d.id), 6, 0, w - 10, h, juce::Justification::centredLeft);
    }
    void timerCallback() override { if (!s_.isLearning()) learn_.setButtonText("Learn"); }
    ControlSurface& s_;
    juce::Array<juce::MidiDeviceInfo> ins_, outs_;
    juce::ComboBox in_, out_, proto_;
    juce::Label inL_, outL_, protoL_, status_, hint_;
    juce::TableListBox table_{"Mappings", this};
    juce::TextButton learn_{"Learn"}, clear_{"Clear"};
};
}  // namespace

void MainComponent::showControlSurfaces() {
    surfaceWindow_.reset();
    auto* w = new PlainWindow(new SurfacePanel(*surface_), "Control Surfaces / MIDI", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { surfaceWindow_.reset(); }); };
    surfaceWindow_.reset(w);
}

// ---------------------------------------------------------------- restoration / spectral

juce::File MainComponent::processedFolder() const {
    if (session_.file.existsAsFile())
        return session_.file.getSiblingFile(session_.file.getFileNameWithoutExtension() + " Processed");
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Lathe Processed");
}

int MainComponent::targetRegion() const {
    if (int id = timeline_.selectedRegion()) return id;
    double t = engine_.positionSeconds();
    for (auto& r : session_.regions)
        if (r.lane == timeline_.selectedLane() && t >= r.start && t < r.end()) return r.id;
    return 0;
}

void MainComponent::showRestore(int id) {
    auto* r = session_.findRegion(id);
    if (!r) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Restore", "Select a region first (click it on the timeline).");
        return;
    }
    restoreWindow_.reset();
    auto sel = timeline_.selection();
    bool overlaps = !sel.isEmpty() && sel.getEnd() > r->start && sel.getStart() < r->end();
    auto* p = new RestorePanel(r->name, noise_ && noise_->valid(), overlaps);
    juce::Component::SafePointer<RestorePanel> safe(p);
    p->onLearnNoise = [this, id, safe] {
        auto* reg = session_.findRegion(id);
        auto s = timeline_.selection();
        if (!reg || s.isEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Learn noise",
                                                   "Drag across a stretch of the region that contains only the noise (a pause, the run-in), then press Learn.");
            return;
        }
        Region part = *reg;
        auto a = juce::jlimit<juce::int64>(reg->srcIn, reg->srcOut, reg->srcIn + juce::int64((s.getStart() - reg->start) * reg->fileRate));
        auto b = juce::jlimit<juce::int64>(a, reg->srcOut, reg->srcIn + juce::int64((s.getEnd() - reg->start) * reg->fileRate));
        part.srcIn = a;
        part.srcOut = b;
        dsp::Channels ch;
        juce::int64 first = 0;
        if (!readRegionAudio(engine_.formats(), part, ch, first, 0)) return;
        auto np = std::make_shared<dsp::NoiseProfile>(dsp::learnNoise(ch, reg->fileRate));
        if (!np->valid()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Learn noise", "The selection is too short; use at least 0.2 s of noise.");
            return;
        }
        noise_ = np;
        if (safe) safe->setProfileStatus("Noise profile: learned from " + juce::String(s.getLength(), 2) + " s at " + msfString(s.getStart()));
    };
    p->onProcess = [this, id, safe] {
        auto* reg = session_.findRegion(id);
        if (!reg || !safe) return;
        auto settings = safe->settings();
        auto range = safe->selectionOnly() ? timeline_.selection() : juce::Range<double>();
        auto snapshot = std::make_shared<Region>(*reg);
        auto noise = noise_;
        auto folder = processedFolder();
        auto res = std::make_shared<RestoreResult>();
        runJob("Restoring " + reg->name + "...",
               [this, snapshot, range, settings, noise, folder, res](Job& j) {
                   *res = restoreRegion(engine_.formats(), *snapshot, range, settings, noise.get(), folder, [&](double p) { return j.keep(p); });
               },
               [this, id, res](bool) {
                   if (!res->ok) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Restore", res->error); return; }
                   applyProcessed(session_, id, *res, "restored");
                   sessionChanged();
                   juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Restore",
                                                          res->report + "\n\nPress Cmd+Z to compare with the original, Cmd+Shift+Z to come back.");
               });
    };
    auto* w = new PlainWindow(p, "Restore", true);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { restoreWindow_.reset(); }); };
    restoreWindow_.reset(w);
}

void MainComponent::showSpectral(int id) {
    auto* r = session_.findRegion(id);
    if (!r) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Spectral Editor", "Select a region first (click it on the timeline).");
        return;
    }
    spectralWindow_.reset();
    // Show the destination selection if it's inside this region, otherwise the 20 s around the playhead.
    auto sel = timeline_.selection();
    juce::Range<double> view;
    if (!sel.isEmpty() && sel.getEnd() > r->start && sel.getStart() < r->end())
        view = {juce::jmax(0.0, sel.getStart() - r->start), juce::jmin(r->length(), sel.getEnd() - r->start)};
    else {
        double c = juce::jlimit(0.0, r->length(), engine_.positionSeconds() - r->start);
        view = {juce::jmax(0.0, c - 10.0), juce::jmin(r->length(), c + 10.0)};
    }
    auto* v = new SpectralView(engine_, session_, id, view);
    juce::Component::SafePointer<SpectralView> safe(v);
    v->onAudition = [this](double a, double b) { auditionRange(a, b); };
    v->onApply = [this, id, safe](double t0, double t1, double f0, double f1, dsp::SpectralMode m, double g) {
        auto* reg = session_.findRegion(id);
        if (!reg) return;
        auto snapshot = std::make_shared<Region>(*reg);
        auto folder = processedFolder();
        auto res = std::make_shared<RestoreResult>();
        runJob("Spectral edit...",
               [this, snapshot, t0, t1, f0, f1, m, g, folder, res](Job&) { *res = spectralEditRegion(engine_.formats(), *snapshot, t0, t1, f0, f1, m, g, folder); },
               [this, id, res, safe](bool) {
                   if (!res->ok) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Spectral edit", res->error); return; }
                   applyProcessed(session_, id, *res, "spectral");
                   sessionChanged();
                   if (safe) safe->refresh();
               });
    };
    auto* w = new PlainWindow(v, "Spectral Editor - " + r->name, false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { spectralWindow_.reset(); }); };
    spectralWindow_.reset(w);
}

void MainComponent::jumpToMarker(int dir) {
    double pos = markTime();
    double best = dir < 0 ? -1.0 : 1e12;
    for (auto& m : activeSession().markers) {
        if (dir < 0 && m.t < pos - 0.02 && m.t > best) best = m.t;
        if (dir > 0 && m.t > pos + 0.02 && m.t < best) best = m.t;
    }
    if (dir < 0 && best < 0) best = 0;
    if (dir > 0 && best > 1e11) return;
    locate(best);
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
    engine_.regionFx().syncAndCapture(session_);
    if (fxWindow_ && !session_.findRegion(fxWindowRegion_)) fxWindow_.reset();
    session_.pluginChainState = engine_.chain().getState();
    auto now = juce::JSON::toString(session_.toVar(), true);
    if (now == lastState_) { timeline_.repaint(); sourceView_.timeline().repaint(); return; }
    if (undoable && lastState_.isNotEmpty()) {
        undo_.add(lastState_);
        if (undo_.size() > 300) undo_.remove(0);
        redo_.clear();
    }
    lastState_ = now;
    dirty_ = true;
    if (analysis_ && !analysisStale_) { analysisStale_ = true; timeline_.setAnalysisStale(true); }
    applyFollowRate();
    engine_.loadSession(session_);
    if (gearAb_ != 0) {   // (the engine was playing the live gear and its print side by side)
        gearAb_ = 0;
        engine_.chain().setCompareAt(0);
        engine_.chain().setHearPrint(false);
    }
    if (cap_.hearing != 0) {   // (the engine was playing the session as sent, against the capture)
        cap_.hearing = 0;
        engine_.setHearUnprocessed(false);
        engine_.setLevelMatch(false);
    }
    if (auto st = juce::JSON::toString(srcSession_.toVar(), true); st != lastSourceState_) {
        lastSourceState_ = st;
        engine_.loadSourceSession(srcSession_);
    }
    timeline_.sessionUpdated();
    sourceView_.timeline().sessionUpdated();
    pqList_.updateContent();
    pqList_.repaint();
    refreshIssues();
    if (xfadeEditor_) {
        if (xfadeEditor_->stillValid()) xfadeEditor_->refresh();
        else juce::MessageManager::callAsync([this] { xfadeWindow_.reset(); });
    }
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
    fxWindow_.reset();
    {
        juce::StringArray problems;
        engine_.regionFx().loadFrom(session_, problems);
    }
    applyFollowRate();
    engine_.loadSession(session_);
    engine_.loadSourceSession(srcSession_);
    lastSourceState_ = juce::JSON::toString(srcSession_.toVar(), true);
    timeline_.sessionUpdated();
    timeline_.resized();
    sourceView_.timeline().sessionUpdated();
    sourceView_.timeline().resized();
    pqList_.updateContent();
    pqList_.repaint();
    chainList_.updateContent();
    chainList_.repaint();
    refreshIssues();
    refreshDeliveryPanels();
    dirty_ = true;
}

void MainComponent::newSession() {
    engine_.stop();
    engine_.clearReference();
    analysis_.reset();
    rawAnalysis_.reset();
    timeline_.setAnalysis(nullptr, nullptr);
    endCompare();
    engine_.resetMatch();
    refWindow_.reset();
    analysisWindow_.reset();
    session_ = Session();
    attachSource();
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
    engine_.clearReference();
    analysis_.reset();
    rawAnalysis_.reset();
    timeline_.setAnalysis(nullptr, nullptr);
    endCompare();
    engine_.resetMatch();
    refWindow_.reset();
    analysisWindow_.reset();
    restoreWindow_.reset();
    spectralWindow_.reset();
    session_ = s;
    attachSource();
    // Audio that moved with the session (same folder, or an Audio folder beside it) is found silently.
    const auto dir = f.getParentDirectory();
    const int foundBeside = session_.findMissingAudioIn({dir, dir.getChildFile("Audio"), dir.getChildFile("Audio Files")});
    juce::StringArray problems;
    engine_.chain().setState(session_.pluginChainState, problems);
    if (juce::isPositiveAndBelow(session_.activeReference, session_.references.size()))
        engine_.loadReference(session_.references[session_.activeReference].file);
    const bool missing = !session_.missingAudio().isEmpty();
    undo_.clear();
    redo_.clear();
    lastState_ = juce::JSON::toString(session_.toVar(), true);
    refreshAll();
    timeline_.zoomToFit();
    dirty_ = foundBeside > 0;   // new locations: worth saving
    if (!problems.isEmpty()) showText("Session opened with plugin problems", "Plugin problems:\n" + problems.joinIntoString("\n"));
    if (missing) juce::MessageManager::callAsync([this] { locateMissingAudio(false); });
    return true;
}

void MainComponent::openSession() {
    chooser_ = std::make_unique<juce::FileChooser>("Open session", juce::File(), "*.lathe");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadSessionFile(fc.getResult()); });
}

bool MainComponent::saveSession(bool saveAs) {
    engine_.regionFx().syncAndCapture(session_);
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
    engine_.regionFx().syncAndCapture(session_);
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
        int id = session_.append(reg, session_.autoSpaceGap);
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

void MainComponent::attachSource() {
    session_.source = &srcSession_;
    // Global locks follow you into every session.
    if (props_.getUserSettings()->getBoolValue("lockMarks", false)) session_.lockMarks = true;
    if (props_.getUserSettings()->getBoolValue("lockGain", false)) session_.lockGain = true;
    juce::String err;
    if (!session_.sourceDoc.isObject() || !srcSession_.fromVar(session_.sourceDoc, err)) {
        srcSession_ = Session();
        srcSession_.lanes = 1;
    }
    srcSession_.source = nullptr;
    srcSession_.lockGain = session_.lockGain;
    sourceView_.timeline().clearInOut();
    sourceView_.timeline().setEditPoint(0);
    timeline_.clearInOut();
    timeline_.setEditPoint(0);
    sourceView_.timeline().zoomToFit();
}

void MainComponent::showSourceOpenDialog() {
    chooser_ = std::make_unique<juce::FileChooser>("Add to the source timeline", juce::File(), engine_.formats().getWildcardForAllFormats());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser& fc) { openSourceFiles(fc.getResults()); });
}

void MainComponent::openSourceFiles(const juce::Array<juce::File>& files) {
    juce::StringArray bad;
    int first = 0;
    for (auto& f : files) {
        std::unique_ptr<juce::AudioFormatReader> r(engine_.formats().createReaderFor(f));
        if (!r) { bad.add(f.getFileName()); continue; }
        Region reg = srcSession_.makeRegion(f, r->sampleRate, r->lengthInSamples);
        // Each file after the last, one second apart, on the first targeted lane.
        reg.lane = srcSession_.targetLanes().getFirst();
        double laneEnd = 0;
        bool any = false;
        for (auto& o : srcSession_.regions)
            if (o.lane == reg.lane) { laneEnd = juce::jmax(laneEnd, o.end()); any = true; }
        reg.start = any ? laneEnd + 1.0 : 0.0;
        srcSession_.regions.add(reg);
        if (!first) first = reg.id;
    }
    if (first) {
        auto& st = sourceView_.timeline();
        if (auto* r = srcSession_.findRegion(first)) {
            st.setEditPoint(r->start);
            st.selectRegion(first);
        }
        setActiveTimeline(true);
        sessionChanged();
        st.zoomToFit();
    }
    if (!bad.isEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Some files couldn't be read", bad.joinIntoString("\n"));
}

void MainComponent::setActiveTimeline(bool source) {
    sourceActive_ = source;
    sourceView_.timeline().setActive(source);
    timeline_.setActive(!source);
    engine_.setHearSource(source);
}

double MainComponent::markTime() { return activePlaying() ? activePosition() : activeTimeline().editPoint(); }

void MainComponent::locate(double t) {
    t = juce::jmax(0.0, t);
    if (sourceActive_) {
        sourceView_.timeline().setEditPoint(t, true);
        engine_.setSourcePosition(t);
        sourceView_.timeline().setPlayhead(t, false);
    } else {
        locateDest(t);
    }
}

void MainComponent::locateDest(double t) {
    t = juce::jmax(0.0, t);
    timeline_.setEditPoint(t, true);
    engine_.setPositionSeconds(t);
    timeline_.setPlayhead(t, false);
}

void MainComponent::stopAll() {
    if (print_.printing) { finishPrint(); return; }    // keeps what was recorded
    if (cap_.capturing) { finishCapture(); return; }   // keeps what was recorded
    if (cap_.levelCheck) captureLevelCheck(false);
    lastStopDest_ = engine_.positionSeconds();
    lastStopSource_ = engine_.sourcePosition();
    auditionEnd_ = -1;
    engine_.stop();
    afterStop();
    wasPlaying_ = wasSourcePlaying_ = false;
}

void MainComponent::afterStop() {
    // Either the Edit Point moves to where playback stopped, or the playhead goes back to it.
    if (engine_.isPlaying() || engine_.isSourcePlaying()) return;
    auto& st = sourceView_.timeline();
    if (editFollowsStop_) {
        timeline_.setEditPoint(lastStopDest_);
        st.setEditPoint(lastStopSource_);
    } else {
        engine_.setPositionSeconds(timeline_.editPoint());
        engine_.setSourcePosition(st.editPoint());
    }
    timeline_.setPlayhead(engine_.positionSeconds(), false);
    st.setPlayhead(engine_.sourcePosition(), false);
}

void MainComponent::togglePlay() {
    if (engine_.isPlaying() || engine_.isSourcePlaying()) { stopAll(); return; }
    auditionEnd_ = -1;
    auto& st = sourceView_.timeline();
    // Repeat with an In-Out on the active timeline loops it.
    if (repeat_ && !activeTimeline().selection().isEmpty() && !sourceView_.linked()) {
        auto r = activeTimeline().selection();
        playRange(sourceActive_, r.getStart(), r.getEnd());
        return;
    }
    if (sourceView_.linked() && !srcSession_.regions.isEmpty()) {
        engine_.setPositionSeconds(timeline_.editPoint());
        engine_.setSourcePosition(st.editPoint());
        engine_.setHearSource(sourceActive_);
        engine_.playLinked();
    } else if (sourceActive_) {
        engine_.setSourcePosition(st.editPoint());
        engine_.playSource();
    } else {
        engine_.setPositionSeconds(timeline_.editPoint());
        engine_.play();
    }
}

void MainComponent::playRange(bool source, double from, double to) {
    if (to <= from) return;
    from = juce::jmax(0.0, from);
    auditionSource_ = source;
    auditionStart_ = from;
    auditionEnd_ = to;
    if (source) {
        engine_.setSourcePosition(from);
        engine_.playSource();
    } else {
        engine_.setPositionSeconds(from);
        engine_.play();
    }
    wasPlaying_ = engine_.isPlaying();
    wasSourcePlaying_ = engine_.isSourcePlaying();
}

void MainComponent::playRelative(int which) {
    auto& t = activeTimeline();
    const double e = t.editPoint();
    const double in = t.inPoint() >= 0 ? t.inPoint() : e, out = t.outPoint() >= 0 ? t.outPoint() : e;
    switch (which) {
        case 0: playRange(sourceActive_, in - preRoll_, in); break;
        case 1: playRange(sourceActive_, in, in + postRoll_); break;
        case 2: playRange(sourceActive_, out - preRoll_, out); break;
        case 3: playRange(sourceActive_, out, out + postRoll_); break;
        default: playRange(sourceActive_, e - preRoll_, e + postRoll_); break;
    }
}

juce::Array<int> MainComponent::editLanes(const Session& s, const Timeline& t, int clipLanes) const {
    auto targets = s.targetLanes();
    // A one-lane clip goes on the selected lane when that lane is targeted, else the first target.
    if (clipLanes <= 1) return {targets.contains(t.selectedLane()) ? t.selectedLane() : targets.getFirst()};
    juce::Array<int> out;
    for (int l : targets)
        if (l >= t.selectedLane() || out.size() + (targets.size() - targets.indexOf(l)) <= clipLanes) out.add(l);
    return out.isEmpty() ? targets : out;
}

void MainComponent::sourceEdit(EditKind kind) {
    auto& st = sourceView_.timeline();
    if (srcSession_.regions.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Nothing in the source",
                                               "Open or drop audio on the source timeline (above the destination), mark its In and Out, then edit.");
        return;
    }
    double sIn = st.inPoint(), sOut = st.outPoint();
    const double dIn = timeline_.inPoint(), dOut = timeline_.outPoint();
    const bool destRange = dIn >= 0 && dOut > dIn;
    // Source range: In-Out, else the selected source region, else everything.
    if (sIn < 0 && sOut < 0) {
        if (auto* r = srcSession_.findRegion(st.selectedRegion())) { sIn = r->start; sOut = r->end(); }
        else { sIn = 0; sOut = srcSession_.end(); }
    } else if (sOut < 0) {   // 3-point: the destination range gives the length
        sOut = destRange && kind != EditKind::Fit ? sIn + (dOut - dIn) : srcSession_.end();
    } else if (sIn < 0) {    // back-timed source
        sIn = juce::jmax(0.0, destRange && kind != EditKind::Fit ? sOut - (dOut - dIn) : 0.0);
    }
    if (sOut <= sIn) return;
    if ((kind == EditKind::Fit || kind == EditKind::Constrained) && !destRange) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, kind == EditKind::Fit ? "Fit needs a destination In and Out" : "Replace Constrained needs a destination In and Out",
                                               "Mark the destination In and Out (drag across the destination, or Shift+I / Shift+O), then try again.");
        return;
    }
    Clip clip = srcSession_.copyRange(sIn, sOut, srcSession_.targetLanes());
    if (clip.regions.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "No audio there", "The source In-Out has no audio on its targeted lanes.");
        return;
    }
    auto lanes = editLanes(session_, timeline_, clip.laneCount);
    const double at = dIn >= 0 ? dIn : timeline_.editPoint();
    juce::Array<int> ids;
    double endT = at + clip.length;
    switch (kind) {
        case EditKind::Insert: ids = session_.paste(clip, lanes, at, PasteMode::Insert); break;
        case EditKind::Replace: ids = session_.paste(clip, lanes, at, PasteMode::Replace); break;
        case EditKind::Overlay: ids = session_.paste(clip, lanes, at, PasteMode::Overlay); break;
        case EditKind::Constrained:
            ids = session_.paste(clip, lanes, dIn, PasteMode::Constrained, dOut);
            endT = juce::jmin(dOut, dIn + clip.length);
            break;
        case EditKind::ReplaceLeft: {
            const double end = dOut >= 0 ? dOut : (dIn >= 0 ? dIn : timeline_.editPoint());
            ids = session_.paste(clip, lanes, at, PasteMode::ReplaceLeft, end);
            endT = end;
            break;
        }
        case EditKind::Fit: {
            juce::Array<int> used;
            for (int k = 0; k < clip.laneCount && k < lanes.size(); ++k) used.add(lanes[k]);
            session_.clearRange(dIn, dOut, used, false);
            session_.ripple(dOut - 1e-4, clip.length - (dOut - dIn), 0, &used);
            ids = session_.paste(clip, lanes, dIn, PasteMode::Overlay);
            break;
        }
    }
    crossfadeNewEdits(ids, session_);
    if (!ids.isEmpty()) timeline_.selectRegion(ids.getFirst());
    timeline_.clearInOut();
    locateDest(endT);   // ready for the next edit
    setActiveTimeline(false);
    sessionChanged();
}

void MainComponent::appendSource() {
    auto& st = sourceView_.timeline();
    if (srcSession_.regions.isEmpty()) return;
    auto r = st.selection();
    if (r.isEmpty()) {
        if (auto* reg = srcSession_.findRegion(st.selectedRegion())) r = {reg->start, reg->end()};
        else r = {0.0, srcSession_.end()};
    }
    Clip clip = srcSession_.copyRange(r.getStart(), r.getEnd(), srcSession_.targetLanes());
    if (clip.regions.isEmpty()) return;
    double laneEnd = 0;
    bool any = false;
    for (auto& o : session_.regions) { laneEnd = juce::jmax(laneEnd, o.end()); any = true; }
    const double at = any ? laneEnd + 2.0 : 2.0;
    auto ids = session_.paste(clip, editLanes(session_, timeline_, clip.laneCount), at, PasteMode::Overlay);
    if (!ids.isEmpty()) {
        const int m = session_.addMarker(Marker::TrackStart, at, ids.getFirst());
        if (auto* mk = session_.findMarker(m)) {
            mk->text.title = clip.regions.getFirst().name.toStdString();
            mk->text.performer = session_.discPerformer.toStdString();
        }
        if (any) session_.addMarker(Marker::PauseStart, laneEnd, ids.getFirst());
    }
    sessionChanged();
}

void MainComponent::copyActive(bool cut) {
    auto& t = activeTimeline();
    auto& ss = activeSession();
    auto r = t.selection();
    if (r.isEmpty())
        if (auto* reg = ss.findRegion(t.selectedRegion())) r = {reg->start, reg->end()};
    if (r.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, cut ? "Cut" : "Copy", "Mark an In and Out (or select a region) first.");
        return;
    }
    clip_ = ss.copyRange(r.getStart(), r.getEnd(), ss.targetLanes());
    commands_.commandStatusChanged();
    if (!cut) return;
    ss.clearRange(r.getStart(), r.getEnd(), ss.targetLanes(), true);
    {
        juce::Array<int> joined;   // what now starts at the cut: crossfade the new splice
        for (auto& reg : ss.regions)
            if (std::abs(reg.start - r.getStart()) < 1e-3) joined.add(reg.id);
        crossfadeNewEdits(joined, ss);
    }
    t.clearInOut();
    t.selectRegion(0);
    locate(r.getStart());
    sessionChanged();
}

void MainComponent::eraseActive() {
    auto& t = activeTimeline();
    auto r = t.selection();
    if (r.isEmpty()) return;
    activeSession().clearRange(r.getStart(), r.getEnd(), activeSession().targetLanes(), false);
    sessionChanged();
}

void MainComponent::pasteActive(PasteMode mode) {
    if (clip_.isEmpty()) return;
    auto& t = activeTimeline();
    auto& ss = activeSession();
    const double in = t.inPoint(), out = t.outPoint();
    if (mode == PasteMode::Constrained && !(in >= 0 && out > in)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Paste Constrained", "Mark an In and Out to paste into.");
        return;
    }
    const double at = in >= 0 ? in : t.editPoint();
    const double destOut = mode == PasteMode::ReplaceLeft ? (out >= 0 ? out : (in >= 0 ? in : t.editPoint())) : out;
    auto ids = ss.paste(clip_, editLanes(ss, t, clip_.laneCount), at, mode, destOut);
    crossfadeNewEdits(ids, ss);
    if (!ids.isEmpty()) t.selectRegion(ids.getFirst());
    double end = at + clip_.length;
    if (mode == PasteMode::ReplaceLeft) end = destOut;
    if (mode == PasteMode::Constrained) end = juce::jmin(out, at + clip_.length);
    t.clearInOut();
    locate(end);
    sessionChanged();
}

void MainComponent::nudgePoint(bool in, int dir) {
    auto& t = activeTimeline();
    const double d = nudgeSizes_[nudgeSize_] * dir;
    if (in) {
        if (t.inPoint() < 0) return;
        const double o = t.outPoint();
        const double v = juce::jmax(0.0, t.inPoint() + d);
        if (o >= 0 && v >= o) return;
        t.setInPoint(v);
        if (o >= 0) t.setOutPoint(o);
        t.reveal(v);
        playRange(sourceActive_, v, v + postRoll_);   // audition the new In
    } else {
        if (t.outPoint() < 0) return;
        const double v = juce::jmax(0.0, t.outPoint() + d);
        if (t.inPoint() >= 0 && v <= t.inPoint()) return;
        t.setOutPoint(v);
        t.reveal(v);
        playRange(sourceActive_, v - preRoll_, v);   // audition up to the new Out
    }
}

void MainComponent::showMovePoint(int which0) {
    auto& t = activeTimeline();
    auto* w = new juce::AlertWindow("Move Point", "Move a point on the " + juce::String(sourceActive_ ? "source" : "destination") +
                                                      " timeline by an amount (e.g. +0.250, -1.5, +3f for CD frames) or to a time (m:ss.fff or m:ss:ff).",
                                    juce::MessageBoxIconType::NoIcon);
    juce::StringArray pts{"Edit Point", "In", "Out", "Selected SRP"};
    w->addComboBox("point", pts, "Point");
    w->getComboBoxComponent("point")->setSelectedItemIndex(which0);
    w->addTextEditor("amount", "+0.000", "Move by / to");
    w->addButton("Move", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, &t](int r) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (r != 1) return;
        const int which = w->getComboBoxComponent("point")->getSelectedItemIndex();
        auto txt = w->getTextEditorContents("amount").trim();
        double cur = which == 0 ? t.editPoint() : which == 1 ? t.inPoint() : which == 2 ? t.outPoint() : -1;
        Srp* srp = nullptr;
        if (which == 3) { srp = activeSession().findSrp(t.selectedSrp()); if (srp) cur = srp->t; }
        if (cur < 0) return;
        double v;
        auto secondsOf = [](juce::String x) {
            x = x.trim();
            if (x.endsWithIgnoreCase("f")) return x.dropLastCharacters(1).getDoubleValue() / 75.0;
            if (x.endsWithIgnoreCase("ms")) return x.dropLastCharacters(2).getDoubleValue() / 1000.0;
            auto parts = juce::StringArray::fromTokens(x, ":", "");
            if (parts.size() == 3) return parts[0].getDoubleValue() * 60 + parts[1].getDoubleValue() + parts[2].getDoubleValue() / 75.0;
            if (parts.size() == 2) return parts[0].getDoubleValue() * 60 + parts[1].getDoubleValue();
            return x.getDoubleValue();
        };
        if (txt.startsWith("+") || txt.startsWith("-")) v = cur + (txt.startsWith("-") ? -1.0 : 1.0) * secondsOf(txt.substring(1));
        else v = secondsOf(txt);
        v = juce::jmax(0.0, v);
        switch (which) {
            case 0: locate(v); break;
            case 1: { const double o = t.outPoint(); t.setInPoint(v); if (o > v) t.setOutPoint(o); break; }
            case 2: { const double i = t.inPoint(); t.setOutPoint(v); if (i >= 0 && i < v) t.setInPoint(i); t.setOutPoint(v); break; }
            default:
                if (srp) { srp->t = v; activeSession().sortSrps(); sessionChanged(); }
                break;
        }
        t.reveal(v);
    }), false);
}

void MainComponent::showPlaybackSettings() {
    auto* w = new juce::AlertWindow("Playback & Nudge Settings", "Pre-roll and post-roll are used by Play to / from In and Out and by the "
                                                                  "nudge auditions. Nudge sizes: seconds, or CD frames with an f (e.g. 1f).",
                                    juce::MessageBoxIconType::NoIcon);
    auto fmt = [](double v) { const double f = v * 75; return std::abs(f - std::round(f)) < 1e-6 && v < 1 ? juce::String(juce::roundToInt(f)) + "f" : juce::String(v, 3); };
    w->addTextEditor("pre", juce::String(preRoll_, 2), "Pre-roll (s)");
    w->addTextEditor("post", juce::String(postRoll_, 2), "Post-roll (s)");
    w->addTextEditor("a", fmt(nudgeSizes_[0]), "Nudge A");
    w->addTextEditor("b", fmt(nudgeSizes_[1]), "Nudge B");
    w->addTextEditor("c", fmt(nudgeSizes_[2]), "Nudge C");
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (r != 1) return;
        auto val = [&](const char* k, double dflt) {
            auto x = w->getTextEditorContents(k).trim();
            double v = x.endsWithIgnoreCase("f") ? x.dropLastCharacters(1).getDoubleValue() / 75.0 : x.getDoubleValue();
            return v > 0 ? juce::jmin(v, 60.0) : dflt;
        };
        preRoll_ = val("pre", preRoll_);
        postRoll_ = val("post", postRoll_);
        nudgeSizes_[0] = val("a", nudgeSizes_[0]);
        nudgeSizes_[1] = val("b", nudgeSizes_[1]);
        nudgeSizes_[2] = val("c", nudgeSizes_[2]);
        auto* u = props_.getUserSettings();
        u->setValue("preRoll", preRoll_);
        u->setValue("postRoll", postRoll_);
        u->setValue("nudgeA", nudgeSizes_[0]);
        u->setValue("nudgeB", nudgeSizes_[1]);
        u->setValue("nudgeC", nudgeSizes_[2]);
        commands_.commandStatusChanged();
    }), false);
}

void MainComponent::addSrp(bool withText) {
    const bool src = sourceActive_;
    auto& ss = activeSession();
    const int id = ss.addSrp(markTime());
    activeTimeline().selectSrp(id);
    sessionChanged();
    if (withText) editSrpText(src, id);
}

void MainComponent::editSrpText(bool source, int id) {
    auto& ss = source ? srcSession_ : session_;
    auto* p = ss.findSrp(id);
    if (!p) return;
    auto* w = new juce::AlertWindow("SRP", "Name this reference point.", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("text", p->text, "Text");
    w->addComboBox("lock", {"Moves with the material", "Locked (keeps its time when material ripples)"}, "Position");
    w->getComboBoxComponent("lock")->setSelectedItemIndex(p->locked ? 1 : 0);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, source, id](int r) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (r != 1) return;
        auto& s2 = source ? srcSession_ : session_;
        if (auto* q = s2.findSrp(id)) {
            q->text = w->getTextEditorContents("text").trim();
            q->locked = w->getComboBoxComponent("lock")->getSelectedItemIndex() == 1;
            sessionChanged();
        }
    }), false);
}

void MainComponent::jumpToSrp(int dir) {
    const double pos = markTime();
    double best = -1;
    for (auto& p : activeSession().srps) {
        if (dir < 0 && p.t < pos - 0.005 && p.t > best) best = p.t;
        if (dir > 0 && p.t > pos + 0.005 && (best < 0 || p.t < best)) best = p.t;
    }
    if (best >= 0) locate(best);
}

void MainComponent::saveFadeSettings() {
    auto* u = props_.getUserSettings();
    u->setValue("fadeEditor", juce::JSON::toString(fadePrefs_.toVar(), true));
    juce::Array<juce::var> arr;
    for (auto& p : fadeLib_) arr.add(p.toVar());
    u->setValue("fadeLibrary", juce::JSON::toString(juce::var(arr), true));
    props_.saveIfNeeded();
}

void MainComponent::crossfadeNewEdits(const juce::Array<int>& ids, Session& s) {
    if (!fadePrefs_.defaultOnEdits || ids.isEmpty()) return;
    const auto& p = fadeLib_[0];
    for (auto [a, b] : s.splicesAround(ids)) {
        auto* ra = s.findRegion(a);
        auto* rb = s.findRegion(b);
        // Only butt splices: an edit that already overlaps keeps its crossfade.
        if (!ra || !rb || ra->end() > rb->start + 1e-4) continue;
        applyFadePreset(s, a, b, p, true, Session::Ripple::None);
    }
}

void MainComponent::addMarkerAtPlayhead(Marker::Type t) {
    if (takeInProgress()) {
        // While recording nothing is reloaded (that would interrupt the take): the moment is noted,
        // shown on the timeline, and becomes a real marker on the take when it stops.
        recMarks_.add({t, engine_.livePosition()});
        updateLiveTake();
        return;
    }
    int id = session_.addMarker(t, engine_.positionSeconds());
    timeline_.selectMarker(id);
    sessionChanged();
}

void MainComponent::updateTimecodeStrip() {
    TimecodeStrip::Values v;
    auto& tl = activeTimeline();
    v.source = sourceActive_;
    v.recording = takeInProgress();
    if (v.recording) {
        v.main = engine_.livePosition();
        v.playing = true;
        v.source = false;
    } else if (sourceActive_) {
        v.playing = engine_.isSourcePlaying();
        v.main = v.playing ? engine_.sourcePosition() : tl.editPoint();
    } else {
        v.playing = engine_.isPlaying();
        v.main = v.playing ? engine_.positionSeconds() : tl.editPoint();
    }
    v.in = tl.inPoint();
    v.out = tl.outPoint();
    auto& s = activeSession();
    v.rate = s.effectiveRate();
    // LEFT / RIGHT: segments picked in the Text View, else the selected region.
    juce::Array<int> ids;
    if (!sourceActive_ && textViewPanel_ && textViewWindow_) ids = textViewPanel_->selectedIds();
    if (ids.isEmpty() && tl.selectedRegion()) ids.add(tl.selectedRegion());
    for (int id : ids)
        if (auto* r = s.findRegion(id)) {
            v.left = v.left < 0 ? r->start : juce::jmin(v.left, r->start);
            v.right = juce::jmax(v.right, r->end());
        }
    v.leftWhat = ids.size() > 1 ? juce::String(ids.size()) + " segments" : ids.size() == 1 ? juce::String("region") : juce::String();
    tcStrip_.setValues(v);
}

void MainComponent::updateLiveTake() {
    if (!takeInProgress()) return;
    Timeline::LiveTake t;
    t.active = true;
    t.marks = recMarks_;
    t.peakCount = [this] { return engine_.livePeakCount(); };
    t.peak = [this](int i) { return engine_.livePeak(i); };
    const double rate = engine_.recordRate();
    t.peakDt = double(engine_.livePeakSamples()) / rate;
    const double now = engine_.livePosition();
    const double first = double(engine_.recordStartSample()) / rate;   // timeline time of the file's first sample, before the delay
    if (cap_.capturing) {
        t.lane = cap_.plan.underLane;
        t.underLane = true;
        t.start = cap_.plan.range.getStart();
        t.end = juce::jmin(now, cap_.plan.range.getEnd());
        t.peaksT0 = first - double(cap_.roundTrip + engine_.masterLatency()) / rate;
        t.name = cap_.file.getFileNameWithoutExtension();
    } else if (print_.printing) {
        t.lane = print_.plan.underLane;
        t.underLane = true;
        t.start = print_.plan.range.getStart();
        t.end = juce::jmin(now, print_.plan.range.getEnd());
        t.peaksT0 = first - double(engine_.chain().latencyThrough(print_.gear)) / rate;
        t.name = print_.file.getFileNameWithoutExtension();
    } else if (rec_.print && engine_.isPlaying()) {
        int maxLane = 0;
        for (auto& x : session_.regions) maxLane = juce::jmax(maxLane, x.lane);
        t.lane = maxLane;
        t.underLane = true;
        const int lat = (rec_.measured > 0 ? rec_.measured : engine_.reportedRoundTrip()) + rec_.extraLatency + engine_.masterLatency();
        t.peaksT0 = first - double(lat) / rate;
        t.start = recPunch_.isEmpty() ? juce::jmax(0.0, t.peaksT0) : recPunch_.getStart();
        t.end = recPunch_.isEmpty() ? now : juce::jmin(now, recPunch_.getEnd());
        t.name = recFile_.getFileNameWithoutExtension();
    } else {
        t.lane = juce::jlimit(0, juce::jmax(0, session_.lanes - 1), timeline_.selectedLane());
        t.start = engine_.recordStartPosition();
        t.end = now;
        t.peaksT0 = t.start;
        t.name = recFile_.getFileNameWithoutExtension();
    }
    timeline_.setLiveTake(t);
}

void MainComponent::placeRecMarks(int regionId) {
    if (auto* r = session_.findRegion(regionId))
        for (auto& m : recMarks_)
            if (m.t >= r->start - 1.0e-6 && m.t <= r->end() + 1.0e-6) session_.addMarker(m.type, m.t, regionId);
    recMarks_.clear();
    timeline_.clearLiveTake();
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

void MainComponent::openFadeNearEditPoint() {
    auto& tl = activeTimeline();
    const double t = activePlaying() ? (sourceActive_ ? engine_.sourcePosition() : engine_.positionSeconds()) : tl.editPoint();
    const auto p = tl.fadeNear(t);
    if (!p.first && !p.second) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Edit Fade",
                                               "There is no audio on this timeline yet, so there is no fade to edit.");
        return;
    }
    openCrossfade(p.first, p.second, sourceActive_);
}

void MainComponent::openCrossfade(int out, int in, bool source) {
    xfadeWindow_.reset();
    auto& s = source ? srcSession_ : session_;
    auto* ed = new CrossfadeEditor(s, thumbs_, engine_.formats(), fadePrefs_, fadeLib_, out, in);
    ed->onChange = [this] { sessionChanged(); };
    ed->onPrefsChanged = [this] { saveFadeSettings(); };
    ed->onLibraryChanged = [this] { saveFadeSettings(); };
    ed->onAudition = [this, source](double a, double b) { playRange(source, a, b); };
    xfadeEditor_ = ed;
    auto* w = new XfadeWindow(ed);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { xfadeWindow_.reset(); }); };
    xfadeWindow_.reset(w);
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
    locateDest(session_.markers[row].t);
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
    if (session_.programmeChannels() == 6)
        s << "Note   5.1 programme: monitoring on " << (engine_.playbackChannels() == 6 ? "six outputs" : "two outputs (folded down; enable six outputs in Audio Setup)")
          << "; the master chain is stereo and stays out of the 5.1 path. CD and DDP need stereo lanes.\n";
    if (const double early = session_.audioBeforeFirstTrack(); early > 0 && !session_.htoa)
        s << "Warn   Audio plays for " << juce::String(early, 2) << " s before the first track mark (set HTOA in Release Info if intended)\n";
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
    o.dialogTitle = "Scan for Plugins (Options > Scan). Each plugin is checked in a separate helper, so a crash can't stop Lathe.";
    o.dialogBackgroundColour = colours::bg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showPluginManager() {
    if (pluginManagerWindow_) { pluginManagerWindow_->toFront(true); return; }
    auto* panel = new PluginManagerPanel(engine_.pluginHost());
    panel->onScan = [this] { showPluginScanner(); };
    auto* w = new PlainWindow(panel, "Plugin Manager", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { pluginManagerWindow_.reset(); }); };
    pluginManagerWindow_.reset(w);
}

void MainComponent::showPluginMenu() {
    auto& ch = engine_.chain();
    juce::PopupMenu m;
    auto types = ch.host().activeTypes();
    m.addItem(-2, "External Gear (hardware insert: out to your gear and back)");
    m.addSeparator();
    if (types.isEmpty()) m.addItem(1, ch.known().getNumTypes() ? "Every plugin is deactivated" : "No plugins found yet; scan first", false);
    juce::KnownPluginList::addToMenu(m, types, juce::KnownPluginList::sortByManufacturer, {});
    m.addSeparator();
    m.addItem(-1, "Plugin Manager (scan, activate / deactivate)...");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&pluginsBtn_), [this, types](int r) {
        if (r == -1) { showPluginManager(); return; }
        if (r == -2) { addExternalGear(); return; }
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
    m.addSectionHeader("DISC");
    m.addItem(1, "DDP image for replication (CD master)");
    m.addSectionHeader("WHOLE PROGRAMME");
    m.addItem(3, "WAV, 24-bit at the project rate");
    m.addItem(4, "WAV, 32-bit float at the project rate");
    m.addItem(10, "Other format (MP3, AAC, Apple Lossless)...");
    m.addSectionHeader("EACH TRACK");
    m.addItem(2, "WAV, 16-bit / 44.1 kHz (as on the CD)");
    m.addItem(11, "WAV, 24-bit at the project rate");
    m.addItem(12, "Other format (MP3, AAC, Apple Lossless)...");
    m.addSectionHeader("PART");
    m.addItem(13, juce::String("In-Out") + (sourceActive_ ? " of the source" : "") + "...", !activeTimeline().selection().isEmpty());
    m.addItem(14, "Selected segments...");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportBtn_), [this](int r) {
        if (r == 1) exportDdp();
        if (r == 2) exportTrackWavs();
        if (r == 10) deliver("the whole programme", {programmeItem(session_)}, false);
        if (r == 11) {
            props_.getUserSettings()->setValue("deliveryFormat", int(DeliveryFormat::Wav24));
            deliver("every track", trackItems(session_), false);
        }
        if (r == 12) deliver("every track", trackItems(session_), false);
        if (r == 13) exportInOut();
        if (r == 14) exportSelectedSegments();
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

namespace {
// Asks how to export: the format, a cover image (for MP3 / AAC / Apple Lossless) and, for one
// file, its name.
class DeliveryDialog : public juce::Component {
public:
    DeliveryDialog(const juce::String& what, int count, const juce::String& name, DeliveryFormat initial, const juce::File& cover)
        : cover_(cover) {
        title_.setText("Export " + what + (count > 1 ? "  (" + juce::String(count) + " files)" : juce::String()), juce::dontSendNotification);
        title_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        title_.setColour(juce::Label::textColourId, colours::text);
        for (auto* l : {&formatL_, &coverL_, &nameL_, &note_}) {
            l->setColour(juce::Label::textColourId, colours::dim);
            l->setMinimumHorizontalScale(1.0f);
        }
        formatL_.setText("Format", juce::dontSendNotification);
        coverL_.setText("Cover image", juce::dontSendNotification);
        nameL_.setText("File name", juce::dontSendNotification);
        note_.setJustificationType(juce::Justification::topLeft);
        for (int k = 0; k < kNumDeliveryFormats; ++k) {
            const auto f = DeliveryFormat(k);
            juce::String why;
            const bool ok = formatAvailable(f, &why);
            format_.addItem(formatName(f) + (ok ? juce::String() : "   (" + why + ")"), k + 1);
            format_.setItemEnabled(k + 1, ok);
        }
        format_.setSelectedId(int(initial) + 1, juce::dontSendNotification);
        if (!formatAvailable(initial)) format_.setSelectedId(int(DeliveryFormat::Wav24) + 1, juce::dontSendNotification);
        format_.onChange = [this] { update(); };
        name_.setText(name, false);
        name_.setVisible(count == 1);
        nameL_.setVisible(count == 1);
        for (auto* b : {&chooseCover_, &clearCover_, &go_, &cancel_}) styleButton(*b, b == &go_);
        chooseCover_.onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Cover image (JPEG or PNG)", cover_.existsAsFile() ? cover_ : juce::File(), "*.jpg;*.jpeg;*.png");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
                if (fc.getResult().existsAsFile()) { cover_ = fc.getResult(); update(); }
            });
        };
        clearCover_.onClick = [this] { cover_ = juce::File(); update(); };
        go_.onClick = [this] { if (onGo) onGo(DeliveryFormat(format_.getSelectedId() - 1), cover_, name_.getText().trim()); };
        cancel_.onClick = [this] { if (onCancel) onCancel(); };
        for (auto* c : std::initializer_list<juce::Component*>{&title_, &formatL_, &format_, &coverL_, &coverName_, &chooseCover_, &clearCover_, &nameL_,
                                                               &name_, &note_, &go_, &cancel_})
            addAndMakeVisible(*c);
        coverName_.setColour(juce::Label::textColourId, colours::text);
        update();
        setSize(560, 270);
    }
    std::function<void(DeliveryFormat, juce::File cover, juce::String name)> onGo;
    std::function<void()> onCancel;
    void paint(juce::Graphics& g) override { g.fillAll(colours::bg); }
    void resized() override {
        auto r = getLocalBounds().reduced(14, 12);
        title_.setBounds(r.removeFromTop(22));
        r.removeFromTop(8);
        auto row = [&](juce::Label& l) {
            auto x = r.removeFromTop(26);
            r.removeFromTop(6);
            l.setBounds(x.removeFromLeft(100));
            return x;
        };
        format_.setBounds(row(formatL_));
        {
            auto x = row(coverL_);
            clearCover_.setBounds(x.removeFromRight(60));
            x.removeFromRight(4);
            chooseCover_.setBounds(x.removeFromRight(90));
            x.removeFromRight(6);
            coverName_.setBounds(x);
        }
        name_.setBounds(row(nameL_));
        auto bottom = r.removeFromBottom(30);
        go_.setBounds(bottom.removeFromRight(110));
        bottom.removeFromRight(8);
        cancel_.setBounds(bottom.removeFromRight(90));
        r.removeFromBottom(6);
        note_.setBounds(r);
    }

private:
    void update() {
        const auto f = DeliveryFormat(format_.getSelectedId() - 1);
        const bool comp = isCompressed(f);
        for (auto* c : std::initializer_list<juce::Component*>{&coverL_, &coverName_, &chooseCover_, &clearCover_}) c->setEnabled(comp);
        coverName_.setText(cover_.existsAsFile() ? cover_.getFileName() : juce::String("none"), juce::dontSendNotification);
        note_.setText(comp ? "Tagged from the CD-Text and release details (title, artist, album, track number, ISRC, year, genre). After encoding, "
                             "each file is checked for peaks over full scale."
                           : f == DeliveryFormat::Wav16Cd ? "Resampled to 44.1 kHz and dithered, as for the CD."
                                                          : "At the project's sample rate, no dither.",
                      juce::dontSendNotification);
    }
    juce::File cover_;
    juce::Label title_, formatL_, coverL_, coverName_, nameL_, note_;
    juce::ComboBox format_;
    juce::TextEditor name_;
    juce::TextButton chooseCover_{"Choose..."}, clearCover_{"None"}, go_{"Export..."}, cancel_{"Cancel"};
    std::unique_ptr<juce::FileChooser> chooser_;
};
}  // namespace

void MainComponent::exportInOut() {
    const auto sel = activeTimeline().selection();
    if (sel.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export In-Out",
                                               "Mark an In (I) and an Out (O) first, on the timeline you want to export from.");
        return;
    }
    DeliveryItem it;
    it.range = sel;
    it.tags = releaseTags(session_);
    it.name = juce::File::createLegalFileName((session_.file.existsAsFile() ? session_.file.getFileNameWithoutExtension() : juce::String("Untitled")) +
                                              " In-Out " + msfString(sel.getStart()).replace(":", "-"));
    it.tags.title = it.name;
    deliver(juce::String("the In-Out") + (sourceActive_ ? " of the source" : "") + " (" + msfString(sel.getStart()) + " to " + msfString(sel.getEnd()) + ")", {it},
            sourceActive_);
}

void MainComponent::deliver(const juce::String& what, juce::Array<DeliveryItem> items, bool source) {
    if (items.isEmpty()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export", "There is nothing to export (no tracks yet?).");
        return;
    }
    auto* u = props_.getUserSettings();
    const auto initial = DeliveryFormat(juce::jlimit(0, kNumDeliveryFormats - 1, u->getIntValue("deliveryFormat", int(DeliveryFormat::Mp3Cbr320))));
    auto* dlg = new DeliveryDialog(what, items.size(), items[0].name, initial, juce::File(u->getValue("deliveryCover")));
    auto* w = new PlainWindow(dlg, "Export", true);
    deliveryWindow_.reset(w);
    dlg->onCancel = [this] { juce::MessageManager::callAsync([this] { deliveryWindow_.reset(); }); };
    w->onClose = dlg->onCancel;
    dlg->onGo = [this, items, source](DeliveryFormat f, juce::File cover, juce::String name) mutable {
        auto* us = props_.getUserSettings();
        us->setValue("deliveryFormat", int(f));
        us->setValue("deliveryCover", cover.getFullPathName());
        for (auto& it : items) it.tags.cover = cover;
        if (items.size() == 1 && name.isNotEmpty()) items.getReference(0).name = juce::File::createLegalFileName(name);
        juce::MessageManager::callAsync([this] { deliveryWindow_.reset(); });
        auto start = [this, items, f, source](juce::File folder, juce::File one) {
            DeliveryRequest rq;
            rq.format = f;
            rq.items = items;
            rq.folder = folder;
            rq.singleFile = one;
            rq.es = exportSettings();
            if (source) rq.es.runPlugins = false;   // the source plays raw, so it exports raw
            auto snapshot = std::make_shared<Session>(source ? srcSession_ : session_);
            snapshot->source = nullptr;
            auto res = std::make_shared<DeliveryResult>();
            runJob("Exporting " + formatName(f) + "...", [this, snapshot, rq, res](Job& j) { *res = runDelivery(exporter_, *snapshot, rq, [&](double p) { return j.keep(p); }); },
                   [this, res](bool cancelled) {
                       if (cancelled) return;
                       if (res->error.isNotEmpty()) {
                           juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", res->error);
                           return;
                       }
                       showText(res->warnings.isEmpty() ? "Exported" : "Exported: check the warnings", res->summary());
                   });
        };
        const auto ext = formatExtension(f);
        if (items.size() == 1) {
            chooser_ = std::make_unique<juce::FileChooser>("Export to", session_.file.getParentDirectory().getChildFile(items[0].name + ext), "*" + ext);
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [start, ext](const juce::FileChooser& fc) {
                                      if (fc.getResult() != juce::File()) start({}, fc.getResult().withFileExtension(ext));
                                  });
        } else {
            chooser_ = std::make_unique<juce::FileChooser>("Choose a folder for the files", session_.file.getParentDirectory());
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [start](const juce::FileChooser& fc) {
                if (fc.getResult() != juce::File()) start(fc.getResult(), {});
            });
        }
    };
}

void MainComponent::exportDdp() {
    if (session_.programmeChannels() > 2) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Stereo only",
                                               "CD, DDP and the per-track deliverables are stereo, and this programme is 5.1 (a lane is set to 5.1). "
                                               "Use Export Programme WAV for the 5.1 master, or set the lanes to stereo for the CD.");
        return;
    }
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
    if (session_.programmeChannels() > 2) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Stereo only",
                                               "CD, DDP and the per-track deliverables are stereo, and this programme is 5.1 (a lane is set to 5.1). "
                                               "Use Export Programme WAV for the 5.1 master, or set the lanes to stereo for the CD.");
        return;
    }
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
    if (audioSetupWindow_) { audioSetupWindow_->setVisible(true); audioSetupWindow_->toFront(true); return; }
    auto* p = new AudioSetupPanel(engine_, io_, [this] { return session_.file; },
                                  [this] { return session_.regions.isEmpty() ? 0.0 : session_.effectiveRate(); });
    p->onChange = [this] { saveIo(); applyIo(); };
    auto* w = new PlainWindow(p, "Audio Setup", true);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { audioSetupWindow_.reset(); }); };
    audioSetupWindow_.reset(w);
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
    if (sourceView_.getBounds().contains(x, y) && !audio.isEmpty()) { openSourceFiles(audio); return; }
    importFiles(audio);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* src) {
    if (src == commands_.getKeyMappings()) { saveKeyMappings(); return; }
    if (std::abs(engine_.deviceRate() - lastDeviceRate_) > 0.5) {
        lastDeviceRate_ = engine_.deviceRate();
        engine_.loadSession(session_);
        engine_.loadSourceSession(srcSession_);
    }
}

void MainComponent::timerCallback() {
    double pos = engine_.positionSeconds();
    const double spos = engine_.sourcePosition();
    if (auditionEnd_ >= 0) {
        const bool playing = auditionSource_ ? engine_.isSourcePlaying() : engine_.isPlaying();
        const double p = auditionSource_ ? spos : pos;
        if (playing && p >= auditionEnd_) {
            if (repeat_ && auditionStart_ >= 0) {
                if (auditionSource_) engine_.setSourcePosition(auditionStart_); else engine_.setPositionSeconds(auditionStart_);
            } else {
                auditionEnd_ = -1;
                stopAll();
            }
        } else if (!playing) {
            auditionEnd_ = -1;
        }
    }
    // Playback ended on its own (end of material): same bookkeeping as Stop.
    if ((wasPlaying_ && !engine_.isPlaying()) || (wasSourcePlaying_ && !engine_.isSourcePlaying())) {
        if (wasPlaying_ && !engine_.isPlaying()) lastStopDest_ = pos;
        if (wasSourcePlaying_ && !engine_.isSourcePlaying()) lastStopSource_ = spos;
        afterStop();
    }
    wasPlaying_ = engine_.isPlaying();
    wasSourcePlaying_ = engine_.isSourcePlaying();
    pos = engine_.positionSeconds();
    timeline_.setPlayhead(pos, engine_.isPlaying());
    sourceView_.timeline().setPlayhead(engine_.sourcePosition(), engine_.isSourcePlaying());
    sourceView_.setPlaying(engine_.isSourcePlaying());
    if (xfadeEditor_) xfadeEditor_->setPlayhead(pos);
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
    if (sourceActive_ || engine_.isSourcePlaying()) {
        const double sp = engine_.sourcePosition();
        timeDisplay_.setText(msfString(engine_.isSourcePlaying() ? sp : sourceView_.timeline().editPoint()), juce::dontSendNotification);
        timeDisplay_.setColour(juce::Label::textColourId, colours::wave);
    }
    recordBtn_.setToggleState(engine_.isRecording(), juce::dontSendNotification);
    if (print_.printing) {
        timeDisplay_.setColour(juce::Label::textColourId, colours::error);
        if (engine_.captureComplete() || !engine_.printActive()) finishPrint();
    } else if (cap_.capturing) {
        timeDisplay_.setColour(juce::Label::textColourId, colours::error);
        if (engine_.captureComplete() || !engine_.captureActive()) finishCapture();
    } else if (cap_.levelCheck) {
        // Round and round the range, until Level check is switched off.
        if (!engine_.captureActive()) cap_.levelCheck = false;
        else if (!engine_.isPlaying() || pos >= cap_.plan.range.getEnd()) {
            engine_.setPositionSeconds(cap_.plan.range.getStart());
            if (!engine_.isPlaying()) engine_.play();
        }
    } else if (engine_.isRecording()) {
        timeDisplay_.setColour(juce::Label::textColourId, colours::error);
        if (!engine_.isPlaying() && !rec_.print) {
            timeDisplay_.setText("REC " + msfString(double(engine_.recordedSamples()) / engine_.recordRate()), juce::dontSendNotification);
            timeline_.setPlayhead(engine_.livePosition(), true);   // the playhead follows the take
        }
        if (rec_.print && rec_.autoPunch && !recPunch_.isEmpty() && pos > recPunch_.getEnd() + 1.0) finishRecording();
        if (rec_.print && !engine_.isPlaying()) finishRecording();   // playback reached the end
    } else {
        timeDisplay_.setColour(juce::Label::textColourId, (sourceActive_ || engine_.isSourcePlaying()) ? colours::wave : colours::text);
    }
    {
        juce::String why;
        double g = referenceGainDb(&why);
        engine_.setReferenceGainDb(g);
        juce::String ab;
        if (engine_.listeningToReference() && juce::isPositiveAndBelow(session_.activeReference, session_.references.size()))
            ab = "REF: " + session_.references[session_.activeReference].name + "  " + (g >= 0 ? "+" : "") + juce::String(g, 1) + " dB";
        else if (compare_) {
            CompareBar::State st;
            st.hearingUnprocessed = engine_.hearingUnprocessed();
            st.levelMatch = engine_.levelMatch();
            st.measured = engine_.matchMeasured();
            st.cutDb = engine_.matchCutDb();
            ab = CompareBar::describe(st);
        }
        abDisplay_.setText(ab, juce::dontSendNotification);
    }
    if (takeInProgress()) updateLiveTake();
    else if (timeline_.liveTake().active) timeline_.clearLiveTake();
    updateTimecodeStrip();
    meterHub_.sessionTarget = session_.loudnessTarget;
    meterHub_.prepare(engine_.deviceRate());
    meterHub_.feed(engine_.meterTap());
    {
        // Inputs: armed while Record or Capture is open, or with Arm Inputs on.
        const bool arm = armInputs_ || recWindow_ != nullptr || captureWindow_ != nullptr;
        if (arm != engine_.isArmed()) {
            engine_.setArmed(arm);
            if (arm && !recWindow_ && !captureWindow_) engine_.setInputSelection(io_.defaultInput, 2);
        }
        if (arm && !engine_.inputSelectionOn()) {
            // Switch the armed inputs on at the interface (once per choice: it may not have them).
            const int key = engine_.inputSelection() * 8 + engine_.inputSelectionChannels();
            if (key != armTried_) {
                armTried_ = key;
                engine_.ensureChannels(engine_.inputSelection(), engine_.inputSelectionChannels(), 0, 0);
            }
        } else if (!arm) {
            armTried_ = -1;
        }
        const bool live = engine_.inputsLive();
        inputHub_.standard = meterHub_.standard;
        inputHub_.sessionTarget = session_.loudnessTarget;
        inputHub_.prepare(engine_.deviceRate());
        if (live && !inputsWereLive_) { inputHub_.reset(); engine_.inputTap().clear(); engine_.resetInputOvers(); }
        inputsWereLive_ = live;
        inputHub_.feed(engine_.inputTap());
        meters_->showInput = live;
        meters_->inputOvers = engine_.inputOvers();
        meters_->inputMaxDb = juce::Decibels::gainToDecibels(engine_.inputMax(), -200.0f);
        const int first = engine_.inputSelection(), n = engine_.inputSelectionChannels();
        meters_->inputName = n == 1 ? juce::String(first + 1) : juce::String(first + 1) + "-" + juce::String(first + n);
        if (meterBridge_) meterBridge_->inputsLive = live;
    }
    meters_->repaint();
    if (meterBridge_ && meterWindow_ && meterWindow_->isVisible()) meterBridge_->refresh();

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

// ---------------------------------------------------------------- delivery

void MainComponent::saveStudio() {
    props_.getUserSettings()->setValue("studioProfile", juce::JSON::toString(studio_.toVar(), true));
    props_.saveIfNeeded();
}

void MainComponent::refreshDeliveryPanels() {
    if (productsPanel_) productsPanel_->refresh();
    if (releasePanel_) releasePanel_->refresh();
    if (creditsPanel_) creditsPanel_->refresh();
    if (markInfoPanel_) markInfoPanel_->refresh();
    if (textViewPanel_) textViewPanel_->refresh();
    if (breakEditor_) {
        auto* p = session_.findProduct(breakEditor_->productId());
        if (!p || breakEditor_->breakIndex() + 1 >= p->units.size()) breakWindow_.reset();
        else breakEditor_->setBreak(p->id, breakEditor_->breakIndex());
    }
    timeline_.resized();
    timeline_.repaint();
}

void MainComponent::showProducts() {
    if (productsWindow_) { productsWindow_->toFront(true); return; }
    auto* panel = new ProductsPanel(session_, [this]() -> const dsp::LoudnessMap* { return analysis_ && !analysis_->map.momentary.empty() ? &analysis_->map : nullptr; });
    panel->onChange = [this] {
        sessionChanged();
        timeline_.resized();
        timeline_.repaint();
        if (breakEditor_) {
            auto* p = session_.findProduct(breakEditor_->productId());
            if (!p || breakEditor_->breakIndex() + 1 >= p->units.size()) breakWindow_.reset();
            else breakEditor_->setBreak(p->id, breakEditor_->breakIndex());
        }
    };
    panel->onEditBreak = [this](int pid, int k) { openBreakEditor(pid, k); };
    panel->onLocate = [this](double t) { locateDest(t); };
    panel->onProductSelected = [this](int pid) { timeline_.setProductView(pid); };
    productsPanel_ = panel;
    auto* w = new PlainWindow(panel, "Products & Splitting", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { productsWindow_.reset(); }); };
    productsWindow_.reset(w);
    if (panel->selectedProduct()) timeline_.setProductView(panel->selectedProduct());
}

void MainComponent::showProductStripMenu(int pid, double t, int k) {
    auto* p = session_.findProduct(pid);
    if (!p) return;
    const auto noun = juce::String(productSpec(p->kind).unitNoun);
    bool inside = false;
    for (auto& u : p->units) inside |= t > u.start + 1.0 && t < u.end - 1.0;
    juce::PopupMenu m;
    m.addSectionHeader(p->name + " at " + msfString(t));
    m.addItem(1, "Split " + noun + " Here", inside);
    m.addItem(2, "Join " + noun + "s at This Break", k >= 0);
    m.addItem(3, "Edit Break...", k >= 0);
    m.addSeparator();
    m.addItem(4, "Auto-Split (Balanced)");
    m.addItem(5, "Auto-Split (Fill to Track Markers)");
    m.addItem(6, "One " + noun + " for the Whole Programme");
    m.addSeparator();
    if (session_.products.size() > 1) {
        juce::PopupMenu show;
        for (int i = 0; i < session_.products.size(); ++i)
            show.addItem(100 + i, session_.products[i].name, true, session_.products[i].id == pid);
        m.addSubMenu("Show Product", show);
    }
    m.addItem(7, "Products & Splitting...");
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [this, pid, t, k](int r) {
        auto* q = session_.findProduct(pid);
        if (!q || r == 0) return;
        juce::String msg;
        bool changed = false;
        if (r == 1) changed = splitUnitAt(*q, t);
        if (r == 2) changed = joinUnits(*q, k);
        if (r == 3) { openBreakEditor(pid, k); return; }
        if (r == 4 || r == 5) {
            SplitOptions o;
            o.atTrackMarkers = r == 5;
            q->splitMode = r == 5 ? 1 : 0;
            msg = autoSplit(session_, *q, analysis_ && !analysis_->map.momentary.empty() ? &analysis_->map : nullptr, o);
            changed = true;
        }
        if (r == 6) { singleUnit(session_, *q); changed = true; }
        if (r == 7) { showProducts(); return; }
        if (r >= 100) { timeline_.setProductView(session_.products[r - 100].id); return; }
        if (changed) {
            sessionChanged();
            timeline_.resized();
            timeline_.repaint();
            if (productsPanel_) productsPanel_->refresh();
            if (breakEditor_) {
                auto* pp = session_.findProduct(breakEditor_->productId());
                if (!pp || breakEditor_->breakIndex() + 1 >= pp->units.size()) breakWindow_.reset();
                else breakEditor_->setBreak(pp->id, breakEditor_->breakIndex());
            }
        }
        if (msg.startsWith("Couldn't"))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Auto-Split", msg);
    });
}

void MainComponent::openBreakEditor(int pid, int k) {
    auto* p = session_.findProduct(pid);
    if (!p || k < 0 || k + 1 >= p->units.size()) return;
    timeline_.setProductView(pid);
    if (breakWindow_ && breakEditor_) {
        breakEditor_->setBreak(pid, k);
        breakWindow_->toFront(true);
        return;
    }
    auto* ed = new BreakEditor(session_, thumbs_, pid, k);
    ed->onChange = [this] {
        sessionChanged();
        timeline_.repaint();
        if (productsPanel_) productsPanel_->refresh();
    };
    ed->onAudition = [this](const BreakPlan& plan) { engine_.startBreakAudition(session_, plan); };
    ed->onStop = [this] { engine_.stopBreakAudition(); };
    ed->auditionPosition = [this] { return engine_.breakAuditionTimeline(); };
    breakEditor_ = ed;
    auto* w = new PlainWindow(ed, "Break Editor", false);
    w->onClose = [this] { engine_.stopBreakAudition(); juce::MessageManager::callAsync([this] { breakWindow_.reset(); }); };
    breakWindow_.reset(w);
}

void MainComponent::breakNearPlayhead() {
    auto* p = session_.findProduct(timeline_.productView());
    if (!p || p->units.size() < 2) { showProducts(); return; }
    const double pos = engine_.positionSeconds();
    int best = 0;
    double bestD = 1e18;
    for (int k = 0; k + 1 < p->units.size(); ++k) {
        double d = std::abs((p->units[k].end + p->units[k + 1].start) * 0.5 - pos);
        if (d < bestD) { bestD = d; best = k; }
    }
    openBreakEditor(p->id, best);
}

void MainComponent::showReleaseInfo() {
    if (releaseWindow_) { releaseWindow_->toFront(true); return; }
    auto* panel = new ReleaseInfoPanel(session_);
    panel->onChange = [this] {
        sessionChanged();
        updatingFields_ = true;
        title_.setText(session_.discTitle, false);
        performer_.setText(session_.discPerformer, false);
        upc_.setText(session_.upcEan, false);
        client_.setText(session_.client, false);
        engineer_.setText(session_.engineer, false);
        updatingFields_ = false;
    };
    releasePanel_ = panel;
    auto* w = new PlainWindow(panel, "Release Info", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { releaseWindow_.reset(); }); };
    releaseWindow_.reset(w);
}

void MainComponent::showTrackCredits() {
    if (creditsWindow_) { creditsWindow_->toFront(true); return; }
    auto* panel = new TrackCreditsPanel(session_);
    panel->onChange = [this] {
        sessionChanged();
        pqList_.updateContent();
        pqList_.repaint();
    };
    creditsPanel_ = panel;
    auto* w = new PlainWindow(panel, "Track Credits", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { creditsWindow_.reset(); }); };
    creditsWindow_.reset(w);
}

void MainComponent::showLaneMenu(bool source, int lane) {
    auto& ss = source ? srcSession_ : session_;
    juce::PopupMenu m;
    m.addSectionHeader("Lane " + juce::String(lane + 1));
    const int f = ss.laneFormat(lane);
    m.addItem(1, "Mono", true, f == 1);
    m.addItem(2, "Stereo", true, f == 2);
    m.addItem(6, "5.1 (L R C LFE Ls Rs)", true, f == 6);
    m.addSeparator();
    m.addItem(10, ss.laneTargeted(lane) ? "Leave this lane out of edits (None)" : "Include this lane in edits");
    m.showMenuAsync({}, [this, source, lane](int r) {
        auto& s2 = source ? srcSession_ : session_;
        if (r == 1 || r == 2 || r == 6) s2.setLaneFormat(lane, r);
        else if (r == 10) { if (s2.laneTargeted(lane)) s2.lanesOff.addIfNotAlreadyThere(lane); else s2.lanesOff.removeAllInstancesOf(lane); }
        else return;
        sessionChanged();
        (source ? sourceView_.timeline() : timeline_).repaint();
    });
}

void MainComponent::exportSelectedSegments() {
    // What to export: segments picked in the Text View; else the selected region, or the In-Out
    // (asking which when both are there).
    juce::Array<int> ids;
    if (textViewPanel_ && textViewWindow_) ids = textViewPanel_->selectedIds();
    const auto sel = timeline_.selection();
    auto regionItems = [this](const juce::Array<int>& which) {
        juce::Array<DeliveryItem> items;
        int k = 0;
        for (int id : which)
            if (auto* r = session_.findRegion(id)) {
                DeliveryItem it;
                it.range = {r->start, r->end()};
                it.tags = releaseTags(session_);
                it.tags.title = r->name;
                it.name = juce::File::createLegalFileName(juce::String(++k).paddedLeft('0', 2) + " " + r->name);
                items.add(it);
            }
        return items;
    };
    if (!ids.isEmpty()) { deliver(juce::String(ids.size()) + (ids.size() == 1 ? " segment" : " segments"), regionItems(ids), false); return; }
    const int region = session_.findRegion(timeline_.selectedRegion()) ? timeline_.selectedRegion() : 0;
    if (region && !sel.isEmpty()) {
        auto* r = session_.findRegion(region);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Export",
                                           "Both an In-Out (" + msfString(sel.getStart()) + " to " + msfString(sel.getEnd()) + ") and a region (" + r->name +
                                               ") are selected. Which do you want to export?",
                                           "The In-Out", "The region", this, juce::ModalCallbackFunction::create([this, region, regionItems](int inOut) {
                                               if (inOut) exportInOut();
                                               else deliver("the region", regionItems({region}), false);
                                           }));
        return;
    }
    if (region) { deliver("the region", regionItems({region}), false); return; }
    if (!sel.isEmpty()) { exportInOut(); return; }
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export Selected Segments",
                                           "Select a segment (or several in the Text View), or mark an In and Out.");
}

void MainComponent::showMeterBridge() {
    if (meterWindow_) { meterWindow_->setVisible(true); meterWindow_->toFront(true); return; }
    auto* bridge = new MeterBridge(meterHub_, &inputHub_);
    bridge->onReset = [this] { meterHub_.reset(); inputHub_.reset(); engine_.resetInputOvers(); };
    bridge->setSource(MeterBridge::Source(juce::jlimit(0, 2, props_.getUserSettings()->getIntValue("meterSource", 0))));
    bridge->onSourceChanged = [this] { if (meterBridge_) props_.getUserSettings()->setValue("meterSource", int(meterBridge_->source())); };
    bridge->onStandardChanged = [this] { props_.getUserSettings()->setValue("meterStandard", meterHub_.standard); meters_->repaint(); };
    bridge->onRefreshReferences = [this] { loadReferenceProfiles(true); };
    meterBridge_ = bridge;
    auto* w = new PlainWindow(bridge, "Meter Bridge", false);   // not on top: it can live on a second screen
    w->onClose = [this] { juce::MessageManager::callAsync([this] { meterWindow_.reset(); }); };
    meterWindow_.reset(w);
    loadReferenceProfiles(false);
}

void MainComponent::loadReferenceProfiles(bool explain) {
    // The tonal balance target: the session's reference tracks (up to 8), measured once each.
    juce::Array<juce::File> files;
    for (auto& r : session_.references)
        if (r.file.existsAsFile() && files.size() < 8) files.add(r.file);
    if (files.isEmpty()) {
        meterHub_.setReferenceProfiles({}, {});
        if (explain)
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Tonal balance references",
                                                   "Add reference tracks in Tools > References (commercial releases or your own approved masters "
                                                   "in the same style). The Balance view then shows a zone from them, and how far each range of "
                                                   "this programme sits from it.");
        return;
    }
    juce::Array<juce::File> todo;
    for (auto& f : files)
        if (refProfiles_.find(f.getFullPathName() + "|" + juce::String(f.getLastModificationTime().toMilliseconds())) == refProfiles_.end()) todo.add(f);
    auto apply = [this, files] {
        std::vector<dsp::BalanceProfile> ps;
        juce::StringArray names;
        for (auto& f : files) {
            auto it = refProfiles_.find(f.getFullPathName() + "|" + juce::String(f.getLastModificationTime().toMilliseconds()));
            if (it != refProfiles_.end() && it->second.valid()) { ps.push_back(it->second); names.add(f.getFileNameWithoutExtension()); }
        }
        meterHub_.setReferenceProfiles(std::move(ps), names);
        if (meterBridge_) meterBridge_->refresh();
    };
    if (todo.isEmpty()) { apply(); return; }
    auto results = std::make_shared<std::vector<std::pair<juce::String, dsp::BalanceProfile>>>();
    runJob("Measuring reference tracks...",
           [this, todo, results](Job& j) {
               for (int i = 0; i < todo.size(); ++i) {
                   auto p = MeterHub::profileOfFile(engine_.formats(), todo[i], [&](double x) { return j.keep((i + x) / todo.size()); });
                   results->push_back({todo[i].getFullPathName() + "|" + juce::String(todo[i].getLastModificationTime().toMilliseconds()), p});
                   if (j.threadShouldExit()) return;
               }
           },
           [this, results, apply](bool) {
               for (auto& [k, p] : *results) refProfiles_[k] = p;
               apply();
           });
}

void MainComponent::showMarkInfo() {
    if (markInfoWindow_) { markInfoWindow_->toFront(true); return; }
    auto* panel = new MarkInfoPanel(session_);
    panel->onChange = [this] {
        sessionChanged();
        pqList_.updateContent();
        pqList_.repaint();
    };
    panel->onPlay = [this](double t) { locateDest(t); setActiveTimeline(false); if (!engine_.isPlaying()) togglePlay(); };
    panel->onSegmentsToMarks = [this] { commands_.invokeDirectly(cmd::segmentsToMarks, false); };
    panel->onEditedBlack = [this] { commands_.invokeDirectly(cmd::editedBlackToMarks, false); };
    panel->onAnalogBlack = [this] { runAnalogBlack(); };
    panel->onAutoSpace = [this] { commands_.invokeDirectly(cmd::autoSpace, false); };
    panel->onLockAll = [this] { commands_.invokeDirectly(cmd::lockAllMarks, false); };
    markInfoPanel_ = panel;
    auto* w = new PlainWindow(panel, "Mark Info", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { markInfoWindow_.reset(); }); };
    markInfoWindow_.reset(w);
}

void MainComponent::showTextView() {
    if (textViewWindow_) { textViewWindow_->toFront(true); return; }
    auto* panel = new SegmentListPanel(session_);
    panel->onGain = [this](juce::Array<int> ids) { setActiveTimeline(false); showSegmentGain(ids); };
    panel->onLocate = [this](double t) { setActiveTimeline(false); locateDest(t); };
    panel->onSelect = [this](int id) { timeline_.selectRegion(id); };
    textViewPanel_ = panel;
    auto* w = new PlainWindow(panel, "Text View", false);
    w->onClose = [this] { juce::MessageManager::callAsync([this] { textViewWindow_.reset(); }); };
    textViewWindow_.reset(w);
}

void MainComponent::showSegmentGain(juce::Array<int> ids) {
    auto& ss = activeSession();
    if (ids.isEmpty()) return;
    auto* first = ss.findRegion(ids.getFirst());
    if (!first) return;
    const bool src = sourceActive_;
    auto* w = new juce::AlertWindow("Segment Gain", juce::String(ids.size()) + (ids.size() == 1 ? " segment: " + first->name : juce::String(" segments")),
                                    juce::MessageBoxIconType::NoIcon);
    w->addComboBox("mode", {"Set gain to (dB)", "Change gain by (dB)", "Normalize peak to (dBFS)"}, "Gain");
    w->getComboBoxComponent("mode")->setSelectedItemIndex(ids.size() == 1 ? 0 : 1);
    w->addTextEditor("value", ids.size() == 1 ? juce::String(first->gainDb, 2) : juce::String("0.0"), "Value");
    w->addComboBox("pol", {"Leave polarity", "Normal polarity", "Inverted polarity"}, "Polarity");
    w->getComboBoxComponent("pol")->setSelectedItemIndex(0);
    w->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, ids, src](int r) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (r != 1) return;
        auto& s2 = src ? srcSession_ : session_;
        const auto mode = SegmentGainMode(w->getComboBoxComponent("mode")->getSelectedItemIndex());
        const double v = w->getTextEditorContents("value").getDoubleValue();
        const int pol = w->getComboBoxComponent("pol")->getSelectedItemIndex() - 1;
        if (applySegmentGain(s2, ids, mode, v, pol, [this](const Region& reg) { return regionPeakDb(engine_.formats(), reg); }) > 0) sessionChanged();
    }), false);
}

void MainComponent::runAnalogBlack() {
    auto* w = new juce::AlertWindow("Analog Black to Marks", "Silence inside the audio becomes a track end and the next track's start.",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("thresh", "-60", "Quieter than (dBFS peak)");
    w->addTextEditor("len", "1.0", "For at least (s)");
    w->addButton("Find", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (r != 1) return;
        const double th = juce::jlimit(-120.0, -10.0, w->getTextEditorContents("thresh").getDoubleValue());
        const double len = juce::jlimit(0.1, 30.0, w->getTextEditorContents("len").getDoubleValue());
        auto snapshot = std::make_shared<Session>(session_);
        snapshot->source = nullptr;
        auto found = std::make_shared<juce::Array<juce::Range<double>>>();
        runJob("Finding silence...",
               [this, snapshot, th, len, found](Job& j) { *found = findSilences(engine_.formats(), *snapshot, th, len, [&](double p) { return j.keep(p); }); },
               [this, found](bool) {
                   const int n = analogBlackToMarks(session_, *found);
                   if (n > 0) sessionChanged();
                   juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Analog Black to Marks",
                                                          juce::String(found->size()) + " silence(s) found, " + juce::String(n) + " mark(s) added.");
               });
    }), false);
}

void MainComponent::showStudioProfile() {
    if (studioWindow_) { studioWindow_->toFront(true); return; }
    auto* panel = new StudioPanel(studio_);
    panel->onChange = [this] { saveStudio(); };
    auto* w = new PlainWindow(panel, "Studio Profile", false);
    w->onClose = [this] { saveStudio(); juce::MessageManager::callAsync([this] { studioWindow_.reset(); }); };
    studioWindow_.reset(w);
}

DeliveryData MainComponent::previewDelivery() const {
    DeliveryData d;
    d.disc = session_.toDisc(&d.discNotes);
    d.fullDdp = session_.end() <= productSpec(ProductKind::CD).hardMax;
    d.dither = ditherName(exportSettings().shape);
    d.hiRate = session_.effectiveRate();
    d.settingsText = "Preliminary paperwork from the session: nothing has been rendered, so there are no checksums or loudness figures yet.";
    for (int pi = 0; pi < session_.products.size(); ++pi) {
        const auto& p = session_.products.getReference(pi);
        for (int ui = 0; ui < p.units.size(); ++ui) {
            UnitRender u;
            u.product = pi;
            u.unit = ui;
            u.label = unitLabel(p, ui);
            u.longLabel = p.name + " " + unitLabel(p, ui, true);
            u.window = p.units[ui];
            if (productSpec(p.kind).cdFormat) {
                Session ps = session_;
                if (p.upc.isNotEmpty()) ps.upcEan = p.upc;
                if (p.catalogue.isNotEmpty()) { ps.catalogue = p.catalogue; ps.masterId = {}; }
                u.disc = ps.toDiscForUnit(u.window, ui + 1, p.units.size(), u.lead, &u.notes);
            } else {
                u.rate = d.hiRate;
                u.bits = 24;
            }
            d.units.push_back(u);
        }
    }
    return d;
}

void MainComponent::exportPaperworkOnly() {
    chooser_ = std::make_unique<juce::FileChooser>("Choose where to write the Paperwork folder", juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
        auto parent = fc.getResult();
        if (!parent.isDirectory()) return;
        auto folder = parent.getNonexistentChildFile(juce::File::createLegalFileName((session_.discTitle.isEmpty() ? juce::String("Lathe") : session_.discTitle) + " Paperwork"), "", false);
        folder.createDirectory();
        auto res = writePaperwork(session_, previewDelivery(), studio_, folder);
        if (res.error.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Paperwork", res.error);
            return;
        }
        folder.getChildFile("Paperwork").revealToUser();
    });
}

// ---------------------------------------------------------------- missing audio

struct RelinkState {
    int located = 0, foundAutomatically = 0, total = 0;
    juce::Array<juce::File> skipped;
    juce::File lastFolder;
};

namespace {
juce::String describeAudio(double rate, juce::int64 frames) {
    return juce::String(rate / 1000.0, 1) + " kHz, " + msfString(rate > 0 ? double(frames) / rate : 0.0).substring(0, 5) + " long";
}
}  // namespace

void MainComponent::locateMissingAudio(bool fromMenu) {
    auto st = std::make_shared<RelinkState>();
    if (session_.file.existsAsFile()) {
        auto dir = session_.file.getParentDirectory();
        st->foundAutomatically = session_.findMissingAudioIn({dir, dir.getChildFile("Audio"), dir.getChildFile("Audio Files")});
    }
    auto missing = session_.missingAudio();
    st->total = missing.size() + st->foundAutomatically;
    if (missing.isEmpty()) {
        if (st->foundAutomatically > 0) finishRelink(st);
        else if (fromMenu)
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Locate Missing Audio", "Every audio file in this session is where it should be.");
        return;
    }
    juce::String msg;
    msg << missing.size() << (missing.size() == 1 ? " audio file this session uses can't be found:" : " audio files this session uses can't be found:") << "\n\n";
    for (int i = 0; i < juce::jmin(8, missing.size()); ++i) msg << "    " << missing[i].getFileName() << "\n";
    if (missing.size() > 8) msg << "    ...and " << (missing.size() - 8) << " more\n";
    msg << "\nLocate them now? Lathe asks for each one; when you find one, any others in the same folder are picked up automatically.";
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Audio Missing", msg, "Locate...", "Not Now", nullptr,
                                       juce::ModalCallbackFunction::create([this, st](int r) {
                                           if (r == 1) locateNextMissing(st);
                                           else if (st->foundAutomatically > 0) finishRelink(st);
                                       }));
}

void MainComponent::locateNextMissing(std::shared_ptr<RelinkState> st) {
    juce::File target;
    int remaining = 0;
    for (auto& m : session_.missingAudio())
        if (!st->skipped.contains(m)) { if (target == juce::File()) target = m; ++remaining; }
    if (target == juce::File()) { finishRelink(st); return; }

    // What the session expects, to catch the wrong file being picked.
    double rate = 0;
    juce::int64 frames = -1;
    for (auto& r : session_.regions)
        if (r.file == target) { rate = r.fileRate; frames = r.fileLength; break; }

    auto start = st->lastFolder.isDirectory() ? st->lastFolder
               : target.getParentDirectory().isDirectory() ? target.getParentDirectory()
               : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    chooser_ = std::make_unique<juce::FileChooser>("Locate \"" + target.getFileName() + "\"" + (remaining > 1 ? " (" + juce::String(remaining) + " missing)" : juce::String()),
                                                   start, engine_.formats().getWildcardForAllFormats());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, st, target, rate, frames](const juce::FileChooser& fc) {
        auto chosen = fc.getResult();
        if (!chosen.existsAsFile()) { finishRelink(st); return; }   // cancelled: stop asking
        std::unique_ptr<juce::AudioFormatReader> reader(engine_.formats().createReaderFor(chosen));
        if (!reader) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Can't read that file",
                                                   chosen.getFileName() + " isn't an audio file Lathe can read. Choose another.");
            juce::MessageManager::callAsync([this, st] { locateNextMissing(st); });
            return;
        }
        auto apply = [this, st, target, chosen] {
            session_.relinkAudio(target, chosen);
            ++st->located;
            st->lastFolder = chosen.getParentDirectory();
            st->foundAutomatically += session_.findMissingAudioIn({st->lastFolder, st->lastFolder.getChildFile("Audio")});
            juce::MessageManager::callAsync([this, st] { locateNextMissing(st); });
        };
        const bool rateDiffers = rate > 0 && std::abs(reader->sampleRate - rate) > 0.5;
        const bool lengthDiffers = frames >= 0 && reader->lengthInSamples != frames;
        if (!rateDiffers && !lengthDiffers) { apply(); return; }
        juce::String msg;
        msg << chosen.getFileName() << " is " << describeAudio(reader->sampleRate, reader->lengthInSamples) << ", but the session expects "
            << describeAudio(rate, frames) << ".\n\nIf it's a different version of the file, edits and markers may land on the wrong audio. Use it anyway?";
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Different File?", msg, "Use It", "Choose Another", nullptr,
                                           juce::ModalCallbackFunction::create([this, st, apply](int r) {
                                               if (r == 1) apply();
                                               else juce::MessageManager::callAsync([this, st] { locateNextMissing(st); });
                                           }));
    });
}

void MainComponent::finishRelink(std::shared_ptr<RelinkState> st) {
    const int found = st->located + st->foundAutomatically;
    if (found > 0) {
        if (juce::isPositiveAndBelow(session_.activeReference, session_.references.size()))
            engine_.loadReference(session_.references[session_.activeReference].file);
        sessionChanged();
        refreshAll();
    }
    auto still = session_.missingAudio();
    if (found == 0 && !still.isEmpty()) return;   // nothing changed; the user chose not to look
    juce::String msg;
    msg << "Found " << found << " of " << st->total << " missing file" << (st->total == 1 ? "" : "s") << ".";
    if (!still.isEmpty()) {
        msg << "\n\nStill missing:\n";
        for (int i = 0; i < juce::jmin(8, still.size()); ++i) msg << "    " << still[i].getFileName() << "\n";
        msg << "\nUse File > Locate Missing Audio to try again.";
    }
    msg << "\n\nSave the session to keep the new locations.";
    juce::AlertWindow::showMessageBoxAsync(still.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon, "Locate Missing Audio", msg);
}

}  // namespace lathe
