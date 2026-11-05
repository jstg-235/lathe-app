// Playback: disk streaming on a background thread (BufferingAudioSource), transport,
// the AU/VST3 insert chain, and lock-free hand-off of audio to the meters.
#pragma once

#include "PluginChain.h"
#include "ProgramRenderer.h"
#include "Session.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace lathe {

// Adapts ProgramRenderer to JUCE's positionable source interface.
class ProgramSource : public juce::PositionableAudioSource {
public:
    ProgramSource(juce::AudioFormatManager& fm, const Session& s, double rate) : r_(fm) { r_.setSession(s, rate); }
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        r_.render(*info.buffer, info.startSample, info.numSamples);
    }
    void setNextReadPosition(juce::int64 p) override { r_.seek(p); }
    juce::int64 getNextReadPosition() const override { return r_.position(); }
    juce::int64 getTotalLength() const override { return r_.totalSamples(); }
    bool isLooping() const override { return false; }
private:
    ProgramRenderer r_;
};

// Single-producer/single-consumer ring of stereo samples for the meters.
class MeterTap {
public:
    MeterTap() : fifo_(1 << 18) { buf_.setSize(2, 1 << 18); }
    void push(const juce::AudioBuffer<float>& b, int start, int n);   // audio thread
    int pull(juce::AudioBuffer<float>& dst);                          // UI thread
    void clear() { fifo_.reset(); }
private:
    juce::AbstractFifo fifo_;
    juce::AudioBuffer<float> buf_;
};

class AudioEngine : public juce::AudioSource {
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& devices() { return devices_; }
    juce::AudioFormatManager& formats() { return formats_; }
    PluginChain& chain() { return chain_; }
    MeterTap& meterTap() { return tap_; }

    // Rebuilds the playback source from the session. Keeps position and play state.
    void loadSession(const Session& s);

    void play();
    void stop();
    bool isPlaying() const { return transport_.isPlaying(); }
    void setPositionSeconds(double sec);
    double positionSeconds() const { return transport_.getCurrentPosition(); }
    double lengthSeconds() const { return transport_.getLengthInSeconds(); }
    double deviceRate() const { return rate_; }

    // While exporting, the chain is used offline, so playback is suspended.
    void suspendForOffline(bool suspend);

    // AudioSource
    void prepareToPlay(int block, double rate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

private:
    juce::AudioDeviceManager devices_;
    juce::AudioFormatManager formats_;
    juce::AudioSourcePlayer player_;
    juce::TimeSliceThread readAhead_{"Lathe disk read-ahead"};
    juce::AudioTransportSource transport_;
    std::unique_ptr<ProgramSource> program_;
    std::unique_ptr<juce::BufferingAudioSource> buffered_;
    PluginChain chain_;
    MeterTap tap_;
    double rate_ = 44100;
    int block_ = 512;
    std::atomic<bool> offline_{false};
    juce::CriticalSection sourceLock_;
};

}  // namespace lathe
