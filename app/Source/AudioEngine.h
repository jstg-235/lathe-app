// Playback: disk streaming on a background thread (BufferingAudioSource), transport,
// the AU/VST3 insert chain, and lock-free hand-off of audio to the meters.
#pragma once

#include "PluginChain.h"
#include "Products.h"
#include "ProgramRenderer.h"
#include "Session.h"

#include "../../core/dsp/Meters.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace lathe {

// Adapts ProgramRenderer to JUCE's positionable source interface.
class ProgramSource : public juce::PositionableAudioSource {
public:
    ProgramSource(juce::AudioFormatManager& fm, const Session& s, double rate, RegionFxBank* fx, int channels = 2) : r_(fm) {
        r_.setRegionFx(fx);
        r_.setSession(s, rate);
        dbl_.setSize(channels, 8192);
    }
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        // Mix in 64-bit, convert to the device's 32-bit float once at the end.
        if (dbl_.getNumSamples() < info.numSamples) dbl_.setSize(dbl_.getNumChannels(), info.numSamples, false, false, true);
        r_.render(dbl_, 0, info.numSamples);
        const int have = dbl_.getNumChannels();
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch) {
            auto* d = info.buffer->getWritePointer(ch, info.startSample);
            if (have > 2 && ch >= have) { juce::FloatVectorOperations::clear(d, info.numSamples); continue; }
            auto* s = dbl_.getReadPointer(have > 2 ? ch : juce::jmin(ch, 1));
            for (int i = 0; i < info.numSamples; ++i) d[i] = float(s[i]);
        }
    }
    void setNextReadPosition(juce::int64 p) override { r_.seek(p); }
    juce::int64 getNextReadPosition() const override { return r_.position(); }
    juce::int64 getTotalLength() const override { return r_.totalSamples(); }
    bool isLooping() const override { return false; }
private:
    ProgramRenderer r_;
    juce::AudioBuffer<double> dbl_;
};

// Break audition source: the end of one unit, the flip gap, then the top of the next, as
// one continuous stream (so the read-ahead buffer makes the jump seamless).
class BreakSource : public juce::PositionableAudioSource {
public:
    BreakSource(juce::AudioFormatManager& fm, const Session& s, double rate, RegionFxBank* fx, BreakPlan plan);
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;
    void setNextReadPosition(juce::int64 p) override { pos_ = p; }
    juce::int64 getNextReadPosition() const override { return pos_; }
    juce::int64 getTotalLength() const override { return juce::int64(plan_.length() * rate_); }
    bool isLooping() const override { return false; }
private:
    ProgramRenderer r_;
    BreakPlan plan_;
    double rate_;
    juce::int64 pos_ = 0;
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
    int deviceOutputs() const;
    // Channels playback uses for the loaded session: 6 for a 5.1 programme on an interface with
    // six or more outputs, otherwise 2 (5.1 folded down).
    int playbackChannels() const { return playChannels_; }
    bool surroundProgramme() const { return surround_; }

    // ---- Source timeline (its own session; region effects and the master chain don't apply) ----
    void loadSourceSession(const Session& s);   // keeps position and play state
    void playSource();
    void stopSource();
    // Linked play: source and destination run together from their own positions; you hear one
    // of them (switching crossfades in 10 ms), so takes can be compared in step.
    void playLinked();
    void setHearSource(bool b) { hearSource_ = b; }
    bool hearingSource() const { return hearSource_; }
    bool isLinkedPlaying() const { return transport_.isPlaying() && sourceTransport_.isPlaying(); }
    bool isSourcePlaying() const { return sourceTransport_.isPlaying(); }
    double sourcePosition() const { return sourceTransport_.getCurrentPosition(); }
    void setSourcePosition(double sec) { sourceTransport_.setPosition(sec); }
    double sourceLength() const { return sourceTransport_.getLengthInSeconds(); }

    // ---- Recording ----
    // Records input channels [firstInput, firstInput+1] to `file` (24-bit or 32-bit float WAV).
    // With withPlayback, the session plays at the same time (printing through outboard).
    bool startRecording(const juce::File& file, int firstInput, int bits, bool withPlayback, juce::String& error);
    void stopRecording();
    bool isRecording() const { return recording_; }
    double recordStartPosition() const { return recStart_; }   // timeline seconds at the first recorded sample
    juce::int64 recordedSamples() const { return recSamples_; }
    double recordRate() const { return rate_; }
    // Round-trip latency the interface reports (output + input), in samples.
    int reportedRoundTrip() const;
    int numInputChannels() const;
    juce::StringArray inputPairNames() const;
    // Loopback latency test: plays a click on the outputs and listens on firstInput.
    // Returns -1 while running; result in samples when done (0 = nothing heard).
    void startLatencyTest(int firstInput);
    int latencyTestResult() const { return latResult_; }

    // ---- DDP player (plays a finished image straight to the outputs, no processing) ----
    bool loadPlayer(const juce::File& image);
    void playPlayer() { transport_.stop(); sourceTransport_.stop(); playerTransport_.start(); }
    void stopPlayer() { playerTransport_.stop(); }
    bool isPlayerPlaying() const { return playerTransport_.isPlaying(); }
    double playerPosition() const { return playerTransport_.getCurrentPosition(); }
    void setPlayerPosition(double s) { playerTransport_.setPosition(s); }
    void clearPlayer();

    // ---- Break audition: unit k's tail (with its fade), the flip gap, unit k+1's head ----
    // Plays through the master chain; unit fades are applied after it, as in the render.
    void startBreakAudition(const Session& s, const BreakPlan& plan);
    void stopBreakAudition();
    bool isAuditioningBreak() const { return breakTransport_.isPlaying(); }
    // Timeline seconds being heard, or -1 during the flip gap / when stopped.
    double breakAuditionTimeline() const;

    // ---- Reference track A/B (plays alongside the master; switching crossfades in 10 ms) ----
    bool loadReference(const juce::File& f);
    void clearReference();
    bool hasReference() const { return refReader_ != nullptr; }
    void setListenToReference(bool b) { listenRef_ = b; }
    bool listeningToReference() const { return listenRef_; }
    void setReferenceGainDb(double db) { refGain_ = float(juce::Decibels::decibelsToGain(db)); }
    double referencePosition() const { return refTransport_.getCurrentPosition(); }
    void setReferencePosition(double s) { refTransport_.setPosition(s); }
    double referenceLength() const { return refTransport_.getLengthInSeconds(); }

    // ---- Processed / unprocessed compare ----
    //  A  processed: master chain and region effects (normal playback)
    //  B  unprocessed: no master chain, no region effects (edits, gains and fades stay)
    //  C  processed, turned down to the loudness of B
    // Both sides play all the time, time-aligned (the unprocessed side is delayed by the master
    // chain's latency, region effect latency is compensated per region), so switching is
    // instant and crossfades in 10 ms. The loudness of both is measured together while the
    // session plays; level matching only ever turns the processed side down. None of this
    // reaches exports. The meters always read the processed side before any match.
    void setHearUnprocessed(bool b) { hearDry_ = b; }
    bool hearingUnprocessed() const { return hearDry_; }
    void setLevelMatch(bool b) { levelMatch_ = b; }
    bool levelMatch() const { return levelMatch_; }
    // Cut applied to the processed side in C (dB, 0 or below), and whether it has been measured.
    double matchCutDb() const { return matchCut_.load(); }
    bool matchMeasured() const { return matchReady_.load(); }
    double matchDifferenceDb() const { return matchDiff_.load(); }   // processed minus unprocessed, LU
    void resetMatch() { matchResetPending_ = true; }

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
    juce::AudioTransportSource refTransport_;
    juce::AudioTransportSource playerTransport_;
    juce::AudioTransportSource breakTransport_;
    std::unique_ptr<BreakSource> breakSource_;
    std::unique_ptr<juce::BufferingAudioSource> breakBuffered_;
    BreakPlan breakPlan_;
    juce::CriticalSection breakLock_;
    std::unique_ptr<juce::AudioFormatReaderSource> playerReader_;
    juce::CriticalSection playerLock_;
    std::unique_ptr<juce::AudioFormatReaderSource> refReader_;
    juce::AudioBuffer<float> refBuf_;
    std::atomic<bool> listenRef_{false};
    std::atomic<float> refGain_{1.0f};
    // compare (audio thread state below the atomics)
    std::atomic<bool> hearDry_{false}, levelMatch_{false}, matchReady_{false}, matchResetPending_{false};
    std::atomic<double> matchCut_{0.0}, matchDiff_{0.0};
    std::unique_ptr<ProgramSource> dryProgram_;                   // only when regions have effects
    std::unique_ptr<juce::BufferingAudioSource> dryBuffered_;
    std::unique_ptr<juce::ResamplingAudioSource> dryResampler_;   // matches the transport's own (ratio 1, a few samples late)
    juce::CriticalSection dryLock_;
    juce::AudioBuffer<float> dryBuf_, wetBuf_, delayBuf_;
    int delayPos_ = 0;
    float dryMix_ = 0.0f, matchGain_ = 1.0f;
    dsp::LoudnessMatch match_;
    int matchChannels_ = 0;
    double matchRate_ = 0;
    bool dryFresh_ = true;   // the copy's resampler was just flushed (audio thread, or under dryLock_)
    void processCompare(const juce::AudioSourceChannelInfo& info, juce::int64 endPos, bool playing);
    std::atomic<bool> recording_{false}, recWithPlayback_{false};
    std::atomic<double> recStart_{0.0};
    std::atomic<juce::int64> recSamples_{0};
    std::atomic<int> recFirstInput_{0};
    juce::TimeSliceThread writerThread_{"Lathe recorder"};
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter_;
    juce::CriticalSection recLock_;
    juce::AudioBuffer<float> inBuf_;
    // latency test (audio thread)
    std::atomic<int> latState_{0};   // 0 idle, 1 running
    std::atomic<int> latResult_{0};
    int latPos_ = 0, latFirstInput_ = 0, latBest_ = -1;
    float latPeak_ = 0;
    float refMix_ = 0.0f;   // audio thread only: 0 = master, 1 = reference
    juce::CriticalSection refLock_;   // UI swaps the reference under this; audio thread only try-locks
    std::unique_ptr<ProgramSource> sourceProgram_;
    std::unique_ptr<juce::BufferingAudioSource> sourceBuffered_;
    juce::AudioBuffer<float> sourceBuf_;
    std::atomic<int> playChannels_{2};
    std::atomic<bool> surround_{false};   // 5.1 programme: the (stereo) master chain is bypassed
    std::atomic<bool> hearSource_{false};
    float srcMix_ = 0.0f;   // audio thread only: 0 = destination, 1 = source
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
