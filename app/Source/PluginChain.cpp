#include "PluginChain.h"

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

PluginHost::PluginHost() {
    formats_.addDefaultFormats();
    if (auto xml = juce::XmlDocument::parse(appData().getChildFile("plugins.xml")))
        known_.recreateFromXml(*xml);
}

PluginHost::~PluginHost() { saveKnownList(); }

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
    for (auto* s : slots_) s->plugin->reset();
}

juce::String PluginChain::nameAt(int i) const {
    auto* s = slots_[i];
    return s && s->plugin ? s->plugin->getName() : juce::String();
}

bool PluginChain::isBypassed(int i) const { return slots_[i] && slots_[i]->bypassed; }

void PluginChain::setBypassed(int i, bool b) {
    const juce::ScopedLock sl(lock_);
    if (auto* s = slots_[i]) s->bypassed = b;
}

bool PluginChain::add(const juce::PluginDescription& d, juce::String& error) {
    auto inst = host_.formats().createPluginInstance(d, rate_, block_, error);
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
    slot->plugin = std::move(inst);
    const juce::ScopedLock sl(lock_);
    slots_.add(slot.release());
    return true;
}

void PluginChain::addInstance(std::unique_ptr<juce::AudioPluginInstance> inst) {
    if (preparedDouble_ && inst->supportsDoublePrecisionProcessing())
        inst->setProcessingPrecision(juce::AudioProcessor::doublePrecision);
    inst->prepareToPlay(rate_, block_);
    auto slot = std::make_unique<Slot>();
    slot->plugin = std::move(inst);
    const juce::ScopedLock sl(lock_);
    slots_.add(slot.release());
}

void PluginChain::remove(int i) {
    std::unique_ptr<Slot> gone;
    {
        const juce::ScopedLock sl(lock_);
        if (!juce::isPositiveAndBelow(i, slots_.size())) return;
        gone.reset(slots_.removeAndReturn(i));
    }
    gone->window.reset();   // editor must die before its plugin
    gone->plugin->releaseResources();
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
    }
    s->window->setVisible(true);
    s->window->toFront(true);
}

void PluginChain::closeAllEditors() {
    for (auto* s : slots_) s->window.reset();
}

void PluginChain::prepare(double rate, int block, bool doublePrecision) {
    const juce::ScopedLock sl(lock_);
    rate_ = rate;
    block_ = block;
    preparedDouble_ = doublePrecision;
    scratch_.setSize(2, juce::jmax(block, 8192));
    dscratch_.setSize(2, juce::jmax(block, 8192));
    for (auto* s : slots_) {
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
        if (!s->bypassed && s->plugin->getProcessingPrecision() == juce::AudioProcessor::doublePrecision) ++n;
    return n;
}

void PluginChain::process(juce::AudioBuffer<double>& b, int start, int n) {
    const juce::ScopedTryLock sl(lock_);
    if (!sl.isLocked() || !prepared_ || slots_.isEmpty()) return;
    for (int done = 0; done < n;) {
        int k = juce::jmin(n - done, block_, dscratch_.getNumSamples());
        juce::AudioBuffer<double> dview(dscratch_.getArrayOfWritePointers(), 2, k);
        juce::AudioBuffer<float> fview(scratch_.getArrayOfWritePointers(), 2, k);
        for (int ch = 0; ch < 2; ++ch) dview.copyFrom(ch, 0, b, juce::jmin(ch, b.getNumChannels() - 1), start + done, k);
        for (auto* s : slots_) {
            if (s->bypassed) continue;
            midi_.clear();
            if (s->plugin->getProcessingPrecision() == juce::AudioProcessor::doublePrecision) {
                s->plugin->processBlock(dview, midi_);
            } else {
                // 32-bit plugin inside the 64-bit chain: convert just around it.
                for (int ch = 0; ch < 2; ++ch) {
                    auto* d = dview.getReadPointer(ch);
                    auto* f = fview.getWritePointer(ch);
                    for (int i = 0; i < k; ++i) f[i] = float(d[i]);
                }
                s->plugin->processBlock(fview, midi_);
                for (int ch = 0; ch < 2; ++ch) {
                    auto* f = fview.getReadPointer(ch);
                    auto* d = dview.getWritePointer(ch);
                    for (int i = 0; i < k; ++i) d[i] = double(f[i]);
                }
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
    for (auto* s : slots_) s->plugin->releaseResources();
    prepared_ = false;
}

void PluginChain::process(juce::AudioBuffer<float>& b, int start, int n) {
    const juce::ScopedTryLock sl(lock_);
    if (!sl.isLocked() || !prepared_ || slots_.isEmpty()) return;
    // Plugins are given at most block_ samples at a time, in a buffer they own.
    for (int done = 0; done < n;) {
        int k = juce::jmin(n - done, block_, scratch_.getNumSamples());
        juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), 2, k);
        for (int ch = 0; ch < 2; ++ch) view.copyFrom(ch, 0, b, juce::jmin(ch, b.getNumChannels() - 1), start + done, k);
        for (auto* s : slots_) {
            if (s->bypassed) continue;
            midi_.clear();
            s->plugin->processBlock(view, midi_);
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
        if (!s->bypassed) l += s->plugin->getLatencySamples();
    return l;
}

void PluginChain::setNonRealtime(bool offline) {
    const juce::ScopedLock sl(lock_);
    for (auto* s : slots_) s->plugin->setNonRealtime(offline);
}

juce::MemoryBlock PluginChain::getState() const {
    juce::XmlElement root("LATHE_CHAIN");
    for (auto* s : slots_) {
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
    }
    if (state.isEmpty()) return;
    auto xml = juce::XmlDocument::parse(state.toString());
    if (!xml) { problems.add("The saved plugin chain couldn't be read."); return; }
    for (auto* e : xml->getChildWithTagNameIterator("SLOT")) {
        juce::PluginDescription d;
        auto* de = e->getChildByName("PLUGIN");
        if (!de || !d.loadFromXml(*de)) continue;
        juce::String err;
        if (!add(d, err)) { problems.add(d.name + ": " + err); continue; }
        juce::MemoryBlock st;
        st.fromBase64Encoding(e->getStringAttribute("state"));
        auto* slot = slots_.getLast();
        slot->plugin->setStateInformation(st.getData(), int(st.getSize()));
        slot->bypassed = e->getBoolAttribute("bypassed");
    }
}

}  // namespace lathe
