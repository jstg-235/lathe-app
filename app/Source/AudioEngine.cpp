#include "AudioEngine.h"

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
    readAhead_.startThread(juce::Thread::Priority::high);
    auto saved = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                     .getChildFile("Lathe").getChildFile("audio-device.xml");
    auto xml = juce::XmlDocument::parse(saved);
    devices_.initialise(0, 2, xml.get(), true);
    player_.setSource(this);
    devices_.addAudioCallback(&player_);
}

AudioEngine::~AudioEngine() {
    if (auto xml = devices_.createStateXml()) {
        auto f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Lathe");
        f.createDirectory();
        xml->writeTo(f.getChildFile("audio-device.xml"));
    }
    devices_.removeAudioCallback(&player_);
    player_.setSource(nullptr);
    transport_.setSource(nullptr);
    sourceTransport_.setSource(nullptr);
    sourceReader_.reset();
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
        program_ = std::make_unique<ProgramSource>(formats_, s, rate_);
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

void AudioEngine::playSource() {
    transport_.stop();
    if (sourceTransport_.getCurrentPosition() >= sourceTransport_.getLengthInSeconds() - 0.01) sourceTransport_.setPosition(0);
    sourceTransport_.start();
}

void AudioEngine::stopSource() { sourceTransport_.stop(); }

void AudioEngine::play() {
    sourceTransport_.stop();
    if (transport_.getCurrentPosition() >= transport_.getLengthInSeconds() - 0.01) transport_.setPosition(0);
    transport_.start();
}

void AudioEngine::stop() { transport_.stop(); }

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
    sourceBuf_.setSize(2, juce::jmax(block, 8192));
    chain_.prepare(rate, block);
    juce::ignoreUnused(rateChanged);   // the UI reloads the session when the device rate changes
}

void AudioEngine::releaseResources() {
    transport_.releaseResources();
    sourceTransport_.releaseResources();
    chain_.release();
}

void AudioEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    if (offline_) { info.clearActiveBufferRegion(); return; }
    if (sourceTransport_.isPlaying()) {
        // Source audition: raw file, no processing, still metered.
        sourceTransport_.getNextAudioBlock(info);
        tap_.push(*info.buffer, info.startSample, info.numSamples);
        return;
    }
    transport_.getNextAudioBlock(info);
    chain_.process(*info.buffer, info.startSample, info.numSamples);
    if (transport_.isPlaying()) tap_.push(*info.buffer, info.startSample, info.numSamples);
}

}  // namespace lathe
