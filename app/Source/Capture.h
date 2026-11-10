// Capturing through outboard: the settings that are remembered (where recordings go, the
// measured round trips), what a capture is made of, and where the finished take lands on the
// timeline. Pure data and layout: no audio I/O and no windows here.
#pragma once

#include "Session.h"

#include <map>

namespace lathe {

// Audio Setup's own settings (the interface itself is remembered by the audio device manager).
struct IoSettings {
    int monitorOut = -1;          // first channel of the monitor pair; -1 = the interface's first outputs
    bool monitorSix = false;      // a 5.1 programme plays on six outputs from there
    int defaultInput = 0;         // first channel of the default input pair
    bool nextToSession = true;    // recordings go in "<Session> Recordings" beside the session
    juce::String folder;          // otherwise here
    int bits = 24;                // 24, or 32 for 32-bit float
    bool inputMonitoring = false;
    // Capture route and options
    int captureOut = 0, captureIn = 0;   // first channel of the send and of the return
    bool captureMute = false;            // mute the original after capturing
    double capturePostRoll = 2.0;        // seconds recorded after the range, for the gear's tail
    // Measured round trips in samples, one per interface, output pair, input pair, sample rate
    // and buffer size (the buffer is part of the trip).
    std::map<juce::String, int> delays;

    juce::var toVar() const;
    void fromVar(const juce::var& v);
    static juce::String delayKey(const juce::String& outputDevice, const juce::String& inputDevice, int firstOutput, int firstInput,
                                 double rate, int bufferSize);
    int delay(const juce::String& key) const;   // -1 = not measured
};

// Where recordings go: the chosen folder, or "<Session> Recordings" next to the session (in
// Music/Lathe Recordings while the session has never been saved).
juce::File recordingsFolder(const IoSettings& io, const juce::File& sessionFile);
// "<Session> <kind> 2026-11-10 161500" (kind: Recording, Print or Capture).
juce::String recordingName(const juce::File& sessionFile, const juce::String& kind, juce::Time when);
// Size on disk of `seconds` of audio, in bytes.
juce::int64 recordingBytes(double seconds, double rate, int channels, int bits);

// What a capture takes in.
struct CapturePlan {
    juce::Range<double> range;    // the In-Out, or the whole programme
    bool whole = false;           // nothing was selected: the whole programme
    int channels = 2;             // 1, 2 or 6: the lane type of what is captured
    int underLane = 0;            // the lowest lane heard in the range; the capture lands under it
    juce::Array<int> sources;     // the regions heard in the range
    bool valid() const { return !range.isEmpty() && !sources.isEmpty(); }
};
// With an empty selection the whole programme is captured.
CapturePlan planCapture(const Session& s, juce::Range<double> selection);

// A finished take.
struct CaptureTake {
    juce::File file;
    double rate = 44100;
    juce::int64 length = 0;        // samples in the file
    juce::int64 startSample = 0;   // the timeline sample (at the file's rate) being played when the file's first sample was taken
    int roundTrip = 0;             // samples the return arrives late
};
// Puts the take on a new lane directly under the material it was made from, in sync with it
// (the round trip is taken off), trimmed to the range; the pre-roll and the tail stay in the file
// behind the region's edges. With muteSources, what was captured is muted (regions are cut at
// the edges of the range, so only that stretch goes quiet). Returns the new region's id, or 0
// when the file doesn't reach the range.
int placeCapture(Session& s, const CapturePlan& plan, const CaptureTake& take, bool muteSources, const juce::String& name);

// A session holding only that region (no effects, no timeline gain): the capture on its own, for
// switching against what was sent.
Session captureAlone(const Session& s, int regionId);

}  // namespace lathe
