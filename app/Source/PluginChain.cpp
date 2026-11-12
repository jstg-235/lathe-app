#include "PluginChain.h"

#include "PluginScan.h"

namespace lathe {

namespace {
class EditorWindow : public juce::DocumentWindow {
public:
    EditorWindow(juce::AudioProcessorEditor* ed, const juce::String& name)
        : DocumentWindow(name, juce::Colours::black, DocumentWindow::closeButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(ed, true);
        setResizable(ed->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { setVisible(false); }
};
}  // namespace

PluginHost::PluginHost(bool scanOutOfProcess) {
    formats_.addDefaultFormats();
    if (auto xml = juce::XmlDocument::parse(appData().getChildFile("plugins.xml")))
        known_.recreateFromXml(*xml);
    deactivated_.addLines(deactivatedFile().loadFileAsString());
    deactivated_.removeEmptyStrings();
    if (scanOutOfProcess) known_.setCustomScanner(scan::makeOutOfProcessScanner());
    // Save as the list changes (each plugin found, each one blacklisted), not only at quit.
    known_.addChangeListener(this);
}

PluginHost::~PluginHost() {
    known_.removeChangeListener(this);
    saveKnownList();
}

bool PluginHost::isDeactivated(const juce::PluginDescription& d) const { return deactivated_.contains(d.createIdentifierString()); }

void PluginHost::setDeactivated(const juce::PluginDescription& d, bool off) {
    const auto id = d.createIdentifierString();
    if (off) deactivated_.addIfNotAlreadyThere(id);
    else deactivated_.removeString(id);
    deactivatedFile().replaceWithText(deactivated_.joinIntoString("\n"));
}

juce::Array<juce::PluginDescription> PluginHost::activeTypes() const {
    juce::Array<juce::PluginDescription> out;
    for (auto& d : known_.getTypes())
        if (!isDeactivated(d)) out.add(d);
    return out;
}

juce::File PluginHost::appData() const {
    auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Lathe");
    d.createDirectory();
    return d;
}

juce::File PluginHost::deadMansPedal() const { return appData().getChildFile("plugin-scan-in-progress.txt"); }

void PluginHost::saveKnownList() {
    if (auto xml = known_.createXml()) xml->writeTo(appData().getChildFile("plugins.xml"));
}

PluginChain::PluginChain(PluginHost& host, juce::String label) : host_(host), label_(std::move(label)) {}

PluginChain::~PluginChain() { closeAllEditors(); }

void PluginChain::reset() {
    const juce::ScopedLock sl(lock_);
    for (auto* s : slots_)
        if (s->plugin) s->plugin->reset();
}

juce::String PluginChain::nameAt(int i) const {
    auto* s = slots_[i];
    if (s && s->plugin) return s->plugin->getName();
    if (s && s->dormant)
        if (auto* d = s->dormant->getChildByName("PLUGIN")) return d->getStringAttribute("name") + " (deactivated)";
    return {};
}

bool PluginChain::isDormant(int i) const { return slots_[i] && !slots_[i]->plugin; }

bool PluginChain::isBypassed(int i) const { return slots_[i] && slots_[i]->bypassed; }

void PluginChain::setBypassed(int i, bool b) {
    const juce::ScopedLock sl(lock_);
    if (auto* s = slots_[i]) s->bypassed = b;
}

void PluginChain::countBuiltins() {
    int n = 0;
    for (auto* s : slots_) n += s->builtin ? 1 : 0;
    builtins_ = n;
}

int PluginChain::slotOfAnchor(int anchor) const {
    if (anchor != 0)
        for (int i = 0; i < slots_.size(); ++i)
            if (slots_[i]->builtin && slots_[i]->builtin->anchorId() == anchor) return i;
    return -1;
}

int PluginChain::lastLiveHardwareSlot() const {
    for (int i = slots_.size(); --i >= 0;)
        if (slots_[i]->builtin && !slots_[i]->bypassed && slots_[i]->builtin->needsHardware()) return i;
    return -1;
}

int PluginChain::latencyThrough(int anchor) const {
    const int at = slotOfAnchor(anchor);
    int l = 0;
    for (int i = 0; i <= at; ++i)
        if (slots_[i]->plugin && (!slots_[i]->bypassed || slots_[i]->builtin)) l += slots_[i]->plugin->getLatencySamples();
    return l;
}

bool PluginChain::add(const juce::PluginDescription& d, juce::String& error) {
    std::unique_ptr<juce::AudioPluginInstance> inst;
    if (d.pluginFormatName == kBuiltinFormat) {
        if (builtinFactory) inst = builtinFactory(d);
        if (!inst) { error = d.name + " can only go in the master chain."; return false; }
    } else {
        inst = host_.formats().createPluginInstance(d, rate_, block_, error);
    }
    if (!inst) return false;
    // Mastering is stereo in, stereo out.
    auto layout = juce::AudioProcessor::BusesLayout();
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    if (!inst->setBusesLayout(layout)) {
        error = d.name + " doesn't support stereo in/out.";
        return false;
    }
    inst->setNonRealtime(false);
    if (preparedDouble_ && inst->supportsDoublePrecisionProcessing())
        inst->setProcessingPrecision(juce::AudioProcessor::doublePrecision);
    inst->prepareToPlay(rate_, block_);
    auto slot = std::make_unique<Slot>();
    slot->builtin = dynamic_cast<BuiltinInsert*>(inst.get());
    slot->plugin = std::move(inst);
    const juce::ScopedLock sl(lock_);
    slots_.add(slot.release());
    countBuiltins();
    return true;
}

void PluginChain::addInstance(std::unique_ptr<juce::AudioPluginInstance> inst) {
    if (preparedDouble_ && inst->supportsDoublePrecisionProcessing())
        inst->setProcessingPrecision(juce::AudioProcessor::doublePrecision);
    inst->prepareToPlay(rate_, block_);
    auto slot = std::make_unique<Slot>();
    slot->builtin = dynamic_cast<BuiltinInsert*>(inst.get());
    slot->plugin = std::move(inst);
    const juce::ScopedLock sl(lock_);
    slots_.add(slot.release());
    countBuiltins();
}

void PluginChain::remove(int i) {
    std::unique_ptr<Slot> gone;
    {
        const juce::ScopedLock sl(lock_);
        if (!juce::isPositiveAndBelow(i, slots_.size())) return;
        gone.reset(slots_.removeAndReturn(i));
        countBuiltins();
    }
    gone->window.reset();   // editor must die before its plugin
    if (gone->plugin) gone->plugin->releaseResources();
}

void PluginChain::move(int from, int to) {
    const juce::ScopedLock sl(lock_);
    slots_.move(from, to);
}

void PluginChain::showEditor(int i) {
    auto* s = slots_[i];
    if (!s || !s->plugin) return;
    if (!s->window) {
        auto* ed = s->plugin->createEditorIfNeeded();
        if (!ed) ed = new juce::GenericAudioProcessorEditor(*s->plugin);
        s->window = std::make_unique<EditorWindow>(ed, label_.isEmpty() ? s->plugin->getName() : s->plugin->getName() + "  -  " + label_);
        if (windowKeys && s->builtin) s->window->addKeyListener(windowKeys);   // (built-in inserts only: a plugin's own keys stay its own)
    }
    s->window->setVisible(true);
    s->window->toFront(true);
}

void PluginChain::closeAllEditors() {
    for (auto* s : slots_) {
        if (s->window && windowKeys) s->window->removeKeyListener(windowKeys);
        s->window.reset();
    }
}

void PluginChain::prepare(double rate, int block, bool doublePrecision) {
    const juce::ScopedLock sl(lock_);
    rate_ = rate;
    block_ = block;
    preparedDouble_ = doublePrecision;
    scratch_.setSize(2, juce::jmax(block, 8192));
    dscratch_.setSize(2, juce::jmax(block, 8192));
    for (int bus = 0; bus < kMaxBuses; ++bus)
        for (int ch = 0; ch < 2; ++ch) {
            ringF_[size_t(bus)][size_t(ch)].assign(size_t(kRing), 0.0f);
            if (doublePrecision) ringD_[size_t(bus)][size_t(ch)].assign(size_t(kRing), 0.0);
        }
    ringPos_.fill(0);
    for (int ch = 0; ch < 2; ++ch) {
        busScratchF_[ch].assign(size_t(scratch_.getNumSamples()), 0.0f);
        busScratchD_[ch].assign(size_t(dscratch_.getNumSamples()), 0.0);
    }
    for (auto* s : slots_) {
        if (!s->plugin) continue;
        s->plugin->releaseResources();
        s->plugin->setRateAndBufferSizeDetails(rate, block);
        // 64-bit processing for plugins that offer it (offline renders); 32-bit otherwise.
        bool dbl = doublePrecision && s->plugin->supportsDoublePrecisionProcessing();
        s->plugin->setProcessingPrecision(dbl ? juce::AudioProcessor::doublePrecision : juce::AudioProcessor::singlePrecision);
        s->plugin->prepareToPlay(rate, block);
    }
    prepared_ = true;
}

int PluginChain::countDoublePrecision() const {
    int n = 0;
    for (auto* s : slots_)
        if (s->plugin && !s->bypassed && s->plugin->getProcessingPrecision() == juce::AudioProcessor::doublePrecision) ++n;
    return n;
}

template <> std::array<std::array<std::vector<float>, 2>, PluginChain::kMaxBuses>& PluginChain::rings<float>() { return ringF_; }
template <> std::array<std::array<std::vector<double>, 2>, PluginChain::kMaxBuses>& PluginChain::rings<double>() { return ringD_; }

template <typename T>
void PluginChain::delayBus(int bus, const T* l, const T* r, int n, int delay, T* outL, T* outR) {
    auto& ring = rings<T>()[size_t(bus)];
    if (int(ring[0].size()) != kRing) {   // not prepared for this precision: no delay
        std::copy(l, l + n, outL);
        std::copy(r, r + n, outR);
        return;
    }
    delay = juce::jlimit(0, kRing - 1, delay);
    int w = ringPos_[size_t(bus)];
    for (int i = 0; i < n; ++i) {
        ring[0][size_t(w)] = l[i];
        ring[1][size_t(w)] = r[i];
        const int rd = (w - delay + kRing) & (kRing - 1);
        outL[i] = ring[0][size_t(rd)];
        outR[i] = ring[1][size_t(rd)];
        w = (w + 1) & (kRing - 1);
    }
    ringPos_[size_t(bus)] = w;
}

// Buses whose insert isn't in this chain (it was removed) join at the input, like other material.
template <typename T>
void PluginChain::joinAtInput(juce::AudioBuffer<T>& view, const juce::AudioBuffer<T>& b, int at, int k, const int* anchors, int numBuses) {
    for (int bus = 0; bus < numBuses; ++bus) {
        if (b.getNumChannels() < 4 + 2 * bus || slotOfAnchor(anchors[bus]) >= 0) continue;
        for (int ch = 0; ch < 2; ++ch) view.addFrom(ch, 0, b, 2 + 2 * bus + ch, at, k);
    }
}

void PluginChain::process(juce::AudioBuffer<double>& b, int start, int n, const int* anchors, int numBuses) {
    numBuses = anchors ? juce::jmin(numBuses, kMaxBuses) : 0;
    const juce::ScopedTryLock sl(lock_);
    if (!sl.isLocked() || !prepared_) return;
    if (slots_.isEmpty() && numBuses == 0) return;
    for (int done = 0; done < n;) {
        int k = juce::jmin(n - done, block_, dscratch_.getNumSamples());
        juce::AudioBuffer<double> dview(dscratch_.getArrayOfWritePointers(), 2, k);
        juce::AudioBuffer<float> fview(scratch_.getArrayOfWritePointers(), 2, k);
        for (int ch = 0; ch < 2; ++ch) dview.copyFrom(ch, 0, b, juce::jmin(ch, b.getNumChannels() - 1), start + done, k);
        joinAtInput(dview, b, start + done, k, anchors, numBuses);
        int cum = 0;
        for (auto* s : slots_) {
            if (!s->plugin) continue;
            if (s->bypassed && !s->builtin) continue;
            midi_.clear();
            if (s->bypassed || s->plugin->getProcessingPrecision() != juce::AudioProcessor::doublePrecision) {
                // 32-bit plugin inside the 64-bit chain: convert just around it.
                for (int ch = 0; ch < 2; ++ch) {
                    auto* d = dview.getReadPointer(ch);
                    auto* f = fview.getWritePointer(ch);
                    for (int i = 0; i < k; ++i) f[i] = float(d[i]);
                }
                if (s->bypassed) s->builtin->processBypassed(fview);
                else s->plugin->processBlock(fview, midi_);
                for (int ch = 0; ch < 2; ++ch) {
                    auto* f = fview.getReadPointer(ch);
                    auto* d = dview.getWritePointer(ch);
                    for (int i = 0; i < k; ++i) d[i] = double(f[i]);
                }
            } else {
                s->plugin->processBlock(dview, midi_);
            }
            cum += s->plugin->getLatencySamples();
            if (s->builtin)
                for (int bus = 0; bus < numBuses; ++bus) {
                    if (anchors[bus] != s->builtin->anchorId() || b.getNumChannels() < 4 + 2 * bus) continue;
                    delayBus<double>(bus, b.getReadPointer(2 + 2 * bus, start + done), b.getReadPointer(3 + 2 * bus, start + done), k, cum,
                                     busScratchD_[0].data(), busScratchD_[1].data());
                    for (int ch = 0; ch < 2; ++ch) dview.addFrom(ch, 0, busScratchD_[ch].data(), k);
                }
        }
        for (int ch = 0; ch < juce::jmin(2, b.getNumChannels()); ++ch) {
            auto* p = dview.getWritePointer(ch);
            for (int i = 0; i < k; ++i)
                if (!std::isfinite(p[i])) p[i] = 0;
            b.copyFrom(ch, start + done, dview, ch, 0, k);
        }
        done += k;
    }
}

void PluginChain::release() {
    const juce::ScopedLock sl(lock_);
    for (auto* s : slots_)
        if (s->plugin) s->plugin->releaseResources();
    prepared_ = false;
}

void PluginChain::process(juce::AudioBuffer<float>& b, int start, int n, const int* anchors, int numBuses) {
    numBuses = anchors ? juce::jmin(numBuses, kMaxBuses) : 0;
    const juce::ScopedTryLock sl(lock_);
    if (!sl.isLocked() || !prepared_) {
        // Being edited (or not ready): the signal passes, prints join as they are, latency reading kept.
        for (int bus = 0; bus < numBuses; ++bus)
                for (int ch = 0; ch < 2 && b.getNumChannels() >= 4 + 2 * bus; ++ch) b.addFrom(ch, start, b, 2 + 2 * bus + ch, start, n);
        if (sl.isLocked()) lastLatency_ = 0;
        return;
    }
    if (slots_.isEmpty() && numBuses == 0) { lastLatency_ = 0; return; }
    lastLatency_ = latencySamples();
    const int tapAnchor = tapAnchor_, abAnchor = abAnchor_;
    if (abReset_.exchange(false)) {
        abMeter_.prepare(rate_, 2);
        abReady_ = false;
        abDiff_ = 0.0;
        abMix_ = abHearPrint_ ? 1.0f : 0.0f;
        abGainLive_ = abGainPrint_ = 1.0f;
    }
    // Plugins are given at most block_ samples at a time, in a buffer they own.
    for (int done = 0; done < n;) {
        int k = juce::jmin(n - done, block_, scratch_.getNumSamples());
        juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), 2, k);
        for (int ch = 0; ch < 2; ++ch) view.copyFrom(ch, 0, b, juce::jmin(ch, b.getNumChannels() - 1), start + done, k);
        joinAtInput(view, b, start + done, k, anchors, numBuses);
        int cum = 0;
        for (auto* s : slots_) {
            if (!s->plugin) continue;
            if (s->bypassed && !s->builtin) continue;
            midi_.clear();
            if (s->bypassed) s->builtin->processBypassed(view);
            else s->plugin->processBlock(view, midi_);
            cum += s->plugin->getLatencySamples();
            if (!s->builtin) continue;
            const int id = s->builtin->anchorId();
            if (id == tapAnchor && tapDest_ && tapDest_->getNumSamples() >= done + k)
                for (int ch = 0; ch < 2; ++ch) tapDest_->copyFrom(ch, done, view, ch, 0, k);
            for (int bus = 0; bus < numBuses; ++bus) {
                if (anchors[bus] != id || b.getNumChannels() < 4 + 2 * bus) continue;
                float* pl = busScratchF_[0].data();
                float* pr = busScratchF_[1].data();
                delayBus<float>(bus, b.getReadPointer(2 + 2 * bus, start + done), b.getReadPointer(3 + 2 * bus, start + done), k, cum, pl, pr);
                if (id != abAnchor) {
                    view.addFrom(0, 0, pl, k);
                    view.addFrom(1, 0, pr, k);
                    continue;
                }
                // Live against print: measure both, hear one.
                const float* live[2] = {view.getReadPointer(0), view.getReadPointer(1)};
                const float* print[2] = {pl, pr};
                abMeter_.process(live, print, k);
                if (abMeter_.ready()) { abDiff_ = abMeter_.differenceDb(); abReady_ = true; }
                const double diff = abDiff_.load();   // live minus print
                const bool match = abMatch_ && abReady_;
                const float tLive = match ? float(juce::Decibels::decibelsToGain(-juce::jmax(0.0, diff))) : 1.0f;
                const float tPrint = match ? float(juce::Decibels::decibelsToGain(juce::jmin(0.0, diff))) : 1.0f;
                const float tMix = abHearPrint_ ? 1.0f : 0.0f;
                const float step = 1.0f / float(juce::jmax(1.0, rate_ * 0.010));
                const float glide = float(1.0 - std::exp(-1.0 / juce::jmax(1.0, rate_ * 0.050)));
                float* vl = view.getWritePointer(0);
                float* vr = view.getWritePointer(1);
                for (int i = 0; i < k; ++i) {
                    abMix_ = tMix > abMix_ ? juce::jmin(tMix, abMix_ + step) : juce::jmax(tMix, abMix_ - step);
                    abGainLive_ += (tLive - abGainLive_) * glide;
                    abGainPrint_ += (tPrint - abGainPrint_) * glide;
                    vl[i] = vl[i] * abGainLive_ * (1.0f - abMix_) + pl[i] * abGainPrint_ * abMix_;
                    vr[i] = vr[i] * abGainLive_ * (1.0f - abMix_) + pr[i] * abGainPrint_ * abMix_;
                }
            }
        }
        for (int ch = 0; ch < juce::jmin(2, b.getNumChannels()); ++ch) {
            // Never let a misbehaving plugin send NaN/inf to the outputs or the master.
            auto* p = view.getWritePointer(ch);
            for (int i = 0; i < k; ++i)
                if (!std::isfinite(p[i])) p[i] = 0;
            b.copyFrom(ch, start + done, view, ch, 0, k);
        }
        done += k;
    }
}

int PluginChain::latencySamples() const {
    int l = 0;
    for (auto* s : slots_)
        if (s->plugin && (!s->bypassed || s->builtin)) l += s->plugin->getLatencySamples();   // a bypassed built-in insert keeps its delay
    return l;
}

void PluginChain::setNonRealtime(bool offline) {
    const juce::ScopedLock sl(lock_);
    for (auto* s : slots_)
        if (s->plugin) s->plugin->setNonRealtime(offline);
}

juce::MemoryBlock PluginChain::getState() const {
    juce::XmlElement root("LATHE_CHAIN");
    for (auto* s : slots_) {
        if (!s->plugin) {   // deactivated: write back exactly what was loaded
            if (s->dormant) root.addChildElement(new juce::XmlElement(*s->dormant));
            continue;
        }
        auto* e = root.createNewChildElement("SLOT");
        e->setAttribute("bypassed", s->bypassed);
        e->addChildElement(s->plugin->getPluginDescription().createXml().release());
        juce::MemoryBlock st;
        s->plugin->getStateInformation(st);
        e->setAttribute("state", st.toBase64Encoding());
    }
    auto text = root.toString();
    return juce::MemoryBlock(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void PluginChain::setState(const juce::MemoryBlock& state, juce::StringArray& problems) {
    closeAllEditors();
    {
        const juce::ScopedLock sl(lock_);
        slots_.clear();
        countBuiltins();
    }
    if (state.isEmpty()) return;
    auto xml = juce::XmlDocument::parse(state.toString());
    if (!xml) { problems.add("The saved plugin chain couldn't be read."); return; }
    for (auto* e : xml->getChildWithTagNameIterator("SLOT")) {
        juce::PluginDescription d;
        auto* de = e->getChildByName("PLUGIN");
        if (!de || !d.loadFromXml(*de)) continue;
        if (host_.isDeactivated(d)) {
            auto slot = std::make_unique<Slot>();
            slot->dormant = std::make_unique<juce::XmlElement>(*e);
            slot->bypassed = e->getBoolAttribute("bypassed");
            const juce::ScopedLock sl(lock_);
            slots_.add(slot.release());
            problems.add(d.name + " is deactivated (Plugin Manager), so it wasn't loaded. Its settings are kept in the session.");
            continue;
        }
        juce::String err;
        if (!add(d, err)) { problems.add(d.pluginFormatName == kBuiltinFormat ? err : d.name + ": " + err); continue; }
        juce::MemoryBlock st;
        st.fromBase64Encoding(e->getStringAttribute("state"));
        auto* slot = slots_.getLast();
        slot->plugin->setStateInformation(st.getData(), int(st.getSize()));
        slot->bypassed = e->getBoolAttribute("bypassed");
    }
}

}  // namespace lathe
