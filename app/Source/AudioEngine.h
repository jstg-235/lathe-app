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
    MeterTap& inputTap() { return inTap_; }   // what arrives on the armed inputs

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

    // ---- Outputs and inputs ----
    // Channel numbers here are the interface's own (0 = its first channel), whichever channels
    // happen to be switched on.
    // Where playback goes: the first channel of the monitor pair, or -1 for the first outputs that
    // are switched on. With `six`, a 5.1 programme plays on six outputs from there.
    void setMonitorOutput(int firstChannel, bool six = false);
    int monitorOutput() const { return monitorOut_; }
    juce::StringArray outputPairNames() const;
    int numOutputChannels() const;    // channels the interface has (not only the ones switched on)
    int numInputChannelsOnDevice() const;
    // Switches on the channels asked for (leaving the others as they are). Empty string = fine.
    juce::String ensureChannels(int firstInput, int inputCount, int firstOutput, int outputCount);
    // Inputs and outputs are on different interfaces (their clocks can drift apart).
    bool splitDevices() const;
    juce::String outputDeviceName() const;
    juce::String inputDeviceName() const;
    int bufferSize() const { return block_; }

    // Arming: the chosen inputs are measured (input meters) without recording. Recording and
    // capturing measure them too.
    void setArmed(bool on) { armed_ = on; }
    bool isArmed() const { return armed_; }
    bool inputsLive() const { return armed_ || recording_ || captureOn_; }
    // Which inputs are armed: first channel and how many (1, 2 or 6).
    void setInputSelection(int firstChannel, int channels);
    int inputSelection() const { return inFirst_; }
    bool inputSelectionOn() const { return inIdx_ >= 0; }   // those inputs are switched on at the interface
    int inputSelectionChannels() const { return inChannels_; }
    // Samples at or above full scale on the armed inputs since the last reset, and the highest
    // sample seen (linear).
    int inputOvers() const { return inOvers_; }
    float inputMax() const { return inMax_; }
    void resetInputOvers() { inOvers_ = 0; inMax_ = 0.0f; }
    // Input monitoring: the armed inputs are also played on the monitor pair. Never while the
    // programme is being sent to the gear on those same outputs (that would feed back).
    void setInputMonitoring(bool on) { inputMonitoring_ = on; }
    bool inputMonitoring() const { return inputMonitoring_; }

    // ---- Recording ----
    // Records `channels` inputs from firstInput to `file` (24-bit or 32-bit float WAV).
    // With withPlayback, the session plays at the same time (printing through outboard); the file
    // then starts with the first block the session plays.
    bool startRecording(const juce::File& file, int firstInput, int bits, bool withPlayback, juce::String& error, int channels = 2);
    void stopRecording();
    bool isRecording() const { return recording_; }
    double recordStartPosition() const { return recStart_; }   // timeline seconds at the first recorded sample
    // The same, exact: the timeline sample (at the device rate) that was being played when the
    // file's first sample was taken. Subtract nothing: the round trip is the caller's to add.
    juce::int64 recordStartSample() const { return recStartSample_; }
    juce::int64 recordedSamples() const { return recSamples_; }
    double recordRate() const { return rate_; }
    // Round-trip latency the interface reports (output + input), in samples.
    int reportedRoundTrip() const;
    int numInputChannels() const;
    juce::StringArray inputPairNames() const;
    // Loopback latency test: plays a click on an output pair (-1 = the monitor pair) and listens
    // on firstInput. Returns -1 while running; result in samples when done (0 = nothing heard).
    void startLatencyTest(int firstInput, int firstOutput = -1);
    int latencyTestResult() const { return latResult_; }

    // ---- Capture: the programme out to the gear on one set of outputs, the return recorded ----
    struct CaptureRequest {
        juce::File file;            // where the return is written
        bool levelCheck = false;    // route and measure only: nothing is written, nothing stops
        int firstInput = 0;         // the return
        int firstOutput = 0;        // the send
        int channels = 2;           // 1, 2 or 6
        int bits = 24;
        double from = 0, to = 0;    // timeline seconds to capture
        double preRoll = 2.0, postRoll = 2.0;
    };
    // Locates to the pre-roll, starts playback to the send outputs and records the return. The
    // programme is silent after `to`; recording carries on for the post-roll (the gear's tail)
    // and then stops by itself on the audio thread, whatever the UI is doing.
    bool startCapture(const CaptureRequest& rq, const Session& s, juce::String& error);
    void stopCapture();                                          // keeps what was recorded
    bool captureActive() const { return captureOn_; }            // routed to the gear (capturing or checking levels)
    bool captureRecording() const { return captureOn_ && !capLevelOnly_; }
    bool captureComplete() const { return capDone_; }            // the range and its post-roll are in the file
    double captureProgress() const;                              // 0..1 through the range
    // Samples the master chain delays playback by (0 for a 5.1 programme, which bypasses it). What
    // goes out to the gear is this late, so a capture is moved earlier by it as well.
    int masterLatency() const { return surround_ ? 0 : chain_.lastLatency(); }
    // Sets the unprocessed side of the compare to another session (the capture on its own), so
    // "what was sent" and "what came back" can be switched in step; level matching then turns
    // down whichever side is louder. Cleared by loadSession().
    void loadSessionWithAlternate(const Session& main, const Session& alternate);
    bool hasAlternate() const { return altCompare_; }

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
    // Waits (up to `timeoutMs`) until the read-ahead holds the next few blocks of the programme
    // and of the unprocessed copy. For tests and offline use, not the audio thread.
    bool waitForReadAhead(int timeoutMs);

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
    void syncDryTo(juce::int64 pos);   // UI thread: start the unprocessed copy where the transport now is
    bool dryFresh_ = true;   // the copy's resampler was just flushed (audio thread, or under dryLock_)
    void processCompare(const juce::AudioSourceChannelInfo& info, juce::int64 endPos, bool playing);
    void loadSessionImpl(const Session& s, const Session* alternate);
    std::atomic<bool> altCompare_{false};
    // The device's block: inputs out, programme rendered on its own, then routed to the outputs.
    void renderBlock(const juce::AudioSourceChannelInfo& info);
    void updateRouting();                       // message thread: device channel numbers -> buffer channels
    int programmeOutputs() const;               // 6 when a 5.1 programme can play on six outputs
    std::atomic<int> numIn_{-1}, numOut_{-1};   // channels switched on; -1 = no device (tests): the buffer is both
    std::atomic<int> monitorOut_{-1};
    std::atomic<bool> monitorSix_{false};
    std::atomic<int> monIdx_{0}, sendIdx_{-1}, latOutIdx_{-1}, inIdx_{0};   // buffer channels
    std::atomic<int> inFirst_{0}, inChannels_{2}, sendFirst_{0}, latOutFirst_{-1}, latInFirst_{0}, latInIdx_{0};
    bool blockPlaying_ = false;        // audio thread: the transport played this block
    juce::int64 blockEnd_ = 0;         // audio thread: the transport's read position after it
    float matchGainB_ = 1.0f;
    bool openWriter(const juce::File& file, int channels, int bits, juce::String& error);
    std::atomic<bool> armed_{false}, inputMonitoring_{false};
    std::atomic<int> inOvers_{0};
    std::atomic<float> inMax_{0.0f};
    juce::AudioBuffer<float> playBuf_;
    MeterTap inTap_;
    // capture
    std::atomic<bool> captureOn_{false}, capLevelOnly_{false}, capDone_{false};
    std::atomic<juce::int64> capFrom_{0}, capEnd_{0}, capPost_{0};
    juce::int64 capTail_ = 0;     // audio thread: samples recorded after the range
    bool capStarted_ = false;     // audio thread: the transport has been seen playing
    std::atomic<bool> recording_{false}, recWithPlayback_{false};
    std::atomic<double> recStart_{0.0};
    std::atomic<juce::int64> recSamples_{0}, recStartSample_{0};
    std::atomic<int> recChannels_{2};
    juce::TimeSliceThread writerThread_{"Lathe recorder"};
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recWriter_;
    juce::CriticalSection recLock_;
    juce::AudioBuffer<float> inBuf_;
    // latency test (audio thread)
    std::atomic<int> latState_{0};   // 0 idle, 1 running
    std::atomic<int> latResult_{0};
    int latPos_ = 0, latBest_ = -1;
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
