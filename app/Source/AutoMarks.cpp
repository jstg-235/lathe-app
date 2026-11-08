#include "AutoMarks.h"

#include "ProgramRenderer.h"

namespace lathe {

juce::Array<juce::Range<double>> findSilences(juce::AudioFormatManager& formats, const Session& s, double thresholdDb, double minLen,
                                              std::function<bool(double)> progress) {
    juce::Array<juce::Range<double>> out;
    constexpr double rate = 11025.0;
    constexpr int block = 441;   // 40 ms
    ProgramRenderer pr(formats);
    pr.setSession(s, rate);
    const juce::int64 total = pr.totalSamples();
    const float thresh = float(juce::Decibels::decibelsToGain(thresholdDb));
    juce::AudioBuffer<float> buf(2, block);
    double silentFrom = -1;
    for (juce::int64 pos = 0; pos < total; pos += block) {
        const int n = int(juce::jmin<juce::int64>(block, total - pos));
        buf.clear();
        pr.render(buf, 0, n);
        const float peak = juce::jmax(buf.getMagnitude(0, 0, n), buf.getMagnitude(1, 0, n));
        const double t = double(pos) / rate;
        if (peak < thresh) {
            if (silentFrom < 0) silentFrom = t;
        } else if (silentFrom >= 0) {
            if (t - silentFrom >= minLen) out.add({silentFrom, t});
            silentFrom = -1;
        }
        if (progress && (pos / block) % 200 == 0 && !progress(double(pos) / double(juce::jmax<juce::int64>(1, total)))) return {};
    }
    if (silentFrom >= 0 && double(total) / rate - silentFrom >= minLen) out.add({silentFrom, double(total) / rate});
    return out;
}

int analogBlackToMarks(Session& s, const juce::Array<juce::Range<double>>& silences) {
    if (silences.isEmpty()) return 0;
    double first = 1e12, last = 0;
    for (auto& r : s.regions)
        if (!r.mute) { first = juce::jmin(first, r.start); last = juce::jmax(last, r.end()); }
    const double frame = 1.0 / cd::kFramesPerSecond;
    int added = 0;
    for (auto g : silences) {
        // Leading silence (before any sound) and trailing silence aren't between tracks.
        const bool leading = g.getStart() <= first + 0.05, trailing = g.getEnd() >= last - 0.05;
        if (trailing) continue;
        bool hasStart = false, hasPause = false;
        for (auto& m : s.markers) {
            hasStart = hasStart || (m.type == Marker::TrackStart && m.t >= g.getStart() - frame && m.t <= g.getEnd() + 0.5);
            hasPause = hasPause || (m.type == Marker::PauseStart && m.t >= g.getStart() - frame && m.t <= g.getEnd() + frame);
        }
        if (!hasStart) {
            const int id = s.addMarker(Marker::TrackStart, g.getEnd());
            if (auto* mk = s.findMarker(id))
                if (auto* r = s.findRegion(mk->attachedRegion)) mk->text.title = r->name.toStdString();
            ++added;
        }
        if (!leading && !hasPause) { s.addMarker(Marker::PauseStart, g.getStart()); ++added; }
    }
    return added;
}

}  // namespace lathe
