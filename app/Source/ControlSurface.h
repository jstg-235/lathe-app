// Control surfaces over MIDI:
//  - Mackie Control (MCU and compatibles: transport, jog, cursor, zoom, markers, undo/save,
//    F1-F8; transport LEDs and the timecode display),
//  - HUI keep-alive (so HUI-mode units stay online) with buttons assigned by MIDI learn,
//  - MIDI learn: any note / CC (or HUI zone+port) can trigger any Lathe command.
#pragma once

#include "Commands.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <map>

namespace lathe {

class ControlSurface : public juce::MidiInputCallback, private juce::Timer {
public:
    enum class Protocol { MackieControl = 0, Hui = 1, LearnOnly = 2 };

    ControlSurface(juce::ApplicationCommandManager& cmds, juce::PropertiesFile& props);
    ~ControlSurface() override;

    // Feedback sources (message thread).
    std::function<double()> position;   // timeline seconds for the timecode display
    std::function<bool()> isPlaying, isRecording;
    std::function<void(double)> jog;    // jog wheel: seconds to move (+/-)

    juce::String inputId() const { return inId_; }
    juce::String outputId() const { return outId_; }
    Protocol protocol() const { return proto_; }
    void configure(const juce::String& inputId, const juce::String& outputId, Protocol p);
    juce::String status() const { return status_; }

    // MIDI learn.
    void learn(juce::CommandID id) { learning_ = id; }
    bool isLearning() const { return learning_ != 0; }
    void clearMapping(juce::CommandID id);
    juce::String mappingFor(juce::CommandID id) const;
    std::function<void()> onLearned;

    // Exposed for tests: handle one message as if it arrived from the device.
    void handle(const juce::MidiMessage& m);

private:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override;
    void timerCallback() override;
    void invoke(juce::CommandID id);
    void save();
    void load();
    juce::String signature(const juce::MidiMessage& m) const;
    void sendTimecode();

    juce::ApplicationCommandManager& cmds_;
    juce::PropertiesFile& props_;
    std::unique_ptr<juce::MidiInput> in_;
    std::unique_ptr<juce::MidiOutput> out_;
    juce::String inId_, outId_, status_;
    Protocol proto_ = Protocol::MackieControl;
    std::atomic<int> learning_{0};
    std::map<juce::String, juce::CommandID> map_;   // signature -> command
    juce::CriticalSection mapLock_;
    int huiZone_ = -1;
    bool scrub_ = false;
    bool lastPlay_ = false, lastRec_ = false;
    juce::String lastTc_;
    int pingCount_ = 0;
};

}  // namespace lathe
