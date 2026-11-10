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
    devices_.initialise(2, 2, xml.get(), true);   // stereo in for recording / printing
    writerThread_.startThread();
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

void AudioEngine::loadSession(const Session& s) {
    bool wasPlaying = transport_.isPlaying();
    double pos = transport_.getCurrentPosition();
    transport_.stop();
    transport_.setSource(nullptr);
    surround_ = s.programmeChannels() == 6;
    const int chans = surround_ && deviceOutputs() >= 6 ? 6 : 2;
    playChannels_ = chans;
    {
        const juce::ScopedLock sl(sourceLock_);
        buffered_.reset();
        program_ = std::make_unique<ProgramSource>(formats_, s, rate_, &fx_, chans);
        // 4 s of read-ahead absorbs slow disks and network volumes.
        buffered_ = std::make_unique<juce::BufferingAudioSource>(program_.get(), readAhead_, false,
                                                                  int(rate_ * 4), chans, true);
    }
    transport_.setSource(buffered_.get(), 0, nullptr, rate_, chans);
    transport_.setPosition(juce::jmin(pos, transport_.getLengthInSeconds()));
    {
        // The unprocessed side of the compare. Without region effects it is simply the signal
        // before the master chain; with them it needs its own, effect-free copy of the programme.
        bool regionFx = false;
        for (auto& r : s.regions) regionFx |= r.fxState.isNotEmpty();
        std::unique_ptr<ProgramSource> prog;
        std::unique_ptr<juce::BufferingAudioSource> buf;
        std::unique_ptr<juce::ResamplingAudioSource> rs;
        if (regionFx) {
            prog = std::make_unique<ProgramSource>(formats_, s, rate_, nullptr, chans);
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

bool AudioEngine::startRecording(const juce::File& file, int firstInput, int bits, bool withPlayback, juce::String& error) {
    stopRecording();
    auto* d = devices_.getCurrentAudioDevice();
    // Make sure the requested inputs are switched on (skipped with no device, e.g. tests).
    auto setup = devices_.getAudioDeviceSetup();
    juce::BigInteger want;
    want.setRange(firstInput, 2, true);
    if (d && (setup.inputChannels & want) != want) {
        setup.useDefaultInputChannels = false;
        setup.inputChannels |= want;
        auto err = devices_.setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) { error = err; return false; }
    }
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> os(file.createOutputStream().release());
    if (!os) { error = "Couldn't create " + file.getFullPathName(); return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate_, 2, bits, {}, 0));
    if (!w) { error = "Couldn't create a " + juce::String(bits) + "-bit WAV writer."; return false; }
    os.release();
    {
        const juce::ScopedLock sl(recLock_);
        recWriter_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(w.release(), writerThread_, 1 << 19);
    }
    // Input channel index within the callback buffer: count active inputs below firstInput.
    auto active = devices_.getAudioDeviceSetup().inputChannels;
    int idx = 0;
    for (int i = 0; i < firstInput; ++i) idx += active[i] ? 1 : 0;
    recFirstInput_ = d ? idx : firstInput;
    recSamples_ = 0;
    recStart_ = transport_.getCurrentPosition();
    recWithPlayback_ = withPlayback;
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

void AudioEngine::startLatencyTest(int firstInput) {
    auto active = devices_.getAudioDeviceSetup().inputChannels;
    int idx = 0;
    for (int i = 0; i < firstInput; ++i) idx += active[i] ? 1 : 0;
    latFirstInput_ = devices_.getCurrentAudioDevice() ? idx : firstInput;
    latPos_ = 0;
    latBest_ = -1;
    latPeak_ = 0;
    latResult_ = -1;
    latState_ = 1;
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

BreakSource::BreakSource(juce::AudioFormatManager& fm, const Session& s, double rate, RegionFxBank* fx, BreakPlan plan)
    : r_(fm), plan_(plan), rate_(rate) {
    r_.setRegionFx(fx);
    r_.setSession(s, rate);
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
            if (dbl_.getNumSamples() < n) dbl_.setSize(2, n, false, false, true);
            r_.render(dbl_, 0, n);
            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch) {
                auto* d = info.buffer->getWritePointer(ch, info.startSample + done);
                auto* src = dbl_.getReadPointer(juce::jmin(ch, 1));
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
        breakSource_ = std::make_unique<BreakSource>(formats_, s, rate_, &fx_, plan);
        breakBuffered_ = std::make_unique<juce::BufferingAudioSource>(breakSource_.get(), readAhead_, false, int(rate_ * 4), 2, true);
        breakTransport_.setSource(breakBuffered_.get(), 0, nullptr, rate_, 2);
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
    inBuf_.setSize(2, juce::jmax(block, 8192));
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

void AudioEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    // AudioSourcePlayer hands us the input channels in the buffer: capture them first.
    const bool rec = recording_;
    const int latState = latState_;
    if ((rec || latState) && inBuf_.getNumSamples() >= info.numSamples) {
        const int first = rec ? recFirstInput_.load() : latFirstInput_;
        const int nch = info.buffer->getNumChannels();
        for (int ch = 0; ch < 2; ++ch) {
            int src = juce::jmin(first + ch, nch - 1);
            if (src >= 0) inBuf_.copyFrom(ch, 0, *info.buffer, src, info.startSample, info.numSamples);
            else inBuf_.clear(ch, 0, info.numSamples);
        }
    }
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
    if (rec) {
        const juce::ScopedTryLock sl(recLock_);
        if (sl.isLocked() && recWriter_) {
            if (recSamples_ == 0) recStart_ = transport_.getCurrentPosition();
            const float* ch[2] = {inBuf_.getReadPointer(0), inBuf_.getReadPointer(1)};
            recWriter_->write(ch, info.numSamples);
            recSamples_ += info.numSamples;
        }
        if (!recWithPlayback_) {
            info.clearActiveBufferRegion();   // no input monitoring (avoids feedback through outboard)
            juce::AudioBuffer<float> view(inBuf_.getArrayOfWritePointers(), 2, info.numSamples);
            tap_.push(view, 0, info.numSamples);
            return;
        }
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
            chain_.process(*info.buffer, info.startSample, info.numSamples);
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
    processCompare(info, transport_.getNextReadPosition(), playing);

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
        if (!surround_) chain_.process(*info.buffer, info.startSample, n);
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
            constexpr int kLead = 3;
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
    if (!haveDry)
        for (int ch = 0; ch < nch; ++ch) dryBuf_.copyFrom(ch, 0, *info.buffer, ch, info.startSample, n);

    // 2. The processed side. 5.1: the master chain is stereo, so it stays out of the path.
    int latency = 0;
    if (!surround_) {
        chain_.process(*info.buffer, info.startSample, n);
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
    if (dryMix_ == targetMix && matchGain_ == targetGain && dryMix_ == 0.0f && matchGain_ == 1.0f) return;   // plain A
    for (int ch = 0; ch < nch; ++ch) wetBuf_.copyFrom(ch, 0, *info.buffer, ch, info.startSample, n);
    for (int ch = nch; ch < wetBuf_.getNumChannels(); ++ch) wetBuf_.clear(ch, 0, n);
    const float step = 1.0f / float(juce::jmax(1.0, rate_ * 0.010));                  // 10 ms switch
    const float glide = float(1.0 - std::exp(-1.0 / juce::jmax(1.0, rate_ * 0.050)));   // 50 ms gain glide
    float mix = dryMix_, gain = matchGain_;
    for (int i = 0; i < n; ++i) {
        mix = targetMix > mix ? juce::jmin(targetMix, mix + step) : juce::jmax(targetMix, mix - step);
        gain += (targetGain - gain) * glide;
        if (std::abs(gain - targetGain) < 1.0e-4f) gain = targetGain;   // (a 0.001 dB step)
        for (int ch = 0; ch < nch; ++ch) {
            auto* d = info.buffer->getWritePointer(ch, info.startSample);
            d[i] = d[i] * gain * (1.0f - mix) + dryBuf_.getSample(ch, i) * mix;
        }
    }
    dryMix_ = mix;
    matchGain_ = gain;
}

}  // namespace lathe
