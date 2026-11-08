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

}  // namespace lathe
