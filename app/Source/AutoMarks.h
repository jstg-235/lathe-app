// Automatic PQ marks from the audio itself ("analog black": silence inside the programme).
#pragma once

#include "Session.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace lathe {

// Stretches of the programme quieter than `thresholdDb` (peak, dBFS) for at least `minLen`
// seconds. `progress` gets 0..1 and returns false to cancel.
juce::Array<juce::Range<double>> findSilences(juce::AudioFormatManager& formats, const Session& s, double thresholdDb, double minLen,
                                              std::function<bool(double)> progress = {});

// A pause (track end) at the start of each silence and a track start at its end, skipping
// silence before the first audio and after the last. Returns how many marks were added.
int analogBlackToMarks(Session& s, const juce::Array<juce::Range<double>>& silences);

// Peak level of a region's audio (its source span, before gain, envelope and fades), dBFS.
// -inf for silence or an unreadable file.
double regionPeakDb(juce::AudioFormatManager& formats, const Region& r);

// Segment Gain on several regions. Absolute sets the gain, Relative adds to it, Normalize sets
// it so each region's peak (from `peakDb`) lands on `value` dBFS. Polarity: -1 leave, 0 normal,
// 1 inverted. Returns how many regions changed.
enum class SegmentGainMode { Absolute, Relative, Normalize };
int applySegmentGain(Session& s, const juce::Array<int>& ids, SegmentGainMode mode, double value, int polarity,
                     std::function<double(const Region&)> peakDb = {});

}  // namespace lathe
