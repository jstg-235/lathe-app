#include "Session.h"

namespace lathe {

static constexpr double kEps = 1e-9;

juce::String fadeShapeName(FadeShape s) {
    switch (s) {
        case FadeShape::Linear: return "Linear";
        case FadeShape::EqualPower: return "Equal power";
        case FadeShape::SCurve: return "S-curve";
        case FadeShape::Exponential: return "Exponential";
        case FadeShape::Logarithmic: return "Logarithmic";
    }
    return {};
}

double fadeGain(FadeShape s, double x) {
    x = juce::jlimit(0.0, 1.0, x);
    switch (s) {
        case FadeShape::Linear: return x;
        case FadeShape::EqualPower: return std::sin(juce::MathConstants<double>::halfPi * x);
        case FadeShape::SCurve: return 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * x);
        case FadeShape::Exponential: return x * x;
        case FadeShape::Logarithmic: return std::sqrt(x);
    }
    return x;
}

double Region::envelopeDb(double t) const {
    if (envelope.isEmpty()) return 0;
    if (t <= envelope.getFirst().t) return envelope.getFirst().db;
    if (t >= envelope.getLast().t) return envelope.getLast().db;
    for (int i = 1; i < envelope.size(); ++i) {
        const auto& a = envelope.getReference(i - 1);
        const auto& b = envelope.getReference(i);
        if (t <= b.t) {
            double f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1.0;
            return a.db + (b.db - a.db) * f;
        }
    }
    return envelope.getLast().db;
}

// ---- queries ----------------------------------------------------------------------

double Session::effectiveRate() const {
    if (projectRate > 0) return projectRate;
    return regions.isEmpty() ? 44100.0 : regions.getReference(0).fileRate;
}

double Session::end() const {
    double e = 0;
    for (auto& r : regions)
        if (!r.mute) e = juce::jmax(e, r.end());
    for (auto& m : markers)
        if (m.type == Marker::EndOfDisc) e = m.t;
    return e;
}

Region* Session::findRegion(int id) {
    for (auto& r : regions)
        if (r.id == id) return &r;
    return nullptr;
}

const Region* Session::findRegion(int id) const {
    for (auto& r : regions)
        if (r.id == id) return &r;
    return nullptr;
}

int Session::indexOfRegion(int id) const {
    for (int i = 0; i < regions.size(); ++i)
        if (regions.getReference(i).id == id) return i;
    return -1;
}

Marker* Session::findMarker(int id) {
    for (auto& m : markers)
        if (m.id == id) return &m;
    return nullptr;
}

void Session::sortMarkers() {
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
        if (std::abs(a.t - b.t) > kEps) return a.t < b.t;
        // At the same time: pause before track start before index.
        auto rank = [](Marker::Type t) { return t == Marker::PauseStart ? 0 : t == Marker::TrackStart ? 1 : t == Marker::Index ? 2 : 3; };
        return rank(a.type) < rank(b.type);
    });
}

EffectiveFades Session::fadesFor(int i) const {
    const Region& r = regions.getReference(i);
    EffectiveFades f{r.fadeIn, r.fadeOut, r.fadeInShape, r.fadeOutShape};
    for (int j = 0; j < regions.size(); ++j) {
        if (j == i) continue;
        const Region& o = regions.getReference(j);
        if (o.lane != r.lane || o.mute) continue;
        if (o.start < r.start - kEps && o.end() > r.start + kEps)   // o fades out into r
            f.in = juce::jmax(f.in, juce::jmin(o.end(), r.end()) - r.start);
        if (o.start > r.start + kEps && o.start < r.end() - kEps)   // r fades out into o
            f.out = juce::jmax(f.out, r.end() - o.start);
    }
    f.in = juce::jmin(f.in, r.length());
    f.out = juce::jmin(f.out, r.length());
    return f;
}

juce::Array<juce::Range<double>> Session::trackRanges() const {
    juce::Array<double> starts;
    for (auto& m : markers)
        if (m.type == Marker::TrackStart) starts.add(m.t);
    std::sort(starts.begin(), starts.end());
    juce::Array<juce::Range<double>> out;
    for (int i = 0; i < starts.size(); ++i)
        out.add({starts[i], i + 1 < starts.size() ? starts[i + 1] : end()});
    return out;
}

// ---- editing ----------------------------------------------------------------------

Region Session::makeRegion(const juce::File& f, double rate, juce::int64 length) {
    Region r;
    r.id = nextId++;
    r.file = f;
    r.name = f.getFileNameWithoutExtension();
    r.fileRate = rate > 0 ? rate : 44100;
    r.fileLength = length;
    r.srcIn = 0;
    r.srcOut = length;
    return r;
}

void Session::regionMoved(int id, double dt) {
    if (std::abs(dt) < kEps) return;
    for (auto& m : markers)
        if (m.attachedRegion == id) m.t += dt;
    sortMarkers();
}

int Session::append(Region r, double gap, bool addTrackMarker) {
    double laneEnd = 0;
    bool any = false;
    for (auto& o : regions)
        if (o.lane == r.lane) { laneEnd = juce::jmax(laneEnd, o.end()); any = true; }
    if (!any) gap = juce::jmax(gap, 2.0);   // Red Book: at least 2 s before track 1
    r.start = laneEnd + gap;
    if (r.id == 0) r.id = nextId++;
    regions.add(r);
    if (addTrackMarker) {
        int m = addMarker(Marker::TrackStart, r.start, r.id);
        if (auto* mk = findMarker(m)) {
            mk->text.title = r.name.toStdString();
            mk->text.performer = discPerformer.toStdString();
        }
        if (any && gap > 0) addMarker(Marker::PauseStart, laneEnd, r.id);
    }
    return r.id;
}

void Session::moveRegion(int id, double newStart, int newLane) {
    auto* r = findRegion(id);
    if (!r) return;
    newStart = juce::jmax(0.0, newStart);
    double dt = newStart - r->start;
    r->start = newStart;
    r->lane = juce::jlimit(0, lanes - 1, newLane);
    regionMoved(id, dt);
}

void Session::trimRegionStart(int id, double newStart) {
    auto* r = findRegion(id);
    if (!r) return;
    double earliest = r->start - double(r->srcIn) / r->fileRate;
    newStart = juce::jlimit(juce::jmax(0.0, earliest), r->end() - 0.01, newStart);
    auto delta = juce::int64(std::llround((newStart - r->start) * r->fileRate));
    r->srcIn = juce::jlimit<juce::int64>(0, r->srcOut - 1, r->srcIn + delta);
    double shift = double(delta) / r->fileRate;
    r->start += shift;
    for (auto& p : r->envelope) p.t -= shift;
}

void Session::trimRegionEnd(int id, double newEnd) {
    auto* r = findRegion(id);
    if (!r) return;
    double latest = r->start + double(r->fileLength - r->srcIn) / r->fileRate;
    newEnd = juce::jlimit(r->start + 0.01, latest, newEnd);
    r->srcOut = juce::jlimit<juce::int64>(r->srcIn + 1, r->fileLength, r->srcIn + juce::int64(std::llround((newEnd - r->start) * r->fileRate)));
}

void Session::deleteRegion(int id, bool rippleAfter) {
    int i = indexOfRegion(id);
    if (i < 0) return;
    Region r = regions[i];
    regions.remove(i);
    for (int k = markers.size(); --k >= 0;)
        if (markers.getReference(k).attachedRegion == id) markers.remove(k);
    if (rippleAfter) ripple(r.end() - kEps * 10, -r.length());
}

int Session::split(int id, double t) {
    auto* r = findRegion(id);
    if (!r || t <= r->start + 0.001 || t >= r->end() - 0.001) return 0;
    Region b = *r;
    b.id = nextId++;
    auto cut = r->srcIn + juce::int64(std::llround((t - r->start) * r->fileRate));
    b.srcIn = cut;
    b.start = r->start + double(cut - r->srcIn) / r->fileRate;
    b.fadeIn = 0;
    b.envelope.clear();
    for (auto& p : r->envelope)
        if (p.t >= t - r->start) b.envelope.add({p.t - (b.start - r->start), p.db});
    if (!r->envelope.isEmpty()) {
        double dbAtCut = r->envelopeDb(t - r->start);
        b.envelope.insert(0, {0.0, dbAtCut});
        juce::Array<GainPoint> keep;
        for (auto& p : r->envelope)
            if (p.t < t - r->start) keep.add(p);
        keep.add({t - r->start, dbAtCut});
        r->envelope = keep;
    }
    r->srcOut = cut;
    r->fadeOut = 0;
    const int aId = r->id;
    for (auto& m : markers)
        if (m.attachedRegion == aId && m.t >= b.start - kEps) m.attachedRegion = b.id;
    regions.add(b);
    return b.id;
}

void Session::ripple(double t, double dt, int except) {
    if (std::abs(dt) < kEps) return;
    for (auto& r : regions)
        if (r.id != except && r.start >= t - kEps) r.start = juce::jmax(0.0, r.start + dt);
    for (auto& m : markers)
        if (m.t >= t - kEps && (except == 0 || m.attachedRegion != except)) m.t = juce::jmax(0.0, m.t + dt);
    sortMarkers();
}

int Session::sourceEdit(const juce::File& f, double rate, juce::int64 len, juce::int64 sIn, juce::int64 sOut, int lane,
                        double dIn, double dOut, EditMode mode) {
    sIn = juce::jlimit<juce::int64>(0, len, sIn);
    sOut = juce::jlimit<juce::int64>(sIn + 1, len, sOut);
    const double srcLen = double(sOut - sIn) / rate;
    dIn = juce::jmax(0.0, dIn);

    auto clearRange = [this, lane](double a, double b) {
        for (int i = regions.size(); --i >= 0;) {
            Region r = regions[i];
            if (r.lane != lane || r.end() <= a + kEps || r.start >= b - kEps) continue;
            if (r.start >= a - kEps && r.end() <= b + kEps) { regions.remove(i); continue; }
            if (r.start < a && r.end() > b) {
                int tail = split(r.id, b);
                trimRegionEnd(r.id, a);
                juce::ignoreUnused(tail);
                continue;
            }
            if (r.start < a) trimRegionEnd(r.id, a);
            else trimRegionStart(r.id, b);
        }
    };

    if (mode == EditMode::Insert || (mode == EditMode::Replace && dOut <= dIn)) {
        for (auto& r : regions)
            if (r.lane == lane && r.start < dIn - kEps && r.end() > dIn + kEps) { split(r.id, dIn); break; }
        ripple(dIn, srcLen);
    } else if (mode == EditMode::Overwrite) {
        clearRange(dIn, dIn + srcLen);
    } else {   // Replace: 4-point
        clearRange(dIn, dOut);
        ripple(dOut, srcLen - (dOut - dIn));
    }

    Region r = makeRegion(f, rate, len);
    r.srcIn = sIn;
    r.srcOut = sOut;
    r.lane = lane;
    r.start = dIn;
    regions.add(r);
    return r.id;
}

void Session::setCrossfadeLength(int outgoing, int incoming, double len, bool rippleAfter) {
    auto* a = findRegion(outgoing);
    auto* b = findRegion(incoming);
    if (!a || !b) return;
    len = juce::jlimit(0.0, juce::jmin(a->length(), b->length()) * 0.95, len);
    double newStart = a->end() - len;
    double oldStart = b->start;
    double dt = newStart - oldStart;
    b->start = newStart;
    regionMoved(b->id, dt);
    if (rippleAfter) {
        ripple(oldStart + 1e-6, dt, b->id);
        a = findRegion(outgoing);
        b = findRegion(incoming);
    }
    a->fadeOut = juce::jmin(a->fadeOut, len);
    b->fadeIn = juce::jmin(b->fadeIn, len);
}

void Session::slip(int id, double dt) {
    auto* r = findRegion(id);
    if (!r) return;
    auto d = juce::int64(std::llround(dt * r->fileRate));
    d = juce::jlimit(-r->srcIn, r->fileLength - r->srcOut, d);
    r->srcIn += d;
    r->srcOut += d;
}

int Session::addMarker(Marker::Type type, double t, int attachTo) {
    Marker m;
    m.id = nextId++;
    m.type = type;
    m.t = juce::jmax(0.0, t);
    if (attachTo < 0) {
        attachTo = 0;
        for (auto& r : regions)
            if (t >= r.start - kEps && t < r.end() && (attachTo == 0 || r.lane < findRegion(attachTo)->lane)) attachTo = r.id;
    }
    m.attachedRegion = attachTo;
    if (type == Marker::TrackStart) {
        if (auto* r = findRegion(attachTo)) m.text.title = r->name.toStdString();
        m.text.performer = discPerformer.toStdString();
    }
    markers.add(m);
    sortMarkers();
    return m.id;
}

void Session::removeMarker(int id) {
    for (int i = 0; i < markers.size(); ++i)
        if (markers.getReference(i).id == id) { markers.remove(i); return; }
}

void Session::moveMarker(int id, double t) {
    if (auto* m = findMarker(id)) m->t = juce::jmax(0.0, t);
    sortMarkers();
}

static int64_t toSector(double sec) { return int64_t(std::llround(sec * cd::kFramesPerSecond)); }

namespace {

// Builds a Red Book disc from sorted markers whose times are already relative to the disc.
cd::Disc buildDisc(const juce::Array<Marker>& ms, double lengthSec, int offsetFrames, juce::StringArray* adj) {
    cd::Disc d;
    d.leadOutSector = int64_t(std::ceil(lengthSec * cd::kFramesPerSecond - 1e-6));
    double pendingPause = -1;
    int skippedIndexes = 0;
    for (auto& m : ms) {
        if (m.type == Marker::PauseStart) { pendingPause = m.t; continue; }
        if (m.type == Marker::EndOfDisc) continue;
        if (m.type == Marker::Index) {
            if (d.tracks.empty()) { ++skippedIndexes; continue; }
            auto& t = d.tracks.back();
            int64_t sec = toSector(m.t);
            if (sec > t.indexes.back().sector && t.indexes.size() < 100) t.indexes.push_back({t.indexes.back().number + 1, sec});
            else ++skippedIndexes;
            continue;
        }
        cd::Track t;
        int64_t start = toSector(m.t);
        int64_t prev = d.tracks.empty() ? -1 : d.tracks.back().indexes.back().sector;
        if (d.tracks.empty()) t.indexes.push_back({0, 0});
        else if (pendingPause >= 0) {
            int64_t p = toSector(pendingPause);
            if (p > prev && p < start) t.indexes.push_back({0, p});
        }
        pendingPause = -1;
        if (!t.indexes.empty() && t.indexes.back().sector >= start) t.indexes.pop_back();
        t.indexes.push_back({1, start});
        t.isrc = cd::normaliseIsrc(m.isrc);
        t.copyPermitted = m.copyPermitted;
        t.preEmphasis = m.preEmphasis;
        t.scms = m.scms;
        t.text = m.text;
        d.tracks.push_back(t);
    }
    if (adj && skippedIndexes) adj->add(juce::String(skippedIndexes) + " index marker(s) outside a track were skipped.");

    // PQ offset: move each track start (index 01) earlier so slow transports don't clip the
    // first notes. Never past the track's own pause, the previous track's last index, or
    // (track 1) the 2-second minimum pregap.
    if (offsetFrames > 0) {
        int clamped = 0;
        for (size_t ti = 0; ti < d.tracks.size(); ++ti) {
            auto& t = d.tracks[ti];
            for (size_t k = 0; k < t.indexes.size(); ++k) {
                if (t.indexes[k].number != 1) continue;
                int64_t floor = ti == 0 ? 2 * cd::kFramesPerSecond : d.tracks[ti - 1].indexes.back().sector + 1;
                if (k > 0) floor = juce::jmax(floor, t.indexes[k - 1].sector + 1);
                int64_t want = t.indexes[k].sector - offsetFrames;
                if (want < floor) { want = juce::jmin(t.indexes[k].sector, floor); ++clamped; }
                t.indexes[k].sector = want;
            }
        }
        if (adj) {
            adj->add("PQ offset: track starts moved " + juce::String(offsetFrames) + " frame(s) earlier.");
            if (clamped) adj->add(juce::String(clamped) + " track start(s) couldn't take the full offset (pregap or previous marker in the way).");
        }
    }
    return d;
}

}  // namespace

cd::Disc Session::toDisc(juce::StringArray* adj) const {
    Session s = *this;
    s.sortMarkers();
    cd::Disc d = buildDisc(s.markers, end(), pqOffsetFrames, adj);
    d.upcEan = upcEan.retainCharacters("0123456789").toStdString();
    d.masterId = (masterId.isNotEmpty() ? masterId : catalogue).toStdString();
    d.text.title = discTitle.toStdString();
    d.text.performer = discPerformer.toStdString();
    d.discId = cdTextDiscId.toStdString();
    d.genreCode = cdTextGenre;
    d.genreText = genreText.toStdString();
    if (adj && !d.tracks.empty()) {
        int64_t pre = d.tracks[0].startSector();
        if (htoa && pre > 2 * cd::kFramesPerSecond)
            adj->add("Hidden track one audio: " + juce::String(cd::Msf::fromSectors(pre).str()) + " of audio before track 1.");
        else if (!htoa && pre > 3 * cd::kFramesPerSecond)
            adj->add("Track 1 pregap is " + juce::String(cd::Msf::fromSectors(pre).str()) + "; turn on HTOA if audio there is intended.");
    }
    return d;
}

cd::Disc Session::toDiscForUnit(const Unit& u, int unitNumber, int unitCount, double& lead, juce::StringArray* notes) const {
    Session s = *this;
    s.sortMarkers();
    const Marker* current = nullptr;   // track playing at the unit's start
    juce::Array<Marker> inside;
    for (auto& m : s.markers) {
        if (m.type == Marker::EndOfDisc) continue;
        if (m.t <= u.start + 1e-6 && m.type == Marker::TrackStart) current = &m;
        if (m.t >= u.start - 1e-6 && m.t < u.end - 1e-6) inside.add(m);
    }
    double firstTrack = -1;
    for (auto& m : inside)
        if (m.type == Marker::TrackStart) { firstTrack = m.t - u.start; break; }

    // The unit starts inside a track's audio when no track starts within 4 s (Red Book minimum).
    const bool continued = current != nullptr && (firstTrack < 0 || firstTrack >= 4.0) && current->t < u.start - 1e-6;
    if (continued) {
        Marker c = *current;
        c.id = 0;
        c.t = u.start;
        c.text.title = (juce::String::fromUTF8(c.text.title.c_str()) + " (cont.)").toStdString();
        inside.insert(0, c);
        firstTrack = 0;
        if (notes) notes->add("Disc " + juce::String(unitNumber) + " starts mid-track: \"" + juce::String::fromUTF8(c.text.title.c_str()) +
                              "\" added as track 1" + (c.isrc.empty() ? juce::String() : " (ISRC reused)") + ".");
    }
    // Remove anything before the first track (index/pause markers in the pre-roll).
    while (!inside.isEmpty() && inside.getFirst().type != Marker::TrackStart) inside.remove(0);

    lead = firstTrack < 0 ? 2.0 : juce::jmax(0.0, 2.0 - firstTrack);
    lead = std::ceil(lead * cd::kFramesPerSecond - 1e-9) / cd::kFramesPerSecond;
    for (auto& m : inside) m.t = m.t - u.start + lead;

    cd::Disc d = buildDisc(inside, u.length() + lead, pqOffsetFrames, notes);
    d.upcEan = upcEan.retainCharacters("0123456789").toStdString();
    auto id = masterId.isNotEmpty() ? masterId : catalogue;
    if (unitCount > 1 && id.isNotEmpty()) id << "-D" << unitNumber;
    d.masterId = id.substring(0, 48).toStdString();
    auto title = discTitle;
    if (unitCount > 1 && title.isNotEmpty()) title << " (Disc " << unitNumber << ")";
    d.text.title = title.toStdString();
    d.text.performer = discPerformer.toStdString();
    d.discId = cdTextDiscId.toStdString();
    d.genreCode = cdTextGenre;
    d.genreText = genreText.toStdString();
    return d;
}

Product* Session::findProduct(int id) {
    for (auto& p : products)
        if (p.id == id) return &p;
    return nullptr;
}

juce::Array<juce::File> Session::missingAudio() const {
    juce::Array<juce::File> out;
    for (auto& r : regions)
        if (!r.file.existsAsFile()) out.addIfNotAlreadyThere(r.file);
    for (auto& r : references)
        if (!r.file.existsAsFile()) out.addIfNotAlreadyThere(r.file);
    return out;
}

int Session::relinkAudio(const juce::File& from, const juce::File& to) {
    int n = 0;
    for (auto& r : regions)
        if (r.file == from) { r.file = to; ++n; }
    for (auto& r : references)
        if (r.file == from) { r.file = to; ++n; }
    return n;
}

int Session::findMissingAudioIn(const juce::Array<juce::File>& folders) {
    int found = 0;
    for (auto& m : missingAudio())
        for (auto& dir : folders) {
            auto candidate = dir.getChildFile(m.getFileName());
            if (dir.isDirectory() && candidate.existsAsFile()) {
                relinkAudio(m, candidate);
                ++found;
                break;
            }
        }
    return found;
}

const ProductSpec& productSpec(ProductKind k) {
    // Targets are conservative cutting/duplication norms; hard limits are what can be made at all.
    static const ProductSpec specs[] = {
        {"CD", "Disc", 79 * 60 + 40, 79 * 60 + 57, false, true},
        {"12\" LP (33 1/3)", "Side", 22 * 60, 26 * 60, true, false},
        {"12\" (45)", "Side", 12 * 60, 15 * 60, true, false},
        {"10\" (33 1/3)", "Side", 14 * 60, 16 * 60, true, false},
        {"7\" (45)", "Side", 5 * 60, 7 * 60, true, false},
        {"Cassette", "Side", 30 * 60, 60 * 60, true, false},
    };
    int i = juce::jlimit(0, int(ProductKind::kCount) - 1, int(k));
    return specs[i];
}

// ---- persistence ------------------------------------------------------------------

static juce::var textToVar(const cd::CdText& t) {
    auto* o = new juce::DynamicObject();
    o->setProperty("title", juce::String::fromUTF8(t.title.c_str()));
    o->setProperty("performer", juce::String::fromUTF8(t.performer.c_str()));
    o->setProperty("songwriter", juce::String::fromUTF8(t.songwriter.c_str()));
    o->setProperty("composer", juce::String::fromUTF8(t.composer.c_str()));
    o->setProperty("arranger", juce::String::fromUTF8(t.arranger.c_str()));
    o->setProperty("message", juce::String::fromUTF8(t.message.c_str()));
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
    root->setProperty("version", 2);
    root->setProperty("discTitle", discTitle);
    root->setProperty("discPerformer", discPerformer);
    root->setProperty("upcEan", upcEan);
    root->setProperty("masterId", masterId);
    root->setProperty("client", client);
    root->setProperty("engineer", engineer);
    root->setProperty("projectRate", projectRate);
    root->setProperty("lanes", lanes);
    root->setProperty("nextId", nextId);
    root->setProperty("pluginChain", pluginChainState.toBase64Encoding());
    juce::Array<juce::var> rl;
    for (auto& r : regions) {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", r.id);
        o->setProperty("file", r.file.getFullPathName());
        o->setProperty("name", r.name);
        o->setProperty("fileRate", r.fileRate);
        o->setProperty("fileLength", r.fileLength);
        o->setProperty("srcIn", r.srcIn);
        o->setProperty("srcOut", r.srcOut);
        o->setProperty("lane", r.lane);
        o->setProperty("start", r.start);
        o->setProperty("gainDb", r.gainDb);
        o->setProperty("mute", r.mute);
        o->setProperty("fadeIn", r.fadeIn);
        o->setProperty("fadeOut", r.fadeOut);
        o->setProperty("fadeInShape", int(r.fadeInShape));
        o->setProperty("fadeOutShape", int(r.fadeOutShape));
        juce::Array<juce::var> env;
        for (auto& p : r.envelope) env.add(juce::Array<juce::var>{p.t, p.db});
        o->setProperty("envelope", env);
        if (r.fxState.isNotEmpty()) o->setProperty("fx", r.fxState);
        rl.add(o);
    }
    root->setProperty("regions", rl);
    juce::Array<juce::var> ml;
    for (auto& m : markers) {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", m.id);
        o->setProperty("type", int(m.type));
        o->setProperty("t", m.t);
        o->setProperty("region", m.attachedRegion);
        o->setProperty("isrc", juce::String(m.isrc));
        o->setProperty("dcp", m.copyPermitted);
        o->setProperty("pre", m.preEmphasis);
        o->setProperty("scms", m.scms);
        o->setProperty("text", textToVar(m.text));
        if (!m.publisher.empty()) o->setProperty("publisher", juce::String::fromUTF8(m.publisher.c_str()));
        if (!m.pro.empty()) o->setProperty("pro", juce::String::fromUTF8(m.pro.c_str()));
        if (m.explicitContent) o->setProperty("explicit", true);
        ml.add(o);
    }
    root->setProperty("markers", ml);
    juce::Array<juce::var> refs;
    for (auto& r : references) {
        auto* o = new juce::DynamicObject();
        o->setProperty("file", r.file.getFullPathName());
        o->setProperty("name", r.name);
        o->setProperty("lufs", r.lufs);
        o->setProperty("trim", r.trimDb);
        refs.add(o);
    }
    root->setProperty("references", refs);
    root->setProperty("activeReference", activeReference);
    root->setProperty("levelMatchReference", levelMatchReference);
    root->setProperty("loudnessTarget", loudnessTarget);
    auto* meta = new juce::DynamicObject();
    meta->setProperty("label", label);
    meta->setProperty("catalogue", catalogue);
    meta->setProperty("releaseDate", releaseDate);
    meta->setProperty("pLine", pLine);
    meta->setProperty("cLine", cLine);
    meta->setProperty("genreText", genreText);
    meta->setProperty("notes", notes);
    meta->setProperty("cdTextDiscId", cdTextDiscId);
    meta->setProperty("cdTextGenre", cdTextGenre);
    meta->setProperty("pqOffsetFrames", pqOffsetFrames);
    meta->setProperty("htoa", htoa);
    root->setProperty("release", meta);
    juce::Array<juce::var> pl;
    for (auto& p : products) {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", p.id);
        o->setProperty("kind", int(p.kind));
        o->setProperty("name", p.name);
        o->setProperty("catalogue", p.catalogue);
        o->setProperty("upc", p.upc);
        o->setProperty("maxUnit", p.maxUnit);
        o->setProperty("flipGap", p.flipGap);
        o->setProperty("evenUnits", p.evenUnits);
        juce::Array<juce::var> ul;
        for (auto& u : p.units)
            ul.add(juce::Array<juce::var>{u.start, u.end, u.fadeIn, u.fadeOut, int(u.inShape), int(u.outShape)});
        o->setProperty("units", ul);
        pl.add(o);
    }
    root->setProperty("products", pl);
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
    regions.clear();
    markers.clear();
    nextId = 1;

    if (int(v["version"]) < 2) {
        // v1 sessions: an ordered clip list with gaps. Lay them out and create markers.
        lanes = 2;
        double cursor = 0;
        bool first = true;
        if (auto* arr = v["clips"].getArray()) {
            for (auto& o : *arr) {
                Region r;
                r.id = nextId++;
                r.file = juce::File(o["file"].toString());
                r.name = o["name"].toString();
                r.fileRate = double(o["fileRate"]) > 0 ? double(o["fileRate"]) : 44100;
                r.fileLength = juce::int64(o["fileLength"]);
                r.srcIn = juce::int64(o["trimStart"]);
                r.srcOut = r.fileLength - juce::int64(o["trimEnd"]);
                r.gainDb = double(o["gainDb"]);
                r.fadeIn = double(o["fadeIn"]);
                r.fadeOut = double(o["fadeOut"]);
                double gap = double(o["gapBefore"]);
                if (first) gap = juce::jmax(0.0, gap);
                r.start = juce::jmax(0.0, cursor + gap);
                regions.add(r);
                Marker m;
                m.id = nextId++;
                m.type = Marker::TrackStart;
                m.t = r.start;
                m.attachedRegion = r.id;
                m.isrc = o["isrc"].toString().toStdString();
                m.copyPermitted = bool(o["dcp"]);
                m.preEmphasis = bool(o["pre"]);
                m.scms = bool(o["scms"]);
                m.text = textFromVar(o["text"]);
                markers.add(m);
                double pause = double(o["pause"]);
                if (!first && pause > 0 && gap > 0)
                    markers.add({nextId++, Marker::PauseStart, juce::jmax(cursor, r.start - pause), r.id});
                if (auto* subs = o["subIndexes"].getArray())
                    for (auto& s : *subs) markers.add({nextId++, Marker::Index, r.start + double(s), r.id});
                cursor = r.end();
                first = false;
            }
        }
        sortMarkers();
        return true;
    }

    lanes = juce::jlimit(1, 16, int(v["lanes"]));
    nextId = juce::jmax(1, int(v["nextId"]));
    if (auto* arr = v["regions"].getArray()) {
        for (auto& o : *arr) {
            Region r;
            r.id = int(o["id"]);
            r.file = juce::File(o["file"].toString());
            r.name = o["name"].toString();
            r.fileRate = double(o["fileRate"]) > 0 ? double(o["fileRate"]) : 44100;
            r.fileLength = juce::int64(o["fileLength"]);
            r.srcIn = juce::int64(o["srcIn"]);
            r.srcOut = juce::int64(o["srcOut"]);
            r.lane = juce::jlimit(0, lanes - 1, int(o["lane"]));
            r.start = double(o["start"]);
            r.gainDb = double(o["gainDb"]);
            r.mute = bool(o["mute"]);
            r.fadeIn = double(o["fadeIn"]);
            r.fadeOut = double(o["fadeOut"]);
            r.fadeInShape = FadeShape(juce::jlimit(0, 4, int(o["fadeInShape"])));
            r.fadeOutShape = FadeShape(juce::jlimit(0, 4, int(o["fadeOutShape"])));
            if (auto* env = o["envelope"].getArray())
                for (auto& p : *env) r.envelope.add({double(p[0]), double(p[1])});
            r.fxState = o["fx"].toString();
            nextId = juce::jmax(nextId, r.id + 1);
            regions.add(r);
        }
    }
    references.clear();
    if (auto* arr = v["references"].getArray())
        for (auto& o : *arr) {
            Reference r;
            r.file = juce::File(o["file"].toString());
            r.name = o["name"].toString();
            r.lufs = o.hasProperty("lufs") ? double(o["lufs"]) : -1000.0;
            r.trimDb = double(o["trim"]);
            references.add(r);
        }
    activeReference = v.hasProperty("activeReference") ? int(v["activeReference"]) : -1;
    if (activeReference >= references.size()) activeReference = -1;
    levelMatchReference = v.hasProperty("levelMatchReference") ? bool(v["levelMatchReference"]) : true;
    loudnessTarget = v.hasProperty("loudnessTarget") ? double(v["loudnessTarget"]) : -14.0;
    {
        auto meta = v["release"];
        label = meta["label"].toString();
        catalogue = meta["catalogue"].toString();
        releaseDate = meta["releaseDate"].toString();
        pLine = meta["pLine"].toString();
        cLine = meta["cLine"].toString();
        genreText = meta["genreText"].toString();
        notes = meta["notes"].toString();
        cdTextDiscId = meta["cdTextDiscId"].toString();
        cdTextGenre = juce::jlimit(0, cd::kGenreCount - 1, int(meta["cdTextGenre"]));
        pqOffsetFrames = juce::jlimit(0, 75, int(meta["pqOffsetFrames"]));
        htoa = bool(meta["htoa"]);
    }
    products.clear();
    if (auto* arr = v["products"].getArray())
        for (auto& o : *arr) {
            Product p;
            p.id = int(o["id"]);
            p.kind = ProductKind(juce::jlimit(0, int(ProductKind::kCount) - 1, int(o["kind"])));
            p.name = o["name"].toString();
            p.catalogue = o["catalogue"].toString();
            p.upc = o["upc"].toString();
            p.maxUnit = double(o["maxUnit"]);
            p.flipGap = o.hasProperty("flipGap") ? double(o["flipGap"]) : 2.0;
            p.evenUnits = o.hasProperty("evenUnits") ? bool(o["evenUnits"]) : true;
            if (auto* ul = o["units"].getArray())
                for (auto& u : *ul) {
                    Unit x;
                    x.start = double(u[0]);
                    x.end = double(u[1]);
                    x.fadeIn = double(u[2]);
                    x.fadeOut = double(u[3]);
                    x.inShape = FadeShape(juce::jlimit(0, 4, int(u[4])));
                    x.outShape = FadeShape(juce::jlimit(0, 4, int(u[5])));
                    p.units.add(x);
                }
            nextId = juce::jmax(nextId, p.id + 1);
            products.add(p);
        }
    if (auto* arr = v["markers"].getArray()) {
        for (auto& o : *arr) {
            Marker m;
            m.id = int(o["id"]);
            m.type = Marker::Type(juce::jlimit(0, 3, int(o["type"])));
            m.t = double(o["t"]);
            m.attachedRegion = int(o["region"]);
            m.isrc = o["isrc"].toString().toStdString();
            m.copyPermitted = bool(o["dcp"]);
            m.preEmphasis = bool(o["pre"]);
            m.scms = bool(o["scms"]);
            m.text = textFromVar(o["text"]);
            m.publisher = o["publisher"].toString().toStdString();
            m.pro = o["pro"].toString().toStdString();
            m.explicitContent = bool(o["explicit"]);
            nextId = juce::jmax(nextId, m.id + 1);
            markers.add(m);
        }
    }
    sortMarkers();
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
