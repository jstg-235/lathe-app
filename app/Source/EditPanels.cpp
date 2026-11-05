#include "EditPanels.h"

namespace lathe {

// ================================================================= SourcePanel

SourcePanel::SourcePanel(AudioEngine& e, ThumbPool& pool) : engine_(e), pool_(pool) {
    for (auto* b : {&openBtn_, &playBtn_, &inBtn_, &outBtn_, &playRange_, &insertBtn_, &overwriteBtn_, &replaceBtn_, &appendBtn_}) {
        styleButton(*b, b == &replaceBtn_);
        addAndMakeVisible(*b);
    }
    info_.setColour(juce::Label::textColourId, colours::dim);
    info_.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(info_);

    openBtn_.onClick = [this] { showOpenDialog(); };
    playBtn_.onClick = [this] { togglePlay(); };
    inBtn_.onClick = [this] { markIn(); };
    outBtn_.onClick = [this] { markOut(); };
    playRange_.onClick = [this] { playInOut(); };
    insertBtn_.setTooltip("Insert the source In-Out at the destination playhead (or selection start); later material moves right");
    overwriteBtn_.setTooltip("Overwrite the destination from the playhead for the length of the source In-Out");
    replaceBtn_.setTooltip("4-point edit: replace the destination selection with the source In-Out; later material moves to fit");
    appendBtn_.setTooltip("Add the source In-Out as a new track at the end, with a track marker");
    insertBtn_.onClick = [this] { if (onEdit) onEdit(Session::EditMode::Insert); };
    overwriteBtn_.onClick = [this] { if (onEdit) onEdit(Session::EditMode::Overwrite); };
    replaceBtn_.onClick = [this] { if (onEdit) onEdit(Session::EditMode::Replace); };
    appendBtn_.onClick = [this] { if (onAppend) onAppend(); };
}

bool SourcePanel::open(const juce::File& f) {
    std::unique_ptr<juce::AudioFormatReader> r(engine_.formats().createReaderFor(f));
    if (!r || !engine_.loadSource(f)) return false;
    file_ = f;
    rate_ = r->sampleRate;
    length_ = r->lengthInSamples;
    lenSec_ = double(length_) / rate_;
    in_ = out_ = -1;
    pool_.get(f, this);
    info_.setText(f.getFileName() + "   " + juce::String(rate_ / 1000.0, 1) + " kHz  " + juce::String(r->bitsPerSample) + "-bit  " +
                      msfString(lenSec_),
                  juce::dontSendNotification);
    if (onOpened) onOpened(f);
    repaint();
    return true;
}

void SourcePanel::showOpenDialog() {
    chooser_ = std::make_unique<juce::FileChooser>("Open source audio", juce::File(), engine_.formats().getWildcardForAllFormats());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) open(fc.getResult()); });
}

void SourcePanel::playInOut() {
    if (!hasSource()) return;
    auto r = inOut();
    engine_.setSourcePosition(r.getStart());
    stopAtOut_ = true;
    engine_.playSource();
}

juce::Range<double> SourcePanel::inOut() const {
    double a = in_ >= 0 ? in_ : 0.0, b = out_ >= 0 ? out_ : lenSec_;
    if (b <= a) b = lenSec_;
    return {a, b};
}

void SourcePanel::togglePlay() {
    if (!hasSource()) return;
    stopAtOut_ = false;
    engine_.isSourcePlaying() ? engine_.stopSource() : engine_.playSource();
}

void SourcePanel::tick() {
    if (stopAtOut_ && engine_.isSourcePlaying() && engine_.sourcePosition() >= inOut().getEnd()) {
        engine_.stopSource();
        stopAtOut_ = false;
    }
    playBtn_.setButtonText(engine_.isSourcePlaying() ? "Stop" : "Play");
    if (hasSource()) repaint(waveArea());
}

juce::Rectangle<int> SourcePanel::waveArea() const { return getLocalBounds().reduced(6).withTrimmedTop(54); }

double SourcePanel::xToTime(int x) const {
    auto w = waveArea();
    return juce::jlimit(0.0, lenSec_, double(x - w.getX()) / juce::jmax(1, w.getWidth()) * lenSec_);
}

void SourcePanel::resized() {
    auto r = getLocalBounds().reduced(6);
    auto row = r.removeFromTop(24);
    openBtn_.setBounds(row.removeFromLeft(70));
    row.removeFromLeft(6);
    info_.setBounds(row);
    r.removeFromTop(4);
    row = r.removeFromTop(22);
    for (auto* b : {&playBtn_, &inBtn_, &outBtn_, &playRange_}) {
        b->setBounds(row.removeFromLeft(b == &playRange_ ? 90 : 50));
        row.removeFromLeft(4);
    }
    for (auto* b : {&appendBtn_, &replaceBtn_, &overwriteBtn_, &insertBtn_}) {
        b->setBounds(row.removeFromRight(78));
        row.removeFromRight(4);
    }
}

void SourcePanel::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    g.setColour(colours::dim);
    g.setFont(11.0f);
    g.drawText("SOURCE", getLocalBounds().reduced(8, 4).removeFromBottom(14), juce::Justification::right);
    auto w = waveArea();
    g.setColour(colours::bg);
    g.fillRect(w);
    if (!hasSource()) {
        g.setColour(colours::dim);
        g.drawText("Open a file to audition it, mark In and Out, then Insert, Overwrite, Replace or Append", w, juce::Justification::centred);
        return;
    }
    auto io = inOut();
    if (in_ >= 0 || out_ >= 0) {
        float a = float(w.getX() + io.getStart() / lenSec_ * w.getWidth());
        float b = float(w.getX() + io.getEnd() / lenSec_ * w.getWidth());
        g.setColour(colours::selection);
        g.fillRect(juce::Rectangle<float>(a, float(w.getY()), b - a, float(w.getHeight())));
    }
    if (auto* th = pool_.get(file_, nullptr)) {
        g.setColour(colours::wave);
        th->drawChannels(g, w.reduced(0, 2), 0.0, lenSec_, 1.0f);
    }
    auto mark = [&](double t, const char* label) {
        float x = float(w.getX() + t / lenSec_ * w.getWidth());
        g.setColour(colours::accent);
        g.drawLine(x, float(w.getY()), x, float(w.getBottom()), 1.5f);
        g.drawText(label, int(x) + 3, w.getY() + 2, 30, 12, juce::Justification::left);
    };
    if (in_ >= 0) mark(in_, "IN");
    if (out_ >= 0) mark(out_, "OUT");
    float px = float(w.getX() + engine_.sourcePosition() / juce::jmax(0.001, lenSec_) * w.getWidth());
    g.setColour(colours::text);
    g.drawLine(px, float(w.getY()), px, float(w.getBottom()), 1.5f);
    g.setColour(colours::dim);
    g.drawText(msfString(engine_.sourcePosition()) + "   In-Out " + msfString(io.getLength()), w.reduced(6, 2),
               juce::Justification::topRight);
}

void SourcePanel::mouseDown(const juce::MouseEvent& e) {
    if (!hasSource() || !waveArea().contains(e.getPosition())) return;
    dragFrom_ = xToTime(e.x);
    engine_.setSourcePosition(dragFrom_);
    repaint();
}

void SourcePanel::mouseDrag(const juce::MouseEvent& e) {
    if (!hasSource() || e.getDistanceFromDragStart() < 4) return;
    double t = xToTime(e.x);
    in_ = juce::jmin(dragFrom_, t);
    out_ = juce::jmax(dragFrom_, t);
    repaint();
}

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
