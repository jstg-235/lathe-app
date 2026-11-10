#include "ExternalGear.h"

#include "Theme.h"

namespace lathe {

namespace {
constexpr int kDryRing = 1 << 17;   // the longest round trip the dry path can follow (about 3 s at 44.1 kHz)
const juce::Colour kWarn(0xfff0c43a), kBad(0xffe0605a), kGood(0xff5fd17a), kSend(0xfff0c43a), kReturn(0xff39c2c9);
}  // namespace

ExternalGear::ExternalGear(GearIo* io, std::shared_ptr<GearActions> actions)
    : AudioPluginInstance(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())),
      io_(io), actions_(std::move(actions)) {
    id_ = 1 + juce::Random::getSystemRandom().nextInt(0x7ffffff0);
    for (auto& p : sendPeak_) p = 0.0f;
    for (auto& p : returnPeak_) p = 0.0f;
    for (auto& d : dry_) d.assign(size_t(kDryRing), 0.0f);
}

juce::PluginDescription ExternalGear::description() {
    juce::PluginDescription d;
    d.name = "External Gear";
    d.descriptiveName = "Hardware insert: out to your gear and back";
    d.pluginFormatName = kBuiltinFormat;
    d.fileOrIdentifier = "external-gear";
    d.manufacturerName = "Lathe";
    d.category = "Insert";
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
    d.uniqueId = 0x4c474541;
    return d;
}

void ExternalGear::prepareToPlay(double rate, int) {
    rate_ = rate;
    setLatencySamples(juce::jlimit(0, kDryRing - 1, ping_.load()));
    reset();
}

void ExternalGear::reset() {
    for (auto& d : dry_) std::fill(d.begin(), d.end(), 0.0f);
    dryPos_ = 0;
    silentFor_ = 0;
}

void ExternalGear::setPing(int samples) {
    ping_ = juce::jlimit(0, kDryRing - 1, samples);
    setLatencySamples(ping_);
}

juce::String ExternalGear::setRoute(int firstOutput, int firstInput) {
    if (io_) {
        if (io_->gearSendIsMonitor(firstOutput))
            return "That output pair is the one you monitor on. Sending to it would feed the return straight back into the gear. Choose another pair.";
        auto err = io_->gearUse(firstOutput, firstInput);
        if (err.isNotEmpty()) return err;
    }
    out_ = firstOutput;
    in_ = firstInput;
    return {};
}

void ExternalGear::run(juce::AudioBuffer<float>& b, bool bypassed) {
    const int n = b.getNumSamples();
    bool live = io_ != nullptr && !isNonRealtime();
    if (live) {
        const int blk = io_->gearBlock();
        if (blk != block_) { block_ = blk; pos_ = 0; }
        if (pos_ + n > io_->gearBlockSize()) live = false;   // (not the device's block: nothing to send into)
    }
    const int ping = juce::jlimit(0, kDryRing - 1, ping_.load());
    const float sg = sendGain_, rg = returnGain_ * (invert_ ? -1.0f : 1.0f);
    // Offline there is no hardware: the signal passes, as late as the gear would make it.
    const float wet = bypassed || !live ? 0.0f : wet_.load();
    const int out = out_, in = in_;
    float sendMax = 0.0f, returnMax = 0.0f;
    bool blocked = false;
    for (int ch = 0; ch < juce::jmin(2, b.getNumChannels()); ++ch) {
        float* x = b.getWritePointer(ch);
        float* send = live ? io_->gearOutput(out + ch) : nullptr;
        const float* ret = live ? io_->gearInput(in + ch) : nullptr;
        if (live && !send) blocked = true;
        auto& ring = dry_[ch];
        float sp = 0.0f, rp = 0.0f;
        int w = dryPos_;
        for (int i = 0; i < n; ++i) {
            const float d = x[i];
            const float s = d * sg;
            if (send) send[pos_ + i] += s;
            sp = juce::jmax(sp, std::abs(s));
            ring[size_t(w)] = d;
            const float delayed = ring[size_t((w - ping + kDryRing) & (kDryRing - 1))];
            w = (w + 1) & (kDryRing - 1);
            const float r = ret ? ret[pos_ + i] * rg : 0.0f;
            rp = juce::jmax(rp, std::abs(r));
            x[i] = delayed * (1.0f - wet) + r * wet;
        }
        if (sp > sendPeak_[ch].load()) sendPeak_[ch] = sp;
        if (rp > returnPeak_[ch].load()) returnPeak_[ch] = rp;
        if (sp >= 1.0f) sendClip_ = true;
        if (rp >= 1.0f) returnClip_ = true;
        sendMax = juce::jmax(sendMax, sp);
        returnMax = juce::jmax(returnMax, rp);
    }
    dryPos_ = (dryPos_ + n) & (kDryRing - 1);
    pos_ += n;
    if (live) {
        blocked_ = blocked;
        // Signal going out (above -50 dB) with nothing coming back (below -70 dB) for a second.
        if (returnMax > 0.0003f) silentFor_ = 0;
        else if (sendMax > 0.003f) silentFor_ += n;
        silent_ = silentFor_ > int(rate_);
    }
}

void ExternalGear::getStateInformation(juce::MemoryBlock& dest) {
    juce::XmlElement e("GEAR");
    e.setAttribute("id", id_);
    e.setAttribute("out", out_.load());
    e.setAttribute("in", in_.load());
    e.setAttribute("send", sendDb_.load());
    e.setAttribute("return", returnDb_.load());
    e.setAttribute("mix", mix_.load());
    e.setAttribute("invert", invert_.load());
    e.setAttribute("ping", ping_.load());
    copyXmlToBinary(e, dest);
}

void ExternalGear::setStateInformation(const void* data, int size) {
    auto e = getXmlFromBinary(data, size);
    if (!e || !e->hasTagName("GEAR")) return;
    id_ = juce::jmax(1, e->getIntAttribute("id", id_));
    out_ = juce::jmax(0, e->getIntAttribute("out", 2));
    in_ = juce::jmax(0, e->getIntAttribute("in", 0));
    setSendDb(e->getDoubleAttribute("send", 0.0));
    setReturnDb(e->getDoubleAttribute("return", 0.0));
    setMix(e->getDoubleAttribute("mix", 1.0));
    invert_ = e->getBoolAttribute("invert", false);
    setPing(e->getIntAttribute("ping", 0));
    if (io_ && !io_->gearSendIsMonitor(out_)) io_->gearUse(out_, in_);   // its sockets on again (when the interface has them)
}

// ================================================================= the insert's window

namespace {
class GearEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit GearEditor(ExternalGear& g) : AudioProcessorEditor(g), g_(g) {
        auto label = [this](juce::Label& l, const juce::String& t) {
            l.setText(t, juce::dontSendNotification);
            l.setFont(juce::FontOptions(13.0f));
            l.setColour(juce::Label::textColourId, colours::dim);
            l.setMinimumHorizontalScale(1.0f);
            addAndMakeVisible(l);
        };
        label(sendL_, "Send (to the gear)");
        label(returnL_, "Return (from the gear)");
        label(sendLevelL_, "Send level");
        label(returnLevelL_, "Return level");
        label(mixL_, "Dry / wet");
        label(pingInfo_, "");
        pingInfo_.setJustificationType(juce::Justification::topLeft);
        for (auto* c : {&send_, &return_}) {
            c->setColour(juce::ComboBox::backgroundColourId, colours::panel);
            c->setColour(juce::ComboBox::textColourId, colours::text);
            c->setColour(juce::ComboBox::outlineColourId, colours::line);
            addAndMakeVisible(*c);
        }
        auto slider = [this](juce::Slider& s, double lo, double hi, double step, const juce::String& suffix) {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 22);
            s.setRange(lo, hi, step);
            s.setTextValueSuffix(suffix);
            s.setColour(juce::Slider::textBoxTextColourId, colours::text);
            s.setColour(juce::Slider::textBoxOutlineColourId, colours::line);
            s.setColour(juce::Slider::trackColourId, colours::accent.darker(0.3f));
            s.setColour(juce::Slider::thumbColourId, colours::accent);
            addAndMakeVisible(s);
        };
        slider(sendLevel_, -24.0, 12.0, 0.1, " dB");
        slider(returnLevel_, -24.0, 12.0, 0.1, " dB");
        slider(mix_, 0.0, 100.0, 1.0, " % wet");
        sendLevel_.setDoubleClickReturnValue(true, 0.0);
        returnLevel_.setDoubleClickReturnValue(true, 0.0);
        mix_.setDoubleClickReturnValue(true, 100.0);
        sendLevel_.setValue(g_.sendDb(), juce::dontSendNotification);
        returnLevel_.setValue(g_.returnDb(), juce::dontSendNotification);
        mix_.setValue(g_.mix() * 100.0, juce::dontSendNotification);
        sendLevel_.onValueChange = [this] { g_.setSendDb(sendLevel_.getValue()); };
        returnLevel_.onValueChange = [this] { g_.setReturnDb(returnLevel_.getValue()); };
        mix_.onValueChange = [this] { g_.setMix(mix_.getValue() / 100.0); };
        for (auto* s : {&sendLevel_, &returnLevel_, &mix_}) s->onDragEnd = [this] { changed(); };
        invert_.setButtonText("Invert polarity");
        invert_.setColour(juce::ToggleButton::textColourId, colours::text);
        invert_.setToggleState(g_.inverted(), juce::dontSendNotification);
        invert_.onClick = [this] { g_.setInverted(invert_.getToggleState()); changed(); };
        addAndMakeVisible(invert_);
        for (auto* b : {&ping_, &print_, &useLive_, &usePrint_, &hearLive_, &hearPrint_, &endAb_}) {
            styleButton(*b, false);
            addAndMakeVisible(*b);
        }
        print_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff9a2a24));
        ping_.setTooltip("Plays one click out of the send and times it on the return. The round trip becomes this insert's delay, so everything stays lined up.");
        print_.setTooltip("Plays the In-Out (or the whole programme) through the gear in real time and records what comes out of this insert onto a new lane. "
                          "The print replaces the live path there, so exports no longer need the hardware.");
        useLive_.setTooltip("Play the original material through the gear again; the prints are muted, not deleted.");
        usePrint_.setTooltip("Play the prints; the original material under them is muted.");
        hearLive_.setTooltip("Both run in step: hear the live gear. Level-matched (the louder side is turned down).");
        hearPrint_.setTooltip("Both run in step: hear the print. Level-matched (the louder side is turned down).");
        send_.onChange = [this] { routeChanged(); };
        return_.onChange = [this] { routeChanged(); };
        ping_.onClick = [this] {
            if (!g_.io()) return;
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon, "Ping the gear",
                "One click is played out of the send and timed on the return. Leave the gear patched in and set as you will use it. Turn your monitors "
                "down, then press OK.",
                "OK", this, juce::ModalCallbackFunction::create([this](int) {
                    if (auto err = g_.io()->gearUse(g_.sendOutput(), g_.returnInput()); err.isNotEmpty()) { note_ = err; return; }
                    g_.io()->gearPing(g_.returnInput(), g_.sendOutput());
                    pinging_ = true;
                }));
        };
        auto* a = g_.actions();
        print_.onClick = [this, a] { if (a && a->print) a->print(g_.anchorId()); };
        useLive_.onClick = [this, a] { if (a && a->use) a->use(g_.anchorId(), false); };
        usePrint_.onClick = [this, a] { if (a && a->use) a->use(g_.anchorId(), true); };
        hearLive_.onClick = [this, a] { if (a && a->hear) a->hear(g_.anchorId(), 1); };
        hearPrint_.onClick = [this, a] { if (a && a->hear) a->hear(g_.anchorId(), 2); };
        endAb_.onClick = [this, a] { if (a && a->hear) a->hear(g_.anchorId(), 0); };
        refreshRoute();
        timerCallback();
        startTimerHz(20);
        setSize(540, 560);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(colours::bg);
        static const char* heads[3] = {"ROUTE", "LEVELS", "PRINT"};
        for (int i = 0; i < 3; ++i) {
            g.setColour(colours::accent);
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText(heads[i], head_[i], juce::Justification::centredLeft);
            if (i > 0) {
                g.setColour(colours::line);
                g.drawHorizontalLine(head_[i].getY() - 5, 12.0f, float(getWidth() - 12));
            }
        }
        // Send and return meters with clip lights (click the meters to clear them).
        auto r = meters_;
        auto scale = [](float db, float w) { return w * juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f); };
        auto strip = [&](const juce::String& name, juce::Colour c, const float* db, bool clipped) {
            auto row = r.removeFromTop(30);
            r.removeFromTop(4);
            g.setColour(c);
            g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
            g.drawText(name, row.removeFromLeft(64), juce::Justification::centredLeft);
            auto light = row.removeFromRight(52).reduced(4, 5);
            g.setColour(clipped ? kBad : colours::line);
            g.fillRoundedRectangle(light.toFloat(), 3);
            g.setColour(clipped ? juce::Colours::white : colours::dim);
            g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            g.drawText(clipped ? "CLIP" : "ok", light, juce::Justification::centred);
            auto num = row.removeFromRight(70);
            for (int ch = 0; ch < 2; ++ch) {
                auto bar = row.removeFromTop(13).toFloat();
                row.removeFromTop(4);
                g.setColour(colours::line);
                g.fillRect(bar);
                juce::ColourGradient grad(kGood, bar.getX(), 0, kBad, bar.getRight(), 0, false);
                grad.addColour(1.0 - 6.0 / 60.0, kWarn);
                g.setGradientFill(grad);
                g.fillRect(bar.withWidth(scale(db[ch], bar.getWidth())));
            }
            const float pk = juce::jmax(db[0], db[1]);
            g.setFont(juce::FontOptions(12.5f));
            g.setColour(pk > -1.0f ? kBad : colours::text);
            g.drawText(pk > -100.0f ? juce::String(pk, 1) + " dB" : juce::String("--"), num, juce::Justification::centredRight);
        };
        strip("SEND", kSend, sendDb_, g_.sendClipped());
        strip("RETURN", kReturn, returnDb_, g_.returnClipped());
        g.setFont(juce::FontOptions(12.5f));
        g.setColour(warnBad_ ? kBad : kWarn);
        g.drawFittedText(warn_, warnArea_, juce::Justification::topLeft, 3, 1.0f);
        g.setColour(colours::text);
        g.drawFittedText(printStatus_, printArea_, juce::Justification::topLeft, 4, 1.0f);
        g.setColour(colours::dim);
        g.drawFittedText("To bypass: the Bypass button under the plugin list. The delay is kept, so nothing jumps.",
                         invert_.getBounds().withLeft(invert_.getX() + 170), juce::Justification::centredLeft, 2, 1.0f);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (meters_.contains(e.getPosition())) g_.clearLights();
    }

    void resized() override {
        auto r = getLocalBounds().reduced(14, 10);
        head_[0] = r.removeFromTop(20);
        r.removeFromTop(4);
        auto row = [&](juce::Label& l, juce::Component& c, int w = 0) {
            auto x = r.removeFromTop(26);
            r.removeFromTop(6);
            l.setBounds(x.removeFromLeft(150));
            c.setBounds(w > 0 ? x.removeFromLeft(w) : x);
            return x;
        };
        row(sendL_, send_, 240);
        auto rest = row(returnL_, return_, 240);
        rest.removeFromLeft(10);
        ping_.setBounds(rest.removeFromLeft(90));
        pingInfo_.setBounds(r.removeFromTop(36));
        r.removeFromTop(8);
        head_[1] = r.removeFromTop(20);
        r.removeFromTop(4);
        row(sendLevelL_, sendLevel_);
        row(returnLevelL_, returnLevel_);
        row(mixL_, mix_);
        invert_.setBounds(r.removeFromTop(24));
        r.removeFromTop(6);
        meters_ = r.removeFromTop(68);
        warnArea_ = r.removeFromTop(52);
        r.removeFromTop(6);
        head_[2] = r.removeFromTop(20);
        r.removeFromTop(4);
        {
            auto x = r.removeFromTop(30);
            print_.setBounds(x.removeFromLeft(150));
            x.removeFromLeft(14);
            useLive_.setBounds(x.removeFromLeft(110));
            x.removeFromLeft(4);
            usePrint_.setBounds(x.removeFromLeft(110));
        }
        r.removeFromTop(6);
        {
            auto x = r.removeFromTop(28);
            hearLive_.setBounds(x.removeFromLeft(150));
            x.removeFromLeft(4);
            hearPrint_.setBounds(x.removeFromLeft(150));
            x.removeFromLeft(4);
            endAb_.setBounds(x.removeFromLeft(60));
        }
        r.removeFromTop(6);
        printArea_ = r;
    }

private:
    void changed() {
        if (auto* a = g_.actions(); a && a->changed) a->changed();
    }
    void refreshRoute() {
        filling_ = true;
        auto fill = [](juce::ComboBox& c, const juce::StringArray& names, int first) {
            c.clear(juce::dontSendNotification);
            for (int i = 0; i < names.size(); ++i) c.addItem(names[i], i * 2 + 1);
            const int want = (juce::jmax(0, first) / 2) * 2 + 1;
            if (c.indexOfItemId(want) >= 0) c.setSelectedId(want, juce::dontSendNotification);
            else c.setText("Pair " + juce::String(first + 1) + "-" + juce::String(first + 2) + " (not on this interface)", juce::dontSendNotification);
        };
        if (g_.io()) {
            fill(send_, g_.io()->gearOutputPairs(), g_.sendOutput());
            fill(return_, g_.io()->gearInputPairs(), g_.returnInput());
            stamp_ = g_.io()->gearOutputPairs().size() * 1000 + g_.io()->gearInputPairs().size();
        }
        filling_ = false;
    }
    void routeChanged() {
        if (filling_ || send_.getSelectedId() == 0 || return_.getSelectedId() == 0) return;
        auto err = g_.setRoute(send_.getSelectedId() - 1, return_.getSelectedId() - 1);
        note_ = err;
        if (err.isNotEmpty()) refreshRoute();   // refused: back to what it was
        else changed();
    }
    void timerCallback() override {
        auto* io = g_.io();
        if (io && stamp_ != io->gearOutputPairs().size() * 1000 + io->gearInputPairs().size()) refreshRoute();
        if (pinging_ && io && io->gearPingResult() >= 0) {
            pinging_ = false;
            const int v = io->gearPingResult();
            if (v > 0) { g_.setPing(v); note_ = {}; changed(); }
            else note_ = "No click came back on the return. Check the patch and the gear's levels, then ping again.";
        }
        for (int ch = 0; ch < 2; ++ch) {
            const float s = juce::Decibels::gainToDecibels(g_.takeSendPeak(ch), -200.0f), r = juce::Decibels::gainToDecibels(g_.takeReturnPeak(ch), -200.0f);
            sendDb_[ch] = juce::jmax(s, sendDb_[ch] - 1.5f);
            returnDb_[ch] = juce::jmax(r, returnDb_[ch] - 1.5f);
        }
        const double rate = io ? io->gearRate() : 44100.0;
        const int ping = g_.ping();
        pingInfo_.setColour(juce::Label::textColourId, ping > 0 ? kGood : kWarn);
        pingInfo_.setText(ping > 0 ? "Round trip " + juce::String(ping) + " samples (" + juce::String(ping * 1000.0 / rate, 1) +
                                         " ms), measured. The rest of Lathe waits for it, so everything stays lined up."
                                   : "Not pinged yet: press Ping so the gear's delay is known and the dry / wet mix, the compare and the meters line up.",
                          juce::dontSendNotification);
        // Warnings, worst first.
        juce::StringArray w;
        warnBad_ = false;
        if (note_.isNotEmpty()) { w.add(note_); warnBad_ = true; }
        if (io && io->gearSendIsMonitor(g_.sendOutput())) {
            w.add("The send is the pair you monitor on, so nothing is being sent (it would feed back). Choose another output pair.");
            warnBad_ = true;
        } else if (g_.sendBlocked()) {
            w.add("The interface doesn't have that output pair: nothing is being sent.");
            warnBad_ = true;
        }
        if (g_.returnClipped()) { w.add("The return clipped: turn the gear's output (or the send level) down. Click the meters to clear."); warnBad_ = true; }
        else if (g_.sendClipped()) { w.add("The send clipped: turn the send level down. Click the meters to clear."); warnBad_ = true; }
        if (g_.returnSilent()) w.add("Signal is going out and nothing is coming back. Check the patch and that the gear is switched in.");
        if (io && io->gearSplitDevices()) w.add("Inputs and outputs are on different interfaces: without a shared clock the return drifts against the send.");
        warn_ = w.joinIntoString("\n");
        // Print.
        if (auto* a = g_.actions()) {
            const int id = g_.anchorId();
            const bool printing = a->printing && a->printing(id);
            const int prints = a->printCount ? a->printCount(id) : 0;
            const bool usingPrints = a->usingPrints && a->usingPrints(id);
            const int hearing = a->hearing ? a->hearing(id) : 0;
            printStatus_ = a->printStatus ? a->printStatus(id) : juce::String();
            print_.setButtonText(printing ? "Stop (keep it)" : "Print");
            useLive_.setEnabled(prints > 0 && !printing && hearing == 0);
            usePrint_.setEnabled(prints > 0 && !printing && hearing == 0);
            useLive_.setToggleState(prints > 0 && !usingPrints, juce::dontSendNotification);
            usePrint_.setToggleState(prints > 0 && usingPrints, juce::dontSendNotification);
            hearLive_.setEnabled(prints > 0 && !printing);
            hearPrint_.setEnabled(prints > 0 && !printing);
            endAb_.setEnabled(hearing != 0);
            hearLive_.setToggleState(hearing == 1, juce::dontSendNotification);
            hearPrint_.setToggleState(hearing == 2, juce::dontSendNotification);
        } else {
            for (auto* b : {&print_, &useLive_, &usePrint_, &hearLive_, &hearPrint_, &endAb_}) b->setEnabled(false);
        }
        repaint();
    }

    ExternalGear& g_;
    juce::Label sendL_, returnL_, sendLevelL_, returnLevelL_, mixL_, pingInfo_;
    juce::ComboBox send_, return_;
    juce::Slider sendLevel_, returnLevel_, mix_;
    juce::ToggleButton invert_;
    juce::TextButton ping_{"Ping"}, print_{"Print"}, useLive_{"Use live gear"}, usePrint_{"Use the print"}, hearLive_{"A/B: hear live gear"},
        hearPrint_{"A/B: hear the print"}, endAb_{"End"};
    juce::Rectangle<int> head_[3], meters_, warnArea_, printArea_;
    juce::String warn_, note_, printStatus_;
    bool warnBad_ = false, pinging_ = false, filling_ = false;
    int stamp_ = -1;
    float sendDb_[2] = {-200.0f, -200.0f}, returnDb_[2] = {-200.0f, -200.0f};
};
}  // namespace

juce::AudioProcessorEditor* ExternalGear::createEditor() { return new GearEditor(*this); }

}  // namespace lathe
