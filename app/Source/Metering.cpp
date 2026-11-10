#include "Metering.h"

namespace lathe {

namespace {
constexpr float kFloor = -70.0f;
float lin2db(float v) { return v > 0 ? 20.0f * std::log10(v) : -200.0f; }
juce::String num(double v, int dp = 1, const char* unit = "") {
    if (!std::isfinite(v) || v < -150) return "--";
    juce::String s(v, dp);
    if (unit && *unit) s << " " << unit;
    return s;
}
juce::String signedNum(double v, int dp = 1) {
    if (!std::isfinite(v)) return "--";
    return (v >= 0.05 ? "+" : "") + juce::String(v, dp);
}
// Colours for loudness relative to the target, as on broadcast radar meters.
juce::Colour levelColour(double lu) {
    if (lu > 6.0) return juce::Colour(0xffe0605a);
    if (lu > 0.0) return juce::Colour(0xfff0c43a);
    if (lu > -10.0) return juce::Colour(0xff5fd17a);
    if (lu > -20.0) return juce::Colour(0xff39c2c9);
    return juce::Colour(0xff3d7fd8);
}
const juce::Colour kGreen(0xff5fd17a), kYellow(0xfff0c43a), kRed(0xffe0605a), kCyan(0xff39c2c9), kBlue(0xff3d7fd8), kMono(0xfff2d16b);
}  // namespace

// ================================================================= MeterHub

void MeterHub::prepare(double rate) {
    if (std::abs(rate - rate_) < 0.5) return;
    rate_ = rate;
    loud_.prepare(rate, 2);
    spec_.prepare(rate);
    stereo_.prepare(rate);
    for (auto& t : blockTp_) t.prepare(rate);
    reset();
}

void MeterHub::reset() {
    loud_.reset();
    spec_.reset();
    stereo_.reset();
    for (auto& t : blockTp_) t.reset();
    for (int c = 0; c < 2; ++c) peak_[c] = hold_[c] = max_[c] = 0, holdAge_[c] = 0;
    recentTp_.clear();
    clock_ = 0;
    lastHop_ = 0;
    histS_.clear();
    histM_.clear();
    histC_.clear();
}

int MeterHub::feed(MeterTap& tap) {
    if (rate_ <= 0) return 0;
    const int n = tap.pull(scratch_);
    const double dt = n > 0 ? n / rate_ : 1.0 / 30.0;
    for (int c = 0; c < 2; ++c) {
        holdAge_[c] += dt;
        if (holdAge_[c] > 3.0) { hold_[c] = peak_[c]; holdAge_[c] = 0; }
    }
    if (n <= 0 || paused) {
        for (auto& p : peak_) p *= 0.86f;
        return n;
    }
    const float* l = scratch_.getReadPointer(0);
    const float* r = scratch_.getReadPointer(1);
    const float* ch[2] = {l, r};
    loud_.process(ch, n);
    spec_.process(l, r, n);
    stereo_.process(l, r, n);
    clock_ += dt;
    float tpBlock = 0;
    for (int c = 0; c < 2; ++c) {
        const float m = scratch_.getMagnitude(c, 0, n);
        peak_[c] = juce::jmax(m, peak_[c] * 0.86f);
        if (m >= hold_[c]) { hold_[c] = m; holdAge_[c] = 0; }
        max_[c] = juce::jmax(max_[c], m);
        tpBlock = juce::jmax(tpBlock, blockTp_[c].process(ch[c], n));
    }
    recentTp_.push_back({clock_, tpBlock});
    while (!recentTp_.empty() && recentTp_.front().first < clock_ - 3.0) recentTp_.erase(recentTp_.begin());
    // One history point per 100 ms step of the loudness meter.
    for (; lastHop_ < loud_.hops(); ++lastHop_) {
        histS_.push_back(float(loud_.shortTermLufs()));
        histM_.push_back(float(loud_.momentaryLufs()));
        histC_.push_back(float(stereo_.correlation()));
    }
    return n;
}

double MeterHub::targetLufs() const {
    const auto& s = dsp::loudnessStandards();
    return juce::isPositiveAndBelow(standard, int(s.size())) ? s[size_t(standard)].targetLufs : sessionTarget;
}
double MeterHub::tolerance() const {
    const auto& s = dsp::loudnessStandards();
    return juce::isPositiveAndBelow(standard, int(s.size())) ? s[size_t(standard)].tolerance : 1.0;
}
double MeterHub::maxTruePeak() const {
    const auto& s = dsp::loudnessStandards();
    return juce::isPositiveAndBelow(standard, int(s.size())) ? s[size_t(standard)].maxTruePeak : -1.0;
}
juce::String MeterHub::standardName() const {
    const auto& s = dsp::loudnessStandards();
    return juce::isPositiveAndBelow(standard, int(s.size())) ? juce::String(s[size_t(standard)].name)
                                                             : "Session target " + juce::String(sessionTarget, 1) + " LUFS";
}

float MeterHub::peakDb(int c) const { return lin2db(peak_[c]); }
float MeterHub::holdDb(int c) const { return lin2db(hold_[c]); }
float MeterHub::maxSampleDb(int c) const { return lin2db(max_[c]); }

double MeterHub::plr() const {
    const double i = loud_.integratedLufs(), tp = loud_.truePeakDb();
    return std::isfinite(i) && std::isfinite(tp) ? tp - i : NAN;
}

double MeterHub::psr() const {
    float tp = 0;
    for (auto& p : recentTp_) tp = juce::jmax(tp, p.second);
    const double s = loud_.shortTermLufs();
    return tp > 0 && std::isfinite(s) ? lin2db(tp) - s : NAN;
}

void MeterHub::setReferenceProfiles(std::vector<dsp::BalanceProfile> p, juce::StringArray names) {
    refs_ = std::move(p);
    refNames_ = std::move(names);
    target_ = dsp::BalanceTarget::fromProfiles(refs_);
}

dsp::BalanceProfile MeterHub::programmeProfile() const { return dsp::BalanceProfile::fromPower(spec_.bands(), spec_.averagePower(0)); }

std::vector<dsp::BalanceRange> MeterHub::ranges() const { return dsp::balanceRanges(spec_.bands(), spec_.averagePower(0), target_, refs_); }

dsp::BalanceProfile MeterHub::profileOfFile(juce::AudioFormatManager& fm, const juce::File& f, std::function<bool(double)> keep) {
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    if (!r) return {};
    dsp::SpectrumAnalyzer an;
    an.prepare(r->sampleRate);
    juce::AudioBuffer<float> buf(2, 65536);
    for (juce::int64 pos = 0; pos < r->lengthInSamples;) {
        const int n = int(juce::jmin<juce::int64>(65536, r->lengthInSamples - pos));
        r->read(&buf, 0, n, pos, true, true);
        an.process(buf.getReadPointer(0), buf.getReadPointer(1), n);
        pos += n;
        if (keep && !keep(double(pos) / double(r->lengthInSamples))) return {};
    }
    return dsp::BalanceProfile::fromPower(an.bands(), an.averagePower(0));
}

// ================================================================= MeterStrip

void MeterStrip::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    auto r = getLocalBounds().reduced(6);
    const auto& L = hub_.loudness();
    // Peak bars (dBFS, -60..0) with held peak and true-peak overs in red.
    auto bars = r.removeFromLeft(46);
    auto scale = [](float db, float h) { return h * juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    for (int c = 0; c < 2; ++c) {
        auto b = bars.removeFromLeft(19).toFloat();
        bars.removeFromLeft(4);
        g.setColour(colours::line);
        g.fillRect(b);
        const float p = hub_.peakDb(c);
        const float hgt = scale(p, b.getHeight());
        juce::ColourGradient grad(kGreen, 0, b.getBottom(), kRed, 0, b.getY(), false);
        grad.addColour(1.0 - 6.0 / 60.0, kYellow);
        g.setGradientFill(grad);
        g.fillRect(b.withTop(b.getBottom() - hgt));
        g.setColour(L.truePeakDb(c) > hub_.maxTruePeak() ? kRed : colours::text);
        g.drawHorizontalLine(int(b.getBottom() - scale(hub_.holdDb(c), b.getHeight())), b.getX(), b.getRight());
    }
    r.removeFromLeft(8);
    // Integrated loudness, big, coloured against the target.
    const double I = L.integratedLufs(), target = hub_.targetLufs();
    auto top = r.removeFromTop(34);
    g.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    g.setColour(std::isfinite(I) ? (std::abs(I - target) <= hub_.tolerance() ? kGreen : (I > target ? kYellow : kCyan)) : colours::dim);
    g.drawText(std::isfinite(I) ? juce::String(I, 1) : juce::String("--"), top.removeFromLeft(top.getWidth() - 60), juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(11.0f));
    g.setColour(colours::dim);
    g.drawText("LUFS I", top, juce::Justification::centredRight);
    g.setFont(juce::FontOptions(12.5f));
    auto row = [&](const juce::String& k, const juce::String& v, juce::Colour c = colours::text) {
        auto x = r.removeFromTop(16);
        g.setColour(colours::dim);
        g.drawText(k, x.removeFromLeft(78), juce::Justification::left);
        g.setColour(c);
        g.drawText(v, x, juce::Justification::right);
    };
    const double tp = L.truePeakDb();
    row("Momentary", num(L.momentaryLufs(), 1, "LUFS"));
    row("Short-term", num(L.shortTermLufs(), 1, "LUFS"));
    row("Range", num(L.loudnessRange(), 1, "LU"));
    row("True peak", num(tp, 1, "dBTP"), std::isfinite(tp) && tp > hub_.maxTruePeak() ? kRed : colours::text);
    row("PLR / PSR", (std::isfinite(hub_.plr()) ? juce::String(hub_.plr(), 1) : juce::String("--")) + " / " +
                         (std::isfinite(hub_.psr()) ? juce::String(hub_.psr(), 1) : juce::String("--")));
    // Correlation bar, -1..+1.
    auto cb = r.removeFromTop(14).reduced(0, 3).toFloat();
    g.setColour(colours::line);
    g.fillRect(cb);
    const double corr = hub_.stereo().correlation();
    const float cx = cb.getCentreX(), x = cb.getX() + cb.getWidth() * float((corr + 1) * 0.5);
    g.setColour(corr < 0 ? kRed : corr < 0.3 ? kYellow : kGreen);
    g.fillRect(juce::Rectangle<float>(juce::jmin(cx, x), cb.getY(), std::abs(x - cx), cb.getHeight()));
    g.setColour(colours::dim);
    g.drawVerticalLine(int(cx), cb.getY() - 2, cb.getBottom() + 2);
    // Meter bridge button.
    auto bottom = r.removeFromBottom(18);
    bridgeBtn_ = bottom.removeFromRight(90);
    g.setColour(colours::panel2);
    g.fillRoundedRectangle(bridgeBtn_.toFloat(), 3);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(11.5f));
    g.drawText("Meter Bridge", bridgeBtn_, juce::Justification::centred);
    g.setColour(colours::dim);
    g.drawText("corr " + juce::String(corr, 2), bottom, juce::Justification::left);
}

void MeterStrip::mouseDown(const juce::MouseEvent& e) {
    if (bridgeBtn_.contains(e.getPosition())) { if (onOpenBridge) onOpenBridge(); return; }
    if (onReset) onReset();
}

// ================================================================= MeterBridge

MeterBridge::MeterBridge(MeterHub& h) : hub_(h) {
    for (int i = 0; i < 4; ++i) {
        styleButton(tabs_[i], false);
        tabs_[i].setClickingTogglesState(true);
        tabs_[i].setRadioGroupId(9101);
        tabs_[i].onClick = [this, i] { setView(View(i)); };
        addAndMakeVisible(tabs_[i]);
    }
    tabs_[0].setToggleState(true, juce::dontSendNotification);
    standard_.addItem("Session target (the timeline's loudness target)", 1);
    const auto& st = dsp::loudnessStandards();
    for (size_t i = 0; i < st.size(); ++i)
        standard_.addItem(juce::String(st[i].name) + "  (" + juce::String(st[i].targetLufs, 0) + " LUFS, " + juce::String(st[i].maxTruePeak, 1) + " dBTP)",
                          int(i) + 2);
    standard_.setSelectedId(hub_.standard + 2, juce::dontSendNotification);
    standard_.onChange = [this] {
        hub_.standard = standard_.getSelectedId() - 2;
        if (onStandardChanged) onStandardChanged();
        repaint();
    };
    addAndMakeVisible(standard_);
    for (int m : {1, 2, 4, 10, 30, 60}) speed_.addItem("Radar: " + juce::String(m) + " min", m);
    speed_.setSelectedId(4, juce::dontSendNotification);
    speed_.onChange = [this] { radarMinutes = speed_.getSelectedId(); repaint(); };
    addAndMakeVisible(speed_);
    for (auto* b : {&reset_, &pause_, &refs_, &tiltBtn_, &fastBtn_}) {
        styleButton(*b, false);
        addAndMakeVisible(*b);
    }
    pause_.setClickingTogglesState(true);
    tiltBtn_.setClickingTogglesState(true);
    fastBtn_.setClickingTogglesState(true);
    fastBtn_.setToggleState(true, juce::dontSendNotification);
    reset_.setTooltip("Start every reading again (integrated, range, maxima, spectrum average)");
    pause_.setTooltip("Freeze the readings; playback carries on");
    refs_.setTooltip("Use the session's reference tracks as the tonal balance target (Tools > References to add them)");
    tiltBtn_.setTooltip("Show the spectrum with a 3 dB per octave tilt, so pink noise looks flat");
    fastBtn_.setTooltip("Also draw the live spectrum over the long-term average");
    reset_.onClick = [this] { if (onReset) onReset(); };
    pause_.onClick = [this] { hub_.paused = pause_.getToggleState(); };
    refs_.onClick = [this] { if (onRefreshReferences) onRefreshReferences(); };
    tiltBtn_.onClick = [this] { tilt = tiltBtn_.getToggleState(); repaint(); };
    fastBtn_.onClick = [this] { showFast = fastBtn_.getToggleState(); repaint(); };
    setSize(900, 640);
}

void MeterBridge::setView(View v) {
    view_ = v;
    for (int i = 0; i < 4; ++i) tabs_[i].setToggleState(i == int(v), juce::dontSendNotification);
    resized();
    repaint();
}

void MeterBridge::resized() {
    auto r = getLocalBounds().reduced(8);
    auto bar = r.removeFromTop(26);
    for (auto& t : tabs_) { t.setBounds(bar.removeFromLeft(86)); bar.removeFromLeft(3); }
    bar.removeFromLeft(10);
    reset_.setBounds(bar.removeFromRight(60));
    bar.removeFromRight(4);
    pause_.setBounds(bar.removeFromRight(60));
    bar.removeFromRight(8);
    const bool balance = view_ == View::Balance, radar = view_ == View::Radar;
    refs_.setVisible(balance);
    tiltBtn_.setVisible(balance);
    fastBtn_.setVisible(balance);
    speed_.setVisible(radar);
    if (balance) {
        fastBtn_.setBounds(bar.removeFromRight(50));
        bar.removeFromRight(4);
        tiltBtn_.setBounds(bar.removeFromRight(50));
        bar.removeFromRight(4);
        refs_.setBounds(bar.removeFromRight(100));
        bar.removeFromRight(8);
    }
    if (radar) {
        speed_.setBounds(bar.removeFromRight(130));
        bar.removeFromRight(8);
    }
    standard_.setBounds(bar);
    r.removeFromTop(8);
    content_ = r;
}

void MeterBridge::mouseDown(const juce::MouseEvent&) {}

void MeterBridge::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff0d0f12));
    switch (view_) {
        case View::Radar: paintRadar(g, content_); break;
        case View::Loudness: paintLoudness(g, content_); break;
        case View::Balance: paintBalance(g, content_); break;
        case View::Stereo: paintStereo(g, content_); break;
    }
    if (hub_.paused) {
        g.setColour(kYellow);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText("PAUSED", content_.removeFromTop(16), juce::Justification::centredRight);
    }
}

// ---------------------------------------------------------------- radar

void MeterBridge::paintRadar(juce::Graphics& g, juce::Rectangle<int> area) {
    const auto& L = hub_.loudness();
    const double target = hub_.targetLufs();
    auto side = area.withTrimmedRight(220);
    const float size = float(juce::jmin(side.getWidth(), side.getHeight())) - 40.0f;
    const auto c = side.getCentre().toFloat();
    const float rOuter = size * 0.5f, rRing = rOuter - 14.0f, rMax = rRing - 10.0f, rMin = rMax * 0.16f;
    // LU relative to the target: -36 at the centre hole, +18 at the edge.
    auto radiusOf = [&](double lu) { return rMin + (rMax - rMin) * float(juce::jlimit(0.0, 1.0, (lu + 36.0) / 54.0)); };
    const auto& hist = hub_.shortTermHistory();
    const int perTurn = juce::jmax(60, int(radarMinutes * 600.0));   // 10 points a second
    // Grid rings.
    g.setColour(juce::Colour(0xff26303a));
    for (double lu : {-30.0, -20.0, -10.0, 0.0, 10.0}) g.drawEllipse(juce::Rectangle<float>(radiusOf(lu) * 2, radiusOf(lu) * 2).withCentre(c), lu == 0 ? 1.6f : 0.8f);
    for (int k = 0; k < 12; ++k) {
        const float a = juce::MathConstants<float>::twoPi * k / 12.0f;
        g.drawLine(c.x + rMin * std::sin(a), c.y - rMin * std::cos(a), c.x + rMax * std::sin(a), c.y - rMax * std::cos(a), 0.6f);
    }
    // The sweep: the last turn of short-term history, newest at the hand.
    const int n = int(hist.size());
    const int shown = juce::jmin(n, perTurn);
    const float wedge = juce::MathConstants<float>::twoPi / float(perTurn);
    for (int i = n - shown; i < n; ++i) {
        const double s = hist[size_t(i)];
        if (!std::isfinite(s)) continue;
        const double lu = s - target;
        const float a0 = wedge * float(i % perTurn);
        const float rr = radiusOf(lu);
        // Fade older points slightly so the hand reads as the present.
        const float age = float(n - 1 - i) / float(perTurn);
        // Each wedge is built from the zones it passes through (blue, cyan, green, then yellow
        // above the target and red beyond +6 LU), so the rings read like the classic radar.
        static const double edges[] = {-36.0, -20.0, -10.0, 0.0, 6.0, 18.0};
        for (int z = 0; z < 5; ++z) {
            const double zLo = edges[z], zHi = juce::jmin(edges[z + 1], lu);
            if (zHi <= zLo) break;
            const float r0 = radiusOf(zLo), r1 = radiusOf(zHi);
            juce::Path p;
            p.addPieSegment(juce::Rectangle<float>(r1 * 2, r1 * 2).withCentre(c), a0, a0 + wedge * 1.4f, r0 / r1);
            g.setColour(levelColour(0.5 * (zLo + edges[z + 1])).withAlpha(0.85f - 0.4f * age));
            g.fillPath(p);
        }
        juce::ignoreUnused(rr);
    }
    if (n > 0) {
        const float a = wedge * float((n - 1) % perTurn);
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.drawLine(c.x, c.y, c.x + rMax * std::sin(a), c.y - rMax * std::cos(a), 1.4f);
    }
    // Centre hole.
    g.setColour(juce::Colour(0xff0d0f12));
    g.fillEllipse(juce::Rectangle<float>(rMin * 2, rMin * 2).withCentre(c));
    // Outer ring: momentary loudness, 0 LU at the top, 5 degrees per LU, -36 at the bottom.
    const double m = L.momentaryLufs() - target;
    auto angleOf = [](double lu) { return float(juce::jlimit(-36.0, 18.0, lu) * 5.0 * juce::MathConstants<double>::pi / 180.0); };
    for (int k = -36; k < 18; ++k) {
        const float a0 = angleOf(k) + 0.012f, a1 = angleOf(k + 1) - 0.012f;
        juce::Path seg;
        seg.addCentredArc(c.x, c.y, rRing, rRing, 0, a0, a1, true);
        const bool lit = std::isfinite(m) && k < m;
        g.setColour(lit ? levelColour(k + 0.5) : juce::Colour(0xff2a2f36));
        g.strokePath(seg, juce::PathStrokeType(7.0f));
    }
    g.setFont(juce::FontOptions(12.0f));
    g.setColour(colours::text);
    for (int k = -36; k <= 18; k += 6) {
        const float a = angleOf(k);
        const float rr = rRing + 18.0f;
        g.drawText((k > 0 ? "+" : "") + juce::String(k), juce::Rectangle<float>(40, 16).withCentre({c.x + rr * std::sin(a), c.y - rr * std::cos(a)}),
                   juce::Justification::centred);
    }
    paintBigNumbers(g, area.removeFromRight(210));
}

void MeterBridge::paintBigNumbers(juce::Graphics& g, juce::Rectangle<int> r) {
    const auto& L = hub_.loudness();
    const double target = hub_.targetLufs(), I = L.integratedLufs();
    auto block = [&](const juce::String& label, const juce::String& value, const juce::String& unit, juce::Colour c, float size = 40.0f) {
        auto b = r.removeFromTop(int(size) + 30);
        g.setColour(c);
        g.setFont(juce::FontOptions(size, juce::Font::plain));
        g.drawText(value, b.removeFromTop(int(size) + 6), juce::Justification::centredLeft);
        g.setColour(colours::dim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(label + (unit.isNotEmpty() ? "  (" + unit + ")" : juce::String()), b, juce::Justification::topLeft);
        r.removeFromTop(6);
    };
    const bool inside = std::isfinite(I) && std::abs(I - target) <= hub_.tolerance();
    block("Programme loudness, against target", std::isfinite(I) ? signedNum(I - target) : "--", "LU", inside ? kGreen : (std::isfinite(I) ? kYellow : colours::dim));
    block("Integrated", std::isfinite(I) ? juce::String(I, 1) : "--", "LUFS", colours::text, 28.0f);
    block("Loudness range", L.loudnessRange() > 0 ? juce::String(L.loudnessRange(), 1) : "--", "LRA, LU", kYellow, 28.0f);
    const double tp = L.truePeakDb();
    block("Max true peak", std::isfinite(tp) ? juce::String(tp, 1) : "--", "dBTP", std::isfinite(tp) && tp > hub_.maxTruePeak() ? kRed : colours::text, 28.0f);
    const int secs = int(L.durationSeconds());
    g.setColour(colours::dim);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(juce::String(secs / 3600).paddedLeft('0', 2) + ":" + juce::String((secs / 60) % 60).paddedLeft('0', 2) + ":" + juce::String(secs % 60).paddedLeft('0', 2) +
                   "   " + hub_.standardName(),
               r.removeFromTop(20), juce::Justification::left);
}

// ---------------------------------------------------------------- loudness

void MeterBridge::paintLoudness(juce::Graphics& g, juce::Rectangle<int> area) {
    const auto& L = hub_.loudness();
    const double target = hub_.targetLufs();
    // Bars: L, R (dBFS with true peak), then M, S, I on a loudness scale around the target.
    auto bars = area.removeFromLeft(300);
    auto scaleBox = bars.removeFromLeft(34);
    const auto top = float(bars.getY() + 10), bottom = float(bars.getBottom() - 30);
    auto yDb = [&](double db) { return bottom - (bottom - top) * float(juce::jlimit(0.0, 1.0, (db + 60.0) / 60.0)); };
    g.setFont(juce::FontOptions(11.0f));
    g.setColour(colours::dim);
    for (int db = 0; db >= -60; db -= 6) g.drawText(juce::String(db), juce::Rectangle<float>(30, 12).withCentre({float(scaleBox.getCentreX()), yDb(db)}), juce::Justification::centredRight);
    auto bar = [&](const juce::String& name, double value, double hold, double over, bool loudness) {
        auto b = bars.removeFromLeft(36).reduced(6, 0).toFloat();
        bars.removeFromLeft(6);
        auto col = b.withTop(top).withBottom(bottom);
        g.setColour(juce::Colour(0xff20252c));
        g.fillRect(col);
        // Loudness bars use the same height scale with the target at -18 on it (+18 LU headroom).
        auto y = [&](double v) { return loudness ? yDb(v - target - 18.0) : yDb(v); };
        if (std::isfinite(value)) {
            const float yv = y(value);
            juce::ColourGradient grad(loudness ? kCyan : kGreen, 0, bottom, kRed, 0, top, false);
            grad.addColour(loudness ? double(bottom - y(target)) / (bottom - top) : 0.9, loudness ? kGreen : kYellow);
            g.setGradientFill(grad);
            g.fillRect(col.withTop(yv));
        }
        if (std::isfinite(hold)) {
            g.setColour(std::isfinite(over) && hold > over ? kRed : colours::text);
            g.drawHorizontalLine(int(y(hold)), col.getX(), col.getRight());
        }
        if (loudness) {
            g.setColour(kGreen.withAlpha(0.6f));
            g.drawHorizontalLine(int(y(target)), col.getX() - 3, col.getRight() + 3);
        }
        g.setColour(colours::text);
        g.drawText(name, juce::Rectangle<float>(b.getX() - 6, bottom + 4, b.getWidth() + 12, 14), juce::Justification::centred);
        g.setColour(colours::dim);
        g.drawText(std::isfinite(value) && value > -150 ? juce::String(value, 1) : "--", juce::Rectangle<float>(b.getX() - 8, bottom + 16, b.getWidth() + 16, 12),
                   juce::Justification::centred);
    };
    bar("L", hub_.peakDb(0), L.truePeakDb(0), hub_.maxTruePeak(), false);
    bar("R", hub_.peakDb(1), L.truePeakDb(1), hub_.maxTruePeak(), false);
    bars.removeFromLeft(10);
    bar("M", L.momentaryLufs(), L.maxMomentaryLufs(), NAN, true);
    bar("S", L.shortTermLufs(), L.maxShortTermLufs(), NAN, true);
    bar("I", L.integratedLufs(), NAN, NAN, true);
    {   // LU scale for M, S, I: 0 = the target.
        auto lu = bars.removeFromLeft(34);
        g.setFont(juce::FontOptions(11.0f));
        for (int k = 18; k >= -42; k -= 6) {
            const float y = yDb(double(k) - 18.0);
            g.setColour(k == 0 ? kGreen : colours::dim);
            g.drawText((k > 0 ? "+" : "") + juce::String(k), juce::Rectangle<float>(float(lu.getX()), y - 6, 30, 12), juce::Justification::centredLeft);
        }
        g.setColour(colours::dim);
        g.drawText("LU", juce::Rectangle<float>(float(lu.getX()), bottom + 4, 30, 14), juce::Justification::centredLeft);
    }
    area.removeFromLeft(10);
    // History graph: the last few minutes of momentary (thin) and short-term (bold).
    auto hist = area.removeFromTop(area.getHeight() / 2).toFloat();
    g.setColour(juce::Colour(0xff151a20));
    g.fillRect(hist);
    auto yL = [&](double lufs) { return hist.getBottom() - hist.getHeight() * float(juce::jlimit(0.0, 1.0, (lufs - (target - 30)) / 40.0)); };
    g.setColour(kGreen.withAlpha(0.12f));
    g.fillRect(juce::Rectangle<float>(hist.getX(), yL(target + hub_.tolerance()), hist.getWidth(), yL(target - hub_.tolerance()) - yL(target + hub_.tolerance())));
    g.setFont(juce::FontOptions(10.5f));
    for (int k = -30; k <= 10; k += 10) {
        g.setColour(juce::Colour(0xff26303a));
        g.drawHorizontalLine(int(yL(target + k)), hist.getX(), hist.getRight());
        g.setColour(colours::dim);
        g.drawText(juce::String(target + k, 0), int(hist.getX()) + 2, int(yL(target + k)) - 12, 40, 12, juce::Justification::left);
    }
    const auto& hs = hub_.shortTermHistory();
    const auto& hm = hub_.momentaryHistory();
    const int span = 3000;   // 5 minutes
    auto curve = [&](const std::vector<float>& v, juce::Colour col, float w) {
        juce::Path p;
        bool started = false;
        const int n = int(v.size()), first = juce::jmax(0, n - span);
        for (int i = first; i < n; ++i) {
            if (!std::isfinite(v[size_t(i)]) || v[size_t(i)] < -80) { started = false; continue; }
            const float x = hist.getRight() - hist.getWidth() * float(n - 1 - i) / float(span);
            const float y = yL(v[size_t(i)]);
            if (!started) { p.startNewSubPath(x, y); started = true; } else p.lineTo(x, y);
        }
        g.setColour(col);
        g.strokePath(p, juce::PathStrokeType(w));
    };
    curve(hm, colours::wave.withAlpha(0.55f), 1.0f);
    curve(hs, kYellow, 1.8f);
    if (std::isfinite(L.integratedLufs())) {
        g.setColour(colours::text.withAlpha(0.8f));
        const float y = yL(L.integratedLufs());
        for (float x = hist.getX(); x < hist.getRight(); x += 8) g.drawLine(x, y, x + 4, y, 1.2f);
    }
    g.setColour(colours::dim);
    g.drawText("Last 5 minutes: momentary (blue), short-term (yellow), integrated (dashed); green band = target +/- tolerance",
               hist.toNearestInt().removeFromTop(14).reduced(4, 0), juce::Justification::right);
    area.removeFromTop(10);
    // Statistics and compliance.
    auto stats = area;
    auto col1 = stats.removeFromLeft(stats.getWidth() / 2);
    g.setFont(juce::FontOptions(13.0f));
    auto line = [&](juce::Rectangle<int>& col, const juce::String& k, const juce::String& v, juce::Colour c = colours::text) {
        auto x = col.removeFromTop(19);
        g.setColour(colours::dim);
        g.drawText(k, x.removeFromLeft(150), juce::Justification::left);
        g.setColour(c);
        g.drawText(v, x.withTrimmedRight(14), juce::Justification::right);
    };
    const double I = L.integratedLufs(), tp = L.truePeakDb();
    line(col1, "Integrated", num(I, 1, "LUFS"));
    line(col1, "Loudness range", num(L.loudnessRange(), 1, "LU"));
    line(col1, "Max momentary", num(L.maxMomentaryLufs(), 1, "LUFS"));
    line(col1, "Max short-term", num(L.maxShortTermLufs(), 1, "LUFS"));
    line(col1, "Max true peak L / R", num(L.truePeakDb(0), 1) + " / " + num(L.truePeakDb(1), 1) + " dBTP");
    line(col1, "Max sample peak L / R", num(hub_.maxSampleDb(0), 1) + " / " + num(hub_.maxSampleDb(1), 1) + " dBFS");
    line(col1, "PLR (peak to loudness)", num(hub_.plr(), 1, "dB"));
    line(col1, "PSR (short-term)", num(hub_.psr(), 1, "dB"));
    line(col1, "Overs (full scale)", juce::String(L.clippedSamples()), L.clippedSamples() ? kRed : colours::text);
    auto col2 = stats;
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(hub_.standardName(), col2.removeFromTop(20), juce::Justification::left);
    g.setFont(juce::FontOptions(13.0f));
    const bool iOk = std::isfinite(I) && std::abs(I - target) <= hub_.tolerance();
    const bool tpOk = !std::isfinite(tp) || tp <= hub_.maxTruePeak();
    line(col2, "Target", juce::String(target, 1) + " LUFS +/- " + juce::String(hub_.tolerance(), 1));
    line(col2, "Integrated", std::isfinite(I) ? signedNum(I - target) + " LU  " + (iOk ? "in range" : "out of range") : "--",
         std::isfinite(I) ? (iOk ? kGreen : kYellow) : colours::dim);
    line(col2, "True peak limit", juce::String(hub_.maxTruePeak(), 1) + " dBTP");
    line(col2, "True peak", std::isfinite(tp) ? juce::String(tp, 1) + " dBTP  " + (tpOk ? "ok" : "over") : "--", tpOk ? kGreen : kRed);
    line(col2, "Measured for", juce::String(L.durationSeconds(), 0) + " s of playback");
    col2.removeFromTop(8);
    g.setColour(colours::dim);
    g.setFont(juce::FontOptions(11.5f));
    g.drawFittedText("Readings cover what has played since Reset (they pause with playback). For a release, play the whole "
                     "programme from the top, or use Tools > Analyse Programme for an offline measurement.",
                     col2.removeFromTop(48), juce::Justification::topLeft, 3);
}

// ---------------------------------------------------------------- balance

void MeterBridge::paintBalance(juce::Graphics& g, juce::Rectangle<int> area) {
    const auto& sp = hub_.spectrum();
    const auto& b = sp.bands();
    if (b.size() == 0) return;
    auto graph = area.removeFromTop(int(area.getHeight() * 0.6f)).toFloat();
    g.setColour(juce::Colour(0xff151a20));
    g.fillRect(graph);
    auto xOf = [&](double hz) { return graph.getX() + graph.getWidth() * float(std::log(hz / 20.0) / std::log(1000.0)); };
    const double lo = -40.0, hi = 20.0;
    auto tiltDb = [&](double hz) { return tilt ? 3.0 * std::log2(hz / 1000.0) : 0.0; };
    auto yOf = [&](double db) { return graph.getBottom() - graph.getHeight() * float(juce::jlimit(0.0, 1.0, (db - lo) / (hi - lo))); };
    // Grid.
    g.setFont(juce::FontOptions(10.5f));
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        g.setColour(juce::Colour(0xff26303a));
        g.drawVerticalLine(int(xOf(f)), graph.getY(), graph.getBottom());
        g.setColour(colours::dim);
        g.drawText(f >= 1000 ? juce::String(f / 1000, 0) + "k" : juce::String(f, 0), int(xOf(f)) + 2, int(graph.getBottom()) - 13, 34, 12, juce::Justification::left);
    }
    for (double db = -40; db <= 20; db += 10) {
        g.setColour(juce::Colour(0xff26303a));
        g.drawHorizontalLine(int(yOf(db)), graph.getX(), graph.getRight());
        g.setColour(colours::dim);
        if (db > lo) g.drawText(juce::String(db, 0), int(graph.getX()) + 2, int(yOf(db)) - 12, 30, 12, juce::Justification::left);
    }
    // Display smoothing over neighbouring bands (the lowest bands hold only a few FFT bins).
    auto smooth = [&](const std::vector<double>& v) {
        std::vector<double> o(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            const int w = b.centre[i] < 120 ? 2 : 1;
            double sum = 0;
            int n = 0;
            for (int k = int(i) - w; k <= int(i) + w; ++k)
                if (k >= 0 && k < int(v.size())) { sum += v[size_t(k)]; ++n; }
            o[i] = sum / n;
        }
        return o;
    };
    // Reference zone.
    const auto& t = hub_.target();
    if (t.valid() && int(t.mean.size()) == b.size()) {
        const auto tm = smooth(t.mean), tw = smooth(t.halfWidth);
        juce::Path zone;
        for (int i = 0; i < b.size(); ++i) {
            const auto x = xOf(b.centre[size_t(i)]);
            const auto y = yOf(tm[size_t(i)] + tw[size_t(i)] + tiltDb(b.centre[size_t(i)]));
            if (i == 0) zone.startNewSubPath(x, y); else zone.lineTo(x, y);
        }
        for (int i = b.size(); --i >= 0;)
            zone.lineTo(xOf(b.centre[size_t(i)]), yOf(tm[size_t(i)] - tw[size_t(i)] + tiltDb(b.centre[size_t(i)])));
        zone.closeSubPath();
        g.setColour(kCyan.withAlpha(0.18f));
        g.fillPath(zone);
        g.setColour(kCyan.withAlpha(0.5f));
        g.strokePath(zone, juce::PathStrokeType(1.0f));
    }
    // Programme: the long-term average (bold) and the live spectrum (thin), both against the mids.
    auto drawCurve = [&](const std::vector<double>& power, juce::Colour c, float w) {
        auto prof = dsp::BalanceProfile::fromPower(b, power);
        if (!prof.valid()) return;
        const auto sh = smooth(prof.shareDb);
        juce::Path p;
        for (int i = 0; i < b.size(); ++i) {
            const auto x = xOf(b.centre[size_t(i)]), y = yOf(sh[size_t(i)] + tiltDb(b.centre[size_t(i)]));
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType(w));
    };
    if (showFast) {
        std::vector<double> fast(size_t(b.size()));
        for (int i = 0; i < b.size(); ++i) fast[size_t(i)] = std::pow(10.0, sp.fastDb(0)[size_t(i)] / 10.0);
        drawCurve(fast, colours::text.withAlpha(0.35f), 1.0f);
    }
    drawCurve(sp.averagePower(0), kGreen, 2.2f);
    // Mono check: what the mono sum loses per band (0 dB at the top of the graph's lower quarter).
    {
        const auto& ml = sp.monoLossDb();
        juce::Path p;
        const float y0 = graph.getY() + 12, h = graph.getHeight() * 0.25f;
        for (int i = 0; i < b.size(); ++i) {
            const auto x = xOf(b.centre[size_t(i)]), y = y0 + h * float(juce::jlimit(0.0, 1.0, -ml[size_t(i)] / 20.0));
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(kMono.withAlpha(0.8f));
        g.strokePath(p, juce::PathStrokeType(1.2f));
        g.drawText("mono check (lower = lost when summed to mono)", int(graph.getRight()) - 280, int(y0) - 12, 276, 12, juce::Justification::right);
    }
    g.setColour(colours::dim);
    g.drawText(t.valid() ? "Green: this programme (average since Reset). Blue zone: " + juce::String(t.references) + " reference track(s). Levels against the mids (250 Hz-4 kHz)."
                         : "Green: this programme (average since Reset), against its mids (250 Hz-4 kHz). Add reference tracks (References...) for a target zone.",
               graph.toNearestInt().removeFromTop(14).reduced(40, 0), juce::Justification::left);
    area.removeFromTop(10);
    // Ranges.
    auto ranges = hub_.ranges();
    auto rr = area.removeFromTop(86);
    const int w = rr.getWidth() / juce::jmax(1, int(ranges.size()));
    for (auto& rg : ranges) {
        auto cell = rr.removeFromLeft(w).reduced(4, 0);
        g.setColour(juce::Colour(0xff151a20));
        g.fillRoundedRectangle(cell.toFloat(), 4);
        auto in = cell.reduced(6, 4);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText(rg.name, in.removeFromTop(16), juce::Justification::left);
        g.setColour(colours::dim);
        g.setFont(juce::FontOptions(10.5f));
        auto fmt = [](double f) { return f >= 1000 ? juce::String(f / 1000, f >= 10000 ? 0 : 1) + "k" : juce::String(f, 0); };
        g.drawText(fmt(rg.fLo) + "-" + fmt(rg.fHi) + " Hz", in.removeFromTop(13), juce::Justification::left);
        if (t.valid()) {
            const double out = rg.outsideDb();
            g.setFont(juce::FontOptions(20.0f));
            g.setColour(std::abs(out) < 0.05 ? kGreen : (std::abs(out) < 2.0 ? kYellow : kRed));
            g.drawText(signedNum(rg.diffDb()) + " dB", in.removeFromTop(26), juce::Justification::left);
            // Indicator: the zone and where the programme sits in it (+/- 6 dB across).
            auto ind = in.removeFromTop(10).toFloat();
            g.setColour(juce::Colour(0xff26303a));
            g.fillRect(ind);
            auto xd = [&](double d) { return ind.getCentreX() + ind.getWidth() * 0.5f * float(juce::jlimit(-1.0, 1.0, d / 6.0)); };
            g.setColour(kCyan.withAlpha(0.4f));
            g.fillRect(juce::Rectangle<float>(xd(-rg.halfWidthDb), ind.getY(), xd(rg.halfWidthDb) - xd(-rg.halfWidthDb), ind.getHeight()));
            g.setColour(colours::text);
            g.fillRect(juce::Rectangle<float>(xd(rg.diffDb()) - 1.5f, ind.getY() - 2, 3, ind.getHeight() + 4));
        } else {
            g.setFont(juce::FontOptions(18.0f));
            g.setColour(colours::text);
            g.drawText(signedNum(rg.programmeDb) + " dB", in.removeFromTop(26), juce::Justification::left);
        }
    }
    area.removeFromTop(8);
    // Notes.
    g.setFont(juce::FontOptions(13.0f));
    if (!t.valid()) {
        g.setColour(colours::dim);
        g.drawFittedText("Range levels are shown against the mids. With reference tracks loaded (Tools > References, then References... here), "
                         "each range shows how far it sits from your references and notes appear here.",
                         area.removeFromTop(40), juce::Justification::topLeft, 2);
    } else {
        auto notes = dsp::balanceNotes(ranges);
        g.setColour(notes.empty() ? kGreen : colours::text);
        if (notes.empty()) g.drawText("Every range is inside the reference zone.", area.removeFromTop(18), juce::Justification::left);
        for (auto& n : notes) g.drawText(juce::String(n), area.removeFromTop(18), juce::Justification::left);
        g.setColour(colours::dim);
        g.setFont(juce::FontOptions(11.5f));
        g.drawText("References: " + hub_.referenceNames().joinIntoString(", "), area.removeFromBottom(16), juce::Justification::left, true);
    }
}

// ---------------------------------------------------------------- stereo

void MeterBridge::paintStereo(juce::Graphics& g, juce::Rectangle<int> area) {
    const auto& st = hub_.stereo();
    // Goniometer: M up, S across (L at top-left, R at top-right).
    auto gon = area.removeFromLeft(juce::jmin(area.getHeight(), area.getWidth() / 2)).reduced(6).toFloat();
    g.setColour(juce::Colour(0xff151a20));
    g.fillRect(gon);
    const auto c = gon.getCentre();
    const float rad = gon.getWidth() * 0.46f;
    g.setColour(juce::Colour(0xff26303a));
    g.drawLine(c.x, gon.getY(), c.x, gon.getBottom(), 0.8f);
    g.drawLine(gon.getX(), c.y, gon.getRight(), c.y, 0.8f);
    g.drawLine(c.x - rad, c.y - rad, c.x + rad, c.y + rad, 0.6f);
    g.drawLine(c.x + rad, c.y - rad, c.x - rad, c.y + rad, 0.6f);
    g.setColour(colours::dim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("M", juce::Rectangle<float>(20, 14).withCentre({c.x, gon.getY() + 9}), juce::Justification::centred);
    g.drawText("L", juce::Rectangle<float>(20, 14).withCentre({c.x - rad * 0.72f, c.y - rad * 0.72f}), juce::Justification::centred);
    g.drawText("R", juce::Rectangle<float>(20, 14).withCentre({c.x + rad * 0.72f, c.y - rad * 0.72f}), juce::Justification::centred);
    std::vector<float> l, r;
    st.recentPairs(l, r, 2048);
    // Auto-gain so quiet material still fills the scope (shown as a level readout).
    float peak = 1e-4f;
    for (size_t i = 0; i < l.size(); ++i) peak = juce::jmax(peak, std::abs(l[i] + r[i]) * 0.7071f, std::abs(l[i] - r[i]) * 0.7071f);
    const float gain = juce::jmin(8.0f, 0.9f / peak);
    g.setColour(kGreen.withAlpha(0.55f));
    for (size_t i = 0; i < l.size(); ++i) {
        const float m = (l[i] + r[i]) * 0.7071f * gain, s = (r[i] - l[i]) * 0.7071f * gain;
        g.fillRect(c.x + s * rad - 0.75f, c.y - m * rad - 0.75f, 1.5f, 1.5f);
    }
    g.setColour(colours::dim);
    g.drawText("scope gain +" + juce::String(juce::roundToInt(20 * std::log10(gain))) + " dB", gon.toNearestInt().removeFromBottom(16).reduced(6, 0), juce::Justification::right);
    area.removeFromLeft(14);
    // Correlation, width, balance meters.
    auto hbar = [&](const juce::String& name, double v, double lo, double hi, double mark, juce::Colour col, const juce::String& text) {
        auto row = area.removeFromTop(46);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.0f));
        auto head = row.removeFromTop(18);
        g.drawText(name, head, juce::Justification::left);
        g.drawText(text, head, juce::Justification::right);
        auto b = row.removeFromTop(16).toFloat();
        g.setColour(juce::Colour(0xff20252c));
        g.fillRect(b);
        auto x = [&](double u) { return b.getX() + b.getWidth() * float(juce::jlimit(0.0, 1.0, (u - lo) / (hi - lo))); };
        const float x0 = x(juce::jlimit(lo, hi, 0.0)), x1 = x(v);
        g.setColour(col);
        g.fillRect(juce::Rectangle<float>(juce::jmin(x0, x1), b.getY(), juce::jmax(2.0f, std::abs(x1 - x0)), b.getHeight()));
        if (std::isfinite(mark)) {
            g.setColour(colours::text);
            g.drawVerticalLine(int(x(mark)), b.getY() - 2, b.getBottom() + 2);
        }
        area.removeFromTop(8);
    };
    const double corr = st.correlation();
    hbar("Correlation", corr, -1, 1, st.minCorrelation(), corr < 0 ? kRed : corr < 0.3 ? kYellow : kGreen,
         juce::String(corr, 2) + "   (lowest " + juce::String(st.minCorrelation(), 2) + ")");
    const double w = st.width();
    const double wDb = 10 * std::log10(juce::jmax(w, 1e-6));
    hbar("Width (side against mid, -30 to +6 dB)", juce::jlimit(-30.0, 6.0, wDb), -30, 6, 0.0, wDb > 0 ? kYellow : kCyan,
         w < 0.001 ? juce::String("mono") : juce::String(wDb, 1) + " dB S/M");
    const double bal = st.balanceDb();
    hbar("Balance (left against right)", juce::jlimit(-6.0, 6.0, bal), -6, 6, 0.0, std::abs(bal) > 1.0 ? kYellow : kGreen, signedNum(bal, 1) + " dB");
    // Correlation history.
    {
        auto hist = area.removeFromTop(70).toFloat();
        g.setColour(juce::Colour(0xff151a20));
        g.fillRect(hist);
        g.setColour(juce::Colour(0xff26303a));
        g.drawHorizontalLine(int(hist.getCentreY()), hist.getX(), hist.getRight());
        const auto& hc = hub_.correlationHistory();
        const int span = 600, n = int(hc.size());
        juce::Path p;
        for (int i = juce::jmax(0, n - span); i < n; ++i) {
            const float x = hist.getRight() - hist.getWidth() * float(n - 1 - i) / float(span);
            const float y = hist.getCentreY() - hist.getHeight() * 0.48f * hc[size_t(i)];
            if (i == juce::jmax(0, n - span)) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(kGreen);
        g.strokePath(p, juce::PathStrokeType(1.4f));
        g.setColour(colours::dim);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText("correlation, last minute (+1 top, -1 bottom)", hist.toNearestInt().reduced(4, 2), juce::Justification::topLeft);
        area.removeFromTop(10);
    }
    // Correlation per band.
    {
        const auto& b = hub_.spectrum().bands();
        const auto& bc = hub_.spectrum().bandCorrelation();
        auto chart = area.toFloat();
        g.setColour(juce::Colour(0xff151a20));
        g.fillRect(chart);
        if (b.size() > 0) {
            const float bw = chart.getWidth() / float(b.size());
            const float mid = chart.getCentreY();
            for (int i = 0; i < b.size(); ++i) {
                const float v = float(bc[size_t(i)]);
                const float h = chart.getHeight() * 0.46f * v;
                g.setColour(v < 0 ? kRed : v < 0.3f ? kYellow : kGreen.withAlpha(0.75f));
                g.fillRect(juce::Rectangle<float>(chart.getX() + bw * i + 0.5f, h > 0 ? mid - h : mid, juce::jmax(1.0f, bw - 1.0f), std::abs(h)));
            }
            g.setColour(colours::dim);
            g.setFont(juce::FontOptions(10.5f));
            for (double f : {50.0, 200.0, 1000.0, 5000.0, 15000.0}) {
                int k = 0;
                while (k + 1 < b.size() && b.centre[size_t(k)] < f) ++k;
                g.drawText(f >= 1000 ? juce::String(f / 1000, 0) + "k" : juce::String(f, 0), int(chart.getX() + bw * k), int(chart.getBottom()) - 13, 40, 12, juce::Justification::left);
            }
            g.drawText("correlation per band (red = out of phase, cancels in mono)", chart.toNearestInt().reduced(4, 2), juce::Justification::topLeft);
        }
    }
}

}  // namespace lathe
