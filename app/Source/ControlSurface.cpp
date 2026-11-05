#include "ControlSurface.h"

namespace lathe {

namespace mcu {
// Mackie Control button notes (channel 1, velocity 127 = press, 0 = release).
enum : int {
    F1 = 0x36, F8 = 0x3D, Save = 0x50, Undo = 0x51, Cancel = 0x52, Enter = 0x53, Marker = 0x54, Nudge = 0x55, Cycle = 0x56,
    Drop = 0x57, Replace = 0x58, Click = 0x59, Solo = 0x5A, Rewind = 0x5B, Forward = 0x5C, Stop = 0x5D, Play = 0x5E,
    Record = 0x5F, Up = 0x60, Down = 0x61, Left = 0x62, Right = 0x63, Zoom = 0x64, Scrub = 0x65
};
constexpr int JogCC = 0x3C;
constexpr int TimecodeCC = 0x40;   // 0x40..0x49, rightmost digit first
}  // namespace mcu

ControlSurface::ControlSurface(juce::ApplicationCommandManager& c, juce::PropertiesFile& p) : cmds_(c), props_(p) {
    load();
    configure(props_.getValue("surfaceIn"), props_.getValue("surfaceOut"), Protocol(props_.getIntValue("surfaceProtocol", 0)));
    startTimerHz(15);
}

ControlSurface::~ControlSurface() {
    stopTimer();
    if (in_) in_->stop();
}

void ControlSurface::configure(const juce::String& inId, const juce::String& outId, Protocol p) {
    if (in_) in_->stop();
    in_.reset();
    out_.reset();
    inId_ = inId;
    outId_ = outId;
    proto_ = p;
    status_ = "No control surface";
    if (inId.isNotEmpty()) {
        in_ = juce::MidiInput::openDevice(inId, this);
        if (in_) { in_->start(); status_ = "Listening on " + in_->getName(); }
        else status_ = "Couldn't open the MIDI input";
    }
    if (outId.isNotEmpty()) {
        out_ = juce::MidiOutput::openDevice(outId);
        if (out_) status_ << ", feedback to " << out_->getName();
    }
    lastTc_.clear();
    props_.setValue("surfaceIn", inId);
    props_.setValue("surfaceOut", outId);
    props_.setValue("surfaceProtocol", int(p));
}

juce::String ControlSurface::signature(const juce::MidiMessage& m) const {
    if (m.isNoteOn()) return "N" + juce::String(m.getChannel()) + ":" + juce::String(m.getNoteNumber());
    if (m.isController()) {
        if (proto_ == Protocol::Hui && m.getControllerNumber() == 0x2F)   // HUI port: zone + port
            return "H" + juce::String(huiZone_) + ":" + juce::String(m.getControllerValue() & 0x0F);
        return "C" + juce::String(m.getChannel()) + ":" + juce::String(m.getControllerNumber()) + ":" + juce::String(m.getControllerValue());
    }
    if (m.isProgramChange()) return "P" + juce::String(m.getChannel()) + ":" + juce::String(m.getProgramChangeNumber());
    return {};
}

void ControlSurface::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) {
    juce::MessageManager::callAsync([this, m] { handle(m); });
}

void ControlSurface::invoke(juce::CommandID id) { cmds_.invokeDirectly(id, false); }   // already on the message thread

void ControlSurface::handle(const juce::MidiMessage& m) {
    // HUI: zone select precedes each port message; port bit 0x40 = press.
    if (proto_ == Protocol::Hui && m.isController() && m.getControllerNumber() == 0x0F) { huiZone_ = m.getControllerValue(); return; }
    const bool press = (m.isNoteOn() && m.getVelocity() > 0) ||
                       (m.isController() && (proto_ != Protocol::Hui || m.getControllerNumber() != 0x2F || (m.getControllerValue() & 0x40))) ||
                       m.isProgramChange();
    if (!press) return;

    auto sig = signature(m);
    if (sig.isEmpty()) return;
    if (int id = learning_.exchange(0)) {
        {
            const juce::ScopedLock sl(mapLock_);
            for (auto it = map_.begin(); it != map_.end();) it = it->second == id ? map_.erase(it) : std::next(it);
            map_[sig] = id;
        }
        save();
        if (onLearned) onLearned();
        return;
    }
    {
        const juce::ScopedLock sl(mapLock_);
        auto it = map_.find(sig);
        if (it != map_.end()) { invoke(it->second); return; }
    }
    if (proto_ != Protocol::MackieControl) return;

    // Mackie Control defaults.
    if (m.isController() && m.getControllerNumber() == mcu::JogCC) {
        int v = m.getControllerValue();
        int ticks = v < 0x40 ? v : -(v - 0x40);
        if (jog) jog(ticks * (scrub_ ? 1.0 : 1.0 / 75.0));
        return;
    }
    if (!m.isNoteOn() || m.getChannel() != 1) return;
    switch (m.getNoteNumber()) {
        case mcu::Play: invoke(cmd::playStop); break;
        case mcu::Stop: if (isPlaying && isPlaying()) invoke(cmd::playStop); break;
        case mcu::Record: invoke(cmd::record); break;
        case mcu::Rewind: invoke(cmd::prevMarker); break;
        case mcu::Forward: invoke(cmd::nextMarker); break;
        case mcu::Left: invoke(cmd::locateBackFrame); break;
        case mcu::Right: invoke(cmd::locateFwdFrame); break;
        case mcu::Up: invoke(cmd::zoomIn); break;
        case mcu::Down: invoke(cmd::zoomOut); break;
        case mcu::Zoom: invoke(cmd::zoomFit); break;
        case mcu::Scrub: scrub_ = !scrub_; break;
        case mcu::Marker: invoke(cmd::addTrackMarker); break;
        case mcu::Nudge: invoke(cmd::addIndexMarker); break;
        case mcu::Cycle: invoke(cmd::playSelection); break;
        case mcu::Drop: invoke(cmd::destIn); break;
        case mcu::Replace: invoke(cmd::replaceEdit); break;
        case mcu::Click: invoke(cmd::listenReference); break;
        case mcu::Solo: invoke(cmd::compareUnprocessed); break;
        case mcu::Save: invoke(cmd::saveSession); break;
        case mcu::Undo: invoke(cmd::undo); break;
        case mcu::Cancel: invoke(cmd::clearSelection); break;
        case mcu::Enter: invoke(cmd::returnToStart); break;
        default:
            if (m.getNoteNumber() >= mcu::F1 && m.getNoteNumber() <= mcu::F8) {
                static const juce::CommandID fkeys[8] = {cmd::sourceIn, cmd::sourceOut, cmd::insertEdit, cmd::overwriteEdit,
                                                         cmd::splitAtPlayhead, cmd::crossfadeEditor, cmd::analyse, cmd::exportDdp};
                invoke(fkeys[m.getNoteNumber() - mcu::F1]);
            }
            break;
    }
}

void ControlSurface::sendTimecode() {
    if (!out_ || proto_ != Protocol::MackieControl || !position) return;
    // MM:SS:FF on the 10-digit display (leading digits blank): CD frames, as everywhere in Lathe.
    auto sectors = juce::int64(std::llround(juce::jmax(0.0, position()) * 75));
    int f = int(sectors % 75), s = int((sectors / 75) % 60), mnt = int(sectors / 4500);
    juce::String tc = juce::String(mnt).paddedLeft(' ', 3) + juce::String(s).paddedLeft('0', 2) + juce::String(f).paddedLeft('0', 2);
    tc = tc.paddedLeft(' ', 10);
    if (tc == lastTc_) return;
    for (int i = 0; i < 10; ++i) {
        auto ch = tc[9 - i];
        if (lastTc_.length() == 10 && lastTc_[9 - i] == ch) continue;
        int v = ch == ' ' ? 0x20 : (ch >= '0' && ch <= '9') ? int(ch) : 0x20;
        bool dot = (i == 2 || i == 4);   // dots after frames and seconds
        out_->sendMessageNow(juce::MidiMessage::controllerEvent(1, mcu::TimecodeCC + i, (v & 0x3F) | (dot ? 0x40 : 0)));
    }
    lastTc_ = tc;
}

void ControlSurface::timerCallback() {
    if (!out_) return;
    if (proto_ == Protocol::Hui && ++pingCount_ >= 15) {   // keep-alive about once a second
        pingCount_ = 0;
        out_->sendMessageNow(juce::MidiMessage::noteOn(1, 0, juce::uint8(0)));
    }
    if (proto_ != Protocol::MackieControl) return;
    bool play = isPlaying && isPlaying(), rec = isRecording && isRecording();
    if (play != lastPlay_) {
        out_->sendMessageNow(juce::MidiMessage::noteOn(1, mcu::Play, juce::uint8(play ? 127 : 0)));
        out_->sendMessageNow(juce::MidiMessage::noteOn(1, mcu::Stop, juce::uint8(play ? 0 : 127)));
        lastPlay_ = play;
    }
    if (rec != lastRec_) {
        out_->sendMessageNow(juce::MidiMessage::noteOn(1, mcu::Record, juce::uint8(rec ? 127 : 0)));
        lastRec_ = rec;
    }
    sendTimecode();
}

void ControlSurface::clearMapping(juce::CommandID id) {
    {
        const juce::ScopedLock sl(mapLock_);
        for (auto it = map_.begin(); it != map_.end();) it = it->second == id ? map_.erase(it) : std::next(it);
    }
    save();
}

juce::String ControlSurface::mappingFor(juce::CommandID id) const {
    const juce::ScopedLock sl(mapLock_);
    for (auto& [sig, c] : map_)
        if (c == id) {
            auto parts = juce::StringArray::fromTokens(sig.substring(1), ":", "");
            switch (sig[0]) {
                case 'N': return "Note " + parts[1] + " ch " + parts[0];
                case 'C': return "CC " + parts[1] + "=" + parts[2] + " ch " + parts[0];
                case 'P': return "Program " + parts[1] + " ch " + parts[0];
                case 'H': return "HUI zone " + parts[0] + " port " + parts[1];
                default: return sig;
            }
        }
    return {};
}

void ControlSurface::save() {
    juce::XmlElement x("MIDIMAP");
    {
        const juce::ScopedLock sl(mapLock_);
        for (auto& [sig, id] : map_) {
            auto* e = x.createNewChildElement("MAP");
            e->setAttribute("sig", sig);
            e->setAttribute("cmd", id);
        }
    }
    props_.setValue("midiMap", &x);
    props_.saveIfNeeded();
}

void ControlSurface::load() {
    const juce::ScopedLock sl(mapLock_);
    map_.clear();
    if (auto x = props_.getXmlValue("midiMap"))
        for (auto* e : x->getChildWithTagNameIterator("MAP")) map_[e->getStringAttribute("sig")] = e->getIntAttribute("cmd");
}

}  // namespace lathe
