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
    sourceReader_.reset();
    refTransport_.setSource(nullptr);
    refReader_.reset();
    playerTransport_.setSource(nullptr);
    playerReader_.reset();
    buffered_.reset();
    program_.reset();
    readAhead_.stopThread(2000);
}

void AudioEngine::loadSession(const Session& s) {
    bool wasPlaying = transport_.isPlaying();
    double pos = transport_.getCurrentPosition();
    transport_.stop();
    transport_.setSource(nullptr);
    {
        const juce::ScopedLock sl(sourceLock_);
        buffered_.reset();
        program_ = std::make_unique<ProgramSource>(formats_, s, rate_, &fx_);
        // 4 s of read-ahead absorbs slow disks and network volumes.
        buffered_ = std::make_unique<juce::BufferingAudioSource>(program_.get(), readAhead_, false,
                                                                  int(rate_ * 4), 2, true);
    }
    transport_.setSource(buffered_.get(), 0, nullptr, rate_, 2);
    transport_.setPosition(juce::jmin(pos, transport_.getLengthInSeconds()));
    if (wasPlaying) transport_.start();
}

bool AudioEngine::loadSource(const juce::File& f) {
    sourceTransport_.stop();
    sourceTransport_.setSource(nullptr);
    sourceReader_.reset();
    auto* reader = formats_.createReaderFor(f);
    if (!reader) return false;
    double rate = reader->sampleRate;
    sourceReader_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    sourceTransport_.setSource(sourceReader_.get(), int(rate * 2), &readAhead_, rate, 2);
    return true;
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

void AudioEngine::playSource() {
    transport_.stop();
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

void AudioEngine::play() {
    sourceTransport_.stop();
    playerTransport_.stop();
    if (transport_.getCurrentPosition() >= transport_.getLengthInSeconds() - 0.01) transport_.setPosition(0);
    transport_.start();
    if (refReader_) refTransport_.start();
}

void AudioEngine::stop() {
    transport_.stop();
    refTransport_.stop();
}

void AudioEngine::setPositionSeconds(double sec) {
    transport_.setPosition(juce::jlimit(0.0, juce::jmax(0.0, transport_.getLengthInSeconds()), sec));
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
    sourceBuf_.setSize(2, juce::jmax(block, 8192));
    refBuf_.setSize(2, juce::jmax(block, 8192));
    inBuf_.setSize(2, juce::jmax(block, 8192));
    chain_.prepare(rate, block);
    juce::ignoreUnused(rateChanged);   // the UI reloads the session when the device rate changes
}

void AudioEngine::releaseResources() {
    transport_.releaseResources();
    sourceTransport_.releaseResources();
    refTransport_.releaseResources();
    playerTransport_.releaseResources();
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
    if (sourceTransport_.isPlaying()) {
        // Source audition: raw file, no processing, still metered.
        sourceTransport_.getNextAudioBlock(info);
        tap_.push(*info.buffer, info.startSample, info.numSamples);
        return;
    }
    transport_.getNextAudioBlock(info);
    if (bypassMaster_) {
        info.buffer->applyGain(info.startSample, info.numSamples, bypassGain_);   // level-matched unprocessed
    } else {
        chain_.process(*info.buffer, info.startSample, info.numSamples);
    }

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
    if (transport_.isPlaying()) tap_.push(*info.buffer, info.startSample, info.numSamples);
}

}  // namespace lathe
