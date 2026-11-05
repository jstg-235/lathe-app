#include "RegionFx.h"

namespace lathe {

static juce::String encode(const juce::MemoryBlock& mb) { return mb.isEmpty() ? juce::String() : mb.toBase64Encoding(); }

RegionFxBank::~RegionFxBank() { clear(); }

PluginChain* RegionFxBank::chainFor(int id, bool create, const juce::String& label) {
    auto it = map_.find(id);
    if (it != map_.end()) {
        if (label.isNotEmpty()) it->second.chain->setLabel(label);
        return it->second.chain.get();
    }
    if (!create) return nullptr;
    auto chain = std::make_unique<PluginChain>(host_, label);
    if (rate_ > 0) chain->prepare(rate_, block_, double_);
    auto* raw = chain.get();
    const juce::ScopedLock sl(lock_);
    map_[id] = {std::move(chain), {}};
    return raw;
}

bool RegionFxBank::hasEffects(int id) const {
    auto it = map_.find(id);
    return it != map_.end() && it->second.chain->size() > 0;
}

void RegionFxBank::syncAndCapture(Session& s) {
    // Remove chains of regions that no longer exist.
    std::vector<int> gone;
    for (auto& [id, e] : map_)
        if (!s.findRegion(id)) gone.push_back(id);
    for (int id : gone) {
        std::unique_ptr<PluginChain> dead;
        {
            const juce::ScopedLock sl(lock_);
            dead = std::move(map_[id].chain);
            map_.erase(id);
        }
        dead.reset();   // editors closed, plugins released, outside the lock
    }
    for (auto& r : s.regions) {
        auto it = map_.find(r.id);
        if (it == map_.end()) {
            if (r.fxState.isEmpty()) continue;
            // A region that arrived with effects (split, paste): build its chain.
            auto* c = chainFor(r.id, true, r.name);
            juce::MemoryBlock mb;
            mb.fromBase64Encoding(r.fxState);
            juce::StringArray problems;
            c->setState(mb, problems);
            map_[r.id].loaded = r.fxState;
            continue;
        }
        // Capture live plugin parameters into the session.
        auto st = it->second.chain->size() ? encode(it->second.chain->getState()) : juce::String();
        r.fxState = st;
        it->second.loaded = st;
    }
}

void RegionFxBank::loadFrom(const Session& s, juce::StringArray& problems) {
    std::vector<int> gone;
    for (auto& [id, e] : map_) {
        auto* r = s.findRegion(id);
        if (!r || r->fxState.isEmpty()) gone.push_back(id);
    }
    for (int id : gone) {
        std::unique_ptr<PluginChain> dead;
        {
            const juce::ScopedLock sl(lock_);
            dead = std::move(map_[id].chain);
            map_.erase(id);
        }
    }
    for (auto& r : s.regions) {
        if (r.fxState.isEmpty()) continue;
        auto it = map_.find(r.id);
        if (it != map_.end() && it->second.loaded == r.fxState) continue;
        auto* c = chainFor(r.id, true, r.name);
        juce::MemoryBlock mb;
        mb.fromBase64Encoding(r.fxState);
        juce::StringArray p;
        c->setState(mb, p);
        for (auto& x : p) problems.add(r.name + ": " + x);
        map_[r.id].loaded = r.fxState;
    }
}

void RegionFxBank::clear() {
    std::map<int, Entry> old;
    {
        const juce::ScopedLock sl(lock_);
        old.swap(map_);
    }
}

void RegionFxBank::closeAllEditors() {
    for (auto& [id, e] : map_) e.chain->closeAllEditors();
}

void RegionFxBank::prepare(double rate, int block, bool dbl) {
    const juce::ScopedLock sl(lock_);
    if (std::abs(rate - rate_) < 0.5 && block == block_ && dbl == double_) return;
    rate_ = rate;
    block_ = block;
    double_ = dbl;
    for (auto& [id, e] : map_) e.chain->prepare(rate, block, dbl);
}

int RegionFxBank::latency(int id) {
    if (bypassAll_) return 0;
    const juce::ScopedLock sl(lock_);
    auto it = map_.find(id);
    return it == map_.end() ? 0 : it->second.chain->latencySamples();
}

void RegionFxBank::reset(int id) {
    const juce::ScopedLock sl(lock_);
    auto it = map_.find(id);
    if (it != map_.end()) it->second.chain->reset();
}

void RegionFxBank::process(int id, juce::AudioBuffer<double>& b, int n) {
    if (bypassAll_) return;
    const juce::ScopedLock sl(lock_);
    auto it = map_.find(id);
    if (it == map_.end() || it->second.chain->size() == 0) return;
    it->second.chain->process(b, 0, n);
}

}  // namespace lathe
