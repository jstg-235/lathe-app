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
    ProgramSource(juce::AudioFormatManager& fm, const Session& s, double rate, RegionFxBank* fx) : r_(fm) {
        r_.setRegionFx(fx);
        r_.setSession(s, rate);
    }
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        // Mix in 64-bit, convert to the device's 32-bit float once at the end.
        if (dbl_.getNumSamples() < info.numSamples) dbl_.setSize(2, info.numSamples, false, false, true);
        r_.render(dbl_, 0, info.numSamples);
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch) {
            auto* d = info.buffer->getWritePointer(ch, info.startSample);
            auto* s = dbl_.getReadPointer(juce::jmin(ch, 1));
            for (int i = 0; i < info.numSamples; ++i) d[i] = float(s[i]);
        }
    }
    void setNextReadPosition(juce::int64 p) override { r_.seek(p); }
    juce::int64 getNextReadPosition() const override { return r_.position(); }
    juce::int64 getTotalLength() const override { return r_.totalSamples(); }
    bool isLooping() const override { return false; }
private:
    ProgramRenderer r_;
    juce::AudioBuffer<double> dbl_{2, 8192};
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
    PluginHost& pluginHost() { return host_; }
    RegionFxBank& regionFx() { return fx_; }
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

    // ---- Source window audition (raw file, bypasses the chain) ----
    bool loadSource(const juce::File& f);
    void playSource();
    void stopSource();
    bool isSourcePlaying() const { return sourceTransport_.isPlaying(); }
    double sourcePosition() const { return sourceTransport_.getCurrentPosition(); }
    void setSourcePosition(double sec) { sourceTransport_.setPosition(sec); }
    double sourceLength() const { return sourceTransport_.getLengthInSeconds(); }

    // Switches the interface to `rate` when it supports it, so playback reaches the converters
    // with no sample-rate conversion anywhere (ours or the OS's). Returns a status message.
    juce::String matchDeviceRate(double rate);

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
    juce::AudioTransportSource sourceTransport_;
    std::unique_ptr<juce::AudioFormatReaderSource> sourceReader_;
    juce::AudioBuffer<float> sourceBuf_;
    PluginHost host_;
    PluginChain chain_{host_, "Master"};
    RegionFxBank fx_{host_};
    MeterTap tap_;
    double rate_ = 44100;
    int block_ = 512;
    std::atomic<bool> offline_{false};
    juce::CriticalSection sourceLock_;
};

}  // namespace lathe
