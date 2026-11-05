// Ordered AU/VST3 insert chain with per-slot bypass, editor windows and state recall.
// Plugin scans use a "dead man's pedal" file so a plugin that crashes the scan is
// skipped next time instead of crashing Lathe again.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe {

class PluginChain {
public:
    PluginChain();
    ~PluginChain();

    juce::AudioPluginFormatManager& formats() { return formats_; }
    juce::KnownPluginList& known() { return known_; }
    juce::File deadMansPedal() const;
    void saveKnownList();

    struct Slot {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        bool bypassed = false;
        std::unique_ptr<juce::DocumentWindow> window;
    };

    int size() const { return slots_.size(); }
    juce::String nameAt(int i) const;
    bool isBypassed(int i) const;
    void setBypassed(int i, bool b);

    bool add(const juce::PluginDescription& d, juce::String& error);
    void remove(int i);
    void move(int from, int to);
    void showEditor(int i);
    void closeAllEditors();

    // Real-time side. process() never blocks: if the chain is being edited it passes audio through.
    void prepare(double rate, int block);
    void release();
    void process(juce::AudioBuffer<float>& b, int start, int n);
    int latencySamples() const;
    void setNonRealtime(bool offline);

    // Offline (export): exclusive use from a background thread.
    juce::CriticalSection& lock() { return lock_; }

    juce::MemoryBlock getState() const;
    void setState(const juce::MemoryBlock& state, juce::StringArray& problems);

private:
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    juce::OwnedArray<Slot> slots_;
    juce::CriticalSection lock_;
    juce::AudioBuffer<float> scratch_;
    juce::MidiBuffer midi_;
    double rate_ = 44100;
    int block_ = 512;
    bool prepared_ = false;
    juce::File appData() const;
};

}  // namespace lathe
