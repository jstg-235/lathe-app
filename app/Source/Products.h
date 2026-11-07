// Products: cutting one programme into physical units (CDs, LP sides, tape sides) without
// touching the full programme. Unit fades are applied after the master chain and before
// dither, to that unit's render only.
#pragma once

#include "Session.h"

#include "../../core/dsp/Analyzer.h"

namespace lathe {

// Gain (0..1) of unit `u` at timeline time t: 0 outside the window, fades at its edges.
double unitGain(const Unit& u, double t);

// "Disc 2", "Side C", "LP 2 Side C" style labels.
juce::String unitLabel(const Product& p, int index, bool longForm = false);
juce::String unitLetter(const Product& p, int index);    // "A".."Z" for sides, "1".. for discs

// A track (or part of one) as it falls on a unit, for cutting sheets and label copy.
struct UnitTrack {
    int number = 0;              // programme track number (1-based)
    juce::String title, performer, isrc;
    double start = 0, end = 0;   // seconds relative to the unit's start, clipped to the unit
    bool continuesFromPrevious = false, continuesOnNext = false;
};
std::vector<UnitTrack> unitTracks(const Session& s, const Unit& u);

// Problems a plant or cutting room would flag: length over limit, gaps in coverage, odd side count.
struct UnitIssue { bool error; juce::String message; };
std::vector<UnitIssue> validateProduct(const Session& s, const Product& p);

// Splits the programme into units no longer than the product's limit, as evenly as possible.
// Breaks are at track starts whenever every unit can fit that way; otherwise it breaks inside
// music at the quietest moment near the ideal point (needs `map`; otherwise at the ideal point).
// Mid-music breaks get an overlap with fades (`overlap` seconds each side).
struct SplitOptions {
    double overlap = 5.0;
    double searchWindow = 90.0;   // seconds either side of the ideal point for a quiet break
};
juce::String autoSplit(const Session& s, Product& p, const dsp::LoudnessMap* map, SplitOptions o = {});

// Programme extent used for products: from 0 to the end of the session.
double programmeLength(const Session& s);

// Break audition: the last `pre` seconds of unit k, `gap` seconds of silence, then the first
// `post` seconds of unit k+1. Virtual time runs through the three parts in order.
struct BreakPlan {
    Unit out, in;
    double a0 = 0, a1 = 0;   // timeline span heard from unit k
    double b0 = 0, b1 = 0;   // timeline span heard from unit k+1
    double gap = 0;
    double length() const { return (a1 - a0) + gap + (b1 - b0); }
    // Maps virtual seconds to {timeline seconds, gain}; timeline < 0 means silence.
    std::pair<double, double> at(double v) const;
};
BreakPlan makeBreakPlan(const Product& p, int breakIndex, double pre, double post);

}  // namespace lathe
