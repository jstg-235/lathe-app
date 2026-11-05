// Ordered AU/VST3 insert chain with per-slot bypass, editor windows and state recall.
// Plugin scans use a "dead man's pedal" file so a plugin that crashes the scan is
// skipped next time instead of crashing Lathe again.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe {

// Plugin formats and the scanned plugin list, shared by every chain in the app.
class PluginHost {
public:
    PluginHost();
    ~PluginHost();
    juce::AudioPluginFormatManager& formats() { return formats_; }
    juce::KnownPluginList& known() { return known_; }
    juce::File deadMansPedal() const;
    void saveKnownList();
private:
    juce::File appData() const;
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
};

class PluginChain {
public:
    explicit PluginChain(PluginHost& host, juce::String label = {});
    ~PluginChain();

    PluginHost& host() { return host_; }
    juce::AudioPluginFormatManager& formats() { return host_.formats(); }
    juce::KnownPluginList& known() { return host_.known(); }
    juce::File deadMansPedal() const { return host_.deadMansPedal(); }
    void saveKnownList() { host_.saveKnownList(); }
    void setLabel(const juce::String& l) { label_ = l; }
    void reset();   // clears plugin tails/history (e.g. after a seek)

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
    void addInstance(std::unique_ptr<juce::AudioPluginInstance> inst);   // already-created plugin (tests)
    void remove(int i);
    void move(int from, int to);
    void showEditor(int i);
    void closeAllEditors();

    // Real-time side. process() never blocks: if the chain is being edited it passes audio through.
    void prepare(double rate, int block, bool doublePrecision = false);
    void release();
    void process(juce::AudioBuffer<float>& b, int start, int n);
    void process(juce::AudioBuffer<double>& b, int start, int n);   // offline master path
    int countDoublePrecision() const;
    int latencySamples() const;
    void setNonRealtime(bool offline);

    // Offline (export): exclusive use from a background thread.
    juce::CriticalSection& lock() { return lock_; }

    juce::MemoryBlock getState() const;
    void setState(const juce::MemoryBlock& state, juce::StringArray& problems);

private:
    PluginHost& host_;
    juce::String label_;
    juce::OwnedArray<Slot> slots_;
    juce::CriticalSection lock_;
    juce::AudioBuffer<float> scratch_;
    juce::AudioBuffer<double> dscratch_;
    juce::MidiBuffer midi_;
    double rate_ = 44100;
    int block_ = 512;
    bool prepared_ = false, preparedDouble_ = false;
};

}  // namespace lathe
