// Ordered AU/VST3 insert chain with per-slot bypass, editor windows and state recall.
// Plugins are scanned in a helper process (PluginScan.h), so a plugin that crashes or hangs
// during a scan is blacklisted instead of taking Lathe down. Plugins the user deactivates are
// hidden from menus and never loaded: a session that uses one keeps its settings, unloaded.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace lathe {

// Plugin formats and the scanned plugin list, shared by every chain in the app.
class PluginHost : private juce::ChangeListener {
public:
    // `scanOutOfProcess`: false for tests, which have no helper to launch.
    explicit PluginHost(bool scanOutOfProcess = true);
    ~PluginHost() override;
    juce::AudioPluginFormatManager& formats() { return formats_; }
    juce::KnownPluginList& known() { return known_; }
    juce::File deadMansPedal() const;
    void saveKnownList();

    // Deactivated plugins: hidden from menus and never instantiated. Kept by identifier
    // (format, file, unique id), so a rescan doesn't bring them back.
    bool isDeactivated(const juce::PluginDescription& d) const;
    void setDeactivated(const juce::PluginDescription& d, bool deactivated);
    juce::Array<juce::PluginDescription> activeTypes() const;   // scanned and not deactivated
    int deactivatedCount() const { return deactivated_.size(); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { saveKnownList(); }
    juce::File appData() const;
    juce::File deactivatedFile() const { return appData().getChildFile("plugins-deactivated.txt"); }
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    juce::StringArray deactivated_;
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
        std::unique_ptr<juce::AudioPluginInstance> plugin;   // null for a deactivated plugin
        std::unique_ptr<juce::XmlElement> dormant;           // its saved slot, kept so nothing is lost
        bool bypassed = false;
        std::unique_ptr<juce::DocumentWindow> window;
    };

    int size() const { return slots_.size(); }
    juce::String nameAt(int i) const;
    bool isDormant(int i) const;   // a deactivated plugin held in place, not loaded
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
