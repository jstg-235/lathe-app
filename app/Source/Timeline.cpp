#include "Timeline.h"

#include "Products.h"

namespace lathe {

juce::AudioThumbnail* ThumbPool::get(const juce::File& f, juce::ChangeListener* l) {
    auto key = f.getFullPathName();
    auto it = thumbs_.find(key);
    if (it == thumbs_.end()) {
        auto t = std::make_unique<juce::AudioThumbnail>(256, fm_, cache_);
        t->setSource(new juce::FileInputSource(f));
        it = thumbs_.emplace(key, std::move(t)).first;
    }
    if (l) it->second->addChangeListener(l);
    return it->second.get();
}

Timeline::Timeline(Session& s, ThumbPool& pool) : s_(s), pool_(pool) {
    addAndMakeVisible(hscroll_);
    hscroll_.addListener(this);
    hscroll_.setAutoHide(false);
    setWantsKeyboardFocus(false);
}

Timeline::~Timeline() { hscroll_.removeListener(this); }

int Timeline::laneHeight() const {
    int avail = getHeight() - top() - kScroll;
    return juce::jmax(40, avail / juce::jmax(1, s_.lanes));
}

juce::Rectangle<int> Timeline::laneBounds(int lane) const {
    return {0, top() + lane * laneHeight(), getWidth(), laneHeight()};
}

int Timeline::laneAt(int y) const {
    return juce::jlimit(0, s_.lanes - 1, (y - top()) / laneHeight());
}

int Timeline::regionAt(juce::Point<int> p) const {
    if (p.y < top()) return 0;
    int lane = laneAt(p.y);
    double t = xToTime(float(p.x));
    // Topmost = latest in the list (drawn last).
    for (int i = s_.regions.size(); --i >= 0;) {
        auto& r = s_.regions.getReference(i);
        if (r.lane == lane && t >= r.start && t < r.end()) return r.id;
    }
    return 0;
}

int Timeline::markerAt(juce::Point<int> p) const {
    if (p.y < kRuler || p.y >= kRuler + kMarkerStrip) return 0;
    int best = 0;
    float bestD = 7.0f;
    for (auto& m : s_.markers) {
        float d = std::abs(timeToX(m.t) - float(p.x));
        if (d < bestD) { bestD = d; best = m.id; }
    }
    return best;
}

float Timeline::envY(const Region& r, double db) const {
    auto b = laneBounds(r.lane).reduced(0, 4);
    // 0 dB sits a quarter down; +12 at top, -36 at bottom.
    double norm = juce::jlimit(0.0, 1.0, (12.0 - db) / 48.0);
    return float(b.getY() + norm * b.getHeight());
}

int Timeline::envPointAt(const Region& r, juce::Point<int> p) const {
    for (int i = 0; i < r.envelope.size(); ++i) {
        auto& g = r.envelope.getReference(i);
        if (std::abs(timeToX(r.start + g.t) - float(p.x)) < 6 && std::abs(envY(r, g.db) - float(p.y)) < 6) return i;
    }
    return -1;
}

double Timeline::snapTime(double t, int ignore) const {
    if (!snap_) return t;
    double best = t, bestPx = 8.0;
    auto consider = [&](double c) {
        double px = std::abs((c - t) * pps_);
        if (px < bestPx) { bestPx = px; best = c; }
    };
    consider(playhead_);
    consider(0.0);
    for (auto& r : s_.regions)
        if (r.id != ignore) { consider(r.start); consider(r.end()); }
    for (auto& m : s_.markers) consider(m.t);
    return best;
}

void Timeline::setPlayhead(double sec, bool follow) {
    if (std::abs(sec - playhead_) * pps_ < 0.5) return;
    playhead_ = sec;
    if (follow && drag_ == Drag::None) {
        double w = getWidth() / pps_;
        if (sec > view0_ + w * 0.95 || sec < view0_) { view0_ = juce::jmax(0.0, sec - w * 0.1); updateScrollbar(); }
    }
    repaint();
}

void Timeline::setAnalysis(std::shared_ptr<const dsp::AnalysisResult> processed, std::shared_ptr<const dsp::AnalysisResult> raw) {
    an_ = std::move(processed);
    raw_ = std::move(raw);
    stale_ = false;
    repaint();
}

int Timeline::problemAt(juce::Point<int> p) const {
    if (!an_ || p.y >= kRuler) return -1;
    for (size_t i = 0; i < an_->problems.size(); ++i)
        if (std::abs(timeToX(an_->problems[i].start) - float(p.x)) < 6) return int(i);
    return -1;
}

void Timeline::paintLoudness(juce::Graphics& g) {
    if (!an_ || !showLoudness_) return;
    auto area = juce::Rectangle<float>(0, float(top()), float(getWidth()), float(getHeight() - top() - kScroll));
    const float lo = -36.0f, hi = 0.0f;
    auto yOf = [&](float lufs) { return area.getBottom() - (juce::jlimit(lo, hi, lufs) - lo) / (hi - lo) * area.getHeight(); };
    const float alpha = stale_ ? 0.35f : 1.0f;

    // Target line.
    float ty = yOf(float(s_.loudnessTarget));
    g.setColour(colours::ok.withAlpha(0.45f * alpha));
    g.drawHorizontalLine(int(ty), 0, float(getWidth()));
    g.setFont(10.0f);
    g.drawText(juce::String(s_.loudnessTarget, 0) + " LUFS target", getWidth() - 120, int(ty) - 13, 116, 12, juce::Justification::right);

    auto curve = [&](const dsp::LoudnessMap& m, juce::Colour c, float width, bool dashed) {
        if (m.shortTerm.empty()) return;
        juce::Path p;
        bool started = false;
        // Sample the map per pixel column so long programmes stay cheap to draw.
        for (int x = 0; x < getWidth(); x += 2) {
            double t = xToTime(float(x));
            if (t < 0 || t > double(m.shortTerm.size()) * m.hopSec) { started = false; continue; }
            float v = float(m.valueAt(m.shortTerm, t));
            if (v < lo - 10) { started = false; continue; }
            float y = yOf(v);
            if (!started) { p.startNewSubPath(float(x), y); started = true; }
            else p.lineTo(float(x), y);
        }
        g.setColour(c);
        if (dashed) {
            juce::Path d;
            const float dashes[] = {4.0f, 4.0f};
            juce::PathStrokeType(width).createDashedStroke(d, p, dashes, 2);
            g.fillPath(d);
        } else {
            g.strokePath(p, juce::PathStrokeType(width));
        }
    };
    if (raw_) curve(raw_->map, colours::text.withAlpha(0.35f * alpha), 1.0f, true);
    curve(an_->map, colours::envelope.withAlpha(0.9f * alpha), 1.6f, false);

    // Per-track integrated loudness and true peak.
    g.setFont(11.0f);
    for (size_t i = 0; i < an_->tracks.size(); ++i) {
        auto& t = an_->tracks[i];
        float x = timeToX(t.start);
        if (x > getWidth() || timeToX(t.end) < 0) continue;
        juce::String txt = std::isfinite(t.lufs) ? juce::String(t.lufs, 1) + " LUFS" : juce::String("--");
        if (std::isfinite(t.truePeakDb)) txt << "  " << juce::String(t.truePeakDb, 1) << " dBTP";
        g.setColour(colours::bg.withAlpha(0.7f));
        g.fillRoundedRectangle(juce::Rectangle<float>(juce::jmax(2.0f, x + 4), area.getY() + 3, 150, 15), 3.0f);
        g.setColour((t.truePeakDb > -1.0 ? colours::error : colours::envelope).withAlpha(alpha));
        g.drawText(txt, juce::Rectangle<int>(int(juce::jmax(2.0f, x + 8)), int(area.getY() + 3), 146, 15), juce::Justification::left);
    }
    if (stale_) {
        g.setColour(colours::accent);
        g.drawText("Loudness overlay is out of date - Analyse again", int(area.getX()) + 8, int(area.getBottom()) - 18, 320, 14,
                   juce::Justification::left);
    }
}

void Timeline::zoomToFit() {
    double len = juce::jmax(10.0, s_.end() + 2.0);
    pps_ = juce::jmax(0.5, double(getWidth()) / len);
    view0_ = 0;
    updateScrollbar();
    repaint();
}

void Timeline::zoomBy(double f, double around) {
    double x = (around - view0_) * pps_;
    pps_ = juce::jlimit(0.2, 20000.0, pps_ * f);
    view0_ = juce::jmax(0.0, around - x / pps_);
    updateScrollbar();
    repaint();
}

void Timeline::sessionUpdated() {
    if (s_.regions.isEmpty()) selRegion_ = 0;
    else if (!s_.findRegion(selRegion_)) selRegion_ = 0;
    updateScrollbar();
    repaint();
}

void Timeline::updateScrollbar() {
    double total = juce::jmax(s_.end() + 30.0, view0_ + getWidth() / pps_);
    hscroll_.setRangeLimits(0.0, total, juce::dontSendNotification);
    hscroll_.setCurrentRange(view0_, getWidth() / pps_, juce::dontSendNotification);
}

void Timeline::scrollBarMoved(juce::ScrollBar*, double start) {
    view0_ = start;
    repaint();
}

void Timeline::resized() {
    hscroll_.setBounds(getLocalBounds().removeFromBottom(kScroll));
    updateScrollbar();
}

std::pair<int, int> Timeline::crossfadeNear(double t) const {
    std::pair<int, int> best{0, 0};
    double bestD = 1e9;
    for (auto& a : s_.regions)
        for (auto& b : s_.regions) {
            if (a.id == b.id || a.lane != b.lane || !(b.start > a.start && b.start < a.end())) continue;
            double mid = 0.5 * (b.start + juce::jmin(a.end(), b.end()));
            if (std::abs(mid - t) < bestD) { bestD = std::abs(mid - t); best = {a.id, b.id}; }
        }
    return best;
}

// ---------------------------------------------------------------- painting

void Timeline::paintRuler(juce::Graphics& g) {
    auto r = getLocalBounds().removeFromTop(kRuler);
    g.setColour(colours::bg);
    g.fillRect(r);
    // Pick a tick spacing that keeps labels ~90 px apart.
    const double steps[] = {1.0 / 75, 0.1, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
    double step = 600;
    for (double st : steps)
        if (st * pps_ >= 90) { step = st; break; }
    g.setFont(11.0f);
    double t0 = std::floor(view0_ / step) * step;
    for (double t = t0; timeToX(t) < getWidth(); t += step) {
        float x = timeToX(t);
        g.setColour(colours::line);
        g.drawVerticalLine(int(x), float(r.getBottom() - 6), float(r.getBottom()));
        g.setColour(colours::dim);
        g.drawText(msfString(t), int(x) + 3, 2, 80, kRuler - 6, juce::Justification::left);
    }
}

void Timeline::paintMarkers(juce::Graphics& g) {
    auto strip = juce::Rectangle<int>(0, kRuler, getWidth(), kMarkerStrip);
    g.setColour(colours::panel2);
    g.fillRect(strip);
    int track = 0;
    g.setFont(11.0f);
    for (auto& m : s_.markers) {
        float x = timeToX(m.t);
        if (m.type == Marker::TrackStart) ++track;
        if (x < -40 || x > getWidth() + 40) continue;
        juce::Colour c = m.type == Marker::TrackStart ? colours::marker
                       : m.type == Marker::Index      ? colours::index
                       : m.type == Marker::PauseStart ? colours::pause.brighter(0.4f)
                                                      : colours::end;
        bool sel = m.id == selMarker_;
        juce::Path flag;
        float top = float(strip.getY() + 2), bot = float(strip.getBottom());
        flag.addTriangle(x, top, x, top + 10, x + 8, top + 5);
        g.setColour(sel ? c.brighter(0.5f) : c);
        g.fillPath(flag);
        g.drawLine(x, top, x, bot, sel ? 2.0f : 1.0f);
        juce::String label = m.type == Marker::TrackStart ? juce::String(track)
                           : m.type == Marker::Index      ? juce::String("idx")
                           : m.type == Marker::PauseStart ? juce::String("pause")
                                                          : juce::String("END");
        if (m.type == Marker::TrackStart && !m.text.title.empty())
            label << " " << juce::String::fromUTF8(m.text.title.c_str());
        g.drawText(label, int(x) + 10, strip.getY() + 1, 160, 13, juce::Justification::left);
        // Thin guide through the lanes.
        g.setColour(c.withAlpha(m.type == Marker::TrackStart ? 0.5f : 0.25f));
        g.drawVerticalLine(int(x), bot, float(getHeight() - kScroll));
    }
}

void Timeline::paintRegion(juce::Graphics& g, int i) {
    const Region& r = s_.regions.getReference(i);
    float x0 = timeToX(r.start), x1 = timeToX(r.end());
    if (x1 < 0 || x0 > getWidth()) return;
    auto lane = laneBounds(r.lane).reduced(0, 3);
    auto box = juce::Rectangle<float>(x0, float(lane.getY()), x1 - x0, float(lane.getHeight()));
    bool sel = r.id == selRegion_;
    g.setColour((sel ? colours::panel2.brighter(0.15f) : colours::panel2).withAlpha(r.mute ? 0.4f : 0.95f));
    g.fillRoundedRectangle(box, 3.0f);

    // Waveform, clipped to the visible part.
    auto vis = box.getIntersection(getLocalBounds().toFloat());
    if (auto* th = pool_.get(r.file, this)) {
        double tA = xToTime(vis.getX()), tB = xToTime(vis.getRight());
        double s0 = double(r.srcIn) / r.fileRate + (tA - r.start), s1 = double(r.srcIn) / r.fileRate + (tB - r.start);
        g.setColour((sel ? colours::waveSel : colours::wave).withAlpha(r.mute ? 0.3f : 0.85f));
        auto wb = vis.reduced(0, 14).withTrimmedBottom(-10).toNearestInt();
        if (wb.getWidth() > 0 && s1 > s0)
            th->drawChannels(g, wb, s0, s1, float(juce::Decibels::decibelsToGain(r.gainDb)));
    }

    // Fades (including automatic crossfades).
    auto fades = s_.fadesFor(i);
    g.setColour(colours::text.withAlpha(0.6f));
    auto drawFade = [&](double a, double len, FadeShape shape, bool in) {
        if (len <= 0) return;
        juce::Path p;
        for (int k = 0; k <= 24; ++k) {
            double f = double(k) / 24;
            double gain = fadeGain(shape, in ? f : 1.0 - f);
            float x = timeToX(a + f * len);
            float y = box.getBottom() - float(gain) * box.getHeight();
            if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.strokePath(p, juce::PathStrokeType(1.2f));
    };
    drawFade(r.start, fades.in, fades.inShape, true);
    drawFade(r.end() - fades.out, fades.out, fades.outShape, false);
    // Fade handles.
    g.setColour(colours::text);
    g.fillRect(juce::Rectangle<float>(timeToX(r.start + r.fadeIn) - 3, box.getY(), 6, 6));
    g.fillRect(juce::Rectangle<float>(timeToX(r.end() - r.fadeOut) - 3, box.getY(), 6, 6));

    // Gain envelope.
    if (envelopeMode_ || !r.envelope.isEmpty()) {
        g.setColour(colours::envelope.withAlpha(envelopeMode_ ? 1.0f : 0.5f));
        juce::Path p;
        p.startNewSubPath(x0, envY(r, r.envelopeDb(0)));
        for (auto& pt : r.envelope) p.lineTo(timeToX(r.start + pt.t), envY(r, pt.db));
        p.lineTo(x1, envY(r, r.envelopeDb(r.length())));
        g.strokePath(p, juce::PathStrokeType(1.5f));
        if (envelopeMode_)
            for (auto& pt : r.envelope) g.fillEllipse(timeToX(r.start + pt.t) - 3.5f, envY(r, pt.db) - 3.5f, 7, 7);
    }

    g.setColour(sel ? colours::accent : colours::line.brighter(0.3f));
    g.drawRoundedRectangle(box, 3.0f, sel ? 2.0f : 1.0f);
    g.setColour(colours::text);
    g.setFont(12.0f);
    juce::String label = r.name;
    if (std::abs(r.gainDb) > 0.05) label << "  " << (r.gainDb > 0 ? "+" : "") << juce::String(r.gainDb, 1) << " dB";
    if (r.mute) label << "  (muted)";
    if (r.fxState.isNotEmpty()) {
        // FX badge: this region has its own effects chain.
        auto badge = juce::Rectangle<float>(box.getRight() - 30, box.getY() + 3, 26, 14);
        if (badge.getX() > box.getX() + 40) {
            g.setColour(colours::accent);
            g.fillRoundedRectangle(badge, 3.0f);
            g.setColour(colours::bg);
            g.setFont(10.0f);
            g.drawText("FX", badge.toNearestInt(), juce::Justification::centred);
        }
    }
    g.drawText(label, box.reduced(6, 2).removeFromTop(14).toNearestInt(), juce::Justification::left);
}

void Timeline::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    for (int l = 0; l < s_.lanes; ++l) {
        auto b = laneBounds(l);
        g.setColour(l % 2 ? colours::panel : colours::panel.brighter(0.02f));
        g.fillRect(b);
        if (l == selLane_) { g.setColour(colours::accent.withAlpha(0.06f)); g.fillRect(b); }
        g.setColour(colours::line);
        g.drawHorizontalLine(b.getBottom() - 1, 0, float(getWidth()));
        g.setColour(colours::dim);
        g.setFont(10.0f);
        g.drawText(juce::String(l + 1), b.getX() + 3, b.getBottom() - 14, 20, 12, juce::Justification::left);
    }
    for (int i = 0; i < s_.regions.size(); ++i) paintRegion(g, i);

    // Crossfade marks on overlaps.
    for (auto& a : s_.regions)
        for (auto& b : s_.regions)
            if (a.id != b.id && a.lane == b.lane && b.start > a.start && b.start < a.end()) {
                auto lb = laneBounds(a.lane).reduced(0, 3).toFloat();
                float xa = timeToX(b.start), xb = timeToX(juce::jmin(a.end(), b.end()));
                g.setColour(colours::accent.withAlpha(0.18f));
                g.fillRect(juce::Rectangle<float>(xa, lb.getY(), xb - xa, lb.getHeight()));
                g.setColour(colours::accent.withAlpha(0.8f));
                g.drawLine(xa, lb.getY(), xb, lb.getBottom(), 1.0f);
                g.drawLine(xa, lb.getBottom(), xb, lb.getY(), 1.0f);
            }

    if (!sel_.isEmpty()) {
        float a = timeToX(sel_.getStart()), b = timeToX(sel_.getEnd());
        g.setColour(colours::selection);
        g.fillRect(juce::Rectangle<float>(a, float(kRuler), b - a, float(getHeight() - kRuler - kScroll)));
        g.setColour(colours::wave);
        g.drawVerticalLine(int(a), float(kRuler), float(getHeight() - kScroll));
        g.drawVerticalLine(int(b), float(kRuler), float(getHeight() - kScroll));
    }
    paintLoudness(g);
    paintRuler(g);
    paintMarkers(g);
    paintProducts(g);
    // Problem ticks on the ruler (red = serious, amber = warning); click to jump.
    if (an_ && showLoudness_)
        for (auto& p : an_->problems) {
            float x = timeToX(p.start);
            if (x < -6 || x > getWidth() + 6) continue;
            juce::Path tri;
            tri.addTriangle(x - 5, 2, x + 5, 2, x, 11);
            g.setColour((p.severe ? colours::error : colours::accent).withAlpha(stale_ ? 0.4f : 1.0f));
            g.fillPath(tri);
        }

    g.setColour(colours::text);
    float px = timeToX(playhead_);
    g.drawLine(px, 0, px, float(getHeight() - kScroll), 1.5f);

    if (s_.regions.isEmpty()) {
        g.setColour(colours::dim);
        g.drawText("Drag audio files here, open them in the Source window, or use Import",
                   getLocalBounds().withTrimmedTop(top()), juce::Justification::centred);
    }
}

// ---------------------------------------------------------------- mouse

void Timeline::mouseMove(const juce::MouseEvent& e) {
    auto cursor = juce::MouseCursor::NormalCursor;
    if (markerAt(e.getPosition())) cursor = juce::MouseCursor::LeftRightResizeCursor;
    else if (int id = regionAt(e.getPosition())) {
        auto* r = s_.findRegion(id);
        float xa = timeToX(r->start), xb = timeToX(r->end());
        auto lb = laneBounds(r->lane);
        if (envelopeMode_) cursor = juce::MouseCursor::CrosshairCursor;
        else if (e.y < lb.getY() + 10 && (std::abs(e.x - timeToX(r->start + r->fadeIn)) < 6 || std::abs(e.x - timeToX(r->end() - r->fadeOut)) < 6))
            cursor = juce::MouseCursor::PointingHandCursor;
        else if (std::abs(e.x - xa) < 6 || std::abs(e.x - xb) < 6) cursor = juce::MouseCursor::LeftRightResizeCursor;
        else cursor = juce::MouseCursor::DraggingHandCursor;
    }
    setMouseCursor(cursor);
}

void Timeline::mouseDown(const juce::MouseEvent& e) {
    drag_ = Drag::None;
    changed_ = false;
    dragT0_ = xToTime(float(e.x));
    auto p = e.getPosition();

    if (int pi = problemAt(p); pi >= 0) {
        if (onProblemClicked) onProblemClicked(an_->problems[size_t(pi)].start);
        return;
    }
    if (e.y < kRuler) {
        drag_ = Drag::Locate;
        if (onSeek) onSeek(juce::jmax(0.0, dragT0_));
        return;
    }
    if (int m = markerAt(p)) {
        selMarker_ = m;
        if (e.mods.isPopupMenu()) {
            juce::PopupMenu menu;
            menu.addItem(1, "Delete marker");
            menu.showMenuAsync({}, [this, m](int r) {
                if (r == 1) { s_.removeMarker(m); if (onEdit) onEdit(); }
            });
            return;
        }
        drag_ = Drag::Marker;
        dragId_ = m;
        if (onMarkerSelected) onMarkerSelected(m);
        repaint();
        return;
    }
    if (e.y < top()) { drag_ = Drag::Locate; if (onSeek) onSeek(dragT0_); return; }

    selLane_ = laneAt(e.y);
    int id = regionAt(p);
    if (!id) {
        selRegion_ = 0;
        drag_ = Drag::Select;
        sel_ = {};
        if (onSeek) onSeek(snapTime(dragT0_, 0));
        repaint();
        return;
    }
    selRegion_ = id;
    auto* r = s_.findRegion(id);
    dragId_ = id;
    dragOrigStart_ = r->start;
    dragOrigEnd_ = r->end();
    dragOrigLane_ = r->lane;

    if (e.mods.isPopupMenu()) {
        juce::PopupMenu menu;
        menu.addItem(1, "Split at playhead", playhead_ > r->start && playhead_ < r->end());
        menu.addItem(2, r->mute ? "Unmute" : "Mute");
        menu.addItem(3, "Delete (close gap)");
        menu.addItem(4, "Delete (leave gap)");
        menu.addItem(5, "Clear gain envelope", !r->envelope.isEmpty());
        menu.addItem(6, "Select region range");
        menu.addSeparator();
        menu.addItem(7, r->fxState.isNotEmpty() ? "Region effects... (active)" : "Region effects...");
        menu.addItem(8, "Restore (declick, denoise, hum)...");
        menu.addItem(9, "Spectral editor...");
        menu.showMenuAsync({}, [this, id](int c) {
            auto* rr = s_.findRegion(id);
            if (!rr || c == 0) return;
            if (c == 7) { if (onRegionFx) onRegionFx(id); return; }
            if (c == 8) { selRegion_ = id; if (onRegionRestore) onRegionRestore(id); return; }
            if (c == 9) { selRegion_ = id; if (onRegionSpectral) onRegionSpectral(id); return; }
            if (c == 1) s_.split(id, playhead_);
            if (c == 2) rr->mute = !rr->mute;
            if (c == 3) s_.deleteRegion(id, true);
            if (c == 4) s_.deleteRegion(id, false);
            if (c == 5) rr->envelope.clear();
            if (c == 6) { sel_ = {rr->start, rr->end()}; repaint(); return; }
            if (onEdit) onEdit();
        });
        repaint();
        return;
    }

    auto lb = laneBounds(r->lane);
    if (envelopeMode_) {
        int pt = envPointAt(*r, p);
        if (pt >= 0 && e.mods.isAltDown()) { r->envelope.remove(pt); changed_ = true; repaint(); return; }
        if (pt < 0) {
            // Add a point on the line.
            double t = dragT0_ - r->start;
            double db = juce::jlimit(-36.0, 12.0, 12.0 - (double(e.y - (lb.getY() + 4)) / (lb.getHeight() - 8)) * 48.0);
            if (r->envelope.isEmpty()) { r->envelope.add({0.0, 0.0}); r->envelope.add({r->length(), 0.0}); }
            int at = 0;
            while (at < r->envelope.size() && r->envelope[at].t < t) ++at;
            r->envelope.insert(at, {t, db});
            pt = at;
            changed_ = true;
        }
        drag_ = Drag::EnvPoint;
        dragPoint_ = pt;
        repaint();
        return;
    }
    float xa = timeToX(r->start), xb = timeToX(r->end());
    if (e.y < lb.getY() + 10 && std::abs(e.x - timeToX(r->start + r->fadeIn)) < 6) drag_ = Drag::FadeIn;
    else if (e.y < lb.getY() + 10 && std::abs(e.x - timeToX(r->end() - r->fadeOut)) < 6) drag_ = Drag::FadeOut;
    else if (std::abs(e.x - xa) < 6) drag_ = Drag::TrimStart;
    else if (std::abs(e.x - xb) < 6) drag_ = Drag::TrimEnd;
    else drag_ = Drag::Move;
    repaint();
}

void Timeline::mouseDrag(const juce::MouseEvent& e) {
    double t = xToTime(float(e.x));
    switch (drag_) {
        case Drag::Locate:
            if (onSeek) onSeek(juce::jmax(0.0, t));
            break;
        case Drag::Select: {
            double a = snapTime(juce::jmax(0.0, juce::jmin(dragT0_, t)), 0), b = snapTime(juce::jmax(dragT0_, t), 0);
            sel_ = {a, b};
            break;
        }
        case Drag::Marker:
            s_.moveMarker(dragId_, snapTime(t, 0));
            changed_ = true;
            break;
        case Drag::Move: {
            auto* r = s_.findRegion(dragId_);
            if (!r) break;
            double ns = dragOrigStart_ + (t - dragT0_);
            // Snap either edge.
            double sStart = snapTime(ns, dragId_), sEnd = snapTime(ns + r->length(), dragId_) - r->length();
            if (std::abs(sStart - ns) > 1e-9) ns = sStart;
            else if (std::abs(sEnd - ns) > 1e-9) ns = sEnd;
            int lane = e.mods.isShiftDown() ? dragOrigLane_ : laneAt(e.y);
            s_.moveRegion(dragId_, ns, lane);
            selLane_ = lane;
            changed_ = true;
            break;
        }
        case Drag::TrimStart: s_.trimRegionStart(dragId_, snapTime(t, dragId_)); changed_ = true; break;
        case Drag::TrimEnd: s_.trimRegionEnd(dragId_, snapTime(t, dragId_)); changed_ = true; break;
        case Drag::FadeIn:
            if (auto* r = s_.findRegion(dragId_)) { r->fadeIn = juce::jlimit(0.0, r->length() - r->fadeOut, t - r->start); changed_ = true; }
            break;
        case Drag::FadeOut:
            if (auto* r = s_.findRegion(dragId_)) { r->fadeOut = juce::jlimit(0.0, r->length() - r->fadeIn, r->end() - t); changed_ = true; }
            break;
        case Drag::EnvPoint:
            if (auto* r = s_.findRegion(dragId_); r && juce::isPositiveAndBelow(dragPoint_, r->envelope.size())) {
                auto lb = laneBounds(r->lane);
                auto& pt = r->envelope.getReference(dragPoint_);
                double lo = dragPoint_ > 0 ? r->envelope[dragPoint_ - 1].t : 0.0;
                double hi = dragPoint_ + 1 < r->envelope.size() ? r->envelope[dragPoint_ + 1].t : r->length();
                pt.t = juce::jlimit(lo, hi, t - r->start);
                pt.db = juce::jlimit(-36.0, 12.0, 12.0 - (double(e.y - (lb.getY() + 4)) / (lb.getHeight() - 8)) * 48.0);
                changed_ = true;
            }
            break;
        case Drag::None: break;
    }
    if (e.x > getWidth() - 10) { view0_ += 8 / pps_; updateScrollbar(); }
    if (e.x < 10 && view0_ > 0) { view0_ = juce::jmax(0.0, view0_ - 8 / pps_); updateScrollbar(); }
    repaint();
}

void Timeline::mouseUp(const juce::MouseEvent&) {
    if (drag_ == Drag::Select && sel_.getLength() < 2.0 / pps_) sel_ = {};
    if (changed_ && onEdit) onEdit();
    drag_ = Drag::None;
    changed_ = false;
    repaint();
}

void Timeline::mouseDoubleClick(const juce::MouseEvent& e) {
    if (int m = markerAt(e.getPosition())) { if (onMarkerDoubleClicked) onMarkerDoubleClicked(m); return; }
    if (int k = breakAt(e.getPosition()); k >= 0) { if (onBreakEdit) onBreakEdit(productView(), k); return; }
    double t = xToTime(float(e.x));
    int lane = laneAt(e.y);
    for (auto& a : s_.regions)
        for (auto& b : s_.regions)
            if (a.id != b.id && a.lane == lane && b.lane == lane && b.start > a.start && b.start < a.end() &&
                t >= b.start - 4 / pps_ && t <= juce::jmin(a.end(), b.end()) + 4 / pps_) {
                if (onCrossfade) onCrossfade(a.id, b.id);
                return;
            }
    // Double-click a butt edit (adjacent regions) also opens the editor.
    for (auto& a : s_.regions)
        for (auto& b : s_.regions)
            if (a.id != b.id && a.lane == lane && b.lane == lane && std::abs(a.end() - b.start) * pps_ < 6 &&
                std::abs(t - b.start) * pps_ < 6) {
                if (onCrossfade) onCrossfade(a.id, b.id);
                return;
            }
    if (int id = regionAt(e.getPosition())) {
        auto* r = s_.findRegion(id);
        sel_ = {r->start, r->end()};
        repaint();
    }
}

void Timeline::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
        zoomBy(w.deltaY > 0 ? 1.15 : 1 / 1.15, xToTime(float(e.x)));
        return;
    }
    double dx = (std::abs(w.deltaX) > std::abs(w.deltaY) ? -w.deltaX : -w.deltaY) * 300.0 / pps_;
    view0_ = juce::jmax(0.0, view0_ + dx);
    updateScrollbar();
    repaint();
}

void Timeline::mouseMagnify(const juce::MouseEvent& e, float scale) { zoomBy(scale, xToTime(float(e.x))); }

int Timeline::productView() const {
    for (auto& p : s_.products)
        if (p.id == productView_) return p.id;
    return s_.products.isEmpty() ? 0 : s_.products.getFirst().id;
}

int Timeline::breakAt(juce::Point<int> p) const {
    int pid = productView();
    if (!pid || p.y < kRuler + kMarkerStrip || p.y >= top()) return -1;
    const Product* prod = nullptr;
    for (auto& x : s_.products) if (x.id == pid) prod = &x;
    if (!prod) return -1;
    int best = -1;
    float bestD = 12.0f;
    for (int k = 0; k + 1 < prod->units.size(); ++k) {
        double mid = (prod->units[k].end + prod->units[k + 1].start) * 0.5;
        float d = std::abs(timeToX(mid) - float(p.x));
        float span = std::abs(timeToX(prod->units[k].end) - timeToX(prod->units[k + 1].start)) * 0.5f + 12.0f;
        if (d < juce::jmax(bestD, span) && (best < 0 || d < bestD)) { best = k; bestD = d; }
    }
    return best;
}

void Timeline::paintProducts(juce::Graphics& g) {
    int pid = productView();
    if (!pid) return;
    const Product* prod = nullptr;
    for (auto& x : s_.products) if (x.id == pid) prod = &x;
    if (!prod) return;
    const auto& spec = productSpec(prod->kind);
    auto strip = juce::Rectangle<int>(0, kRuler + kMarkerStrip, getWidth(), kProductStrip);
    g.setColour(colours::panel);
    g.fillRect(strip);
    g.setColour(colours::line);
    g.drawHorizontalLine(strip.getBottom() - 1, 0.0f, float(getWidth()));
    g.setFont(10.0f);
    for (int i = 0; i < prod->units.size(); ++i) {
        const auto& u = prod->units.getReference(i);
        float x0 = timeToX(u.start), x1 = timeToX(u.end);
        if (x1 < 0 || x0 > getWidth()) continue;
        // Alternate rows so overlapping units stay visible.
        auto row = juce::Rectangle<float>(x0, float(strip.getY() + 2 + (i % 2) * 9), x1 - x0, 9.0f);
        auto c = u.length() > spec.hardMax ? colours::error : u.length() > prod->limit() ? colours::accent : colours::ok;
        juce::ColourGradient grad(c.withAlpha(0.15f), x0, 0, c.withAlpha(0.65f), x0 + float(u.fadeIn * pps_), 0, false);
        if (u.fadeIn > 0) {
            g.setGradientFill(grad);
            g.fillRect(row.withWidth(float(juce::jmin(u.fadeIn * pps_, double(row.getWidth())))));
        }
        auto body = row.withTrimmedLeft(float(u.fadeIn * pps_)).withTrimmedRight(float(u.fadeOut * pps_));
        g.setColour(c.withAlpha(0.65f));
        g.fillRect(body);
        if (u.fadeOut > 0) {
            juce::ColourGradient go(c.withAlpha(0.65f), x1 - float(u.fadeOut * pps_), 0, c.withAlpha(0.15f), x1, 0, false);
            g.setGradientFill(go);
            g.fillRect(row.withTrimmedLeft(juce::jmax(0.0f, row.getWidth() - float(u.fadeOut * pps_))));
        }
        g.setColour(colours::bg);
        auto label = prod->name + " " + unitLabel(*prod, i) + "  " + msfString(u.length()).substring(0, 5);
        if (body.getWidth() > 40) g.drawText(label, body.reduced(3, 0).toNearestInt(), juce::Justification::centredLeft, true);
    }
}

}  // namespace lathe
