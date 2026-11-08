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

double regionPeakDb(juce::AudioFormatManager& formats, const Region& r) {
    std::unique_ptr<juce::AudioFormatReader> rd(formats.createReaderFor(r.file));
    if (!rd) return -std::numeric_limits<double>::infinity();
    const int ch = int(juce::jmin<unsigned int>(rd->numChannels, 8));
    std::vector<juce::Range<float>> levels(size_t(juce::jmax(1, ch)));
    rd->readMaxLevels(r.srcIn, r.srcOut - r.srcIn, levels.data(), ch);
    float peak = 0;
    for (auto& l : levels) peak = juce::jmax(peak, std::abs(l.getStart()), std::abs(l.getEnd()));
    return peak > 0 ? 20.0 * std::log10(double(peak)) : -std::numeric_limits<double>::infinity();
}

int applySegmentGain(Session& s, const juce::Array<int>& ids, SegmentGainMode mode, double value, int polarity,
                     std::function<double(const Region&)> peakDb) {
    int n = 0;
    for (int id : ids) {
        auto* r = s.findRegion(id);
        if (!r) continue;
        const double before = r->gainDb;
        const bool inv = r->invert;
        switch (mode) {
            case SegmentGainMode::Absolute: r->gainDb = value; break;
            case SegmentGainMode::Relative: r->gainDb += value; break;
            case SegmentGainMode::Normalize: {
                const double p = peakDb ? peakDb(*r) : -std::numeric_limits<double>::infinity();
                if (std::isfinite(p)) r->gainDb = value - p;
                break;
            }
        }
        r->gainDb = juce::jlimit(-96.0, 48.0, r->gainDb);
        if (polarity >= 0) r->invert = polarity == 1;
        if (std::abs(r->gainDb - before) > 1e-9 || r->invert != inv) ++n;
    }
    return n;
}

}  // namespace lathe
