// The editable project, modelled as an EDL:
//  - Regions: a span of a source file (source in/out) placed on a lane at a destination time.
//    Overlapping regions on one lane crossfade automatically.
//  - Markers: PQ markers (track start, index, pause start, end of disc) on the timeline,
//    independent of the audio but optionally attached to a region so they move with it.
//  - Disc metadata and the plugin chain state.
// Pure data + layout maths; no audio I/O here.
#pragma once

#include "../../core/cd/Disc.h"

#include <juce_core/juce_core.h>

namespace lathe {

enum class FadeShape { Linear = 0, EqualPower, SCurve, Exponential, Logarithmic, Custom };
constexpr int kNumFadeShapes = 6;
juce::String fadeShapeName(FadeShape s);
// Gain (0..1) of a fade-in at position x in [0,1]. Fade-outs use 1-x.
// Custom: the gain halfway through is `dbDown` below unity (3 dB = equal power, 6 dB = linear
// amplitude), and `alpha` (0..1) bends the curve towards an S shape around that point.
double fadeGain(FadeShape s, double x, double dbDown = 3.0, double alpha = 0.0);

struct GainPoint {
    double t = 0;    // seconds from region start
    double db = 0;
};

struct Region {
    int id = 0;
    juce::File file;
    juce::String name;
    double fileRate = 44100;
    juce::int64 fileLength = 0;
    juce::int64 srcIn = 0, srcOut = 0;   // source samples [in, out)
    int lane = 0;
    double start = 0;                    // destination time, seconds
    double gainDb = 0;
    bool mute = false;
    double fadeIn = 0, fadeOut = 0;      // seconds (overlaps on the same lane extend these automatically)
    FadeShape fadeInShape = FadeShape::EqualPower, fadeOutShape = FadeShape::EqualPower;
    // Edit Fade detail. Offsets move a fade inside the region (the fade-in starts this long after
    // the region start, silent before; the fade-out ends this long before the region end). With
    // "manual", the fade keeps its own length instead of stretching to cover the overlap.
    double fadeInOffset = 0, fadeOutOffset = 0;
    double fadeInDb = 3.0, fadeOutDb = 3.0, fadeInAlpha = 0, fadeOutAlpha = 0;   // Custom shape
    bool fadeInManual = false, fadeOutManual = false;
    juce::Array<GainPoint> envelope;     // empty = flat
    juce::String fxState;                // region effects chain (base64), empty = none

    double length() const { return double(srcOut - srcIn) / fileRate; }
    double end() const { return start + length(); }
    double envelopeDb(double t) const;   // linear interpolation between points
};

struct Marker {
    enum Type { TrackStart = 0, Index, PauseStart, EndOfDisc };
    int id = 0;
    Type type = TrackStart;
    double t = 0;                // seconds on the timeline
    int attachedRegion = 0;      // region id, 0 = free; attached markers move with their region
    // Track metadata (TrackStart only)
    std::string isrc;
    bool copyPermitted = false, preEmphasis = false, scms = false;
    cd::CdText text;
    // Label-copy credits (paperwork and tags only, not written to the disc)
    std::string publisher, pro;
    bool explicitContent = false;
};

// ---- Products: the same programme cut into physical units (CDs, LP sides, tape sides) ----
enum class ProductKind { CD = 0, LP12_33, LP12_45, LP10_33, Single7_45, Cassette, kCount };

struct ProductSpec {
    const char* name;        // "CD", "12\" LP (33 1/3)"
    const char* unitNoun;    // "Disc", "Side"
    double target;           // recommended maximum per unit, seconds
    double hardMax;          // beyond this the unit can't be made
    bool sides;              // units come in pairs (A/B) on one physical item
    bool cdFormat;           // 44.1 kHz / 16-bit DDP per unit; otherwise hi-res side files
};
const ProductSpec& productSpec(ProductKind k);

// One unit is a window on the timeline with its own fades. Fades belong to the unit only:
// the full programme (digital delivery) is untouched. Neighbouring units may overlap, so
// music can fade out at the end of side A and fade back in at the top of side B.
struct Unit {
    double start = 0, end = 0;          // timeline seconds, [start, end)
    double fadeIn = 0, fadeOut = 0;     // seconds, inside the window
    FadeShape inShape = FadeShape::EqualPower, outShape = FadeShape::EqualPower;
    double length() const { return end - start; }
};

struct Product {
    int id = 0;
    ProductKind kind = ProductKind::CD;
    juce::String name;          // e.g. "CD", "LP"; shown on paperwork
    juce::String catalogue, upc;   // per product (the CD and the LP usually have different numbers)
    double maxUnit = 0;         // per-unit limit used by auto-split (0 = the spec's target)
    double flipGap = 2.0;       // break audition: silence between units (0 = no gap)
    bool evenUnits = true;      // vinyl/cassette: always an even number of sides
    juce::Array<Unit> units;
    double limit() const { return maxUnit > 0 ? maxUnit : productSpec(kind).target; }
};

// Session reference point: a named locator that isn't a PQ mark (a note, an edit to check,
// "client comment at 2:31"). Locked points stay at their time when material ripples.
struct Srp {
    int id = 0;
    double t = 0;
    juce::String text;
    bool locked = false;
};

// A piece of a timeline copied for pasting (also how source material reaches the destination).
// Region starts are relative to the clip start; lanes are relative to the first copied lane.
struct Clip {
    juce::Array<Region> regions;
    double length = 0;
    int laneCount = 1;
    bool isEmpty() const { return length <= 0; }
};

// Insert: later material moves right. Replace: overwrite from the point for the clip length.
// ReplaceLeft: overwrite so the clip ends at the point (back-timed). Constrained: overwrite the
// destination In-Out only (the clip is cut to fit). Overlay: lay over the top (overlaps crossfade).
enum class PasteMode { Insert = 0, Replace, ReplaceLeft, Constrained, Overlay };
juce::String pasteModeName(PasteMode m);

// Effective fade lengths after automatic crossfades, per region index.
struct EffectiveFades {
    double in = 0, out = 0;
    FadeShape inShape = FadeShape::EqualPower, outShape = FadeShape::EqualPower;
    double inOffset = 0, outOffset = 0;
    double inDb = 3.0, outDb = 3.0, inAlpha = 0, outAlpha = 0;
    // Fade gain at t seconds into a region of `length` seconds.
    double gainAt(double t, double length) const;
};

// A commercial or client reference for A/B comparison. Not part of the master.
struct Reference {
    juce::File file;
    juce::String name;
    double lufs = -1000;      // integrated loudness; -1000 = not measured yet
    double trimDb = 0;        // manual trim on top of the level match
};

class Session {
public:
    juce::Array<Reference> references;
    int activeReference = -1;
    bool levelMatchReference = true;
    double loudnessTarget = -14.0;   // drawn on the loudness overlay
    juce::Array<Region> regions;
    juce::Array<Marker> markers;
    int lanes = 2;
    juce::String discTitle, discPerformer, upcEan, masterId, client, engineer;
    // Release metadata for paperwork, CD-Text and tags
    juce::String label, catalogue, releaseDate, pLine, cLine, genreText, notes, cdTextDiscId;
    int cdTextGenre = 0;          // CD-Text genre code (0 = none)
    int pqOffsetFrames = 0;       // track starts are moved earlier by this many CD frames on delivery
    bool htoa = false;            // hidden track one audio: audio before track 1 index 01 is intended
    juce::Array<Product> products;
    juce::Array<Srp> srps;
    juce::Array<int> lanesOff;    // lanes set to None: edits, pastes and ripples leave them alone
    // The source timeline (the material edits are taken from) is a session of its own, saved
    // inside this one. `source` points at the live one; `sourceDoc` is what was last read from file.
    Session* source = nullptr;
    juce::var sourceDoc;
    double projectRate = 0;                 // 0 = follow the first region's rate
    juce::MemoryBlock pluginChainState;
    juce::File file;
    int nextId = 1;

    double effectiveRate() const;
    double end() const;                     // end of the last region (or EndOfDisc marker)
    Region* findRegion(int id);
    const Region* findRegion(int id) const;
    int indexOfRegion(int id) const;
    Marker* findMarker(int id);
    void sortMarkers();
    EffectiveFades fadesFor(int regionIndex) const;

    // ---- Editing operations (each keeps attached markers in step) -----------------
    Region makeRegion(const juce::File& f, double rate, juce::int64 length);
    // Places a region at the end of lane 0 after `gap` seconds and adds track markers.
    int append(Region r, double gap, bool addTrackMarker = true);
    void moveRegion(int id, double newStart, int newLane);
    void trimRegionStart(int id, double newStart);   // moves the left edge, keeps audio in place
    void trimRegionEnd(int id, double newEnd);
    void deleteRegion(int id, bool ripple);
    // Cuts the region at time t into two regions (no audio change).
    int split(int id, double t);
    // Shifts everything (regions + markers + unlocked SRPs) at or after t by dt seconds.
    // With `onLanes`, only regions on those lanes move (markers always move).
    void ripple(double t, double dt, int exceptRegion = 0, const juce::Array<int>* onLanes = nullptr);

    // ---- Lane targets, clipboard edits ----
    bool laneTargeted(int lane) const { return !lanesOff.contains(lane); }
    juce::Array<int> targetLanes() const;   // targeted lanes in order (all lanes if none are)
    // Copies [a, b) on `lanes` (cutting regions at the edges).
    Clip copyRange(double a, double b, const juce::Array<int>& lanes) const;
    // Removes the audio in [a, b) on `lanes`. With ripple, later material on those lanes moves up
    // and PQ marks / unlocked SRPs inside the range go.
    void clearRange(double a, double b, const juce::Array<int>& lanes, bool ripple);
    // Pastes a clip: clip lane k goes to lanes[k] (lanes are added if needed). `destOut` is the
    // destination Out point (ReplaceLeft ends there; Constrained fills `at`..destOut). Returns
    // the new region ids.
    juce::Array<int> paste(const Clip& c, const juce::Array<int>& lanes, double at, PasteMode mode, double destOut = -1);

    // ---- SRPs ----
    int addSrp(double t, const juce::String& text = {});
    Srp* findSrp(int id);
    void removeSrp(int id);
    void sortSrps();
    // 4-point edit: replace destination [dIn, dOut) on lane with source [sIn, sOut).
    // If dOut < 0 it's a 3-point edit (insert at dIn). Overwrite keeps the timeline length.
    enum class EditMode { Insert, Overwrite, Replace };
    int sourceEdit(const juce::File& f, double fileRate, juce::int64 fileLength, juce::int64 sIn, juce::int64 sOut,
                   int lane, double dIn, double dOut, EditMode mode);
    // Moves the later region of an overlap so the crossfade is `len` seconds.
    // With rippleAfter, everything after the incoming region keeps its spacing to it.
    enum class Ripple { None, All, UntilBlack };
    void setCrossfadeLength(int outgoing, int incoming, double len, bool rippleAfter = false) {
        setCrossfadeLength(outgoing, incoming, len, rippleAfter ? Ripple::All : Ripple::None);
    }
    // UntilBlack: only the material joined to the incoming region moves; the shift is absorbed
    // by the first gap (silence) after it.
    void setCrossfadeLength(int outgoing, int incoming, double len, Ripple mode);
    // Lock Sound: makes the crossfade `len` long, centred on the edit, by trimming both sides
    // into their handles. Nothing moves in time. Works on a butt splice too.
    void crossfadeCentred(int outgoing, int incoming, double len);
    // Pairs (outgoing, incoming) of regions that butt or overlap at the edges of `ids`.
    juce::Array<std::pair<int, int>> splicesAround(const juce::Array<int>& ids) const;
    // Moves the region and, with UntilBlack / All, what follows it (markers attached ride along).
    void shiftWithRipple(int id, double dt, Ripple mode);
    // Slips a region's audio under its fixed boundaries by dt seconds of source.
    void slip(int id, double dt);

    int addMarker(Marker::Type type, double t, int attachTo = -1);   // attachTo -1 = auto (region under t)
    void removeMarker(int id);
    void moveMarker(int id, double t);

    // Converts markers to a Red Book disc (index positions rounded to CD frames, PQ offset
    // applied to track starts).
    cd::Disc toDisc(juce::StringArray* adjustments = nullptr) const;
    // The disc for one CD unit: markers inside the unit's window, re-timed to the unit, with
    // a pregap added in front and a "(cont.)" track when the unit starts mid-track (it reuses
    // the track's ISRC). `leadSeconds` is the silence added before the window's audio.
    cd::Disc toDiscForUnit(const Unit& u, int unitNumber, int unitCount, double& leadSeconds, juce::StringArray* notes = nullptr) const;
    Product* findProduct(int id);

    // ---- Missing audio (moved folders, another computer) ----
    // Distinct audio files used by regions or references that don't exist on this machine.
    juce::Array<juce::File> missingAudio() const;
    // Points every region / reference using `from` at `to`. Returns how many were changed.
    int relinkAudio(const juce::File& from, const juce::File& to);
    // Looks for each missing file by name in `folders` and relinks the ones it finds.
    // Returns how many files were found.
    int findMissingAudioIn(const juce::Array<juce::File>& folders);
    // Each CD track's [start, end) in seconds (index 01 to next track's index 01 / end).
    juce::Array<juce::Range<double>> trackRanges() const;

    juce::var toVar() const;
    bool fromVar(const juce::var& v, juce::String& error);
    bool save(const juce::File& f, juce::String& error);
    bool load(const juce::File& f, juce::String& error);

private:
    void regionMoved(int id, double dt);
};

}  // namespace lathe
