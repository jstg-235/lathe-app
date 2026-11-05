// The editable project: an ordered list of clips (one per CD track), disc metadata,
// and the plugin chain's saved state. Pure data + layout maths; no audio I/O here.
#pragma once

#include "../../core/cd/Disc.h"

#include <juce_core/juce_core.h>

namespace lathe {

struct Clip {
    juce::File file;
    juce::String name;                // shown in the list; defaults to the file name
    double fileRate = 44100;          // source sample rate
    juce::int64 fileLength = 0;       // source length in samples
    juce::int64 trimStart = 0;        // source samples skipped at the head
    juce::int64 trimEnd = 0;          // source samples skipped at the tail
    double gainDb = 0;
    double fadeInSec = 0, fadeOutSec = 0;
    // Time from the end of the previous clip to this clip's start (index 01).
    // Negative = overlap/crossfade with the previous clip. For clip 1 this is the
    // lead-in silence from the top of the disc (Red Book: at least 2 s).
    double gapBeforeSec = 2.0;
    // Where index 00 (the pause) begins, measured back from index 01. 0 = no visible pause,
    // the pause shows on the player counting down. Clamped to the gap.
    double pauseSec = 2.0;
    juce::Array<double> subIndexSec;  // index 02.. offsets from the clip start
    std::string isrc;
    bool copyPermitted = false, preEmphasis = false, scms = false;
    cd::CdText text;

    double durationSec() const { return double(fileLength - trimStart - trimEnd) / fileRate; }
};

// Where every clip sits on the programme timeline, in seconds from the top of the disc.
struct ClipPlacement {
    double start = 0;   // index 01
    double end = 0;
};

struct Layout {
    juce::Array<ClipPlacement> clips;
    double total = 0;   // end of the last clip (lead-out)
};

class Session {
public:
    juce::Array<Clip> clips;
    juce::String discTitle, discPerformer, upcEan, masterId, client, engineer;
    double projectRate = 0;                 // 0 = follow the first clip's rate
    juce::MemoryBlock pluginChainState;     // serialised by the engine
    juce::File file;                        // where it was saved

    Layout layout() const;
    double effectiveRate() const;

    // Converts the timeline to a Red Book disc. Index positions round to the nearest
    // CD frame (1/75 s). `issues` collects anything the conversion had to adjust.
    cd::Disc toDisc(juce::StringArray* adjustments = nullptr) const;

    juce::var toVar() const;
    bool fromVar(const juce::var& v, juce::String& error);
    bool save(const juce::File& f, juce::String& error);
    bool load(const juce::File& f, juce::String& error);
};

}  // namespace lathe
