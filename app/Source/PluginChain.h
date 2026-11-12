// Ordered AU/VST3 insert chain with per-slot bypass, editor windows and state recall.
// Plugins are scanned in a helper process (PluginScan.h), so a plugin that crashes or hangs
// during a scan is blacklisted instead of taking Lathe down. Plugins the user deactivates are
// hidden from menus and never loaded: a session that uses one keeps its settings, unloaded.
#pragma once

#include "../../core/dsp/Meters.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

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

// A built-in insert (not a scanned plugin), such as External Gear. Prints made through one join
// the chain after it, so the chain needs to know which slot it is.
class BuiltinInsert {
public:
    virtual ~BuiltinInsert() = default;
    virtual int anchorId() const = 0;                                    // unique per insert, saved with it
    virtual void processBypassed(juce::AudioBuffer<float>& b) = 0;       // bypassed: the signal passes, as late as when it is in
    virtual bool needsHardware() const = 0;                              // can't be rendered offline
};
// The format name built-in inserts are saved under.
inline const char* const kBuiltinFormat = "Lathe";

class PluginChain {
public:
    static constexpr int kMaxBuses = 3;
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
        BuiltinInsert* builtin = nullptr;                    // the plugin, when it is a built-in insert
    };
    // Makes built-in inserts (set by the owner of a chain that may hold them: the master chain).
    std::function<std::unique_ptr<juce::AudioPluginInstance>(const juce::PluginDescription&)> builtinFactory;
    // Lathe's key commands, given to the plugin windows (so M, Space and the rest work there too).
    juce::KeyListener* windowKeys = nullptr;
    // The built-in insert in a slot, or nullptr.
    BuiltinInsert* builtinAt(int i) const { return slots_[i] ? slots_[i]->builtin : nullptr; }
    juce::AudioPluginInstance* pluginAt(int i) const { return slots_[i] ? slots_[i]->plugin.get() : nullptr; }
    int slotOfAnchor(int anchor) const;         // -1 = not in this chain
    int builtinCount() const { return builtins_; }   // (safe on the audio thread)
    // Slot of the last hardware insert that is switched in, or -1: material entering the chain
    // before it can't be rendered offline.
    int lastLiveHardwareSlot() const;
    // Latency of the slots up to and including that insert (a print made through it is this late).
    int latencyThrough(int anchor) const;

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
    // Print buses: bus k (channels 2 + 2k and 3 + 2k of `b`) holds prints made through the insert
    // anchors[k]; it joins the chain after that insert, delayed to line up with what passes through
    // it. A bus whose insert isn't in the chain joins at the input.
    void process(juce::AudioBuffer<float>& b, int start, int n, const int* anchors = nullptr, int numBuses = 0);
    void process(juce::AudioBuffer<double>& b, int start, int n, const int* anchors = nullptr, int numBuses = 0);   // offline master path
    // Tap: what leaves that insert (before any print joins) is copied to `dest` (two channels, the
    // block's samples from 0), for printing. Anchor 0 = off. Real-time path only.
    void setTap(int anchor, juce::AudioBuffer<float>* dest) { tapDest_ = dest; tapAnchor_ = anchor; }
    // Live against print: at that insert, hear what passes through it or its print bus instead of
    // their sum; switching crossfades in 10 ms. With level match the louder side is turned down
    // (measured as it plays). Anchor 0 = off.
    void setCompareAt(int anchor) { abAnchor_ = anchor; abReset_ = true; }
    int compareAt() const { return abAnchor_; }
    void setHearPrint(bool b) { abHearPrint_ = b; }
    bool hearingPrint() const { return abHearPrint_; }
    void setCompareLevelMatch(bool b) { abMatch_ = b; }
    bool compareLevelMatch() const { return abMatch_; }
    bool compareMeasured() const { return abReady_; }
    double compareDifferenceDb() const { return abDiff_; }   // live minus print, LU
    int countDoublePrecision() const;
    int latencySamples() const;
    // The latency of the last real-time process() call (audio thread; safe to read anywhere).
    int lastLatency() const { return lastLatency_.load(); }
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
    std::atomic<int> lastLatency_{0}, builtins_{0};
    void countBuiltins();
    // print buses: a delay line per bus and channel, for each precision
    static constexpr int kRing = 1 << 17;
    std::array<std::array<std::vector<float>, 2>, kMaxBuses> ringF_;
    std::array<std::array<std::vector<double>, 2>, kMaxBuses> ringD_;
    std::array<int, kMaxBuses> ringPos_{};
    template <typename T> std::array<std::array<std::vector<T>, 2>, kMaxBuses>& rings();
    // Bus k's `n` samples delayed by `delay` into out[0], out[1] (scratch of the caller).
    template <typename T> void delayBus(int bus, const T* l, const T* r, int n, int delay, T* outL, T* outR);
    template <typename T> void joinAtInput(juce::AudioBuffer<T>& view, const juce::AudioBuffer<T>& b, int at, int k, const int* anchors, int numBuses);
    std::vector<float> busScratchF_[2];
    std::vector<double> busScratchD_[2];
    juce::AudioBuffer<float>* tapDest_ = nullptr;
    std::atomic<int> tapAnchor_{0}, abAnchor_{0};
    std::atomic<bool> abHearPrint_{false}, abMatch_{false}, abReady_{false}, abReset_{false};
    std::atomic<double> abDiff_{0.0};
    dsp::LoudnessMatch abMeter_;
    float abMix_ = 0.0f, abGainLive_ = 1.0f, abGainPrint_ = 1.0f;
};

}  // namespace lathe
