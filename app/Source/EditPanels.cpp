#include "EditPanels.h"

#include "ProgramRenderer.h"

namespace lathe {

// ================================================================= fade library

juce::var FadePreset::toVar() const {
    auto* o = new juce::DynamicObject();
    o->setProperty("name", name);
    o->setProperty("length", length);
    o->setProperty("outShape", int(outShape));
    o->setProperty("inShape", int(inShape));
    o->setProperty("outDb", outDb);
    o->setProperty("inDb", inDb);
    o->setProperty("outAlpha", outAlpha);
    o->setProperty("inAlpha", inAlpha);
    o->setProperty("used", used);
    return o;
}

FadePreset FadePreset::fromVar(const juce::var& v) {
    FadePreset p;
    if (!v.isObject()) return p;
    p.name = v["name"].toString();
    p.length = juce::jlimit(0.0, 30.0, double(v.getProperty("length", 0.005)));
    p.outShape = FadeShape(juce::jlimit(0, kNumFadeShapes - 1, int(v["outShape"])));
    p.inShape = FadeShape(juce::jlimit(0, kNumFadeShapes - 1, int(v["inShape"])));
    p.outDb = double(v.getProperty("outDb", 3.0));
    p.inDb = double(v.getProperty("inDb", 3.0));
    p.outAlpha = double(v.getProperty("outAlpha", 0.0));
    p.inAlpha = double(v.getProperty("inAlpha", 0.0));
    p.used = bool(v.getProperty("used", false));
    return p;
}

FadeLibrary defaultFadeLibrary() {
    FadeLibrary lib;
    lib[0].name = "Default crossfade";
    lib[0].length = 0.005;
    lib[0].used = true;
    return lib;
}

void applyFadePreset(Session& s, int outgoing, int incoming, const FadePreset& p, bool lockSound, Session::Ripple ripple) {
    if (lockSound) s.crossfadeCentred(outgoing, incoming, p.length);
    else s.setCrossfadeLength(outgoing, incoming, p.length, ripple);
    auto* a = s.findRegion(outgoing);
    auto* b = s.findRegion(incoming);
    if (!a || !b) return;
    // No overlap could be made (neither side has audio beyond its edge to fade with): leave the
    // two regions exactly as they were, their own fades included.
    if (p.length > 0 && a->end() - b->start < 1e-5) return;
    a->fadeOutShape = p.outShape;
    b->fadeInShape = p.inShape;
    a->fadeOutDb = p.outDb;
    b->fadeInDb = p.inDb;
    a->fadeOutAlpha = p.outAlpha;
    b->fadeInAlpha = p.inAlpha;
    a->fadeOutManual = b->fadeInManual = false;
    a->fadeOutOffset = b->fadeInOffset = 0;
    a->fadeOut = b->fadeIn = 0;   // follow the overlap
}

void applySingleFadePreset(Session& s, int region, bool fadeIn, const FadePreset& p) {
    auto* r = s.findRegion(region);
    if (!r) return;
    const double len = juce::jlimit(0.0, r->length(), p.length);
    if (fadeIn) {
        r->fadeIn = len;
        r->fadeInManual = true;
        r->fadeInOffset = 0;
        r->fadeInShape = p.inShape;
        r->fadeInDb = p.inDb;
        r->fadeInAlpha = p.inAlpha;
    } else {
        r->fadeOut = len;
        r->fadeOutManual = true;
        r->fadeOutOffset = 0;
        r->fadeOutShape = p.outShape;
        r->fadeOutDb = p.outDb;
        r->fadeOutAlpha = p.outAlpha;
    }
}

juce::var FadeEditorPrefs::toVar() const {
    auto* o = new juce::DynamicObject();
    o->setProperty("lockSound", lockSound);
    o->setProperty("powerLock", powerLock);
    o->setProperty("autoZoom", autoZoom);
    o->setProperty("autoAudition", autoAudition);
    o->setProperty("ripple", int(ripple));
    o->setProperty("nudgeA", nudge[0]);
    o->setProperty("nudgeB", nudge[1]);
    o->setProperty("nudgeC", nudge[2]);
    o->setProperty("nudgeSize", nudgeSize);
    o->setProperty("defaultOnEdits", defaultOnEdits);
    return o;
}

void FadeEditorPrefs::fromVar(const juce::var& v) {
    if (!v.isObject()) return;
    lockSound = bool(v.getProperty("lockSound", lockSound));
    powerLock = bool(v.getProperty("powerLock", powerLock));
    autoZoom = bool(v.getProperty("autoZoom", autoZoom));
    autoAudition = bool(v.getProperty("autoAudition", autoAudition));
    ripple = Session::Ripple(juce::jlimit(0, 2, int(v.getProperty("ripple", int(ripple)))));
    nudge[0] = double(v.getProperty("nudgeA", nudge[0]));
    nudge[1] = double(v.getProperty("nudgeB", nudge[1]));
    nudge[2] = double(v.getProperty("nudgeC", nudge[2]));
    nudgeSize = juce::jlimit(0, 2, int(v.getProperty("nudgeSize", nudgeSize)));
    defaultOnEdits = bool(v.getProperty("defaultOnEdits", defaultOnEdits));
}

// ================================================================= CrossfadeEditor

namespace {
void styleSlider(juce::Slider& sl, double lo, double hi, double step, const juce::String& suffix, double mid = -1) {
    sl.setSliderStyle(juce::Slider::LinearHorizontal);
    sl.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    sl.setRange(lo, hi, step);
    if (mid > lo) sl.setSkewFactorFromMidPoint(mid);
    sl.setTextValueSuffix(suffix);
}
}  // namespace

CrossfadeEditor::CrossfadeEditor(Session& s, ThumbPool& pool, juce::AudioFormatManager& formats, FadeEditorPrefs& prefs, FadeLibrary& lib,
                                 int outgoing, int incoming)
    : s_(s), pool_(pool), formats_(formats), prefs_(prefs), lib_(lib), out_(outgoing), in_(incoming) {
    styleSlider(length_, 0.0, 10000.0, 0.1, " ms", 300.0);
    addAndMakeVisible(length_);
    for (auto* l : {&lenLbl_, &rippleLbl_, &nudgeLbl_, &libLbl_, &info_}) {
        l->setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(*l);
    }
    lenLbl_.setText("Crossfade", juce::dontSendNotification);
    rippleLbl_.setText("Then", juce::dontSendNotification);
    nudgeLbl_.setText("Nudge", juce::dontSendNotification);
    libLbl_.setText("Library", juce::dontSendNotification);
    info_.setJustificationType(juce::Justification::centredRight);
    ripple_.addItem("ripple everything after", 2);
    ripple_.addItem("ripple until black", 3);
    ripple_.addItem("move nothing else", 1);
    addAndMakeVisible(ripple_);
    for (int k = 0; k < 10; ++k) libSlot_.addItem(juce::String(k) + "  " + (lib_[size_t(k)].used ? lib_[size_t(k)].name : juce::String("(empty)")), k + 1);
    libSlot_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(libSlot_);

    for (auto* b : {&lockSound_, &powerLock_, &autoZoom_, &zoomIn_, &zoomOut_, &align_, &nA_, &nB_, &nC_, &autoAud_, &audition_, &pre_, &post_,
                    &libApply_, &libSave_}) {
        styleButton(*b, b == &audition_);
        addAndMakeVisible(*b);
    }
    for (auto* b : {&lockSound_, &powerLock_, &autoZoom_, &autoAud_}) b->setClickingTogglesState(true);
    for (auto* b : {&nA_, &nB_, &nC_}) { b->setClickingTogglesState(true); b->setRadioGroupId(7001); }
    lockSound_.setTooltip("Changing the crossfade length trims both sides around the edit; nothing moves in time");
    powerLock_.setTooltip("Shape, dB down and alpha changes apply to both fades together");
    autoZoom_.setTooltip("Keep the view fitted to the crossfade");
    align_.setTooltip("Both fades span the whole overlap, centred on the edit");
    autoAud_.setTooltip("Play through the edit after every nudge");
    ripple_.setTooltip("With Lock Sound off, what happens to the material after the incoming side when the crossfade changes");

    setupSide(o_, true);
    setupSide(i_, false);
    if (single()) {
        // One fade: no crossfade length, Lock Sound, ripple, Power Lock or Align; only its side.
        for (auto* c : std::initializer_list<juce::Component*>{&length_, &lenLbl_, &lockSound_, &rippleLbl_, &ripple_, &powerLock_, &align_}) c->setVisible(false);
        Side& gone = out_ ? i_ : o_;
        for (auto* c : std::initializer_list<juce::Component*>{&gone.title, &gone.shapeLbl, &gone.lenLbl, &gone.offLbl, &gone.dbLbl, &gone.alphaLbl, &gone.editLbl,
                                                               &gone.slipLbl, &gone.shape, &gone.len, &gone.off, &gone.db, &gone.alpha, &gone.editL, &gone.editR,
                                                               &gone.slipL, &gone.slipR, &gone.toEdit})
            c->setVisible(false);
        (out_ ? o_ : i_).title.setText(out_ ? "FADE-OUT" : "FADE-IN", juce::dontSendNotification);
        (out_ ? o_ : i_).len.setTooltip("The fade's length");
        (out_ ? o_ : i_).editLbl.setText("End", juce::dontSendNotification);
        if (in_) i_.editLbl.setText("Start", juce::dontSendNotification);
    }

    length_.onValueChange = [this] {
        if (updating_) return;
        const double len = length_.getValue() / 1000.0;
        if (prefs_.lockSound) s_.crossfadeCentred(out_, in_, len);
        else s_.setCrossfadeLength(out_, in_, len, prefs_.ripple);
        changed(false);
    };
    ripple_.onChange = [this] {
        if (updating_) return;
        prefs_.ripple = Session::Ripple(ripple_.getSelectedId() - 1);
        if (onPrefsChanged) onPrefsChanged();
    };
    auto toggle = [this](juce::TextButton& b, bool& v) {
        b.onClick = [this, &b, &v] { v = b.getToggleState(); refreshControls(); if (onPrefsChanged) onPrefsChanged(); repaint(); };
    };
    toggle(lockSound_, prefs_.lockSound);
    toggle(powerLock_, prefs_.powerLock);
    toggle(autoZoom_, prefs_.autoZoom);
    toggle(autoAud_, prefs_.autoAudition);
    nA_.onClick = [this] { prefs_.nudgeSize = 0; if (onPrefsChanged) onPrefsChanged(); };
    nB_.onClick = [this] { prefs_.nudgeSize = 1; if (onPrefsChanged) onPrefsChanged(); };
    nC_.onClick = [this] { prefs_.nudgeSize = 2; if (onPrefsChanged) onPrefsChanged(); };
    zoomIn_.onClick = [this] { if (prefs_.autoZoom) { zoomSpan_ = viewRange().getLength(); prefs_.autoZoom = false; } zoomSpan_ = juce::jmax(0.005, zoomSpan_ / 1.5); refreshControls(); resultDirty_ = true; startTimer(30); repaint(); };
    zoomOut_.onClick = [this] { if (prefs_.autoZoom) { zoomSpan_ = viewRange().getLength(); prefs_.autoZoom = false; } zoomSpan_ = juce::jmin(120.0, zoomSpan_ * 1.5); refreshControls(); resultDirty_ = true; startTimer(30); repaint(); };
    align_.onClick = [this] {
        auto* a = s_.findRegion(out_);
        auto* b = s_.findRegion(in_);
        if (!a || !b) return;
        a->fadeOutManual = b->fadeInManual = false;
        a->fadeOutOffset = b->fadeInOffset = 0;
        a->fadeOut = b->fadeIn = 0;
        changed(false);
    };
    audition_.onClick = [this] {
        if (!onAudition) return;
        if (single()) {
            if (auto* r = only()) {
                const auto f = s_.fadesFor(s_.indexOfRegion(r->id));
                if (in_) onAudition(r->start - 1.0, r->start + f.inOffset + f.in + 2.0);
                else onAudition(r->end() - f.outOffset - f.out - 2.0, r->end() + 1.0);
            }
            return;
        }
        auto* a = s_.findRegion(out_);
        auto* b = s_.findRegion(in_);
        if (a && b) onAudition(b->start - 2.0, a->end() + 2.0);
    };
    pre_.onClick = [this] {
        if (single()) { if (onAudition && only()) onAudition(edge() - 3.0, edge()); return; }
        if (auto* b = s_.findRegion(in_); b && onAudition) onAudition(b->start - 3.0, b->start);
    };
    post_.onClick = [this] {
        if (single()) { if (onAudition && only()) onAudition(edge(), edge() + 3.0); return; }
        if (auto* a = s_.findRegion(out_); a && onAudition) onAudition(a->end(), a->end() + 3.0);
    };
    libApply_.onClick = [this] { recall(libSlot_.getSelectedId() - 1); };
    libSave_.onClick = [this] {
        const int slot = libSlot_.getSelectedId() - 1;
        auto* w = new juce::AlertWindow("Save Fade", "Save this edit's crossfade to library slot " + juce::String(slot) +
                                                         (slot == 0 ? " (the default crossfade for new edits)." : ". Recall it with Option-" + juce::String(slot) + "."),
                                        juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", lib_[size_t(slot)].used ? lib_[size_t(slot)].name : juce::String("Fade ") + juce::String(slot), "Name");
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, slot](int r) {
            std::unique_ptr<juce::AlertWindow> owner(w);
            if (r != 1) return;
            if (single()) {
                // One fade: its length and shape, for either side.
                auto* reg = only();
                if (!reg) return;
                const auto f = s_.fadesFor(s_.indexOfRegion(reg->id));
                FadePreset p;
                p.name = w->getTextEditorContents("name").trim();
                p.length = in_ ? f.in : f.out;
                p.inShape = p.outShape = in_ ? f.inShape : f.outShape;
                p.inDb = p.outDb = in_ ? f.inDb : f.outDb;
                p.inAlpha = p.outAlpha = in_ ? f.inAlpha : f.outAlpha;
                p.used = true;
                lib_[size_t(slot)] = p;
                libSlot_.changeItemText(slot + 1, juce::String(slot) + "  " + p.name);
                if (onLibraryChanged) onLibraryChanged();
                return;
            }
            auto* a = s_.findRegion(out_);
            auto* b = s_.findRegion(in_);
            if (!a || !b) return;
            const int ia = s_.indexOfRegion(out_), ib = s_.indexOfRegion(in_);
            const auto fa = s_.fadesFor(ia), fb = s_.fadesFor(ib);
            FadePreset p;
            p.name = w->getTextEditorContents("name").trim();
            p.length = overlap() > 0 ? overlap() : juce::jmax(fa.out, fb.in);
            p.outShape = fa.outShape;
            p.inShape = fb.inShape;
            p.outDb = fa.outDb;
            p.inDb = fb.inDb;
            p.outAlpha = fa.outAlpha;
            p.inAlpha = fb.inAlpha;
            p.used = true;
            lib_[size_t(slot)] = p;
            libSlot_.changeItemText(slot + 1, juce::String(slot) + "  " + p.name);
            if (onLibraryChanged) onLibraryChanged();
        }), false);
    };
    if (auto* a = s_.findRegion(out_)) pool_.get(a->file, this);
    if (auto* b = s_.findRegion(in_)) pool_.get(b->file, this);
    refreshControls();
    setSize(980, 680);
    startTimer(30);
}

void CrossfadeEditor::setupSide(Side& sd, bool outgoing) {
    sd.title.setText(outgoing ? "OUTGOING FADE" : "INCOMING FADE", juce::dontSendNotification);
    sd.title.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    sd.title.setColour(juce::Label::textColourId, colours::accent);
    addAndMakeVisible(sd.title);
    const char* names[] = {"Shape", "Length", "Offset", "dB down", "Alpha", "Edit point", "Slip"};
    juce::Label* labels[] = {&sd.shapeLbl, &sd.lenLbl, &sd.offLbl, &sd.dbLbl, &sd.alphaLbl, &sd.editLbl, &sd.slipLbl};
    for (int k = 0; k < 7; ++k) {
        labels[k]->setText(names[k], juce::dontSendNotification);
        labels[k]->setColour(juce::Label::textColourId, colours::dim);
        addAndMakeVisible(*labels[k]);
    }
    for (int k = 0; k < kNumFadeShapes; ++k) sd.shape.addItem(fadeShapeName(FadeShape(k)), k + 1);
    addAndMakeVisible(sd.shape);
    styleSlider(sd.len, 0.0, 10000.0, 0.1, " ms", 300.0);
    styleSlider(sd.off, 0.0, 5000.0, 0.1, " ms", 200.0);
    styleSlider(sd.db, 0.5, 12.0, 0.1, " dB");
    styleSlider(sd.alpha, 0.0, 1.0, 0.01, "");
    for (auto* sl : {&sd.len, &sd.off, &sd.db, &sd.alpha}) addAndMakeVisible(*sl);
    sd.len.setTooltip("This fade's own length (setting it fixes the length instead of following the overlap)");
    sd.off.setTooltip(outgoing ? "End the fade-out this long before the outgoing region ends" : "Start the fade-in this long after the incoming region starts");
    sd.db.setTooltip("Custom shape: level halfway through the fade (3 dB = equal power, 6 dB = linear)");
    sd.alpha.setTooltip("Custom shape: bends the curve towards an S shape");
    sd.toEdit.setButtonText("To Edit Point");
    sd.toEdit.setTooltip(outgoing ? "Move the outgoing side's end to the Edit Point (trim; the audio stays put)"
                                  : "Move the incoming side's start to the Edit Point (trim; the audio stays put)");
    for (auto* b : {&sd.editL, &sd.editR, &sd.slipL, &sd.slipR, &sd.toEdit}) {
        styleButton(*b, false);
        addAndMakeVisible(*b);
    }
    sd.editL.setTooltip("Move this side's edit point earlier by the nudge size (the audio stays put)");
    sd.editR.setTooltip("Move this side's edit point later by the nudge size");
    sd.slipL.setTooltip("Slip this side's audio earlier under its edit point");
    sd.slipR.setTooltip("Slip this side's audio later under its edit point");
    sd.shape.onChange = [this, outgoing] { sideParamChanged(outgoing); };
    for (auto* sl : {&sd.len, &sd.off, &sd.db, &sd.alpha}) sl->onValueChange = [this, outgoing] { sideParamChanged(outgoing); };
    sd.editL.onClick = [this, outgoing] { nudgeEdit(outgoing, -1); };
    sd.editR.onClick = [this, outgoing] { nudgeEdit(outgoing, 1); };
    sd.slipL.onClick = [this, outgoing] { nudgeSlip(outgoing, -1); };
    sd.slipR.onClick = [this, outgoing] { nudgeSlip(outgoing, 1); };
    sd.toEdit.onClick = [this, outgoing] {
        if (playhead_ < 0) return;
        if (outgoing) s_.trimRegionEnd(out_, playhead_); else s_.trimRegionStart(in_, playhead_);
        changed(prefs_.autoAudition, playhead_);
    };
}

void CrossfadeEditor::sideParamChanged(bool outgoing) {
    if (updating_) return;
    if (single()) {
        auto* r = s_.findRegion(outgoing ? out_ : in_);
        if (!r) return;
        Side& sd = outgoing ? o_ : i_;
        const auto f = s_.fadesFor(s_.indexOfRegion(r->id));
        const double len = sd.len.getValue() / 1000.0;
        if (outgoing) {
            if (std::abs(len - f.out) > 1e-5) { r->fadeOut = len; r->fadeOutManual = true; }
            r->fadeOutOffset = sd.off.getValue() / 1000.0;
            r->fadeOutShape = FadeShape(sd.shape.getSelectedId() - 1);
            r->fadeOutDb = sd.db.getValue();
            r->fadeOutAlpha = sd.alpha.getValue();
        } else {
            if (std::abs(len - f.in) > 1e-5) { r->fadeIn = len; r->fadeInManual = true; }
            r->fadeInOffset = sd.off.getValue() / 1000.0;
            r->fadeInShape = FadeShape(sd.shape.getSelectedId() - 1);
            r->fadeInDb = sd.db.getValue();
            r->fadeInAlpha = sd.alpha.getValue();
        }
        changed(false);
        return;
    }
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    if (!a || !b) return;
    Side& sd = outgoing ? o_ : i_;
    const auto shape = FadeShape(sd.shape.getSelectedId() - 1);
    const double len = sd.len.getValue() / 1000.0, off = sd.off.getValue() / 1000.0, db = sd.db.getValue(), alpha = sd.alpha.getValue();
    // Which control moved decides whether the length becomes fixed.
    const int ia = s_.indexOfRegion(out_), ib = s_.indexOfRegion(in_);
    const auto fa = s_.fadesFor(ia), fb = s_.fadesFor(ib);
    if (outgoing) {
        if (std::abs(len - fa.out) > 1e-5) { a->fadeOut = len; a->fadeOutManual = true; }
        a->fadeOutOffset = off;
        a->fadeOutShape = shape;
        a->fadeOutDb = db;
        a->fadeOutAlpha = alpha;
    } else {
        if (std::abs(len - fb.in) > 1e-5) { b->fadeIn = len; b->fadeInManual = true; }
        b->fadeInOffset = off;
        b->fadeInShape = shape;
        b->fadeInDb = db;
        b->fadeInAlpha = alpha;
    }
    if (prefs_.powerLock) {   // the other fade mirrors the shape settings
        if (outgoing) { b->fadeInShape = shape; b->fadeInDb = db; b->fadeInAlpha = alpha; }
        else { a->fadeOutShape = shape; a->fadeOutDb = db; a->fadeOutAlpha = alpha; }
    }
    changed(false);
}

void CrossfadeEditor::nudgeEdit(bool outgoing, int dir) {
    const double d = prefs_.nudge[prefs_.nudgeSize] * dir;
    double at = -1;
    if (outgoing) { if (auto* a = s_.findRegion(out_)) { s_.trimRegionEnd(out_, a->end() + d); at = s_.findRegion(out_)->end(); } }
    else if (auto* b = s_.findRegion(in_)) { s_.trimRegionStart(in_, b->start + d); at = s_.findRegion(in_)->start; }
    changed(prefs_.autoAudition, at);
}

void CrossfadeEditor::nudgeSlip(bool outgoing, int dir) {
    // "<" plays later material earlier: the audio moves left under the fixed edit point.
    s_.slip(outgoing ? out_ : in_, -dir * prefs_.nudge[prefs_.nudgeSize]);
    if (single()) { changed(prefs_.autoAudition, only() ? edge() : -1); return; }
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    changed(prefs_.autoAudition, a && b ? 0.5 * (b->start + a->end()) : -1);
}

void CrossfadeEditor::changed(bool audition, double at) {
    refreshControls();
    resultDirty_ = true;
    startTimer(80);
    if (onChange) onChange();
    if (audition && onAudition && at >= 0) onAudition(at - 1.5, at + 1.5);
    repaint();
}

void CrossfadeEditor::recall(int slot) {
    if (!juce::isPositiveAndBelow(slot, 10) || !lib_[size_t(slot)].used) return;
    if (single()) applySingleFadePreset(s_, out_ ? out_ : in_, in_ != 0, lib_[size_t(slot)]);
    else applyFadePreset(s_, out_, in_, lib_[size_t(slot)], prefs_.lockSound, prefs_.ripple);
    changed(false);
}

const Region* CrossfadeEditor::only() const { return single() ? s_.findRegion(out_ ? out_ : in_) : nullptr; }

double CrossfadeEditor::edge() const {
    auto* r = only();
    return r ? (in_ ? r->start : r->end()) : 0.0;
}

double CrossfadeEditor::overlap() const {
    if (single()) return 0.0;
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    return a && b ? juce::jmax(0.0, a->end() - b->start) : 0.0;
}

juce::Range<double> CrossfadeEditor::viewRange() const {
    if (single()) {
        auto* r = only();
        if (!r) return {0, 1};
        const auto f = s_.fadesFor(s_.indexOfRegion(r->id));
        const double len = in_ ? f.inOffset + f.in : f.outOffset + f.out;
        // The fade and some silence (or the material) on the other side of the edge.
        const double c = in_ ? r->start + len / 2 : r->end() - len / 2;
        const double span = prefs_.autoZoom ? juce::jmax(0.2, len * 2.5) : zoomSpan_;
        return {c - span / 2, c + span / 2};
    }
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    if (!a || !b) return {0, 1};
    const double xs = juce::jmin(b->start, a->end()), xe = juce::jmax(a->end(), b->start);
    const double c = 0.5 * (xs + xe);
    const double span = prefs_.autoZoom ? juce::jmax(0.05, (xe - xs) * 3.0) : zoomSpan_;
    return {c - span / 2, c + span / 2};
}

void CrossfadeEditor::refreshControls() {
    updating_ = true;
    length_.setValue(overlap() * 1000.0, juce::dontSendNotification);
    ripple_.setSelectedId(int(prefs_.ripple) + 1, juce::dontSendNotification);
    ripple_.setEnabled(!prefs_.lockSound);
    lockSound_.setToggleState(prefs_.lockSound, juce::dontSendNotification);
    powerLock_.setToggleState(prefs_.powerLock, juce::dontSendNotification);
    autoZoom_.setToggleState(prefs_.autoZoom, juce::dontSendNotification);
    autoAud_.setToggleState(prefs_.autoAudition, juce::dontSendNotification);
    juce::TextButton* nb[] = {&nA_, &nB_, &nC_};
    for (int k = 0; k < 3; ++k) {
        nb[k]->setToggleState(prefs_.nudgeSize == k, juce::dontSendNotification);
        const double v = prefs_.nudge[k];
        nb[k]->setButtonText(juce::String::charToString(juce::juce_wchar('A' + k)) + " " +
                             (std::abs(v * 75 - std::round(v * 75)) < 1e-6 && v < 1 ? juce::String(juce::roundToInt(v * 75)) + "f" : juce::String(v * 1000, v < 0.01 ? 1 : 0) + "ms"));
    }
    const int ia = s_.indexOfRegion(out_), ib = s_.indexOfRegion(in_);
    {
        auto set = [](Side& sd, FadeShape sh, double len, double off, double db, double alpha) {
            sd.shape.setSelectedId(int(sh) + 1, juce::dontSendNotification);
            sd.len.setValue(len * 1000.0, juce::dontSendNotification);
            sd.off.setValue(off * 1000.0, juce::dontSendNotification);
            sd.db.setValue(db, juce::dontSendNotification);
            sd.alpha.setValue(alpha, juce::dontSendNotification);
            const bool custom = sh == FadeShape::Custom;
            sd.db.setEnabled(custom);
            sd.alpha.setEnabled(custom);
        };
        if (ia >= 0) {
            const auto fa = s_.fadesFor(ia);
            set(o_, fa.outShape, fa.out, fa.outOffset, fa.outDb, fa.outAlpha);
        }
        if (ib >= 0) {
            const auto fb = s_.fadesFor(ib);
            set(i_, fb.inShape, fb.in, fb.inOffset, fb.inDb, fb.inAlpha);
        }
    }
    updating_ = false;
}

void CrossfadeEditor::timerCallback() {
    stopTimer();
    if (resultDirty_) renderResult();
}

void CrossfadeEditor::renderResult() {
    resultDirty_ = false;
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    if (!a && !b) return;
    const Region* any = a ? a : b;
    const auto area = waveArea();
    const int cols = juce::jmax(1, area.getWidth());
    const auto vr = viewRange();
    // Only this lane: the result is this edit, not the mix of every lane.
    Session lane;
    lane.lanes = 1;
    lane.projectRate = s_.effectiveRate();
    for (auto r : s_.regions)
        if (r.lane == any->lane) { r.lane = 0; r.fxState = {}; lane.regions.add(r); }
    const double rate = vr.getLength() > 20 ? 4000.0 : vr.getLength() > 4 ? 11025.0 : 44100.0;
    ProgramRenderer pr(formats_);
    pr.setSession(lane, rate);
    const double t0 = juce::jmax(0.0, vr.getStart());
    const int n = juce::jmax(1, int((vr.getEnd() - t0) * rate));
    juce::AudioBuffer<float> buf(2, n);
    buf.clear();
    pr.seek(juce::int64(t0 * rate));
    pr.render(buf, 0, n);
    resMin_.assign(size_t(cols), 0.0f);
    resMax_.assign(size_t(cols), 0.0f);
    for (int c = 0; c < cols; ++c) {
        const double ta = vr.getStart() + vr.getLength() * c / cols, tb = vr.getStart() + vr.getLength() * (c + 1) / cols;
        int ia = int((ta - t0) * rate), ib = int((tb - t0) * rate);
        ia = juce::jlimit(0, n, ia);
        ib = juce::jlimit(ia, n, juce::jmax(ia + 1, ib));
        float lo = 0, hi = 0;
        for (int k = ia; k < ib && k < n; ++k) {
            const float v = 0.5f * (buf.getSample(0, k) + buf.getSample(1, k));
            lo = juce::jmin(lo, v);
            hi = juce::jmax(hi, v);
        }
        resMin_[size_t(c)] = lo;
        resMax_[size_t(c)] = hi;
    }
    resRange_ = vr;
    // Level through the edit against the level either side of it.
    auto rmsOver = [&](double x0, double x1) {
        int i0 = juce::jlimit(0, n, int((x0 - t0) * rate)), i1 = juce::jlimit(0, n, int((x1 - t0) * rate));
        if (i1 - i0 < 8) return -1.0;
        double sum = 0;
        for (int k = i0; k < i1; ++k) sum += 0.5 * (double(buf.getSample(0, k)) * buf.getSample(0, k) + double(buf.getSample(1, k)) * buf.getSample(1, k));
        return std::sqrt(sum / (i1 - i0));
    };
    if (!a || !b) {
        // One fade: where it is and how long; no level dip to measure.
        const auto f = s_.fadesFor(s_.indexOfRegion(any->id));
        const double len = b ? f.in : f.out, off = b ? f.inOffset : f.outOffset;
        info_.setText((b ? "Fade-in at " + msfString(any->start) : "Fade-out ending at " + msfString(any->end())) + "   length " +
                          juce::String(len * 1000.0, 1) + " ms" + (off > 0 ? "   offset " + juce::String(off * 1000.0, 1) + " ms" : juce::String()),
                      juce::dontSendNotification);
        repaint();
        return;
    }
    const double xs = juce::jmin(b->start, a->end()), xe = juce::jmax(a->end(), b->start);
    const double w = juce::jmax(0.02, xe - xs);
    const double before = rmsOver(xs - w, xs), mid = rmsOver(xs, juce::jmax(xe, xs + 0.005)), after = rmsOver(xe, xe + w);
    dipDb_ = (before > 1e-6 && after > 1e-6 && mid > 0) ? 20.0 * std::log10(mid / (0.5 * (before + after))) : 0.0;
    info_.setText("Edit at " + msfString(xs) + "   overlap " + juce::String(overlap() * 1000.0, 1) + " ms   level through the edit " +
                      (dipDb_ >= 0 ? "+" : "") + juce::String(dipDb_, 1) + " dB",
                  juce::dontSendNotification);
    repaint();
}

juce::Rectangle<int> CrossfadeEditor::waveArea() const {
    auto r = getLocalBounds().reduced(10);
    r.removeFromTop(34);
    r.removeFromBottom(250);
    return r;
}

void CrossfadeEditor::layoutSide(Side& sd, juce::Rectangle<int> r) {
    sd.title.setBounds(r.removeFromTop(20));
    auto row = [&](juce::Label& l, juce::Component& c) {
        auto x = r.removeFromTop(24);
        l.setBounds(x.removeFromLeft(74));
        c.setBounds(x.reduced(0, 1));
        r.removeFromTop(3);
    };
    row(sd.shapeLbl, sd.shape);
    row(sd.lenLbl, sd.len);
    row(sd.offLbl, sd.off);
    row(sd.dbLbl, sd.db);
    row(sd.alphaLbl, sd.alpha);
    auto x = r.removeFromTop(24);
    sd.editLbl.setBounds(x.removeFromLeft(74));
    sd.editL.setBounds(x.removeFromLeft(30));
    sd.editR.setBounds(x.removeFromLeft(30));
    x.removeFromLeft(6);
    sd.toEdit.setBounds(x.removeFromLeft(100));
    x.removeFromLeft(14);
    sd.slipLbl.setBounds(x.removeFromLeft(34));
    sd.slipL.setBounds(x.removeFromLeft(30));
    sd.slipR.setBounds(x.removeFromLeft(30));
}

void CrossfadeEditor::resized() {
    auto r = getLocalBounds().reduced(10);
    auto bar = r.removeFromTop(26);
    auto place = [&](juce::Component& c, int w) { c.setBounds(bar.removeFromLeft(w)); bar.removeFromLeft(4); };
    place(autoZoom_, 82);
    place(zoomOut_, 26);
    place(zoomIn_, 26);
    bar.removeFromLeft(8);
    place(lockSound_, 86);
    place(rippleLbl_, 34);
    place(ripple_, 170);
    bar.removeFromLeft(8);
    place(powerLock_, 84);
    place(align_, 88);
    bar.removeFromLeft(8);
    place(nudgeLbl_, 44);
    place(nA_, 50);
    place(nB_, 56);
    place(nC_, 56);
    place(autoAud_, 98);
    r.removeFromTop(8);

    auto bottom = r.removeFromBottom(250);
    auto top = bottom.removeFromTop(28);
    info_.setBounds(top.removeFromRight(420));
    lenLbl_.setBounds(top.removeFromLeft(70));
    length_.setBounds(top.removeFromLeft(340));
    bottom.removeFromTop(6);
    auto lastRow = bottom.removeFromBottom(26);
    audition_.setBounds(lastRow.removeFromRight(90));
    lastRow.removeFromRight(6);
    post_.setBounds(lastRow.removeFromRight(80));
    lastRow.removeFromRight(6);
    pre_.setBounds(lastRow.removeFromRight(80));
    libLbl_.setBounds(lastRow.removeFromLeft(50));
    libSlot_.setBounds(lastRow.removeFromLeft(200));
    lastRow.removeFromLeft(6);
    libApply_.setBounds(lastRow.removeFromLeft(64));
    lastRow.removeFromLeft(6);
    libSave_.setBounds(lastRow.removeFromLeft(110));
    bottom.removeFromBottom(6);
    if (single()) {
        layoutSide(out_ ? o_ : i_, bottom.removeFromLeft(bottom.getWidth() / 2 - 10));
        return;
    }
    const int half = bottom.getWidth() / 2;
    layoutSide(o_, bottom.removeFromLeft(half - 10));
    bottom.removeFromLeft(20);
    layoutSide(i_, bottom);
}

void CrossfadeEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (!waveArea().contains(e.getPosition())) return;
    if (prefs_.autoZoom) { zoomSpan_ = viewRange().getLength(); prefs_.autoZoom = false; refreshControls(); }
    zoomSpan_ = juce::jlimit(0.005, 120.0, zoomSpan_ * (w.deltaY > 0 ? 1 / 1.15 : 1.15));
    resultDirty_ = true;
    startTimer(60);
    repaint();
}

void CrossfadeEditor::paintSide(juce::Graphics& g, juce::Rectangle<int> area, const Region& r, bool outgoing, double t0, double t1) {
    g.setColour(colours::bg);
    g.fillRect(area);
    auto xOf = [&](double t) { return float(area.getX() + (t - t0) / (t1 - t0) * area.getWidth()); };
    // The whole source around the region, dimmed, so you can see the handles.
    if (auto* th = pool_.get(r.file, nullptr)) {
        double srcAtT0 = double(r.srcIn) / r.fileRate + (t0 - r.start);
        double srcAtT1 = srcAtT0 + (t1 - t0);
        double fileLen = double(r.fileLength) / r.fileRate;
        double a = juce::jmax(0.0, srcAtT0), b = juce::jmin(fileLen, srcAtT1);
        if (b > a) {
            auto full = area.withX(int(xOf(t0 + (a - srcAtT0)))).withRight(int(xOf(t0 + (b - srcAtT0))));
            g.setColour(colours::wave.withAlpha(0.25f));
            th->drawChannels(g, full.reduced(0, 4), a, b, 1.0f);
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
    int idx = s_.indexOfRegion(r.id);
    auto f = s_.fadesFor(idx);
    juce::Path p;
    for (int k = 0; k <= 200; ++k) {
        double t = t0 + (t1 - t0) * k / 200.0;
        double gain = (t >= r.start && t <= r.end()) ? f.gainAt(t - r.start, r.length()) : 0.0;
        float y = float(area.getBottom() - gain * area.getHeight());
        if (k == 0) p.startNewSubPath(xOf(t), y); else p.lineTo(xOf(t), y);
    }
    g.setColour(colours::envelope);
    g.strokePath(p, juce::PathStrokeType(1.5f));
    // This side's edit point.
    const double ep = outgoing ? r.end() : r.start;
    g.setColour(juce::Colour(0xffe8c547));
    g.drawLine(xOf(ep), float(area.getY()), xOf(ep), float(area.getBottom()), 1.5f);
    g.setColour(colours::dim);
    g.drawText((single() ? (outgoing ? "FADE-OUT  " : "FADE-IN  ") : (outgoing ? "OUTGOING  " : "INCOMING  ")) + r.name, area.reduced(6, 4),
               juce::Justification::topLeft);
}

void CrossfadeEditor::paintResult(juce::Graphics& g, juce::Rectangle<int> area, double t0, double t1) {
    g.setColour(colours::bg);
    g.fillRect(area);
    g.setColour(colours::line);
    g.drawHorizontalLine(area.getCentreY(), float(area.getX()), float(area.getRight()));
    if (!resMin_.empty() && std::abs(resRange_.getStart() - t0) < 1e-9 && std::abs(resRange_.getEnd() - t1) < 1e-9) {
        g.setColour(colours::ok.withAlpha(0.85f));
        const float mid = float(area.getCentreY()), h = float(area.getHeight()) * 0.48f;
        for (size_t c = 0; c < resMin_.size(); ++c) {
            const float x = float(area.getX()) + float(c);
            g.drawVerticalLine(int(x), mid - resMax_[c] * h, mid - resMin_[c] * h + 1.0f);
        }
    }
    g.setColour(colours::dim);
    g.drawText("RESULT", area.reduced(6, 4), juce::Justification::topLeft);
}

void CrossfadeEditor::paint(juce::Graphics& g) {
    g.fillAll(colours::panel);
    auto* a = s_.findRegion(out_);
    auto* b = s_.findRegion(in_);
    if (single()) {
        auto* r = only();
        if (!r) return;
        auto area = waveArea();
        const auto vr = viewRange();
        const double t0 = vr.getStart(), t1 = vr.getEnd();
        const int h = (area.getHeight() - 4) / 2;
        paintSide(g, area.removeFromTop(h), *r, out_ != 0, t0, t1);
        area.removeFromTop(4);
        paintResult(g, area, t0, t1);
        auto full = waveArea();
        auto xOf = [&](double t) { return float(full.getX() + (t - t0) / (t1 - t0) * full.getWidth()); };
        const auto f = s_.fadesFor(s_.indexOfRegion(r->id));
        const double xs = in_ ? r->start + f.inOffset : r->end() - f.outOffset - f.out, xe = xs + (in_ ? f.in : f.out);
        g.setColour(colours::accent.withAlpha(0.12f));
        g.fillRect(juce::Rectangle<float>(xOf(xs), float(full.getY()), juce::jmax(1.0f, xOf(xe) - xOf(xs)), float(full.getHeight())));
        if (playhead_ >= t0 && playhead_ <= t1) {
            g.setColour(colours::text);
            g.drawVerticalLine(int(xOf(playhead_)), float(full.getY()), float(full.getBottom()));
        }
        return;
    }
    if (!a || !b) return;
    auto area = waveArea();
    const auto vr = viewRange();
    const double t0 = vr.getStart(), t1 = vr.getEnd();
    const int h = (area.getHeight() - 8) / 3;
    auto top = area.removeFromTop(h);
    area.removeFromTop(4);
    auto midA = area.removeFromTop(h);
    area.removeFromTop(4);
    paintSide(g, top, *a, true, t0, t1);
    paintSide(g, midA, *b, false, t0, t1);
    paintResult(g, area, t0, t1);
    auto full = waveArea();
    auto xOf = [&](double t) { return float(full.getX() + (t - t0) / (t1 - t0) * full.getWidth()); };
    const double xs = juce::jmin(b->start, a->end()), xe = juce::jmax(a->end(), b->start);
    g.setColour(colours::accent.withAlpha(0.12f));
    g.fillRect(juce::Rectangle<float>(xOf(xs), float(full.getY()), juce::jmax(1.0f, xOf(xe) - xOf(xs)), float(full.getHeight())));
    if (playhead_ >= t0 && playhead_ <= t1) {
        g.setColour(colours::text);
        g.drawVerticalLine(int(xOf(playhead_)), float(full.getY()), float(full.getBottom()));
    }
}

}  // namespace lathe
