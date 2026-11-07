#include "Products.h"

namespace lathe {

double unitGain(const Unit& u, double t) {
    if (t < u.start || t >= u.end) return 0.0;
    double g = 1.0;
    if (u.fadeIn > 0 && t < u.start + u.fadeIn) g *= fadeGain(u.inShape, (t - u.start) / u.fadeIn);
    if (u.fadeOut > 0 && t > u.end - u.fadeOut) g *= fadeGain(u.outShape, (u.end - t) / u.fadeOut);
    return g;
}

juce::String unitLetter(const Product& p, int i) {
    if (!productSpec(p.kind).sides) return juce::String(i + 1);
    juce::String s;
    int n = i;
    do { s = juce::String::charToString(juce::juce_wchar('A' + n % 26)) + s; n = n / 26 - 1; } while (n >= 0);
    return s;
}

juce::String unitLabel(const Product& p, int i, bool longForm) {
    const auto& spec = productSpec(p.kind);
    if (!spec.sides) return juce::String(spec.unitNoun) + " " + juce::String(i + 1);
    juce::String side = "Side " + unitLetter(p, i);
    if (!longForm || p.units.size() <= 2) return side;
    juce::String item = p.kind == ProductKind::Cassette ? "Tape" : p.kind == ProductKind::Single7_45 ? "Single" : "Record";
    return item + " " + juce::String(i / 2 + 1) + ", " + side;
}

double programmeLength(const Session& s) { return s.end(); }

std::vector<UnitTrack> unitTracks(const Session& s, const Unit& u) {
    Session c = s;
    c.sortMarkers();
    std::vector<UnitTrack> out;
    juce::Array<const Marker*> starts;
    for (auto& m : c.markers)
        if (m.type == Marker::TrackStart) starts.add(&m);
    const double end = c.end();
    for (int i = 0; i < starts.size(); ++i) {
        double a = starts[i]->t, b = i + 1 < starts.size() ? starts[i + 1]->t : end;
        double x = juce::jmax(a, u.start), y = juce::jmin(b, u.end);
        if (y - x < 0.05) continue;
        UnitTrack t;
        t.number = i + 1;
        t.title = juce::String::fromUTF8(starts[i]->text.title.c_str());
        t.performer = juce::String::fromUTF8(starts[i]->text.performer.c_str());
        if (t.performer.isEmpty()) t.performer = s.discPerformer;
        t.isrc = juce::String(cd::normaliseIsrc(starts[i]->isrc));
        t.start = x - u.start;
        t.end = y - u.start;
        t.continuesFromPrevious = a < u.start - 0.05;
        t.continuesOnNext = b > u.end + 0.05;
        out.push_back(t);
    }
    return out;
}

static juce::String mmss(double sec) {
    int t = int(std::round(sec));
    return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
}

std::vector<UnitIssue> validateProduct(const Session& s, const Product& p) {
    std::vector<UnitIssue> v;
    const auto& spec = productSpec(p.kind);
    const double T = programmeLength(s);
    if (p.units.isEmpty()) {
        v.push_back({true, p.name + ": no " + juce::String(spec.unitNoun).toLowerCase() + "s yet. Use Auto-Split or add them."});
        return v;
    }
    for (int i = 0; i < p.units.size(); ++i) {
        const auto& u = p.units.getReference(i);
        auto lbl = p.name + " " + unitLabel(p, i, true);
        if (u.length() <= 0) { v.push_back({true, lbl + " has no length."}); continue; }
        if (u.length() > spec.hardMax + 0.01)
            v.push_back({true, lbl + " is " + mmss(u.length()) + "; the format holds at most " + mmss(spec.hardMax) + "."});
        else if (u.length() > p.limit() + 0.01)
            v.push_back({false, lbl + " is " + mmss(u.length()) + ", over the " + mmss(p.limit()) + " target" +
                                    (spec.cdFormat ? "." : " (expect lower level or more groove pitch compromise).")});
        if (u.fadeIn + u.fadeOut > u.length() + 1e-6) v.push_back({true, lbl + ": fades are longer than the unit."});
        if (spec.cdFormat && u.length() < 4.0) v.push_back({true, lbl + " is shorter than 4 seconds."});
        if (i + 1 < p.units.size()) {
            const auto& n = p.units.getReference(i + 1);
            if (n.start > u.end + 0.05)
                v.push_back({true, "Gap between " + unitLabel(p, i) + " and " + unitLabel(p, i + 1) + ": " +
                                       juce::String(n.start - u.end, 1) + " s of the programme is on neither."});
            if (n.start < u.start) v.push_back({true, unitLabel(p, i + 1) + " starts before " + unitLabel(p, i) + "."});
        }
    }
    if (p.units.getFirst().start > 0.05) v.push_back({false, "The first " + mmss(p.units.getFirst().start) + " of the programme isn't on any unit."});
    if (p.units.getLast().end < T - 0.05) v.push_back({true, "The last " + juce::String(T - p.units.getLast().end, 1) + " s of the programme isn't on any unit."});
    if (spec.sides && p.evenUnits && p.units.size() % 2 == 1)
        v.push_back({false, "Odd number of sides: the last side of " + p.name + " will be blank (or etched)."});
    return v;
}

// ---------------------------------------------------------------- auto-split

namespace {

struct Cand {
    double t;
    bool mid;   // breaks inside music: overlap + fades
};

// Quietest 1-second stretch within [lo, hi] using the momentary loudness map.
double quietestNear(const dsp::LoudnessMap& m, double lo, double hi) {
    const auto& v = m.momentary.empty() ? m.shortTerm : m.momentary;
    if (v.empty()) return (lo + hi) * 0.5;
    const int w = juce::jmax(1, int(std::round(1.0 / m.hopSec)));
    int a = juce::jmax(0, int(lo / m.hopSec)), b = juce::jmin(int(v.size()) - w, int(hi / m.hopSec));
    double best = 1e9;
    int bestI = -1;
    for (int i = a; i <= b; ++i) {
        double sum = 0;
        for (int k = 0; k < w; ++k) sum += juce::jmax(-120.0, double(v[size_t(i + k)]));
        double avg = sum / w;
        // Prefer the point closest to the middle on ties (within 0.5 LU).
        double centre = std::abs((i + w * 0.5) * m.hopSec - (lo + hi) * 0.5) / juce::jmax(1.0, hi - lo);
        double score = avg + centre * 0.5;
        if (score < best) { best = score; bestI = i; }
    }
    return bestI < 0 ? (lo + hi) * 0.5 : (bestI + w * 0.5) * m.hopSec;
}

// Chooses n-1 breaks from candidates minimising the spread of unit lengths. Returns empty if
// no choice keeps every unit within `limit`.
std::vector<Cand> choose(const std::vector<Cand>& cands, double T, int n, double limit, double overlap) {
    std::vector<Cand> c;
    c.push_back({0, false});
    for (auto& x : cands)
        if (x.t > 1.0 && x.t < T - 1.0) c.push_back(x);
    c.push_back({T, false});
    const size_t C = c.size();
    const double ideal = T / n;
    const double midPenalty = 45.0 * 45.0;   // a track boundary wins unless it costs ~45 s of balance
    const double inf = 1e300;
    std::vector<std::vector<double>> dp(size_t(n + 1), std::vector<double>(C, inf));
    std::vector<std::vector<int>> from(size_t(n + 1), std::vector<int>(C, -1));
    dp[0][0] = 0;
    auto len = [&](size_t i, size_t j) {
        return c[j].t - c[i].t + (c[i].mid ? overlap : 0) + (c[j].mid ? overlap : 0);
    };
    for (int k = 1; k <= n; ++k)
        for (size_t j = 1; j < C; ++j) {
            if (k < n && j == C - 1) continue;
            for (size_t i = 0; i < j; ++i) {
                if (dp[size_t(k - 1)][i] >= inf) continue;
                double L = len(i, j);
                if (L > limit + 1e-6) continue;
                double cost = dp[size_t(k - 1)][i] + (L - ideal) * (L - ideal) + (c[j].mid ? midPenalty : 0);
                if (cost < dp[size_t(k)][j]) { dp[size_t(k)][j] = cost; from[size_t(k)][j] = int(i); }
            }
        }
    if (dp[size_t(n)][C - 1] >= inf) return {};
    std::vector<Cand> breaks;
    int j = int(C - 1);
    for (int k = n; k > 0; --k) {
        int i = from[size_t(k)][size_t(j)];
        if (i > 0) breaks.insert(breaks.begin(), c[size_t(i)]);
        j = i;
    }
    return breaks;
}

}  // namespace

juce::String autoSplit(const Session& s, Product& p, const dsp::LoudnessMap* map, SplitOptions o) {
    const auto& spec = productSpec(p.kind);
    const double T = programmeLength(s);
    if (T <= 0) return "The programme is empty.";
    const double limit = p.limit();
    std::vector<Cand> trackStarts;
    for (auto& m : s.markers)
        if (m.type == Marker::TrackStart) trackStarts.push_back({m.t, false});
    std::sort(trackStarts.begin(), trackStarts.end(), [](const Cand& a, const Cand& b) { return a.t < b.t; });

    int n = juce::jmax(1, int(std::ceil(T / limit - 1e-9)));
    const bool even = spec.sides && p.evenUnits;
    for (int attempt = 0; attempt < 12; ++attempt, ++n) {
        if (even && n % 2 == 1) ++n;
        std::vector<Cand> cands = trackStarts;
        // Mid-music candidates near each ideal break, for tracks longer than a unit.
        for (int k = 1; k < n; ++k) {
            double ideal = T * k / n;
            double lo = juce::jmax(1.0, ideal - o.searchWindow), hi = juce::jmin(T - 1.0, ideal + o.searchWindow);
            cands.push_back({map ? quietestNear(*map, lo, hi) : ideal, true});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.t < b.t; });
        auto breaks = choose(cands, T, n, limit, o.overlap);
        if (breaks.size() + 1 != size_t(n) && n > 1) continue;
        p.units.clear();
        double a = 0;
        bool aMid = false;
        int mids = 0;
        for (size_t i = 0; i <= breaks.size(); ++i) {
            double b = i < breaks.size() ? breaks[i].t : T;
            bool bMid = i < breaks.size() && breaks[i].mid;
            Unit u;
            u.start = aMid ? juce::jmax(0.0, a - o.overlap) : a;
            u.end = bMid ? juce::jmin(T, b + o.overlap) : b;
            u.fadeIn = aMid ? o.overlap : 0;
            u.fadeOut = bMid ? o.overlap : 0;
            p.units.add(u);
            mids += bMid ? 1 : 0;
            a = b;
            aMid = bMid;
        }
        juce::String msg;
        msg << p.name << ": " << p.units.size() << " " << juce::String(spec.unitNoun).toLowerCase() << (p.units.size() == 1 ? "" : "s");
        if (mids) msg << ", " << mids << " break(s) inside music" << (map ? " at quiet points" : " (analyse first to find quiet points)") << " with "
                      << juce::String(o.overlap, 1) << " s overlap and fades";
        msg << ".";
        return msg;
    }
    return "Couldn't split within " + mmss(limit) + " per unit.";
}

// ---------------------------------------------------------------- break audition

BreakPlan makeBreakPlan(const Product& p, int k, double pre, double post) {
    BreakPlan b;
    if (k < 0 || k + 1 >= p.units.size()) return b;
    b.out = p.units[k];
    b.in = p.units[k + 1];
    b.a1 = b.out.end;
    b.a0 = juce::jmax(b.out.start, b.out.end - b.out.fadeOut - pre);
    b.b0 = b.in.start;
    b.b1 = juce::jmin(b.in.end, b.in.start + b.in.fadeIn + post);
    b.gap = juce::jmax(0.0, p.flipGap);
    return b;
}

std::pair<double, double> BreakPlan::at(double v) const {
    const double la = a1 - a0;
    if (v < la) { double t = a0 + v; return {t, unitGain(out, t)}; }
    v -= la;
    if (v < gap) return {-1.0, 0.0};
    v -= gap;
    double t = b0 + v;
    if (t >= b1) return {-1.0, 0.0};
    return {t, unitGain(in, t)};
}

}  // namespace lathe
