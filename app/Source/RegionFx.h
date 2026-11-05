// Per-region effects: every region can have its own AU/VST3 chain, processed at the
// project rate after sample-rate conversion and before region gain, envelope and fades
// (so fades and crossfades always shape the processed sound). Latency is compensated
// per region. Chain states are saved in the session (Region::fxState).
#pragma once

#include "PluginChain.h"
#include "Session.h"

#include <map>

namespace lathe {

class RegionFxBank {
public:
    explicit RegionFxBank(PluginHost& host) : host_(host) {}
    ~RegionFxBank();

    // ---- UI thread -------------------------------------------------------------
    PluginChain* chainFor(int regionId, bool create, const juce::String& label = {});
    bool hasEffects(int regionId) const;
    // After a normal edit: create chains for regions that gained an fxState (e.g. a split
    // copy), drop chains for deleted regions, and write live plugin states into the session.
    void syncAndCapture(Session& s);
    // After undo / load: the session is the truth; rebuild chains whose saved state changed.
    void loadFrom(const Session& s, juce::StringArray& problems);
    void clear();
    void setBypassAll(bool b) { bypassAll_ = b; }
    bool bypassAll() const { return bypassAll_; }
    void closeAllEditors();

    // ---- Renderer threads --------------------------------------------------------
    void prepare(double rate, int block, bool doublePrecision);
    int latency(int regionId);
    void reset(int regionId);
    void process(int regionId, juce::AudioBuffer<double>& b, int n);
    juce::CriticalSection& lock() { return lock_; }

private:
    struct Entry {
        std::unique_ptr<PluginChain> chain;
        juce::String loaded;   // the state string this chain was built from / last captured as
    };
    PluginHost& host_;
    std::map<int, Entry> map_;
    juce::CriticalSection lock_;
    double rate_ = 0;
    int block_ = 0;
    bool double_ = false;
    std::atomic<bool> bypassAll_{false};
};

}  // namespace lathe
