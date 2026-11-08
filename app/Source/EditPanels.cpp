#include "EditPanels.h"

namespace lathe {

// ================================================================= CrossfadeEditor

CrossfadeEditor::CrossfadeEditor(Session& s, ThumbPool& pool, int outgoing, int incoming)
    : s_(s), pool_(pool), out_(outgoing), in_(incoming) {
    length_.setSliderStyle(juce::Slider::LinearHorizontal);
    length_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    length_.setRange(0.0, 10000.0, 1.0);
    length_.setSkewFactorFromMidPoint(500.0);
    length_.setTextValueSuffix(" ms");
    addAndMakeVisible(length_);
    for (auto* c : {&outShape_, &inShape_}) {
        for (int i = 0; i <= 4; ++i) c->addItem(fadeShapeName(FadeShape(i)), i + 1);
        addAndMakeVisible(*c);
    }
    for (auto* l : {&lenLbl_, &outLbl_, &inLbl_, &slipOutLbl_, &slipInLbl_}) {
        l->setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(*l);
    }
    lenLbl_.setText("Crossfade", juce::dontSendNotification);
    outLbl_.setText("Out shape", juce::dontSendNotification);
    inLbl_.setText("In shape", juce::dontSendNotification);
    slipOutLbl_.setText("Slip outgoing", juce::dontSendNotification);
    slipInLbl_.setText("Slip incoming", juce::dontSendNotification);
    for (auto* b : {&slipOutL_, &slipOutR_, &slipInL_, &slipInR_, &slipStep_, &audition_, &pre_, &post_}) {
        styleButton(*b, b == &audition_);
        addAndMakeVisible(*b);
    }

    length_.onValueChange = [this] {
        if (updating_) return;
        s_.setCrossfadeLength(out_, in_, length_.getValue() / 1000.0, true);
        if (onChange) onChange();
        repaint();
    };
    outShape_.onChange = [this] {
        if (auto* r = s_.findRegion(out_); r && !updating_) { r->fadeOutShape = FadeShape(outShape_.getSelectedId() - 1); if (onChange) onChange(); repaint(); }
    };
    inShape_.onChange = [this] {
        if (auto* r = s_.findRegion(in_); r && !updating_) { r->fadeInShape = FadeShape(inShape_.getSelectedId() - 1); if (onChange) onChange(); repaint(); }
    };
    auto slip = [this](int id, double dir) { s_.slip(id, dir * step_); if (onChange) onChange(); repaint(); };
    // Slipping moves the audio under the fixed edit point: "<" plays later material earlier.
    slipOutL_.onClick = [this, slip] { slip(out_, +1); };
    slipOutR_.onClick = [this, slip] { slip(out_, -1); };
    slipInL_.onClick = [this, slip] { slip(in_, +1); };
    slipInR_.onClick = [this, slip] { slip(in_, -1); };
    slipStep_.onClick = [this] {
        step_ = step_ > 0.005 ? 0.001 : step_ > 0.0005 ? 1.0 / 44100 : 0.010;
        slipStep_.setButtonText(step_ > 0.005 ? "10 ms" : step_ > 0.0005 ? "1 ms" : "1 sample");
    };
    audition_.onClick = [this] {
        auto* a = s_.findRegion(out_);
        auto* b = s_.findRegion(in_);
        if (a && b && onAudition) onAudition(b->start - 2.0, a->end() + 2.0);
    };
    pre_.onClick = [this] { if (auto* b = s_.findRegion(in_); b && onAudition) onAudition(b->start - 3.0, b->start); };
    post_.onClick = [this] { if (auto* a = s_.findRegion(out_); a && onAudition) onAudition(a->end(), a->end() + 3.0); };
    pool_.get(s_.findRegion(out_)->file, this);
    pool_.get(s_.findRegion(in_)->file, this);
    refreshControls();
    setSize(900, 460);
}

double CrossfadeEditor::overlap() const {
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    return a && b ? juce::jmax(0.0, a->end() - b->start) : 0.0;
}

void CrossfadeEditor::refreshControls() {
    updating_ = true;
    length_.setValue(overlap() * 1000.0, juce::dontSendNotification);
    if (auto* a = s_.findRegion(out_)) outShape_.setSelectedId(int(a->fadeOutShape) + 1, juce::dontSendNotification);
    if (auto* b = s_.findRegion(in_)) inShape_.setSelectedId(int(b->fadeInShape) + 1, juce::dontSendNotification);
    updating_ = false;
}

void CrossfadeEditor::resized() {
    auto r = getLocalBounds().reduced(10);
    auto controls = r.removeFromBottom(64);
    auto row1 = controls.removeFromTop(28);
    lenLbl_.setBounds(row1.removeFromLeft(80));
    length_.setBounds(row1.removeFromLeft(320));
    row1.removeFromLeft(12);
    outLbl_.setBounds(row1.removeFromLeft(70));
    outShape_.setBounds(row1.removeFromLeft(130));
    row1.removeFromLeft(8);
    inLbl_.setBounds(row1.removeFromLeft(64));
    inShape_.setBounds(row1.removeFromLeft(130));
    controls.removeFromTop(8);
    auto row2 = controls.removeFromTop(26);
    slipOutLbl_.setBounds(row2.removeFromLeft(96));
    slipOutL_.setBounds(row2.removeFromLeft(30));
    slipOutR_.setBounds(row2.removeFromLeft(30));
    row2.removeFromLeft(12);
    slipInLbl_.setBounds(row2.removeFromLeft(96));
    slipInL_.setBounds(row2.removeFromLeft(30));
    slipInR_.setBounds(row2.removeFromLeft(30));
    row2.removeFromLeft(8);
    slipStep_.setBounds(row2.removeFromLeft(80));
    audition_.setBounds(row2.removeFromRight(90));
    row2.removeFromRight(6);
    post_.setBounds(row2.removeFromRight(80));
    row2.removeFromRight(6);
    pre_.setBounds(row2.removeFromRight(80));
}

void CrossfadeEditor::paintSide(juce::Graphics& g, juce::Rectangle<int> area, const Region& r, bool outgoing, double t0, double t1) {
    g.setColour(colours::bg);
    g.fillRect(area);
    auto xOf = [&](double t) { return float(area.getX() + (t - t0) / (t1 - t0) * area.getWidth()); };
    // The whole source around the region, dimmed, so you can see what slipping would reveal.
    if (auto* th = pool_.get(r.file, nullptr)) {
        double srcAtT0 = double(r.srcIn) / r.fileRate + (t0 - r.start);
        double srcAtT1 = srcAtT0 + (t1 - t0);
        double fileLen = double(r.fileLength) / r.fileRate;
        double a = juce::jmax(0.0, srcAtT0), b = juce::jmin(fileLen, srcAtT1);
        if (b > a) {
            auto full = area.withX(int(xOf(t0 + (a - srcAtT0)))).withRight(int(xOf(t0 + (b - srcAtT0))));
            g.setColour(colours::wave.withAlpha(0.25f));
            th->drawChannels(g, full.reduced(0, 4), a, b, 1.0f);
            // The part actually used, bright.
            double ua = juce::jmax(t0, r.start), ub = juce::jmin(t1, r.end());
            if (ub > ua) {
                auto used = area.withX(int(xOf(ua))).withRight(int(xOf(ub)));
                g.saveState();
                g.reduceClipRegion(used);
                g.setColour(colours::wave);
                th->drawChannels(g, full.reduced(0, 4), a, b, 1.0f);
                g.restoreState();
            }
        }
    }
    // Gain curve over the crossfade.
    int idx = s_.indexOfRegion(r.id);
    auto f = s_.fadesFor(idx);
    juce::Path p;
    for (int k = 0; k <= 100; ++k) {
        double t = t0 + (t1 - t0) * k / 100.0;
        double gain = 0;
        if (t >= r.start && t <= r.end()) {
            gain = 1.0;
            if (f.in > 0 && t < r.start + f.in) gain *= fadeGain(f.inShape, (t - r.start) / f.in);
            if (f.out > 0 && t > r.end() - f.out) gain *= fadeGain(f.outShape, (r.end() - t) / f.out);
        }
        float y = float(area.getBottom() - gain * area.getHeight());
        if (k == 0) p.startNewSubPath(xOf(t), y); else p.lineTo(xOf(t), y);
    }
    g.setColour(colours::envelope);
    g.strokePath(p, juce::PathStrokeType(1.5f));
    g.setColour(colours::dim);
    g.drawText((outgoing ? "OUTGOING  " : "INCOMING  ") + r.name, area.reduced(6, 4), juce::Justification::topLeft);
}

void CrossfadeEditor::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    if (!a || !b) return;
    auto area = getLocalBounds().reduced(10).withTrimmedBottom(74);
    double xs = b->start, xe = juce::jmax(a->end(), b->start);
    double span = juce::jmax(1.0, (xe - xs) * 3.0);
    double t0 = 0.5 * (xs + xe) - span / 2, t1 = t0 + span;
    auto top = area.removeFromTop(area.getHeight() / 2 - 2);
    area.removeFromTop(4);
    paintSide(g, top, *a, true, t0, t1);
    paintSide(g, area, *b, false, t0, t1);
    auto full = top.getUnion(area);
    auto xOf = [&](double t) { return float(full.getX() + (t - t0) / (t1 - t0) * full.getWidth()); };
    g.setColour(colours::accent.withAlpha(0.15f));
    g.fillRect(juce::Rectangle<float>(xOf(xs), float(full.getY()), juce::jmax(1.0f, xOf(xe) - xOf(xs)), float(full.getHeight())));
    g.setColour(colours::accent);
    g.drawVerticalLine(int(xOf(xs)), float(full.getY()), float(full.getBottom()));
    g.drawVerticalLine(int(xOf(xe)), float(full.getY()), float(full.getBottom()));
    if (playhead_ >= t0 && playhead_ <= t1) {
        g.setColour(colours::text);
        g.drawVerticalLine(int(xOf(playhead_)), float(full.getY()), float(full.getBottom()));
    }
    g.setColour(colours::dim);
    g.drawText("Edit at " + msfString(xs) + "   overlap " + juce::String(int(std::round((xe - xs) * 1000))) + " ms",
               full.reduced(6, 4), juce::Justification::topRight);
}

}  // namespace lathe
