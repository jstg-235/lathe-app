#include "IoPanels.h"

namespace lathe {

namespace {
const juce::Colour kWarn(0xfff0c43a), kBad(0xffe0605a), kGood(0xff5fd17a), kIn(0xff39c2c9), kOut(0xfff0c43a);

void styleCombo(juce::ComboBox& c) {
    c.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    c.setColour(juce::ComboBox::textColourId, colours::text);
    c.setColour(juce::ComboBox::outlineColourId, colours::line);
}
void heading(juce::Label& l, const juce::String& t) {
    l.setText(t, juce::dontSendNotification);
    l.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    l.setColour(juce::Label::textColourId, colours::accent);
}
void caption(juce::Label& l, const juce::String& t) {
    l.setMinimumHorizontalScale(1.0f);   // wrap, don't squeeze
    l.setText(t, juce::dontSendNotification);
    l.setFont(juce::FontOptions(13.0f));
    l.setColour(juce::Label::textColourId, colours::dim);
}
// Pair lists use the pair's first channel + 1 as the item id.
void fillPairs(juce::ComboBox& c, const juce::StringArray& names, int selectFirst) {
    c.clear(juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i) c.addItem(names[i], i * 2 + 1);
    if (names.isEmpty()) return;
    const int want = (juce::jmax(0, selectFirst) / 2) * 2 + 1;
    c.setSelectedId(c.indexOfItemId(want) >= 0 ? want : 1, juce::dontSendNotification);
}
int firstOf(const juce::ComboBox& c) { return juce::jmax(0, c.getSelectedId() - 1); }
}  // namespace

juce::String samplesAndMs(int samples, double rate) {
    return juce::String(samples) + " samples (" + juce::String(rate > 0 ? samples * 1000.0 / rate : 0.0, 1) + " ms)";
}

juce::String byteSize(juce::int64 b) {
    if (b >= (juce::int64(1) << 30)) return juce::String(double(b) / double(juce::int64(1) << 30), 1) + " GB";
    if (b >= (1 << 20)) return juce::String(juce::roundToInt(double(b) / double(1 << 20))) + " MB";
    return juce::String(juce::jmax<juce::int64>(1, b / 1024)) + " KB";
}

// ================================================================= Audio Setup

AudioSetupPanel::AudioSetupPanel(AudioEngine& e, IoSettings& io, std::function<juce::File()> sessionFile, std::function<double()> sessionRate)
    : e_(e), io_(io), sessionFile_(std::move(sessionFile)), sessionRate_(std::move(sessionRate)) {
    heading(outHead_, "OUTPUT");
    heading(inHead_, "INPUT");
    heading(recHead_, "RECORDING");
    caption(outDevL_, "Interface");
    caption(rateL_, "Sample rate");
    caption(bufL_, "Buffer");
    caption(outPairL_, "Output pair");
    caption(inDevL_, "Interface");
    caption(inPairsL_, "Inputs switched on");
    caption(defInL_, "Default input pair");
    caption(whereL_, "Location");
    caption(formatL_, "Format");
    caption(nameL_, "");
    caption(pathL_, "");
    caption(delayL_, "Round-trip delay");
    caption(delayInfo_, "");
    pathL_.setColour(juce::Label::textColourId, colours::text);
    pathL_.setMinimumHorizontalScale(0.6f);
    nameL_.setJustificationType(juce::Justification::topLeft);
    delayInfo_.setJustificationType(juce::Justification::topLeft);
    warn_.setFont(juce::FontOptions(13.0f));
    warn_.setColour(juce::Label::textColourId, kWarn);
    warn_.setJustificationType(juce::Justification::topLeft);
    for (auto* l : {&outHead_, &inHead_, &recHead_, &outDevL_, &rateL_, &bufL_, &outPairL_, &inDevL_, &inPairsL_, &defInL_, &whereL_, &pathL_, &formatL_,
                    &nameL_, &delayL_, &delayInfo_, &warn_})
        addAndMakeVisible(*l);
    for (auto* c : {&outDev_, &rate_, &buf_, &outPair_, &inDev_, &defIn_, &format_, &delayOut_, &delayIn_}) {
        styleCombo(*c);
        addAndMakeVisible(*c);
    }
    six_.setButtonText("5.1: play on six outputs from this pair");
    nextTo_.setButtonText("Next to the session");
    chosen_.setButtonText("In this folder:");
    monitor_.setButtonText("Input monitoring through Lathe (hear the armed inputs on the output pair)");
    monitor_.setTooltip("Off, you listen to the return on your console or monitor controller. On, Lathe plays the armed inputs on the output pair. "
                        "It is never played back onto the outputs that feed the gear.");
    nextTo_.setRadioGroupId(7201);
    chosen_.setRadioGroupId(7201);
    for (auto* t : {&six_, &nextTo_, &chosen_, &monitor_}) {
        t->setColour(juce::ToggleButton::textColourId, colours::text);
        addAndMakeVisible(*t);
    }
    for (auto* b : {&choose_, &measure_}) {
        styleButton(*b, false);
        addAndMakeVisible(*b);
    }
    format_.addItem("24-bit WAV", 24);
    format_.addItem("32-bit float WAV", 32);

    outDev_.onChange = [this] {
        if (refreshing_) return;
        applySetup([this](auto& s) { s.outputDeviceName = outDev_.getText(); s.useDefaultOutputChannels = true; });
    };
    inDev_.onChange = [this] {
        if (refreshing_) return;
        applySetup([this](auto& s) {
            s.inputDeviceName = inDev_.getSelectedId() == 1 ? juce::String() : inDev_.getText();
            s.useDefaultInputChannels = true;
        });
    };
    rate_.onChange = [this] {
        if (!refreshing_) applySetup([this](auto& s) { s.sampleRate = double(rate_.getSelectedId()); });
    };
    buf_.onChange = [this] {
        if (!refreshing_) applySetup([this](auto& s) { s.bufferSize = buf_.getSelectedId(); });
    };
    auto monitorChanged = [this] {
        if (refreshing_) return;
        io_.monitorOut = firstOf(outPair_);
        io_.monitorSix = six_.getToggleState();
        changed();
        refresh();
    };
    outPair_.onChange = monitorChanged;
    six_.onClick = monitorChanged;
    defIn_.onChange = [this] {
        if (refreshing_) return;
        io_.defaultInput = firstOf(defIn_);
        changed();
    };
    auto whereChanged = [this] {
        if (refreshing_) return;
        io_.nextToSession = nextTo_.getToggleState();
        if (!io_.nextToSession && io_.folder.isEmpty()) choose_.triggerClick();
        changed();
        updateRecordingInfo();
    };
    nextTo_.onClick = whereChanged;
    chosen_.onClick = whereChanged;
    choose_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Folder for recordings", recordingsFolder(io_, sessionFile_ ? sessionFile_() : juce::File()));
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File()) {
                if (io_.folder.isEmpty()) { io_.nextToSession = true; refresh(); }
                return;
            }
            io_.folder = f.getFullPathName();
            io_.nextToSession = false;
            changed();
            refresh();
        });
    };
    format_.onChange = [this] {
        if (refreshing_) return;
        io_.bits = format_.getSelectedId() == 32 ? 32 : 24;
        changed();
        updateRecordingInfo();
    };
    monitor_.onClick = [this] {
        io_.inputMonitoring = monitor_.getToggleState();
        changed();
    };
    delayOut_.onChange = [this] { updateDelayInfo(); };
    delayIn_.onChange = [this] { updateDelayInfo(); };
    measure_.onClick = [this] {
        if (e_.numInputChannelsOnDevice() == 0) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Round-trip delay", "Choose an input interface first.");
            return;
        }
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon, "Measure the round-trip delay",
            "Patch output " + delayOut_.getText().upToFirstOccurrenceOf("  ", false, false) + " back into input " +
                delayIn_.getText().upToFirstOccurrenceOf("  ", false, false) +
                ", with a cable or through the gear with nothing switched in that adds delay of its own. Turn your monitors down, then press OK: one "
                "click is played.",
            "OK", this, juce::ModalCallbackFunction::create([this](int) {
                e_.startLatencyTest(firstOf(delayIn_), firstOf(delayOut_));
                testing_ = true;
            }));
    };
    e_.devices().addChangeListener(this);
    refresh();
    startTimerHz(5);
    setSize(600, 640);
}

AudioSetupPanel::~AudioSetupPanel() { e_.devices().removeChangeListener(this); }

void AudioSetupPanel::applySetup(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup&)>& change) {
    auto setup = e_.devices().getAudioDeviceSetup();
    change(setup);
    auto err = e_.devices().setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Audio Setup", err);
    if (io_.monitorOut >= 0) e_.setMonitorOutput(io_.monitorOut, io_.monitorSix);   // (a new interface: its outputs switched on again)
    changed();
    refresh();
}

juce::String AudioSetupPanel::delayKey() const {
    return IoSettings::delayKey(e_.outputDeviceName(), e_.inputDeviceName(), firstOf(delayOut_), firstOf(delayIn_), e_.deviceRate(), e_.bufferSize());
}

void AudioSetupPanel::refresh() {
    refreshing_ = true;
    auto& dm = e_.devices();
    auto setup = dm.getAudioDeviceSetup();
    auto* dev = dm.getCurrentAudioDevice();
    juce::StringArray outs, ins;
    if (auto* type = dm.getCurrentDeviceTypeObject()) {
        outs = type->getDeviceNames(false);
        ins = type->getDeviceNames(true);
    }
    outDev_.clear(juce::dontSendNotification);
    for (int i = 0; i < outs.size(); ++i) outDev_.addItem(outs[i], i + 1);
    outDev_.setText(setup.outputDeviceName, juce::dontSendNotification);
    inDev_.clear(juce::dontSendNotification);
    inDev_.addItem("None (no recording)", 1);
    for (int i = 0; i < ins.size(); ++i) inDev_.addItem(ins[i], i + 2);
    if (setup.inputDeviceName.isEmpty()) inDev_.setSelectedId(1, juce::dontSendNotification);
    else inDev_.setText(setup.inputDeviceName, juce::dontSendNotification);
    rate_.clear(juce::dontSendNotification);
    buf_.clear(juce::dontSendNotification);
    if (dev) {
        for (double r : dev->getAvailableSampleRates()) rate_.addItem(juce::String(r / 1000.0, 1) + " kHz", juce::roundToInt(r));
        rate_.setSelectedId(juce::roundToInt(dev->getCurrentSampleRate()), juce::dontSendNotification);
        const double sr = dev->getCurrentSampleRate();
        for (int b : dev->getAvailableBufferSizes()) buf_.addItem(juce::String(b) + " samples  (" + juce::String(b * 1000.0 / sr, 1) + " ms)", b);
        buf_.setSelectedId(dev->getCurrentBufferSizeSamples(), juce::dontSendNotification);
    }
    const auto outPairs = e_.outputPairNames(), inPairs = e_.inputPairNames();
    // Never chosen: playback is on the first outputs that are switched on.
    int mon = io_.monitorOut;
    if (mon < 0 && dev) mon = juce::jmax(0, dev->getActiveOutputChannels().findNextSetBit(0));
    fillPairs(outPair_, outPairs, mon);
    six_.setEnabled(e_.numOutputChannels() >= juce::jmax(0, mon) + 6);
    six_.setToggleState(io_.monitorSix && six_.isEnabled(), juce::dontSendNotification);
    fillPairs(defIn_, inPairs, io_.defaultInput);
    const int keepOut = delayOut_.getSelectedId(), keepIn = delayIn_.getSelectedId();
    fillPairs(delayOut_, outPairs, keepOut > 0 ? keepOut - 1 : io_.captureOut);
    fillPairs(delayIn_, inPairs, keepIn > 0 ? keepIn - 1 : io_.captureIn);
    // One button per input pair.
    const int have = dev ? dev->getInputChannelNames().size() : 0;
    const int pairs = juce::jmin(32, (have + 1) / 2);
    if (inPairs_.size() != pairs) {
        inPairs_.clear();
        for (int i = 0; i < pairs; ++i) {
            auto* b = inPairs_.add(new juce::TextButton(i * 2 + 1 < have ? juce::String(i * 2 + 1) + "-" + juce::String(i * 2 + 2) : juce::String(i * 2 + 1)));
            styleButton(*b, false);
            b->setClickingTogglesState(true);
            b->onClick = [this, i, b] {
                const bool on = b->getToggleState();
                applySetup([&](auto& s) {
                    auto* d = e_.devices().getCurrentAudioDevice();
                    if (d) s.inputChannels = d->getActiveInputChannels();
                    s.useDefaultInputChannels = false;
                    s.inputChannels.setRange(i * 2, 2, on);
                });
            };
            addAndMakeVisible(b);
        }
        resized();
    }
    if (dev) {
        const auto active = dev->getActiveInputChannels();
        for (int i = 0; i < inPairs_.size(); ++i) inPairs_[i]->setToggleState(active[i * 2] || active[i * 2 + 1], juce::dontSendNotification);
    }
    nextTo_.setToggleState(io_.nextToSession, juce::dontSendNotification);
    chosen_.setToggleState(!io_.nextToSession, juce::dontSendNotification);
    format_.setSelectedId(io_.bits == 32 ? 32 : 24, juce::dontSendNotification);
    monitor_.setToggleState(io_.inputMonitoring, juce::dontSendNotification);
    // Warnings.
    juce::StringArray w;
    if (!dev) w.add("No audio interface is open.");
    if (e_.splitDevices())
        w.add("Inputs and outputs are on different interfaces. Unless they share a clock (word clock, or an aggregate device with drift correction), "
              "they drift apart and a capture slowly slides out of sync.");
    const double sr = sessionRate_ ? sessionRate_() : 0.0;
    if (dev && sr > 0 && std::abs(sr - dev->getCurrentSampleRate()) > 0.5)
        w.add("The interface is at " + juce::String(dev->getCurrentSampleRate() / 1000.0, 1) + " kHz and the session is " + juce::String(sr / 1000.0, 1) +
              " kHz: playback is resampled, and a recording made now is at the interface's rate.");
    if (dev && have == 0) w.add("No inputs: choose an input interface to record, capture or measure the round trip.");
    warn_.setText(w.joinIntoString("\n"), juce::dontSendNotification);
    refreshing_ = false;
    updateRecordingInfo();
    updateDelayInfo();
}

void AudioSetupPanel::updateRecordingInfo() {
    const auto session = sessionFile_ ? sessionFile_() : juce::File();
    const auto folder = recordingsFolder(io_, session);
    juce::String space;
    auto probe = folder;
    while (!probe.exists() && probe.getParentDirectory() != probe) probe = probe.getParentDirectory();
    const auto freeBytes = probe.getBytesFreeOnVolume();
    if (freeBytes > 0) {
        const double perHour = double(recordingBytes(3600.0, e_.deviceRate(), 2, io_.bits));
        space = "     " + byteSize(freeBytes) + " free (about " + juce::String(double(freeBytes) / perHour, 1) + " hours of stereo)";
    }
    pathL_.setText(folder.getFullPathName() + space, juce::dontSendNotification);
    pathL_.setTooltip(folder.getFullPathName());
    nameL_.setText("Named like:  " + recordingName(session, "Capture", juce::Time::getCurrentTime()) + ".wav" +
                       (!session.existsAsFile() && io_.nextToSession ? "\nSave the session and its recordings go next to it." : ""),
                   juce::dontSendNotification);
}

void AudioSetupPanel::updateDelayInfo() {
    const int saved = io_.delay(delayKey());
    const double rate = e_.deviceRate();
    delayInfo_.setColour(juce::Label::textColourId, saved > 0 ? kGood : colours::dim);
    delayInfo_.setText(saved > 0 ? "Measured: " + samplesAndMs(saved, rate) + ". Captures on these sockets are moved earlier by this much."
                                 : "Not measured for these sockets at this sample rate and buffer. Until it is, the interface's own figure is used: " +
                                       samplesAndMs(e_.reportedRoundTrip(), rate) + ".",
                       juce::dontSendNotification);
}

void AudioSetupPanel::timerCallback() {
    if (testing_ && e_.latencyTestResult() >= 0) {
        testing_ = false;
        const int v = e_.latencyTestResult();
        if (v > 0) {
            io_.delays[delayKey()] = v;
            changed();
        } else {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Round-trip delay",
                                                   "No click came back on that input. Check the patch and the levels, then measure again.");
        }
        updateDelayInfo();
    }
}

void AudioSetupPanel::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    g.setColour(colours::line);
    for (auto* h : {&inHead_, &recHead_}) g.drawHorizontalLine(h->getY() - 6, 12.0f, float(getWidth() - 12));
}

void AudioSetupPanel::resized() {
    auto r = getLocalBounds().reduced(14, 10);
    const int lw = 132;
    auto row = [&](juce::Label& l, int h = 26) {
        auto x = r.removeFromTop(h);
        r.removeFromTop(6);
        l.setBounds(x.removeFromLeft(lw));
        return x;
    };
    outHead_.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    outDev_.setBounds(row(outDevL_));
    {
        auto x = row(rateL_);
        rate_.setBounds(x.removeFromLeft(130));
        x.removeFromLeft(16);
        bufL_.setBounds(x.removeFromLeft(50));
        buf_.setBounds(x);
    }
    {
        auto x = row(outPairL_);
        outPair_.setBounds(x.removeFromLeft(190));
        x.removeFromLeft(10);
        six_.setBounds(x);
    }
    r.removeFromTop(8);
    inHead_.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    inDev_.setBounds(row(inDevL_));
    {
        const int perRow = 9, rows = juce::jmax(1, (inPairs_.size() + perRow - 1) / perRow);
        auto x = row(inPairsL_, rows * 24 + (rows - 1) * 3);
        for (int i = 0; i < inPairs_.size(); ++i)
            inPairs_[i]->setBounds(x.getX() + (i % perRow) * 49, x.getY() + (i / perRow) * 27, 46, 24);
    }
    defIn_.setBounds(row(defInL_).removeFromLeft(190));
    r.removeFromTop(8);
    recHead_.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    {
        auto x = row(whereL_);
        nextTo_.setBounds(x.removeFromLeft(160));
        chosen_.setBounds(x.removeFromLeft(120));
        choose_.setBounds(x.removeFromLeft(90));
    }
    pathL_.setBounds(r.removeFromTop(20).withTrimmedLeft(lw));
    r.removeFromTop(6);
    format_.setBounds(row(formatL_).removeFromLeft(190));
    nameL_.setBounds(r.removeFromTop(34).withTrimmedLeft(lw));
    r.removeFromTop(10);
    {
        auto x = row(delayL_);
        delayOut_.setBounds(x.removeFromLeft(150));
        x.removeFromLeft(6);
        delayIn_.setBounds(x.removeFromLeft(150));
        x.removeFromLeft(8);
        measure_.setBounds(x.removeFromLeft(90));
    }
    delayInfo_.setBounds(r.removeFromTop(34).withTrimmedLeft(lw));
    r.removeFromTop(6);
    monitor_.setBounds(r.removeFromTop(24));
    r.removeFromTop(8);
    warn_.setBounds(r);
}

// ================================================================= Capture

CapturePanel::CapturePanel(AudioEngine& e, IoSettings& io, MeterHub& out, MeterHub& in, Host host)
    : e_(e), io_(io), out_(out), in_(in), host_(std::move(host)) {
    caption(sendL_, "Send (to the gear)");
    caption(returnL_, "Return (from the gear)");
    caption(postL_, "Keep recording after the end for");
    caption(delayInfo_, "");
    delayInfo_.setJustificationType(juce::Justification::topLeft);
    for (auto* l : {&sendL_, &returnL_, &postL_, &delayInfo_}) addAndMakeVisible(*l);
    for (auto* c : {&send_, &return_, &post_}) {
        styleCombo(*c);
        addAndMakeVisible(*c);
    }
    for (int s : {0, 1, 2, 5, 10, 20, 30}) post_.addItem(s == 0 ? juce::String("no tail") : juce::String(s) + " s (the gear's tail)", s + 1);
    post_.setSelectedId(juce::roundToInt(io_.capturePostRoll) + 1, juce::dontSendNotification);
    if (post_.getSelectedId() == 0) post_.setSelectedId(3, juce::dontSendNotification);
    mute_.setButtonText("Mute the original once it is captured");
    mute_.setToggleState(io_.captureMute, juce::dontSendNotification);
    mute_.setColour(juce::ToggleButton::textColourId, colours::text);
    addAndMakeVisible(mute_);
    for (auto* b : {&measure_, &setup_, &check_, &capture_, &hearSent_, &hearCapture_, &match_, &endCheck_, &null_}) {
        styleButton(*b, false);
        addAndMakeVisible(*b);
    }
    capture_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff9a2a24));
    check_.setTooltip("Plays the range round and round through the gear without recording, so you can set the gear's levels on the meters.");
    hearSent_.setTooltip("What went to the gear (the session as it was, through the master plugins).");
    hearCapture_.setTooltip("What came back, in step with what was sent: switching is instant.");
    match_.setTooltip("Turns the louder side down to the other's loudness (measured as it plays), so you compare the sound and not the level.");
    null_.setTooltip("Subtracts the capture from what was sent and reports what is left, the timing and the gain. With a cable in place of the gear, "
                     "nothing should be left: that proves the patch and the alignment.");
    send_.onChange = [this] { routeChanged(); };
    return_.onChange = [this] { routeChanged(); };
    post_.onChange = [this] {
        io_.capturePostRoll = double(post_.getSelectedId() - 1);
        if (host_.settingsChanged) host_.settingsChanged();
    };
    mute_.onClick = [this] {
        io_.captureMute = mute_.getToggleState();
        if (host_.settingsChanged) host_.settingsChanged();
    };
    setup_.onClick = [this] { if (host_.audioSetup) host_.audioSetup(); };
    measure_.onClick = [this] {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon, "Measure the round-trip delay",
            "One click is played on the send and timed on the return. Leave the gear patched in, with anything that adds delay of its own (a "
            "digital box's look-ahead, for example) switched as you will use it, or patch a cable straight across. Turn your monitors down, then press OK.",
            "OK", this, juce::ModalCallbackFunction::create([this](int) {
                e_.startLatencyTest(firstOf(return_), firstOf(send_));
                testing_ = true;
            }));
    };
    check_.onClick = [this] { if (host_.levelCheck) host_.levelCheck(!st_.checkingLevels); };
    capture_.onClick = [this] {
        if (st_.capturing) { if (host_.stop) host_.stop(); }
        else if (host_.capture) host_.capture();
    };
    hearSent_.onClick = [this] { if (host_.hear) host_.hear(1); };
    hearCapture_.onClick = [this] { if (host_.hear) host_.hear(2); };
    endCheck_.onClick = [this] { if (host_.hear) host_.hear(0); };
    match_.onClick = [this] { if (host_.levelMatch) host_.levelMatch(!st_.levelMatch); };
    null_.onClick = [this] { if (host_.nullTest) host_.nullTest(); };
    refreshRoute();
    timerCallback();
    startTimerHz(15);
    setSize(640, 618);
}

juce::String CapturePanel::delayKey() const {
    return IoSettings::delayKey(e_.outputDeviceName(), e_.inputDeviceName(), firstOf(send_), firstOf(return_), e_.deviceRate(), e_.bufferSize());
}

int CapturePanel::roundTrip(bool* measured) const {
    const int saved = io_.delay(delayKey());
    if (measured) *measured = saved > 0;
    return saved > 0 ? saved : e_.reportedRoundTrip();
}

void CapturePanel::refreshRoute() {
    fillPairs(send_, e_.outputPairNames(), io_.captureOut);
    fillPairs(return_, e_.inputPairNames(), io_.captureIn);
}

void CapturePanel::routeChanged() {
    io_.captureOut = firstOf(send_);
    io_.captureIn = firstOf(return_);
    e_.setInputSelection(io_.captureIn, plan_.channels);
    if (host_.settingsChanged) host_.settingsChanged();
}

void CapturePanel::timerCallback() {
    // The interface changed under us (Audio Setup): list its sockets again.
    const int stamp = e_.numOutputChannels() * 1000 + e_.numInputChannelsOnDevice();
    if (stamp != deviceStamp_) {
        deviceStamp_ = stamp;
        refreshRoute();
    }
    if (testing_ && e_.latencyTestResult() >= 0) {
        testing_ = false;
        const int v = e_.latencyTestResult();
        if (v > 0) {
            io_.delays[delayKey()] = v;
            if (host_.settingsChanged) host_.settingsChanged();
        } else {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Round-trip delay",
                                                   "No click came back on the return. Check the patch and the gear's levels, then measure again.");
        }
    }
    if (host_.status) st_ = host_.status();
    if (host_.plan && !st_.capturing) plan_ = host_.plan();
    const double rate = e_.deviceRate();
    // Route.
    bool measured = false;
    const int trip = roundTrip(&measured);
    delayInfo_.setColour(juce::Label::textColourId, measured ? kGood : kWarn);
    delayInfo_.setText(measured ? "Round trip " + samplesAndMs(trip, rate) + ", measured. The capture is moved earlier by this much."
                                : "Round trip not measured for these sockets: the interface's own figure, " + samplesAndMs(trip, rate) +
                                      ", will be used. Measure now for a capture that lines up to the sample.",
                       juce::dontSendNotification);
    const int send = firstOf(send_), mon = e_.monitorOutput() < 0 ? 0 : e_.monitorOutput();
    const bool sameAsMonitor = send < mon + 2 && mon < send + plan_.channels;
    routeNote_ = sameAsMonitor ? "The send is your output pair: you hear what those outputs feed. Listen to the return on your console."
                               : e_.inputMonitoring() ? "While capturing, Lathe plays the return on your output pair (input monitoring is on)."
                                                      : "While capturing, Lathe plays nothing on your output pair: listen to the return on your console, or "
                                                        "switch input monitoring on in Audio Setup.";
    juce::StringArray warn;
    if (e_.numInputChannelsOnDevice() == 0) warn.add("No inputs: choose an input interface in Audio Setup.");
    else if (e_.numInputChannelsOnDevice() < firstOf(return_) + plan_.channels || e_.numOutputChannels() < send + plan_.channels)
        warn.add(plan_.channels == 6 ? "A 5.1 capture needs six outputs from the send and six inputs from the return."
                                     : "The interface doesn't have those sockets.");
    if (e_.splitDevices()) warn.add("Inputs and outputs are on different interfaces: without a shared clock a long capture drifts out of sync.");
    routeWarn_ = warn.joinIntoString("  ");
    // What will be captured.
    if (!plan_.valid()) {
        what_ = "Nothing to capture: there is no audio " + juce::String(plan_.range.isEmpty() ? "in the session." : "between the In and Out points.");
        disk_ = {};
    } else {
        const double len = plan_.range.getLength();
        what_ = (plan_.whole ? juce::String("The whole programme, ") : "In to Out, " + msfString(plan_.range.getStart()) + " to " + msfString(plan_.range.getEnd()) + ", ") +
                juce::String(int(len) / 60) + " min " + juce::String(int(len) % 60) + " s, " +
                (plan_.channels == 6 ? "5.1 (six channels)" : plan_.channels == 1 ? "mono" : "stereo") + ". It lands on a new lane under lane " +
                juce::String(plan_.underLane + 1) + ", in sync, after a 2 s pre-roll.";
        const auto folder = host_.folder ? host_.folder() : juce::File();
        auto probe = folder;
        while (!probe.exists() && probe.getParentDirectory() != probe) probe = probe.getParentDirectory();
        const auto need = recordingBytes(len + 2.0 + io_.capturePostRoll, rate, plan_.channels, io_.bits);
        const auto freeBytes = probe.getBytesFreeOnVolume();
        disk_ = "Needs " + byteSize(need) + (freeBytes > 0 ? "; " + byteSize(freeBytes) + " free" : juce::String()) + " in " + folder.getFullPathName() + "  (" +
                (io_.bits == 32 ? "32-bit float" : "24-bit") + ")";
    }
    // Levels.
    const double a = out_.loudness().shortTermLufs(), b = in_.loudness().shortTermLufs();
    const float inMax = juce::Decibels::gainToDecibels(e_.inputMax(), -200.0f);
    const float outPk = juce::jmax(out_.holdDb(0), out_.holdDb(1)), inPk = juce::jmax(in_.holdDb(0), in_.holdDb(1));
    levelHot_ = inMax > -1.0f;
    const bool live = st_.checkingLevels || st_.capturing;
    if (e_.inputOvers() > 0) levelNote_ = "The return clipped (" + juce::String(e_.inputOvers()) + " samples at full scale). Turn the gear's output down. Click the meters to clear.";
    else if (levelHot_) levelNote_ = "The return came within 1 dB of clipping (" + juce::String(inMax, 1) + " dBFS). Leave a little more room. Click the meters to clear.";
    else if (live && outPk > -50.0f && inPk < -60.0f) levelNote_ = "Nothing is coming back on the return. Check the patch and that the gear is switched in.";
    else if (std::isfinite(a) && std::isfinite(b) && a > -100 && b > -100)
        levelNote_ = "Gain through the gear: " + juce::String(b - a >= 0.05 ? "+" : "") + juce::String(b - a, 1) + " LU (the return against the send, last 3 s).";
    else levelNote_ = live ? "Measuring..." : "Level check plays the range round and round through the gear, without recording.";
    // Buttons.
    const bool canRun = (plan_.valid() && routeWarn_.isEmpty()) || st_.capturing || st_.checkingLevels;
    check_.setButtonText(st_.checkingLevels ? "Stop level check" : "Level check");
    check_.setEnabled(!st_.capturing && canRun);
    capture_.setButtonText(st_.capturing ? "Stop (keep what is recorded)" : "Capture");
    capture_.setEnabled(canRun);
    for (auto* c : {static_cast<juce::Component*>(&send_), static_cast<juce::Component*>(&return_), static_cast<juce::Component*>(&measure_)})
        c->setEnabled(!st_.capturing && !st_.checkingLevels);
    const bool canCheck = st_.haveTake && !st_.capturing && !st_.checkingLevels;
    for (auto* bt : {&hearSent_, &hearCapture_, &null_}) bt->setEnabled(canCheck);
    match_.setEnabled(canCheck && st_.hearing != 0);
    endCheck_.setEnabled(canCheck && st_.hearing != 0);
    hearSent_.setToggleState(st_.hearing == 1, juce::dontSendNotification);
    hearCapture_.setToggleState(st_.hearing == 2, juce::dontSendNotification);
    match_.setToggleState(st_.levelMatch, juce::dontSendNotification);
    repaint();
}

void CapturePanel::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    static const char* heads[4] = {"ROUTE", "WHAT IS CAPTURED", "LEVELS", "CHECK THE CAPTURE"};
    for (int i = 0; i < 4; ++i) {
        g.setColour(colours::accent);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(heads[i], headArea_[i], juce::Justification::centredLeft);
        if (i > 0) {
            g.setColour(colours::line);
            g.drawHorizontalLine(headArea_[i].getY() - 5, 12.0f, float(getWidth() - 12));
        }
    }
    g.setFont(juce::FontOptions(12.5f));
    {
        auto r = routeArea_;
        g.setColour(colours::dim);
        g.drawFittedText(routeNote_, r.removeFromTop(32), juce::Justification::topLeft, 2);
        g.setColour(kWarn);
        g.drawFittedText(routeWarn_, r, juce::Justification::topLeft, 2);
    }
    {
        auto r = whatArea_;
        g.setColour(plan_.valid() ? colours::text : kWarn);
        g.drawFittedText(what_, r.removeFromTop(34), juce::Justification::topLeft, 2);
        g.setColour(colours::dim);
        g.drawText(disk_, r.removeFromTop(18), juce::Justification::left, true);
    }
    {
        // OUT and IN bars side by side, level against the same scale.
        auto r = meterArea_;
        auto note = r.removeFromBottom(34);
        auto scale = [](float db, float w) { return w * juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f); };
        auto strip = [&](MeterHub& h, const juce::String& name, juce::Colour c) {
            auto row = r.removeFromTop(30);
            r.removeFromTop(4);
            g.setColour(c);
            g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
            g.drawText(name, row.removeFromLeft(44), juce::Justification::centredLeft);
            auto nums = row.removeFromRight(178);
            for (int ch = 0; ch < 2; ++ch) {
                auto b = row.removeFromTop(13).toFloat();
                row.removeFromTop(4);
                g.setColour(colours::line);
                g.fillRect(b);
                juce::ColourGradient grad(kGood, b.getX(), 0, kBad, b.getRight(), 0, false);
                grad.addColour(1.0 - 6.0 / 60.0, kWarn);
                g.setGradientFill(grad);
                g.fillRect(b.withWidth(scale(h.peakDb(ch), b.getWidth())));
                g.setColour(h.holdDb(ch) > -1.0f ? kBad : colours::text);
                g.drawVerticalLine(int(b.getX() + scale(h.holdDb(ch), b.getWidth())), b.getY(), b.getBottom());
            }
            const float pk = juce::jmax(h.holdDb(0), h.holdDb(1));
            const double st = h.loudness().shortTermLufs();
            g.setFont(juce::FontOptions(12.5f));
            g.setColour(pk > -1.0f ? kBad : colours::text);
            g.drawText("  peak " + (pk > -150 ? juce::String(pk, 1) + " dB" : juce::String("--")), nums.removeFromLeft(88), juce::Justification::centredLeft);
            g.setColour(colours::text);
            g.drawText(std::isfinite(st) && st > -150 ? juce::String(st, 1) + " LUFS" : juce::String("--"), nums, juce::Justification::centredRight);
        };
        strip(out_, "OUT", kOut);
        strip(in_, "IN", kIn);
        g.setFont(juce::FontOptions(12.5f));
        g.setColour(e_.inputOvers() > 0 || levelHot_ ? kBad : (levelNote_.startsWith("Nothing") ? kWarn : colours::text));
        g.drawFittedText(levelNote_, note, juce::Justification::topLeft, 2);
    }
    {
        auto r = progressArea_;
        if (st_.capturing) {
            g.setColour(colours::line);
            g.fillRect(r.reduced(0, 9));
            g.setColour(kBad);
            g.fillRect(r.reduced(0, 9).withWidth(int(r.getWidth() * juce::jlimit(0.0, 1.0, st_.progress))));
            g.setColour(colours::text);
            g.drawText(st_.progress >= 1.0 ? "Recording the tail..." : "Capturing  " + juce::String(int(st_.progress * 100)) + " %", r, juce::Justification::centred);
        } else {
            g.setColour(st_.message.startsWith("!") ? kWarn : colours::text);
            g.drawFittedText(st_.message.trimCharactersAtStart("!"), r, juce::Justification::centredLeft, 2);
        }
    }
    {
        auto r = checkArea_;
        g.setColour(colours::dim);
        juce::String t;
        if (!st_.haveTake) t = "After a capture: switch between what was sent and what came back, level-matched, and null-test the result.";
        else if (st_.hearing == 0) t = "The capture is on its lane. Play, then switch between the two to hear what the gear did.";
        else {
            t = st_.hearing == 2 ? "Hearing the capture." : "Hearing what was sent.";
            if (st_.matchMeasured) {
                const double d = -st_.matchDifferenceDb;   // capture against sent
                t << "  The capture is " << juce::String(std::abs(d), 1) << " LU " << (d >= 0 ? "louder" : "quieter") << " than what was sent"
                  << (st_.levelMatch ? "; the louder side is turned down to match." : ".");
            } else {
                t << "  Play a few seconds to measure the level difference.";
            }
        }
        g.drawFittedText(t, r, juce::Justification::topLeft, 2);
    }
}

void CapturePanel::resized() {
    auto r = getLocalBounds().reduced(14, 10);
    headArea_[0] = r.removeFromTop(20);
    r.removeFromTop(4);
    {
        auto x = r.removeFromTop(26);
        sendL_.setBounds(x.removeFromLeft(150));
        send_.setBounds(x.removeFromLeft(240));
        x.removeFromLeft(10);
        setup_.setBounds(x.removeFromRight(120));
    }
    r.removeFromTop(6);
    {
        auto x = r.removeFromTop(26);
        returnL_.setBounds(x.removeFromLeft(150));
        return_.setBounds(x.removeFromLeft(240));
        x.removeFromLeft(10);
        measure_.setBounds(x.removeFromRight(120));
    }
    r.removeFromTop(4);
    delayInfo_.setBounds(r.removeFromTop(34));
    routeArea_ = r.removeFromTop(64);
    r.removeFromTop(8);
    headArea_[1] = r.removeFromTop(20);
    r.removeFromTop(4);
    whatArea_ = r.removeFromTop(54);
    {
        auto x = r.removeFromTop(26);
        postL_.setBounds(x.removeFromLeft(220));
        post_.setBounds(x.removeFromLeft(170));
    }
    r.removeFromTop(4);
    mute_.setBounds(r.removeFromTop(24));
    r.removeFromTop(10);
    headArea_[2] = r.removeFromTop(20);
    r.removeFromTop(4);
    meterArea_ = r.removeFromTop(106);
    r.removeFromTop(4);
    {
        auto x = r.removeFromTop(32);
        check_.setBounds(x.removeFromLeft(150));
        x.removeFromLeft(8);
        capture_.setBounds(x.removeFromLeft(220));
        x.removeFromLeft(10);
        progressArea_ = x;
    }
    r.removeFromTop(12);
    headArea_[3] = r.removeFromTop(20);
    r.removeFromTop(4);
    {
        auto x = r.removeFromTop(28);
        hearSent_.setBounds(x.removeFromLeft(150));
        x.removeFromLeft(6);
        hearCapture_.setBounds(x.removeFromLeft(140));
        x.removeFromLeft(6);
        match_.setBounds(x.removeFromLeft(100));
        x.removeFromLeft(6);
        endCheck_.setBounds(x.removeFromLeft(56));
        null_.setBounds(x.removeFromRight(90));
    }
    r.removeFromTop(6);
    checkArea_ = r;
}

}  // namespace lathe
