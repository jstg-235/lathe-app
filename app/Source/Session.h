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

enum class FadeShape { Linear = 0, EqualPower, SCurve, Exponential, Logarithmic };
juce::String fadeShapeName(FadeShape s);
// Gain (0..1) of a fade-in at position x in [0,1]. Fade-outs use 1-x.
double fadeGain(FadeShape s, double x);

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
};

// Effective fade lengths after automatic crossfades, per region index.
struct EffectiveFades {
    double in = 0, out = 0;
    FadeShape inShape = FadeShape::EqualPower, outShape = FadeShape::EqualPower;
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
    // Shifts everything (regions + markers) at or after t by dt seconds.
    void ripple(double t, double dt, int exceptRegion = 0);
    // 4-point edit: replace destination [dIn, dOut) on lane with source [sIn, sOut).
    // If dOut < 0 it's a 3-point edit (insert at dIn). Overwrite keeps the timeline length.
    enum class EditMode { Insert, Overwrite, Replace };
    int sourceEdit(const juce::File& f, double fileRate, juce::int64 fileLength, juce::int64 sIn, juce::int64 sOut,
                   int lane, double dIn, double dOut, EditMode mode);
    // Moves the later region of an overlap so the crossfade is `len` seconds.
    // With rippleAfter, everything after the incoming region keeps its spacing to it.
    void setCrossfadeLength(int outgoing, int incoming, double len, bool rippleAfter = false);
    // Slips a region's audio under its fixed boundaries by dt seconds of source.
    void slip(int id, double dt);

    int addMarker(Marker::Type type, double t, int attachTo = -1);   // attachTo -1 = auto (region under t)
    void removeMarker(int id);
    void moveMarker(int id, double t);

    // Converts markers to a Red Book disc (index positions rounded to CD frames).
    cd::Disc toDisc(juce::StringArray* adjustments = nullptr) const;
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
