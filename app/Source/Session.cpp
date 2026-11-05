#include "Session.h"

namespace lathe {

double Session::effectiveRate() const {
    if (projectRate > 0) return projectRate;
    return clips.isEmpty() ? 44100.0 : clips.getReference(0).fileRate;
}

Layout Session::layout() const {
    Layout l;
    double cursor = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips.getReference(i);
        double gap = c.gapBeforeSec;
        if (i == 0) gap = juce::jmax(0.0, gap);
        // An overlap can't be longer than either clip.
        if (i > 0 && gap < 0) {
            double prevLen = l.clips.getReference(i - 1).end - l.clips.getReference(i - 1).start;
            gap = juce::jmax(gap, -juce::jmin(prevLen, c.durationSec()) * 0.95);
        }
        ClipPlacement p;
        p.start = cursor + gap;
        p.end = p.start + c.durationSec();
        l.clips.add(p);
        cursor = p.end;
        l.total = juce::jmax(l.total, p.end);
    }
    return l;
}

static int64_t toSector(double sec) { return int64_t(std::llround(sec * cd::kFramesPerSecond)); }

cd::Disc Session::toDisc(juce::StringArray* adj) const {
    cd::Disc d;
    d.upcEan = upcEan.retainCharacters("0123456789").toStdString();
    d.masterId = masterId.toStdString();
    d.text.title = discTitle.toStdString();
    d.text.performer = discPerformer.toStdString();
    auto lay = layout();
    d.leadOutSector = int64_t(std::ceil(lay.total * cd::kFramesPerSecond - 1e-9));

    int64_t prevEndSector = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips.getReference(i);
        const auto& p = lay.clips.getReference(i);
        cd::Track t;
        int64_t start = toSector(p.start);
        int64_t pauseLen = toSector(juce::jmax(0.0, c.pauseSec));
        int64_t idx0 = i == 0 ? 0 : juce::jmax(prevEndSector, start - pauseLen);
        if (i > 0 && c.gapBeforeSec < 0) idx0 = start;    // crossfade: no pause possible
        if (idx0 < start) t.indexes.push_back({0, idx0});
        else if (i == 0) t.indexes.push_back({0, 0});
        t.indexes.push_back({1, start});
        int n = 2;
        for (double off : c.subIndexSec) {
            int64_t s = toSector(p.start + off);
            if (s > t.indexes.back().sector && s < toSector(p.end)) t.indexes.push_back({n++, s});
            else if (adj) adj->add("Track " + juce::String(i + 1) + ": a sub-index outside the track was skipped.");
        }
        t.isrc = cd::normaliseIsrc(c.isrc);
        t.copyPermitted = c.copyPermitted;
        t.preEmphasis = c.preEmphasis;
        t.scms = c.scms;
        t.text = c.text;
        if (t.text.title.empty()) t.text.title = c.name.toStdString();
        d.tracks.push_back(t);
        prevEndSector = toSector(p.end);
    }
    return d;
}

// ---- persistence ------------------------------------------------------------------

static juce::var textToVar(const cd::CdText& t) {
    auto* o = new juce::DynamicObject();
    o->setProperty("title", juce::String(t.title));
    o->setProperty("performer", juce::String(t.performer));
    o->setProperty("songwriter", juce::String(t.songwriter));
    o->setProperty("composer", juce::String(t.composer));
    o->setProperty("arranger", juce::String(t.arranger));
    o->setProperty("message", juce::String(t.message));
    return o;
}

static cd::CdText textFromVar(const juce::var& v) {
    cd::CdText t;
    t.title = v["title"].toString().toStdString();
    t.performer = v["performer"].toString().toStdString();
    t.songwriter = v["songwriter"].toString().toStdString();
    t.composer = v["composer"].toString().toStdString();
    t.arranger = v["arranger"].toString().toStdString();
    t.message = v["message"].toString().toStdString();
    return t;
}

juce::var Session::toVar() const {
    auto* root = new juce::DynamicObject();
    root->setProperty("format", "lathe-session");
    root->setProperty("version", 1);
    root->setProperty("discTitle", discTitle);
    root->setProperty("discPerformer", discPerformer);
    root->setProperty("upcEan", upcEan);
    root->setProperty("masterId", masterId);
    root->setProperty("client", client);
    root->setProperty("engineer", engineer);
    root->setProperty("projectRate", projectRate);
    root->setProperty("pluginChain", pluginChainState.toBase64Encoding());
    juce::Array<juce::var> list;
    for (auto& c : clips) {
        auto* o = new juce::DynamicObject();
        o->setProperty("file", c.file.getFullPathName());
        o->setProperty("name", c.name);
        o->setProperty("fileRate", c.fileRate);
        o->setProperty("fileLength", c.fileLength);
        o->setProperty("trimStart", c.trimStart);
        o->setProperty("trimEnd", c.trimEnd);
        o->setProperty("gainDb", c.gainDb);
        o->setProperty("fadeIn", c.fadeInSec);
        o->setProperty("fadeOut", c.fadeOutSec);
        o->setProperty("gapBefore", c.gapBeforeSec);
        o->setProperty("pause", c.pauseSec);
        juce::Array<juce::var> subs;
        for (double s : c.subIndexSec) subs.add(s);
        o->setProperty("subIndexes", subs);
        o->setProperty("isrc", juce::String(c.isrc));
        o->setProperty("dcp", c.copyPermitted);
        o->setProperty("pre", c.preEmphasis);
        o->setProperty("scms", c.scms);
        o->setProperty("text", textToVar(c.text));
        list.add(o);
    }
    root->setProperty("clips", list);
    return root;
}

bool Session::fromVar(const juce::var& v, juce::String& error) {
    if (v["format"].toString() != "lathe-session") { error = "This isn't a Lathe session file."; return false; }
    discTitle = v["discTitle"].toString();
    discPerformer = v["discPerformer"].toString();
    upcEan = v["upcEan"].toString();
    masterId = v["masterId"].toString();
    client = v["client"].toString();
    engineer = v["engineer"].toString();
    projectRate = double(v["projectRate"]);
    pluginChainState.reset();
    pluginChainState.fromBase64Encoding(v["pluginChain"].toString());
    clips.clear();
    if (auto* arr = v["clips"].getArray()) {
        for (auto& o : *arr) {
            Clip c;
            c.file = juce::File(o["file"].toString());
            c.name = o["name"].toString();
            c.fileRate = double(o["fileRate"]);
            c.fileLength = juce::int64(o["fileLength"]);
            c.trimStart = juce::int64(o["trimStart"]);
            c.trimEnd = juce::int64(o["trimEnd"]);
            c.gainDb = double(o["gainDb"]);
            c.fadeInSec = double(o["fadeIn"]);
            c.fadeOutSec = double(o["fadeOut"]);
            c.gapBeforeSec = double(o["gapBefore"]);
            c.pauseSec = double(o["pause"]);
            if (auto* subs = o["subIndexes"].getArray())
                for (auto& s : *subs) c.subIndexSec.add(double(s));
            c.isrc = o["isrc"].toString().toStdString();
            c.copyPermitted = bool(o["dcp"]);
            c.preEmphasis = bool(o["pre"]);
            c.scms = bool(o["scms"]);
            c.text = textFromVar(o["text"]);
            if (c.fileRate <= 0) c.fileRate = 44100;
            clips.add(c);
        }
    }
    return true;
}

bool Session::save(const juce::File& f, juce::String& error) {
    // Write to a temp file then swap in, so a crash mid-save never destroys the session.
    auto tmp = f.getSiblingFile(f.getFileName() + ".tmp");
    if (!tmp.replaceWithText(juce::JSON::toString(toVar(), false))) { error = "Couldn't write " + tmp.getFullPathName(); return false; }
    if (!tmp.moveFileTo(f)) { error = "Couldn't replace " + f.getFullPathName(); return false; }
    file = f;
    return true;
}

bool Session::load(const juce::File& f, juce::String& error) {
    auto v = juce::JSON::parse(f.loadFileAsString());
    if (v.isVoid()) { error = "The session file is damaged or empty."; return false; }
    if (!fromVar(v, error)) return false;
    file = f;
    return true;
}

}  // namespace lathe
