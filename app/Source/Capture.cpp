#include "Capture.h"

namespace lathe {

juce::var IoSettings::toVar() const {
    auto* o = new juce::DynamicObject();
    o->setProperty("monitorOut", monitorOut);
    o->setProperty("monitorSix", monitorSix);
    o->setProperty("defaultInput", defaultInput);
    o->setProperty("nextToSession", nextToSession);
    o->setProperty("folder", folder);
    o->setProperty("bits", bits);
    o->setProperty("inputMonitoring", inputMonitoring);
    o->setProperty("captureOut", captureOut);
    o->setProperty("captureIn", captureIn);
    o->setProperty("captureMute", captureMute);
    o->setProperty("capturePostRoll", capturePostRoll);
    juce::Array<juce::var> keys;
    for (auto& [k, v] : delays) {
        auto* e = new juce::DynamicObject();
        e->setProperty("key", k);
        e->setProperty("samples", v);
        keys.add(juce::var(e));
    }
    o->setProperty("delays", keys);
    return juce::var(o);
}

void IoSettings::fromVar(const juce::var& v) {
    if (!v.isObject()) return;
    monitorOut = int(v.getProperty("monitorOut", -1));
    monitorSix = bool(v.getProperty("monitorSix", false));
    defaultInput = juce::jmax(0, int(v.getProperty("defaultInput", 0)));
    nextToSession = bool(v.getProperty("nextToSession", true));
    folder = v.getProperty("folder", "").toString();
    bits = int(v.getProperty("bits", 24)) == 32 ? 32 : 24;
    inputMonitoring = bool(v.getProperty("inputMonitoring", false));
    captureOut = juce::jmax(0, int(v.getProperty("captureOut", 0)));
    captureIn = juce::jmax(0, int(v.getProperty("captureIn", 0)));
    captureMute = bool(v.getProperty("captureMute", false));
    capturePostRoll = juce::jlimit(0.0, 60.0, double(v.getProperty("capturePostRoll", 2.0)));
    delays.clear();
    if (auto* arr = v["delays"].getArray())
        for (auto& e : *arr)
            if (e["key"].toString().isNotEmpty() && int(e["samples"]) > 0) delays[e["key"].toString()] = int(e["samples"]);
}

juce::String IoSettings::delayKey(const juce::String& outputDevice, const juce::String& inputDevice, int firstOutput, int firstInput,
                                  double rate, int bufferSize) {
    return outputDevice + " | " + inputDevice + " | out " + juce::String(firstOutput + 1) + " | in " + juce::String(firstInput + 1) + " | " +
           juce::String(juce::roundToInt(rate)) + " Hz | buffer " + juce::String(bufferSize);
}

int IoSettings::delay(const juce::String& key) const {
    auto it = delays.find(key);
    return it == delays.end() ? -1 : it->second;
}

juce::File recordingsFolder(const IoSettings& io, const juce::File& sessionFile) {
    if (!io.nextToSession && io.folder.isNotEmpty() && juce::File::isAbsolutePath(io.folder)) return juce::File(io.folder);
    if (sessionFile.existsAsFile()) return sessionFile.getSiblingFile(sessionFile.getFileNameWithoutExtension() + " Recordings");
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Lathe Recordings");
}

juce::String recordingName(const juce::File& sessionFile, const juce::String& kind, juce::Time when) {
    const auto session = sessionFile == juce::File() ? juce::String("Untitled") : sessionFile.getFileNameWithoutExtension();
    return session + " " + kind + " " + when.formatted("%Y-%m-%d %H%M%S");
}

juce::int64 recordingBytes(double seconds, double rate, int channels, int bits) {
    return juce::int64(std::ceil(juce::jmax(0.0, seconds) * rate)) * channels * (bits / 8) + 4096;
}

CapturePlan planCapture(const Session& s, juce::Range<double> selection) {
    CapturePlan p;
    p.whole = selection.isEmpty();
    p.range = p.whole ? juce::Range<double>(0.0, s.end()) : selection;
    bool allMono = true;
    int lowest = -1;
    for (auto& r : s.regions) {
        if (r.mute || r.end() <= p.range.getStart() || r.start >= p.range.getEnd()) continue;
        p.sources.add(r.id);
        lowest = juce::jmax(lowest, r.lane);
        allMono = allMono && s.laneFormat(r.lane) == 1;
    }
    p.underLane = juce::jmax(0, lowest);
    p.channels = s.programmeChannels() == 6 ? 6 : (allMono && !p.sources.isEmpty() ? 1 : 2);
    return p;
}

int placeCapture(Session& s, const CapturePlan& plan, const CaptureTake& take, bool muteSources, const juce::String& name, juce::Array<int>* muted) {
    if (take.length <= 0 || take.rate <= 0) return 0;
    // File sample i holds what was played at timeline sample startSample + i - roundTrip.
    const juce::int64 a = juce::int64(std::llround(plan.range.getStart() * take.rate));
    const juce::int64 b = juce::int64(std::llround(plan.range.getEnd() * take.rate));
    const juce::int64 shift = juce::int64(take.roundTrip) - take.startSample;
    juce::int64 in = a + shift, out = juce::jmin(b + shift, take.length);
    juce::int64 startSample = a;
    if (in < 0) { startSample -= in; in = 0; }   // (the take began after the range did)
    if (out - in < 2) return 0;
    if (muteSources) {
        const double t0 = double(startSample) / take.rate, t1 = double(startSample + (out - in)) / take.rate;
        for (int id : plan.sources) {
            if (!s.findRegion(id)) continue;
            int inside = id;
            if (int right = s.split(id, t0)) inside = right;
            s.split(inside, t1);
            if (auto* r = s.findRegion(inside))
                if (r->start < t1 - 1.0e-6 && r->end() > t0 + 1.0e-6) {
                    r->mute = true;
                    if (muted) muted->add(r->id);
                }
        }
    }
    const int lane = plan.underLane + 1;
    s.insertLane(lane);
    s.setLaneFormat(lane, plan.channels);
    Region reg = s.makeRegion(take.file, take.rate, take.length);
    reg.srcIn = in;
    reg.srcOut = out;
    reg.start = double(startSample) / take.rate;
    reg.lane = lane;
    reg.name = name;
    s.regions.add(reg);
    return reg.id;
}

juce::String renameAudioFile(Session& s, const juce::File& from, const juce::String& newName, juce::File& renamed) {
    if (!from.existsAsFile()) return "Lathe can't find " + from.getFileName() + " to rename it (File > Locate Missing Audio).";
    const auto ext = from.getFileExtension();
    auto base = newName.trim();
    if (ext.isNotEmpty() && base.endsWithIgnoreCase(ext)) base = base.dropLastCharacters(ext.length());
    base = juce::File::createLegalFileName(base).trim();
    while (base.endsWithChar('.')) base = base.dropLastCharacters(1);
    if (base.isEmpty()) return "Type a name for the file.";
    const auto to = from.getSiblingFile(base + ext);
    if (to == from) { renamed = from; return {}; }
    const bool caseOnly = to.getFullPathName().equalsIgnoreCase(from.getFullPathName());
    if (to.exists() && !caseOnly) return "There is already a file called " + to.getFileName() + " in that folder.";
    if (!from.moveFileTo(to)) return "The file couldn't be renamed (is the folder read-only?).";
    const auto oldBase = from.getFileNameWithoutExtension();
    auto apply = [&](Session& x) {
        for (auto& r : x.regions)
            if (r.file == from && r.name == oldBase) r.name = base;
        x.relinkAudio(from, to);
    };
    apply(s);
    if (s.source) apply(*s.source);
    renamed = to;
    return {};
}

bool needsHardware(const Session& s, int lastLiveHardwareSlot, const std::function<int(int)>& slotOfAnchor) {
    if (lastLiveHardwareSlot < 0 || s.programmeChannels() == 6) return false;
    for (auto& r : s.regions) {
        if (r.mute) continue;
        const int enters = r.afterGear != 0 ? slotOfAnchor(r.afterGear) : -1;   // joins after this slot
        if (enters < lastLiveHardwareSlot) return true;
    }
    return false;
}

juce::String hardwareExportMessage() {
    return "The master chain has External Gear switched in, and hardware can't be rendered faster than real time, so this would leave the gear out. "
           "Print the programme through it first (open the External Gear insert and press Print), or bypass the insert.";
}

juce::Array<int> sourcesThrough(const Session& s, const CapturePlan& plan, int gearSlot, const std::function<int(int)>& slotOfAnchor) {
    juce::Array<int> out;
    for (int id : plan.sources)
        if (auto* r = s.findRegion(id)) {
            const int enters = r->afterGear != 0 ? slotOfAnchor(r->afterGear) : -1;
            if (enters < gearSlot) out.add(id);
        }
    return out;
}

Session captureAlone(const Session& s, int regionId) {
    Session alt;
    alt.projectRate = s.effectiveRate();
    alt.lanes = 1;
    if (auto* r = s.findRegion(regionId)) {
        Region c = *r;
        alt.setLaneFormat(0, s.laneFormat(r->lane));
        c.lane = 0;
        c.mute = false;
        c.fxState = {};
        c.afterGear = 0;
        c.printOf.clear();
        alt.regions.add(c);
    }
    return alt;
}

}  // namespace lathe
