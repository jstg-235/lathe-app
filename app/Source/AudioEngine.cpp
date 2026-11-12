#include "AudioEngine.h"
#include "DdpFormat.h"

namespace lathe {

void MeterTap::push(const juce::AudioBuffer<float>& b, int start, int n) {
    int s1, n1, s2, n2;
    fifo_.prepareToWrite(n, s1, n1, s2, n2);   // drops what doesn't fit; meters lag, audio never waits
    for (int ch = 0; ch < 2; ++ch) {
        int src = juce::jmin(ch, b.getNumChannels() - 1);
        if (n1 > 0) buf_.copyFrom(ch, s1, b, src, start, n1);
        if (n2 > 0) buf_.copyFrom(ch, s2, b, src, start + n1, n2);
    }
    fifo_.finishedWrite(n1 + n2);
}

int MeterTap::pull(juce::AudioBuffer<float>& dst) {
    int n = juce::jmin(fifo_.getNumReady(), dst.getNumSamples());
    int s1, n1, s2, n2;
    fifo_.prepareToRead(n, s1, n1, s2, n2);
    for (int ch = 0; ch < 2; ++ch) {
        if (n1 > 0) dst.copyFrom(ch, 0, buf_, ch, s1, n1);
        if (n2 > 0) dst.copyFrom(ch, n1, buf_, ch, s2, n2);
    }
    fifo_.finishedRead(n1 + n2);
    return n1 + n2;
}

AudioEngine::AudioEngine() {
    formats_.registerBasicFormats();   // WAV, AIFF, FLAC, Ogg, CoreAudio formats on Mac
    formats_.registerFormat(new DdpImageFormat(), false);   // IMAGE.DAT from DDP filesets
    readAhead_.startThread(juce::Thread::Priority::high);
    auto saved = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                     .getChildFile("Lathe").getChildFile("audio-device.xml");
    auto xml = juce::XmlDocument::parse(saved);
    devices_.initialise(2, 2, xml.get(), true);   // stereo in to begin with; Audio Setup and Capture switch on more
    writerThread_.startThread();
    for (auto& m : inMap_) m = -1;
    for (auto& m : outMap_) m = -1;
    for (auto& a : busAnchor_) a = 0;
    chain_.builtinFactory = [this](const juce::PluginDescription& d) -> std::unique_ptr<juce::AudioPluginInstance> {
        if (d.fileOrIdentifier == ExternalGear::description().fileOrIdentifier) return std::make_unique<ExternalGear>(this, gearActions);
        return nullptr;
    };
    player_.setSource(this);
    devices_.addAudioCallback(&player_);
}

AudioEngine::~AudioEngine() {
    if (auto xml = devices_.createStateXml()) {
        auto f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Lathe");
        f.createDirectory();
        xml->writeTo(f.getChildFile("audio-device.xml"));
    }
    stopRecording();
    devices_.removeAudioCallback(&player_);
    player_.setSource(nullptr);
    writerThread_.stopThread(2000);
    transport_.setSource(nullptr);
    sourceTransport_.setSource(nullptr);
    sourceBuffered_.reset();
    sourceProgram_.reset();
    refTransport_.setSource(nullptr);
    refReader_.reset();
    playerTransport_.setSource(nullptr);
    playerReader_.reset();
    {
        const juce::ScopedLock dl(dryLock_);
        dryResampler_.reset();
        dryBuffered_.reset();
        dryProgram_.reset();
    }
    buffered_.reset();
    program_.reset();
    readAhead_.stopThread(2000);
}

void AudioEngine::loadSession(const Session& s) { loadSessionImpl(s, nullptr); }

void AudioEngine::loadSessionWithAlternate(const Session& main, const Session& alternate) { loadSessionImpl(main, &alternate); }

void AudioEngine::loadSessionImpl(const Session& s, const Session* alternate) {
    bool wasPlaying = transport_.isPlaying();
    double pos = transport_.getCurrentPosition();
    transport_.stop();
    transport_.setSource(nullptr);
    surround_ = s.programmeChannels() == 6;
    const int chans = surround_ && programmeOutputs() >= 6 ? 6 : 2;
    playChannels_ = chans;
    // Prints made through External Gear travel beside the programme on stereo pairs of their own,
    // and join the master chain after their insert.
    const auto anchors = chans == 2 ? s.printAnchors() : juce::Array<int>();
    const int all = chans + 2 * anchors.size();
    busCount_ = 0;
    {
        const juce::ScopedLock sl(sourceLock_);
        buffered_.reset();
        program_ = std::make_unique<ProgramSource>(formats_, s, rate_, &fx_, chans, anchors);
        // 4 s of read-ahead absorbs slow disks and network volumes.
        buffered_ = std::make_unique<juce::BufferingAudioSource>(program_.get(), readAhead_, false,
                                                                  int(rate_ * 4), all, true);
    }
    transport_.setSource(buffered_.get(), 0, nullptr, rate_, all);
    for (int k = 0; k < anchors.size(); ++k) busAnchor_[size_t(k)] = anchors[k];
    busCount_ = anchors.size();
    transport_.setPosition(juce::jmin(pos, transport_.getLengthInSeconds()));
    {
        // The unprocessed side of the compare. Without region effects it is simply the signal
        // before the master chain; with them it needs its own, effect-free copy of the programme.
        bool regionFx = false;
        for (auto& r : s.regions) regionFx |= r.fxState.isNotEmpty();
        std::unique_ptr<ProgramSource> prog;
        std::unique_ptr<juce::BufferingAudioSource> buf;
        std::unique_ptr<juce::ResamplingAudioSource> rs;
        if (regionFx || alternate) {
            prog = std::make_unique<ProgramSource>(formats_, alternate ? *alternate : s, rate_, nullptr, chans);
            buf = std::make_unique<juce::BufferingAudioSource>(prog.get(), readAhead_, false, int(rate_ * 4), chans, false);
            buf->setNextReadPosition(transport_.getNextReadPosition());   // re-placed on the first block anyway
            // The transport plays through a resampler even at ratio 1 (it delays a few samples), so
            // the unprocessed copy goes through an identical one to stay sample-aligned.
            rs = std::make_unique<juce::ResamplingAudioSource>(buf.get(), false, chans);
            rs->setResamplingRatio(1.0);
            rs->prepareToPlay(block_, rate_);
        }
        const juce::ScopedLock dl(dryLock_);
        std::swap(prog, dryProgram_);
        std::swap(buf, dryBuffered_);
        std::swap(rs, dryResampler_);
        dryFresh_ = true;
        altCompare_ = alternate != nullptr;
        // the old ones are released here, after the swap, outermost first
        rs.reset();
        buf.reset();
        prog.reset();
    }
    if (wasPlaying) transport_.start();
}

int AudioEngine::deviceOutputs() const {
    if (auto* d = const_cast<juce::AudioDeviceManager&>(devices_).getCurrentAudioDevice()) return d->getActiveOutputChannels().countNumberOfSetBits();
    return 2;
}

void AudioEngine::loadSourceSession(const Session& s) {
    const bool wasPlaying = sourceTransport_.isPlaying();
    const double pos = sourceTransport_.getCurrentPosition();
    sourceTransport_.stop();
    sourceTransport_.setSource(nullptr);
    sourceBuffered_.reset();
    sourceProgram_ = std::make_unique<ProgramSource>(formats_, s, rate_, nullptr);
    sourceBuffered_ = std::make_unique<juce::BufferingAudioSource>(sourceProgram_.get(), readAhead_, false, int(rate_ * 4), 2, true);
    sourceTransport_.setSource(sourceBuffered_.get(), 0, nullptr, rate_, 2);
    sourceTransport_.prepareToPlay(block_, rate_);
    sourceTransport_.setPosition(juce::jmin(pos, sourceTransport_.getLengthInSeconds()));
    if (wasPlaying) sourceTransport_.start();
}

bool AudioEngine::loadReference(const juce::File& f) {
    bool playing = transport_.isPlaying();
    auto* reader = formats_.createReaderFor(f);
    if (!reader) return false;
    double rate = reader->sampleRate;
    auto src = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    src->setLooping(true);
    const juce::ScopedLock sl(refLock_);
    refTransport_.stop();
    refTransport_.setSource(nullptr);
    refReader_ = std::move(src);
    refTransport_.setSource(refReader_.get(), int(rate * 2), &readAhead_, rate, 2);
    refTransport_.prepareToPlay(block_, rate_);
    if (playing) refTransport_.start();
    return true;
}

int AudioEngine::numInputChannels() const {
    auto* d = devices_.getCurrentAudioDevice();
    return d ? d->getActiveInputChannels().countNumberOfSetBits() : 0;
}

juce::StringArray AudioEngine::inputPairNames() const {
    juce::StringArray out;
    auto* d = devices_.getCurrentAudioDevice();
    if (!d) return out;
    auto names = d->getInputChannelNames();
    for (int i = 0; i + 1 < names.size(); i += 2) out.add(juce::String(i + 1) + "-" + juce::String(i + 2) + "  (" + names[i] + " / " + names[i + 1] + ")");
    if (names.size() == 1) out.add("1 (mono: " + names[0] + ")");
    return out;
}

int AudioEngine::reportedRoundTrip() const {
    auto* d = devices_.getCurrentAudioDevice();
    return d ? d->getOutputLatencyInSamples() + d->getInputLatencyInSamples() : 0;
}

// ---- outputs and inputs

int AudioEngine::numOutputChannels() const {
    auto* d = devices_.getCurrentAudioDevice();
    return d ? d->getOutputChannelNames().size() : 0;
}

int AudioEngine::numInputChannelsOnDevice() const {
    auto* d = devices_.getCurrentAudioDevice();
    return d ? d->getInputChannelNames().size() : 0;
}

juce::StringArray AudioEngine::outputPairNames() const {
    juce::StringArray out;
    auto* d = devices_.getCurrentAudioDevice();
    if (!d) return out;
    auto names = d->getOutputChannelNames();
    for (int i = 0; i + 1 < names.size(); i += 2) out.add(juce::String(i + 1) + "-" + juce::String(i + 2) + "  (" + names[i] + " / " + names[i + 1] + ")");
    if (names.size() == 1) out.add("1 (mono: " + names[0] + ")");
    return out;
}

juce::String AudioEngine::outputDeviceName() const { return devices_.getAudioDeviceSetup().outputDeviceName; }
juce::String AudioEngine::inputDeviceName() const { return devices_.getAudioDeviceSetup().inputDeviceName; }

bool AudioEngine::splitDevices() const {
    auto setup = devices_.getAudioDeviceSetup();
    return setup.inputDeviceName.isNotEmpty() && setup.outputDeviceName.isNotEmpty() && setup.inputDeviceName != setup.outputDeviceName;
}

juce::String AudioEngine::ensureChannels(int firstInput, int inputCount, int firstOutput, int outputCount) {
    auto* d = devices_.getCurrentAudioDevice();
    if (!d) return {};   // no interface (tests): the caller's buffer is the interface
    const int haveIn = d->getInputChannelNames().size(), haveOut = d->getOutputChannelNames().size();
    if (inputCount > 0 && firstInput + inputCount > haveIn)
        return haveIn == 0 ? juce::String("No input device is selected. Choose one in Audio Setup.")
                           : "Inputs " + juce::String(firstInput + 1) + " to " + juce::String(firstInput + inputCount) + " are needed, and this interface has " +
                                 juce::String(haveIn) + " input" + (haveIn == 1 ? "." : "s.");
    if (outputCount > 0 && firstOutput + outputCount > haveOut)
        return "Outputs " + juce::String(firstOutput + 1) + " to " + juce::String(firstOutput + outputCount) + " are needed, and this interface has " +
               juce::String(haveOut) + " output" + (haveOut == 1 ? "." : "s.");
    juce::BigInteger wantIn, wantOut;
    if (inputCount > 0) wantIn.setRange(firstInput, inputCount, true);
    if (outputCount > 0) wantOut.setRange(firstOutput, outputCount, true);
    const auto ins = d->getActiveInputChannels(), outs = d->getActiveOutputChannels();
    if ((ins & wantIn) == wantIn && (outs & wantOut) == wantOut) { updateRouting(); return {}; }
    auto setup = devices_.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels = ins | wantIn;
    setup.outputChannels = outs | wantOut;
    auto err = devices_.setAudioDeviceSetup(setup, true);
    updateRouting();
    return err;
}

void AudioEngine::updateRouting() {
    auto* d = devices_.getCurrentAudioDevice();
    if (!d) {
        numIn_ = -1;
        numOut_ = -1;
        monIdx_ = juce::jmax(0, monitorOut_.load());
        sendIdx_ = sendFirst_.load();
        latOutIdx_ = latOutFirst_.load();
        inIdx_ = inFirst_.load();
        latInIdx_ = latInFirst_.load();
        for (int c = 0; c < kMaxHw; ++c) { inMap_[size_t(c)] = c; outMap_[size_t(c)] = c; }
        return;
    }
    const auto ins = d->getActiveInputChannels(), outs = d->getActiveOutputChannels();
    // Buffer channel of an interface channel: the switched-on channels below it. -1 when any of
    // the `width` channels from there is off.
    auto index = [](const juce::BigInteger& active, int first, int width) {
        if (first < 0) return -1;
        for (int k = 0; k < width; ++k)
            if (!active[first + k]) return -1;
        int i = 0;
        for (int c = 0; c < first; ++c) i += active[c] ? 1 : 0;
        return i;
    };
    numIn_ = ins.countNumberOfSetBits();
    numOut_ = outs.countNumberOfSetBits();
    for (int c = 0, i = 0, o = 0; c < kMaxHw; ++c) {
        inMap_[size_t(c)] = ins[c] ? i++ : -1;
        outMap_[size_t(c)] = outs[c] ? o++ : -1;
    }
    int mon = index(outs, monitorOut_, 2);
    if (mon < 0) mon = index(outs, monitorOut_, 1);
    monIdx_ = juce::jmax(0, mon);   // not switched on (or never chosen): the first outputs that are
    const int ch = recChannels_;
    sendIdx_ = index(outs, sendFirst_, ch);
    latOutIdx_ = index(outs, latOutFirst_, 2);
    int in = index(ins, inFirst_, inChannels_);
    if (in < 0) in = index(ins, inFirst_, 1);   // (a one-input interface)
    inIdx_ = in;
    latInIdx_ = index(ins, latInFirst_, 1);
}

int AudioEngine::programmeOutputs() const {
    if (captureOn_ && !printOn_) return recChannels_ == 6 ? 6 : 2;
    if (monitorOut_ < 0) return deviceOutputs();
    if (!monitorSix_) return 2;
    auto* d = devices_.getCurrentAudioDevice();
    if (!d) return 6;
    const auto outs = d->getActiveOutputChannels();
    for (int k = 0; k < 6; ++k)
        if (!outs[monitorOut_ + k]) return 2;
    return 6;
}

void AudioEngine::setMonitorOutput(int firstChannel, bool six) {
    monitorOut_ = firstChannel;
    monitorSix_ = six;
    if (firstChannel >= 0) ensureChannels(0, 0, firstChannel, six ? 6 : 2);   // (a failure leaves playback on the first outputs)
    updateRouting();
}

void AudioEngine::setInputSelection(int firstChannel, int channels) {
    inFirst_ = juce::jmax(0, firstChannel);
    inChannels_ = channels == 1 || channels == 6 ? channels : 2;
    updateRouting();
}

// ---- recording

void AudioEngine::resetLivePeaks() {
    // About 100 peaks a second; room for six hours.
    if (livePeaks_.empty()) livePeaks_.assign(size_t(1) << 21, 0.0f);
    peakChunk_ = juce::jmax(64, int(rate_ / 100.0));
    peakFill_ = 0;
    peakAcc_ = 0.0f;
    peakCount_ = 0;
}

double AudioEngine::livePosition() const {
    if (recording_ && !recWithPlayback_) return recStart_ + double(recSamples_.load()) / rate_;
    return transport_.getCurrentPosition();
}

bool AudioEngine::openWriter(const juce::File& file, int channels, int bits, juce::String& error) {
    resetLivePeaks();
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> os(file.createOutputStream().release());
    if (!os) { error = "Couldn't create " + file.getFullPathName(); return false; }
    juce::WavAudioFormat wav;
    const auto layout = channels == 6 ? juce::AudioChannelSet::create5point1()
                                      : channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate_, layout, bits, {}, 0));
    if (!w) { error = "Couldn't create a " + juce::String(bits) + "-bit WAV writer."; return false; }
    os.release();
    const juce::ScopedLock sl(recLock_);
    recWriter_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(w.release(), writerThread_, 1 << 19);
    return true;
}

bool AudioEngine::startRecording(const juce::File& file, int firstInput, int bits, bool withPlayback, juce::String& error, int channels) {
    stopCapture();
    stopRecording();
    const int ch = channels == 1 || channels == 6 ? channels : 2;
    // Make sure the requested inputs are switched on (skipped with no device, e.g. tests). A
    // one-input interface records its one input on both sides of a stereo file.
    const int have = numInputChannelsOnDevice();
    auto err = ensureChannels(firstInput, devices_.getCurrentAudioDevice() && ch == 2 && have == firstInput + 1 ? 1 : ch, 0, 0);
    if (err.isNotEmpty()) { error = err; return false; }
    recChannels_ = ch;
    setInputSelection(firstInput, ch);
    if (!openWriter(file, ch, bits, error)) return false;
    recSamples_ = 0;
    recStart_ = transport_.getCurrentPosition();
    recStartSample_ = transport_.getNextReadPosition();
    recWithPlayback_ = withPlayback;
    capStarted_ = false;
    capTail_ = 0;
    capDone_ = false;
    recording_ = true;
    if (withPlayback) play();
    return true;
}

void AudioEngine::stopRecording() {
    if (!recording_ && !recWriter_) return;
    recording_ = false;
    if (recWithPlayback_) stop();
    const juce::ScopedLock sl(recLock_);
    recWriter_.reset();   // flushes the FIFO and finalises the WAV header
}

void AudioEngine::startLatencyTest(int firstInput, int firstOutput) {
    ensureChannels(firstInput, 1, juce::jmax(0, firstOutput), firstOutput >= 0 ? 2 : 0);
    latInFirst_ = firstInput;
    latOutFirst_ = firstOutput;
    updateRouting();
    latPos_ = 0;
    latBest_ = -1;
    latPeak_ = 0;
    latResult_ = -1;
    latState_ = 1;
}

// ---- capture

bool AudioEngine::startCapture(const CaptureRequest& rq, const Session& s, juce::String& error) {
    stopCapture();
    stopRecording();
    stop();
    const int ch = rq.channels == 1 || rq.channels == 6 ? rq.channels : 2;
    if (!rq.levelCheck && rq.to <= rq.from) { error = "There is nothing to capture."; return false; }
    auto err = ensureChannels(rq.firstInput, ch, rq.firstOutput, ch);
    if (err.isNotEmpty()) { error = err; return false; }
    recChannels_ = ch;
    sendFirst_ = rq.firstOutput;
    setInputSelection(rq.firstInput, ch);
    if (devices_.getCurrentAudioDevice() && (sendIdx_ < 0 || inIdx_ < 0)) {
        error = "The interface didn't switch those channels on.";
        return false;
    }
    capLevelOnly_ = rq.levelCheck;
    capDone_ = false;
    capFrom_ = juce::int64(std::llround(rq.from * rate_));
    capEnd_ = rq.levelCheck ? std::numeric_limits<juce::int64>::max() : juce::int64(std::llround(rq.to * rate_));
    capPost_ = juce::int64(std::llround(juce::jmax(0.0, rq.postRoll) * rate_));
    capStarted_ = false;
    capTail_ = 0;
    captureOn_ = true;
    loadSessionImpl(s, nullptr);   // (a 5.1 programme now plays on the six send outputs)
    setPositionSeconds(juce::jmax(0.0, rq.from - juce::jmax(0.0, rq.preRoll)));
    waitForReadAhead(1500);   // so the very first block is the programme, not a disk still catching up
    if (!rq.levelCheck) {
        if (!openWriter(rq.file, ch, rq.bits, error)) { captureOn_ = false; return false; }
        recSamples_ = 0;
        recStart_ = transport_.getCurrentPosition();
        recStartSample_ = transport_.getNextReadPosition();
        recWithPlayback_ = true;
        recording_ = true;
    }
    play();
    return true;
}

void AudioEngine::stopCapture() {
    if (!captureOn_ && !printOn_) return;
    recording_ = false;
    stop();
    {
        const juce::ScopedLock sl(recLock_);
        recWriter_.reset();
    }
    chain_.setTap(0, nullptr);
    captureOn_ = false;
    printOn_ = false;
    capLevelOnly_ = false;
}

bool AudioEngine::startPrint(const PrintRequest& rq, const Session& s, juce::String& error) {
    stopCapture();
    stopRecording();
    stop();
    if (rq.to <= rq.from) { error = "There is nothing to print."; return false; }
    if (chain_.slotOfAnchor(rq.gear) < 0) { error = "That External Gear insert isn't in the master chain."; return false; }
    if (s.programmeChannels() == 6) { error = "A 5.1 programme doesn't use the master chain, so there is nothing to print through."; return false; }
    recChannels_ = 2;
    capLevelOnly_ = false;
    capDone_ = false;
    capFrom_ = juce::int64(std::llround(rq.from * rate_));
    capEnd_ = juce::int64(std::llround(rq.to * rate_));
    // What leaves the insert is late by the chain's delay up to there: record that much longer.
    capPost_ = juce::int64(std::llround(juce::jmax(0.0, rq.postRoll) * rate_)) + chain_.latencyThrough(rq.gear);
    capStarted_ = false;
    capTail_ = 0;
    printGear_ = rq.gear;
    tapBuf_.clear();
    chain_.setTap(rq.gear, &tapBuf_);
    printOn_ = true;
    loadSessionImpl(s, nullptr);
    setPositionSeconds(juce::jmax(0.0, rq.from - juce::jmax(0.0, rq.preRoll)));
    waitForReadAhead(1500);
    if (!openWriter(rq.file, 2, rq.bits, error)) {
        chain_.setTap(0, nullptr);
        printOn_ = false;
        return false;
    }
    recSamples_ = 0;
    recStart_ = transport_.getCurrentPosition();
    recStartSample_ = transport_.getNextReadPosition();
    recWithPlayback_ = true;
    recording_ = true;
    play();
    return true;
}

// ---- External Gear

const float* AudioEngine::gearInput(int channel) const {
    if (!juce::isPositiveAndBelow(channel, kMaxHw)) return nullptr;
    const int i = inMap_[size_t(channel)];
    return i >= 0 && i < hwInCh_ ? hwIn_.getReadPointer(i) : nullptr;
}

float* AudioEngine::gearOutput(int channel) {
    if (!juce::isPositiveAndBelow(channel, kMaxHw)) return nullptr;
    const int i = outMap_[size_t(channel)];
    if (i < 0 || i >= hwOutCh_) return nullptr;
    const int mon = monIdx_, width = playChannels_ == 6 ? 6 : 2;
    if (i >= mon && i < mon + width) return nullptr;   // never onto the outputs you listen on: that feeds back
    hwOutUsed_ = true;
    return hwOut_.getWritePointer(i);
}

bool AudioEngine::gearSendIsMonitor(int firstOutput) const {
    int mon = monitorOut_;
    if (mon < 0) {
        auto* d = devices_.getCurrentAudioDevice();
        mon = d ? juce::jmax(0, d->getActiveOutputChannels().findNextSetBit(0)) : 0;
    }
    const int width = monitorSix_ ? 6 : 2;
    return firstOutput < mon + width && mon < firstOutput + 2;
}

juce::String AudioEngine::gearUse(int firstOutput, int firstInput) { return ensureChannels(firstInput, 2, firstOutput, 2); }

juce::String AudioEngine::addExternalGear() {
    if (chain_.builtinCount() >= 4) return "Four External Gear inserts is the limit.";
    juce::String err;
    if (!chain_.add(ExternalGear::description(), err)) return err;
    auto* gear = dynamic_cast<ExternalGear*>(chain_.pluginAt(chain_.size() - 1));
    if (!gear) return {};
    // Start on the first output pair that isn't the monitor pair, and the first input pair.
    const int outs = devices_.getCurrentAudioDevice() ? numOutputChannels() : 8;
    for (int first = 0; first + 1 < outs; first += 2)
        if (!gearSendIsMonitor(first)) { gear->setRoute(first, 0); break; }
    return {};
}

int AudioEngine::runChain(juce::AudioBuffer<float>& b, int start, int n) {
    int anchors[PluginChain::kMaxBuses];
    const int buses = juce::jmin(busCount_.load(), PluginChain::kMaxBuses, juce::jmax(0, (b.getNumChannels() - 2) / 2));
    for (int k = 0; k < buses; ++k) anchors[k] = busAnchor_[size_t(k)];
    chain_.process(b, start, n, anchors, buses);
    return buses;
}

double AudioEngine::captureProgress() const {
    const double a = double(capFrom_.load()), b = double(capEnd_.load());
    if ((!captureOn_ && !printOn_) || capLevelOnly_ || b <= a) return 0.0;
    if (capDone_) return 1.0;
    return juce::jlimit(0.0, 1.0, (double(transport_.getNextReadPosition()) - a) / (b - a));
}

bool AudioEngine::loadPlayer(const juce::File& image) {
    auto* reader = formats_.createReaderFor(image);
    if (!reader) return false;
    double rate = reader->sampleRate;
    auto src = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    const juce::ScopedLock sl(playerLock_);
    playerTransport_.stop();
    playerTransport_.setSource(nullptr);
    playerReader_ = std::move(src);
    playerTransport_.setSource(playerReader_.get(), int(rate * 2), &readAhead_, rate, 2);
    playerTransport_.prepareToPlay(block_, rate_);
    return true;
}

void AudioEngine::clearPlayer() {
    const juce::ScopedLock sl(playerLock_);
    playerTransport_.stop();
    playerTransport_.setSource(nullptr);
    playerReader_.reset();
}

void AudioEngine::clearReference() {
    listenRef_ = false;
    const juce::ScopedLock sl(refLock_);
    refTransport_.stop();
    refTransport_.setSource(nullptr);
    refReader_.reset();
}

void AudioEngine::playLinked() {
    breakTransport_.stop();
    playerTransport_.stop();
    transport_.start();
    sourceTransport_.start();
    if (refReader_) refTransport_.start();
}

void AudioEngine::playSource() {
    transport_.stop();
    refTransport_.stop();
    if (sourceTransport_.getCurrentPosition() >= sourceTransport_.getLengthInSeconds() - 0.01) sourceTransport_.setPosition(0);
    sourceTransport_.start();
}

void AudioEngine::stopSource() { sourceTransport_.stop(); }

juce::String AudioEngine::matchDeviceRate(double rate) {
    auto* dev = devices_.getCurrentAudioDevice();
    if (!dev) return "No audio device.";
    if (std::abs(dev->getCurrentSampleRate() - rate) < 0.5) return {};
    bool supported = false;
    for (double r : dev->getAvailableSampleRates()) supported |= std::abs(r - rate) < 0.5;
    if (!supported)
        return dev->getName() + " can't run at " + juce::String(rate / 1000.0, 1) + " kHz; playback is resampled (export is unaffected).";
    auto setup = devices_.getAudioDeviceSetup();
    setup.sampleRate = rate;
    auto err = devices_.setAudioDeviceSetup(setup, true);
    return err;
}

BreakSource::BreakSource(juce::AudioFormatManager& fm, const Session& s, double rate, RegionFxBank* fx, BreakPlan plan, const juce::Array<int>& anchors)
    : r_(fm), plan_(plan), rate_(rate) {
    r_.setRegionFx(fx);
    if (s.programmeChannels() == 2) r_.setPrintBuses(anchors);
    r_.setSession(s, rate);
    dbl_.setSize(2 + 2 * r_.printBuses(), 8192);
}

void BreakSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    info.clearActiveBufferRegion();
    const juce::int64 aLen = juce::int64((plan_.a1 - plan_.a0) * rate_), gap = juce::int64(plan_.gap * rate_);
    const juce::int64 bLen = juce::int64((plan_.b1 - plan_.b0) * rate_);
    int done = 0;
    while (done < info.numSamples) {
        const juce::int64 p = pos_ + done;
        int n = info.numSamples - done;
        juce::int64 timeline = -1;
        if (p < aLen) { n = int(juce::jmin<juce::int64>(n, aLen - p)); timeline = juce::int64(plan_.a0 * rate_) + p; }
        else if (p < aLen + gap) { n = int(juce::jmin<juce::int64>(n, aLen + gap - p)); }
        else if (p < aLen + gap + bLen) { n = int(juce::jmin<juce::int64>(n, aLen + gap + bLen - p)); timeline = juce::int64(plan_.b0 * rate_) + (p - aLen - gap); }
        if (timeline >= 0) {
            if (r_.position() != timeline) r_.seek(timeline);
            if (dbl_.getNumSamples() < n) dbl_.setSize(dbl_.getNumChannels(), n, false, false, true);
            r_.render(dbl_, 0, n);
            for (int ch = 0; ch < juce::jmin(info.buffer->getNumChannels(), dbl_.getNumChannels()); ++ch) {
                auto* d = info.buffer->getWritePointer(ch, info.startSample + done);
                auto* src = dbl_.getReadPointer(ch);
                for (int i = 0; i < n; ++i) d[i] = float(src[i]);
            }
        }
        done += n;
    }
    pos_ += info.numSamples;
}

void AudioEngine::startBreakAudition(const Session& s, const BreakPlan& plan) {
    stopBreakAudition();
    transport_.stop();
    sourceTransport_.stop();
    playerTransport_.stop();
    {
        const juce::ScopedLock sl(breakLock_);
        breakTransport_.setSource(nullptr);
        breakBuffered_.reset();
        breakPlan_ = plan;
        const auto anchors = s.programmeChannels() == 2 ? s.printAnchors() : juce::Array<int>();
        const int all = 2 + 2 * anchors.size();
        breakSource_ = std::make_unique<BreakSource>(formats_, s, rate_, &fx_, plan, anchors);
        breakBuffered_ = std::make_unique<juce::BufferingAudioSource>(breakSource_.get(), readAhead_, false, int(rate_ * 4), all, true);
        breakTransport_.setSource(breakBuffered_.get(), 0, nullptr, rate_, all);
        breakTransport_.prepareToPlay(block_, rate_);
        breakTransport_.setPosition(0);
    }
    chain_.reset();
    breakTransport_.start();
}

void AudioEngine::stopBreakAudition() {
    breakTransport_.stop();
}

double AudioEngine::breakAuditionTimeline() const {
    if (!breakTransport_.isPlaying()) return -1;
    return breakPlan_.at(breakTransport_.getCurrentPosition()).first;
}

void AudioEngine::play() {
    breakTransport_.stop();
    sourceTransport_.stop();
    playerTransport_.stop();
    if (transport_.getCurrentPosition() >= transport_.getLengthInSeconds() - 0.01) { transport_.setPosition(0); syncDryTo(0); }
    transport_.start();
    if (refReader_) refTransport_.start();
}

void AudioEngine::stop() {
    transport_.stop();
    sourceTransport_.stop();
    breakTransport_.stop();
    refTransport_.stop();
}

void AudioEngine::setPositionSeconds(double sec) {
    transport_.setPosition(juce::jlimit(0.0, juce::jmax(0.0, transport_.getLengthInSeconds()), sec));
    syncDryTo(transport_.getNextReadPosition());
    tap_.clear();
}

void AudioEngine::suspendForOffline(bool s) {
    if (s) transport_.stop();
    offline_ = s;
    if (!s) chain_.prepare(rate_, block_);
}

void AudioEngine::prepareToPlay(int block, double rate) {
    bool rateChanged = std::abs(rate - rate_) > 0.5;
    rate_ = rate;
    block_ = block;
    transport_.prepareToPlay(block, rate);
    sourceTransport_.prepareToPlay(block, rate);
    refTransport_.prepareToPlay(block, rate);
    playerTransport_.prepareToPlay(block, rate);
    breakTransport_.prepareToPlay(block, rate);
    sourceBuf_.setSize(2, juce::jmax(block, 8192));
    refBuf_.setSize(2, juce::jmax(block, 8192));
    dryBuf_.setSize(6, juce::jmax(block, 8192));
    wetBuf_.setSize(6, juce::jmax(block, 8192));
    delayBuf_.setSize(6, 1 << 18);   // up to about 5 s of master chain latency at 48 kHz
    delayBuf_.clear();
    delayPos_ = 0;
    {
        const juce::ScopedLock dl(dryLock_);
        if (dryResampler_) dryResampler_->prepareToPlay(block, rate);
    }
    inBuf_.setSize(6, juce::jmax(block, 8192));
    playBuf_.setSize(6 + 2 * PluginChain::kMaxBuses, juce::jmax(block, 8192));
    hwIn_.setSize(kMaxHw, juce::jmax(block, 8192));
    hwOut_.setSize(kMaxHw, juce::jmax(block, 8192));
    tapBuf_.setSize(2, juce::jmax(block, 8192));
    updateRouting();
    chain_.prepare(rate, block);
    juce::ignoreUnused(rateChanged);   // the UI reloads the session when the device rate changes
}

void AudioEngine::releaseResources() {
    transport_.releaseResources();
    sourceTransport_.releaseResources();
    refTransport_.releaseResources();
    playerTransport_.releaseResources();
    breakTransport_.releaseResources();
    chain_.release();
}

namespace {
// The transport plays through a resampler even at ratio 1, which reads this many samples ahead of
// what it plays: the block just played started at (read position after it) - n - kTransportLead.
constexpr int kTransportLead = 3;
}

void AudioEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    const int n = info.numSamples;
    const int bufCh = info.buffer->getNumChannels();
    if (inBuf_.getNumSamples() < n || playBuf_.getNumSamples() < n) {   // a block bigger than prepared: plain playback
        info.clearActiveBufferRegion();
        renderBlock(info);
        return;
    }
    const int nIn = numIn_ < 0 ? bufCh : juce::jmin(numIn_.load(), bufCh);
    const int nOut = numOut_ < 0 ? bufCh : juce::jmin(numOut_.load(), bufCh);
    const bool rec = recording_, capture = captureOn_, print = printOn_;
    const int latState = latState_;

    // 0. External Gear inserts: this block's inputs for their returns, silent buffers for their sends.
    const bool gear = chain_.builtinCount() > 0;
    ++gearSerial_;
    gearN_ = gear ? n : 0;
    hwOutUsed_ = false;
    if (gear) {
        hwInCh_ = juce::jmin(nIn, hwIn_.getNumChannels());
        hwOutCh_ = juce::jmin(nOut, hwOut_.getNumChannels());
        for (int ch = 0; ch < hwInCh_; ++ch) hwIn_.copyFrom(ch, 0, *info.buffer, ch, info.startSample, n);
        for (int ch = 0; ch < hwOutCh_; ++ch) hwOut_.clear(ch, 0, n);
    }

    // 1. The inputs arrive in the device's buffer: take the armed ones out first.
    const bool live = (rec && !print) || capture || armed_ || latState;
    const int inCh = latState ? 1 : inChannels_.load();
    const int inFirst = latState ? latInIdx_.load() : inIdx_.load();
    if (live) {
        for (int ch = 0; ch < inBuf_.getNumChannels(); ++ch) {
            int src = inFirst + ch;
            if (ch == 1 && inCh == 2 && src >= nIn) src = inFirst;   // a one-input interface: its input on both sides
            if (ch < inCh && inFirst >= 0 && src < nIn) inBuf_.copyFrom(ch, 0, *info.buffer, src, info.startSample, n);
            else inBuf_.clear(ch, 0, n);
        }
        if (!latState) {
            float mx = inMax_;
            int overs = 0;
            for (int ch = 0; ch < inCh; ++ch) {
                const float m = inBuf_.getMagnitude(ch, 0, n);
                mx = juce::jmax(mx, m);
                if (m >= 1.0f) {
                    const float* x = inBuf_.getReadPointer(ch);
                    for (int i = 0; i < n; ++i) overs += std::abs(x[i]) >= 1.0f ? 1 : 0;
                }
            }
            inMax_ = mx;
            if (overs) inOvers_ += overs;
            if (inCh == 1) inBuf_.copyFrom(1, 0, inBuf_, 0, 0, n);   // mono: both meter bars
            juce::AudioBuffer<float> view(inBuf_.getArrayOfWritePointers(), 2, n);
            inTap_.push(view, 0, n);
        }
    }

    // 2. The programme, rendered on its own (so no input can leak to an output).
    const int playCh = juce::jlimit(2, 6, playChannels_.load());
    const int P = juce::jmin(playBuf_.getNumChannels(), playCh + (playCh == 2 ? 2 * busCount_.load() : 0));   // with the print buses
    juce::AudioBuffer<float> work(playBuf_.getArrayOfWritePointers(), P, n);
    work.clear();
    if (print) tapBuf_.clear(0, n);
    blockPlaying_ = false;
    renderBlock(juce::AudioSourceChannelInfo(&work, 0, n));

    // 3. Out to the right outputs: the monitor pair, or the send while capturing.
    info.clearActiveBufferRegion();
    const int mon = monIdx_;
    const int dest = latState ? (latOutIdx_ >= 0 ? latOutIdx_.load() : mon) : capture ? sendIdx_.load() : mon;
    const int width = capture && !latState ? juce::jmin(playCh, recChannels_.load()) : playCh;
    if (dest >= 0)
        for (int ch = 0; ch < width && dest + ch < nOut; ++ch) info.buffer->copyFrom(dest + ch, info.startSample, work, ch, 0, n);
    // External Gear sends (never during a latency test: its click must be the only thing out).
    if (gear && hwOutUsed_ && !latState)
        for (int ch = 0; ch < hwOutCh_ && ch < nOut; ++ch) info.buffer->addFrom(ch, info.startSample, hwOut_, ch, 0, n);
    // Input monitoring, never onto outputs that feed the gear.
    bool monitor = inputMonitoring_ && live && !latState && mon >= 0;
    if (capture) monitor = monitor && (dest < 0 || dest + width <= mon || mon + 2 <= dest);
    else if (rec && recWithPlayback_) monitor = false;
    if (monitor)
        for (int ch = 0; ch < 2 && mon + ch < nOut; ++ch) info.buffer->addFrom(mon + ch, info.startSample, inBuf_, ch, 0, n);

    // 4. Recording.
    if (rec && !latState) {
        bool write = true;
        if (recWithPlayback_ && !capStarted_) {
            // The file starts with the first block the session plays, so its place on the
            // timeline is known to the sample.
            if (blockPlaying_) {
                capStarted_ = true;
                recStartSample_ = blockEnd_ - n - kTransportLead;
                recStart_ = double(recStartSample_.load()) / rate_;
            } else {
                write = false;
            }
        } else if (!recWithPlayback_ && recSamples_ == 0) {
            recStartSample_ = transport_.getNextReadPosition();
            recStart_ = transport_.getCurrentPosition();
        }
        if (capDone_) write = false;
        if (write) {
            const juce::ScopedTryLock sl(recLock_);
            if (sl.isLocked() && recWriter_) {
                // A print is taken inside the chain, straight after its insert; everything else from the inputs.
                const float* ch[6];
                for (int c = 0; c < 6; ++c) ch[c] = print ? tapBuf_.getReadPointer(juce::jmin(c, 1)) : inBuf_.getReadPointer(c);
                recWriter_->write(ch, n);
                recSamples_ += n;
                // The take's level for the timeline, as it is written.
                const int nc = print ? 2 : juce::jlimit(1, 6, recChannels_.load());
                int count = peakCount_;
                for (int i = 0; i < n; ++i) {
                    float v = 0.0f;
                    for (int c = 0; c < nc; ++c) v = juce::jmax(v, std::abs(ch[c][i]));
                    peakAcc_ = juce::jmax(peakAcc_, v);
                    if (++peakFill_ >= peakChunk_ && count < int(livePeaks_.size())) {
                        livePeaks_[size_t(count++)] = peakAcc_;
                        peakAcc_ = 0.0f;
                        peakFill_ = 0;
                    }
                }
                peakCount_ = count;
            }
        }
        if (((capture && !capLevelOnly_) || print) && capStarted_ && !capDone_) {
            // After the range (or the end of the programme): the post-roll, then done.
            if (!blockPlaying_ || blockEnd_ - kTransportLead >= capEnd_) {
                capTail_ += n;
                if (capTail_ >= capPost_) capDone_ = true;
            }
        }
    }
}

void AudioEngine::renderBlock(const juce::AudioSourceChannelInfo& info) {
    const bool rec = recording_;
    const int latState = latState_;
    if (latState) {
        // Click at 100 ms, listen for 1 s, report the loudest arrival.
        info.clearActiveBufferRegion();
        const int clickAt = int(rate_ * 0.1);
        for (int i = 0; i < info.numSamples; ++i) {
            int p = latPos_ + i;
            if (p == clickAt)
                for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch) info.buffer->setSample(ch, info.startSample + i, 0.5f);
            float v = std::abs(inBuf_.getSample(0, i));
            if (p > clickAt && v > latPeak_) { latPeak_ = v; latBest_ = p - clickAt; }
        }
        latPos_ += info.numSamples;
        if (latPos_ > int(rate_ * 1.1)) {
            latResult_ = latPeak_ > 0.01f ? latBest_ : 0;
            latState_ = 0;
        }
        return;
    }
    if (rec && !recWithPlayback_) {
        info.clearActiveBufferRegion();   // recording on its own: nothing plays
        return;
    }
    if (offline_) { info.clearActiveBufferRegion(); return; }
    {
        const juce::ScopedTryLock psl(playerLock_);
        if (psl.isLocked() && playerReader_ && playerTransport_.isPlaying()) {
            // DDP player: the image exactly as the plant will press it.
            playerTransport_.getNextAudioBlock(info);
            tap_.push(*info.buffer, info.startSample, info.numSamples);
            return;
        }
    }
    {
        const juce::ScopedTryLock bsl(breakLock_);
        if (bsl.isLocked() && breakBuffered_ && breakTransport_.isPlaying()) {
            // Break audition: master chain, then each unit's own fade, as the render does it.
            const juce::int64 p0 = breakTransport_.getNextReadPosition();
            breakTransport_.getNextAudioBlock(info);
            runChain(*info.buffer, info.startSample, info.numSamples);
            for (int i = 0; i < info.numSamples; ++i) {
                const float g = float(breakPlan_.at(double(p0 + i) / rate_).second);
                for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch) info.buffer->getWritePointer(ch, info.startSample)[i] *= g;
            }
            tap_.push(*info.buffer, info.startSample, info.numSamples);
            if (p0 + info.numSamples >= breakTransport_.getTotalLength()) breakTransport_.stop();
            return;
        }
    }
    if (sourceTransport_.isPlaying() && !transport_.isPlaying()) {
        // Source audition: no processing, still metered.
        sourceTransport_.getNextAudioBlock(info);
        tap_.push(*info.buffer, info.startSample, info.numSamples);
        return;
    }
    const bool playing = transport_.isPlaying();
    transport_.getNextAudioBlock(info);
    blockPlaying_ = playing;
    blockEnd_ = transport_.getNextReadPosition();
    if (((captureOn_ && !capLevelOnly_) || printOn_) && playing) {
        // Capturing a range: the programme is silent after its Out point, so the post-roll
        // records the gear's tail and not the music that follows.
        const juce::int64 start = blockEnd_ - info.numSamples - kTransportLead;
        const juce::int64 keep = capEnd_.load() - start;
        if (keep < info.numSamples) {
            const int from = int(juce::jlimit<juce::int64>(0, info.numSamples, keep));
            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch) info.buffer->clear(ch, info.startSample + from, info.numSamples - from);
        }
    }
    processCompare(info, blockEnd_, playing);

    // Reference A/B: the reference runs alongside the master; a 10 ms ramp switches between them.
    const juce::ScopedTryLock refSl(refLock_);
    const bool haveRef = refSl.isLocked() && refReader_ != nullptr && refBuf_.getNumSamples() >= info.numSamples;
    const float target = (haveRef && listenRef_) ? 1.0f : 0.0f;
    if (haveRef && (refMix_ > 0.0f || target > 0.0f)) {
        juce::AudioSourceChannelInfo ri(&refBuf_, 0, info.numSamples);
        refTransport_.getNextAudioBlock(ri);
        const float g = refGain_;
        const float step = 1.0f / float(juce::jmax(1.0, rate_ * 0.010));
        for (int i = 0; i < info.numSamples; ++i) {
            refMix_ = target > refMix_ ? juce::jmin(target, refMix_ + step) : juce::jmax(target, refMix_ - step);
            for (int ch = 0; ch < juce::jmin(2, info.buffer->getNumChannels()); ++ch) {
                auto* d = info.buffer->getWritePointer(ch, info.startSample);
                d[i] = d[i] * (1.0f - refMix_) + refBuf_.getSample(ch, i) * g * refMix_;
            }
        }
    } else if (haveRef) {
        // Keep the reference's clock running while not heard, so A/B stays in step.
        juce::AudioSourceChannelInfo ri(&refBuf_, 0, info.numSamples);
        refTransport_.getNextAudioBlock(ri);
    }
    // Linked play: the source runs in step; a 10 ms ramp switches which one is heard.
    if (sourceTransport_.isPlaying() && transport_.isPlaying() && sourceBuf_.getNumSamples() >= info.numSamples) {
        juce::AudioSourceChannelInfo si(&sourceBuf_, 0, info.numSamples);
        sourceTransport_.getNextAudioBlock(si);
        const float target = hearSource_ ? 1.0f : 0.0f;
        const float step = 1.0f / float(juce::jmax(1.0, rate_ * 0.010));
        for (int i = 0; i < info.numSamples; ++i) {
            srcMix_ = target > srcMix_ ? juce::jmin(target, srcMix_ + step) : juce::jmax(target, srcMix_ - step);
            if (srcMix_ <= 0.0f) continue;
            for (int ch = 0; ch < juce::jmin(2, info.buffer->getNumChannels()); ++ch) {
                auto* d = info.buffer->getWritePointer(ch, info.startSample);
                d[i] = d[i] * (1.0f - srcMix_) + sourceBuf_.getSample(ch, i) * srcMix_;
            }
        }
    } else {
        srcMix_ = 0.0f;
    }
    if (transport_.isPlaying()) {
        // The meters read the processed side before any level match, whichever side is heard
        // (a reference or the source, when heard, is metered as heard).
        const bool other = refMix_ > 0.0f || srcMix_ > 0.0f;
        if (!other && (dryMix_ > 0.0f || matchGain_ < 1.0f) && wetBuf_.getNumSamples() >= info.numSamples) {
            juce::AudioBuffer<float> view(wetBuf_.getArrayOfWritePointers(), juce::jmin(info.buffer->getNumChannels(), wetBuf_.getNumChannels()), info.numSamples);
            tap_.push(view, 0, info.numSamples);
        } else {
            tap_.push(*info.buffer, info.startSample, info.numSamples);
        }
    }
}

void AudioEngine::syncDryTo(juce::int64 pos) {
    const juce::ScopedLock dl(dryLock_);
    if (!dryResampler_) return;
    dryResampler_->flushBuffers();
    dryBuffered_->setNextReadPosition(pos);
    dryFresh_ = true;
}

bool AudioEngine::waitForReadAhead(int timeoutMs) {
    juce::AudioBuffer<float> scratch(6, juce::jmax(512, block_) * 4);
    juce::AudioSourceChannelInfo info(&scratch, 0, scratch.getNumSamples());
    bool ok = true;
    {
        const juce::ScopedLock sl(sourceLock_);
        if (buffered_) ok = buffered_->waitForNextAudioBlockReady(info, timeoutMs) && ok;
    }
    {
        const juce::ScopedLock dl(dryLock_);
        if (dryBuffered_) ok = dryBuffered_->waitForNextAudioBlockReady(info, timeoutMs) && ok;
    }
    return ok;
}

void AudioEngine::processCompare(const juce::AudioSourceChannelInfo& info, juce::int64 endPos, bool playing) {
    const int n = info.numSamples;
    const int nch = juce::jmin(info.buffer->getNumChannels(), surround_ ? 6 : 2, dryBuf_.getNumChannels());
    const bool fits = dryBuf_.getNumSamples() >= n && nch > 0;
    if (!fits) {   // a block bigger than prepared: play processed, skip the compare this once
        if (!surround_) runChain(*info.buffer, info.startSample, n);
        return;
    }
    // 1. The unprocessed side, before the master chain.
    bool haveDry = false;
    {
        const juce::ScopedTryLock dl(dryLock_);
        if (dl.isLocked() && dryResampler_ && playing) {
            // The transport's resampler (ratio 1) reads 3 samples ahead of what it plays, so the
            // block just played started at endPos - n - 3. The copy follows the same resampler: on a
            // locate (or any slip) it is flushed and started exactly there.
            constexpr int kLead = kTransportLead;
            const juce::int64 outStart = endPos - n - kLead;
            const juce::int64 expect = dryFresh_ ? outStart : outStart + kLead;
            if (dryBuffered_->getNextReadPosition() != expect) {
                dryResampler_->flushBuffers();
                dryBuffered_->setNextReadPosition(juce::jmax<juce::int64>(0, outStart));
                dryFresh_ = true;
            }
            juce::AudioBuffer<float> view(dryBuf_.getArrayOfWritePointers(), nch, n);
            juce::AudioSourceChannelInfo di(&view, 0, n);
            dryResampler_->getNextAudioBlock(di);
            dryFresh_ = false;
            haveDry = true;
        }
    }
    if (!haveDry) {
        for (int ch = 0; ch < nch; ++ch) dryBuf_.copyFrom(ch, 0, *info.buffer, ch, info.startSample, n);
        // (prints are part of what you hear unprocessed, as they are)
        if (!surround_)
            for (int k = 0; k < busCount_ && info.buffer->getNumChannels() >= 4 + 2 * k; ++k)
                for (int ch = 0; ch < juce::jmin(2, nch); ++ch) dryBuf_.addFrom(ch, 0, *info.buffer, 2 + 2 * k + ch, info.startSample, n);
    }

    // 2. The processed side. 5.1: the master chain is stereo, so it stays out of the path.
    int latency = 0;
    if (!surround_) {
        runChain(*info.buffer, info.startSample, n);
        latency = chain_.lastLatency();
    }

    // 3. Delay the unprocessed side by the master chain's latency, so both line up.
    const int dlen = delayBuf_.getNumSamples();
    latency = juce::jlimit(0, dlen - n - 1, latency);
    for (int ch = 0; ch < nch; ++ch) {
        auto* ring = delayBuf_.getWritePointer(ch);
        auto* d = dryBuf_.getWritePointer(ch);
        for (int i = 0; i < n; ++i) {
            const int w = (delayPos_ + i) % dlen;
            ring[w] = d[i];
            d[i] = ring[(w - latency + dlen) % dlen];
        }
    }
    delayPos_ = (delayPos_ + n) % dlen;

    // 4. Measure both while the session plays.
    if (matchResetPending_.exchange(false) || matchChannels_ != nch || std::abs(matchRate_ - rate_) > 0.5) {
        match_.prepare(rate_, nch);
        matchChannels_ = nch;
        matchRate_ = rate_;
        matchReady_ = false;
        matchCut_ = 0.0;
        matchDiff_ = 0.0;
    }
    if (playing) {
        const float* a[6]{};
        const float* b[6]{};
        for (int ch = 0; ch < nch; ++ch) { a[ch] = dryBuf_.getReadPointer(ch); b[ch] = info.buffer->getReadPointer(ch, info.startSample); }
        match_.process(a, b, n);
        if (match_.ready()) {
            const double diff = match_.differenceDb();
            matchDiff_ = diff;
            matchCut_ = -juce::jmax(0.0, diff);   // only ever turn the processed side down
            matchReady_ = true;
        }
    }

    // 5. What is heard: processed (times the match), unprocessed, or a crossfade between them.
    const float targetMix = hearDry_ ? 1.0f : 0.0f;
    const float targetGain = levelMatch_ ? float(juce::Decibels::decibelsToGain(matchCut_.load())) : 1.0f;
    // Against another session (a capture), level matching turns down whichever side is louder.
    const float targetGainB = levelMatch_ && altCompare_ && matchReady_ ? float(juce::Decibels::decibelsToGain(juce::jmin(0.0, matchDiff_.load()))) : 1.0f;
    if (dryMix_ == 0.0f && targetMix == 0.0f) matchGainB_ = targetGainB;
    if (dryMix_ == targetMix && matchGain_ == targetGain && dryMix_ == 0.0f && matchGain_ == 1.0f) return;   // plain A
    for (int ch = 0; ch < nch; ++ch) wetBuf_.copyFrom(ch, 0, *info.buffer, ch, info.startSample, n);
    for (int ch = nch; ch < wetBuf_.getNumChannels(); ++ch) wetBuf_.clear(ch, 0, n);
    const float step = 1.0f / float(juce::jmax(1.0, rate_ * 0.010));                  // 10 ms switch
    const float glide = float(1.0 - std::exp(-1.0 / juce::jmax(1.0, rate_ * 0.050)));   // 50 ms gain glide
    float mix = dryMix_, gain = matchGain_, gainB = matchGainB_;
    for (int i = 0; i < n; ++i) {
        mix = targetMix > mix ? juce::jmin(targetMix, mix + step) : juce::jmax(targetMix, mix - step);
        gain += (targetGain - gain) * glide;
        if (std::abs(gain - targetGain) < 1.0e-4f) gain = targetGain;   // (a 0.001 dB step)
        gainB += (targetGainB - gainB) * glide;
        if (std::abs(gainB - targetGainB) < 1.0e-4f) gainB = targetGainB;
        for (int ch = 0; ch < nch; ++ch) {
            auto* d = info.buffer->getWritePointer(ch, info.startSample);
            d[i] = d[i] * gain * (1.0f - mix) + dryBuf_.getSample(ch, i) * gainB * mix;
        }
    }
    dryMix_ = mix;
    matchGain_ = gain;
    matchGainB_ = gainB;
}

}  // namespace lathe
