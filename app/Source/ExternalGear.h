// External Gear: a hardware insert for the master chain. What reaches it is sent out on an
// output pair; what comes back on an input pair carries on down the chain. The round trip
// (measured with Ping) is reported as the insert's latency, so the chain, the compare and the
// meters stay lined up, and it is kept while the insert is bypassed, so switching doesn't jump.
//
// It talks to the interface through GearIo (the audio engine): inside one device block the send
// goes out and that same block's inputs come back, so the delay is exactly the interface's.
#pragma once

#include "PluginChain.h"

namespace lathe {

class GearIo {
public:
    virtual ~GearIo() = default;
    // ---- audio thread, inside the device's block ----
    virtual int gearBlock() const = 0;                               // changes every device block
    virtual int gearBlockSize() const = 0;                           // samples in this block
    virtual const float* gearInput(int channel) const = 0;           // this block's samples on that input; nullptr = none
    // This block's send buffer for that output (it starts silent; add to it). nullptr when the
    // output doesn't exist, or is one you monitor on (sending there would feed back).
    virtual float* gearOutput(int channel) = 0;
    // ---- message thread ----
    virtual juce::StringArray gearOutputPairs() const = 0;
    virtual juce::StringArray gearInputPairs() const = 0;
    // Switches those sockets on. Returns what is wrong, or nothing.
    virtual juce::String gearUse(int firstOutput, int firstInput) = 0;
    virtual bool gearSendIsMonitor(int firstOutput) const = 0;
    virtual void gearPing(int firstInput, int firstOutput) = 0;
    virtual int gearPingResult() const = 0;                          // -1 running, 0 nothing heard, else samples
    virtual bool gearSplitDevices() const = 0;
    virtual int gearReportedRoundTrip() const = 0;
    virtual double gearRate() const = 0;
};

// What the insert's window asks the main window to do.
struct GearActions {
    std::function<void(int gear)> print;                    // print the In-Out (or everything) through this insert
    std::function<juce::String(int gear)> printStatus;      // one line: progress, or what the prints are doing
    std::function<bool(int gear)> printing;
    std::function<int(int gear)> printCount;                // prints made through it
    std::function<bool(int gear)> usingPrints;              // they play (the live path is muted)
    std::function<void(int gear, bool usePrint)> use;       // switch between the prints and the live gear
    std::function<void(int gear, int which)> hear;          // level-matched A/B: 1 live, 2 print, 0 end
    std::function<int(int gear)> hearing;
    std::function<void()> changed;                          // settings changed: save the chain with the session
};

class ExternalGear : public juce::AudioPluginInstance, public BuiltinInsert {
public:
    ExternalGear(GearIo* io, std::shared_ptr<GearActions> actions);
    static juce::PluginDescription description();

    // BuiltinInsert
    int anchorId() const override { return id_; }
    void processBypassed(juce::AudioBuffer<float>& b) override { run(b, true); }
    bool needsHardware() const override { return true; }

    // Settings (message thread; read on the audio thread)
    int sendOutput() const { return out_; }
    int returnInput() const { return in_; }
    juce::String setRoute(int firstOutput, int firstInput);   // refuses the monitor pair; returns why
    double sendDb() const { return sendDb_; }
    double returnDb() const { return returnDb_; }
    double mix() const { return mix_; }
    bool inverted() const { return invert_; }
    void setSendDb(double db) { sendDb_ = db; sendGain_ = float(juce::Decibels::decibelsToGain(db)); }
    void setReturnDb(double db) { returnDb_ = db; returnGain_ = float(juce::Decibels::decibelsToGain(db)); }
    void setMix(double wet) { mix_ = juce::jlimit(0.0, 1.0, wet); wet_ = float(mix_.load()); }
    void setInverted(bool b) { invert_ = b; }
    int ping() const { return ping_; }            // round trip in samples (0 = never measured)
    void setPing(int samples);
    GearIo* io() const { return io_; }
    GearActions* actions() const { return actions_.get(); }

    // Meters for the window: the highest sample since the last call (linear), and sticky lights.
    float takeSendPeak(int ch) { return sendPeak_[ch].exchange(0.0f); }
    float takeReturnPeak(int ch) { return returnPeak_[ch].exchange(0.0f); }
    bool sendClipped() const { return sendClip_; }
    bool returnClipped() const { return returnClip_; }
    bool returnSilent() const { return silent_; }      // signal is going out and nothing is coming back
    bool sendBlocked() const { return blocked_; }      // the send is the monitor pair (or missing): nothing is sent
    void clearLights() { sendClip_ = false; returnClip_ = false; }

    // AudioProcessor
    const juce::String getName() const override { return "External Gear"; }
    void fillInPluginDescription(juce::PluginDescription& d) const override { d = description(); }
    void prepareToPlay(double rate, int block) override;
    void releaseResources() override {}
    void reset() override;
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { run(b, false); }
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override {}
    bool supportsDoublePrecisionProcessing() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout& l) const override {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

private:
    void run(juce::AudioBuffer<float>& b, bool bypassed);
    GearIo* io_;
    std::shared_ptr<GearActions> actions_;
    int id_;
    std::atomic<int> out_{2}, in_{0}, ping_{0};
    std::atomic<double> sendDb_{0.0}, returnDb_{0.0}, mix_{1.0};
    std::atomic<float> sendGain_{1.0f}, returnGain_{1.0f}, wet_{1.0f};
    std::atomic<bool> invert_{false};
    std::atomic<float> sendPeak_[2], returnPeak_[2];
    std::atomic<bool> sendClip_{false}, returnClip_{false}, silent_{false}, blocked_{false};
    // audio thread
    int block_ = -1, pos_ = 0, silentFor_ = 0;
    double rate_ = 44100;
    std::vector<float> dry_[2];   // the signal as it arrived, delayed by the round trip
    int dryPos_ = 0;
};

}  // namespace lathe
