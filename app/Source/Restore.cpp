#include "Restore.h"

namespace lathe {

static constexpr double kMargin = 1.0;   // seconds of context kept either side of a processed region

bool readRegionAudio(juce::AudioFormatManager& fm, const Region& r, dsp::Channels& out, juce::int64& first, double marginSec) {
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(r.file));
    if (!rd) return false;
    auto m = juce::int64(marginSec * r.fileRate);
    first = juce::jmax<juce::int64>(0, r.srcIn - m);
    auto last = juce::jmin<juce::int64>(rd->lengthInSamples, r.srcOut + m);
    const int n = int(last - first);
    out.assign(2, std::vector<double>(size_t(n), 0.0));
    juce::AudioBuffer<float> buf(2, 65536);
    for (int p = 0; p < n; p += 65536) {
        int k = juce::jmin(65536, n - p);
        rd->read(&buf, 0, k, first + p, true, true);
        if (rd->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, k);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < k; ++i) out[size_t(ch)][size_t(p + i)] = buf.getSample(ch, i);
    }
    return true;
}

static bool writeChannels(const juce::File& f, const dsp::Channels& ch, double rate) {
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
    if (!os) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate, 2, 32, {}, 0));
    if (!w) return false;
    os.release();
    const size_t n = ch[0].size();
    juce::AudioBuffer<float> b(2, 65536);
    for (size_t p = 0; p < n; p += 65536) {
        int k = int(std::min<size_t>(65536, n - p));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < k; ++i) b.setSample(c, i, float(ch[size_t(c)][p + size_t(i)]));
        if (!w->writeFromAudioSampleBuffer(b, 0, k)) return false;
    }
    return true;
}

static juce::File nextFile(const juce::File& folder, const Region& r, const juce::String& what) {
    auto base = r.name.upToFirstOccurrenceOf(" (", false, false);
    return folder.getNonexistentChildFile(juce::File::createLegalFileName(base + " " + what), ".wav", false);
}

RestoreResult restoreRegion(juce::AudioFormatManager& fm, const Region& r, juce::Range<double> range, const RestoreSettings& s,
                            const dsp::NoiseProfile* noise, const juce::File& folder, const std::function<bool(double)>& progress) {
    RestoreResult res;
    dsp::Channels orig;
    juce::int64 first = 0;
    if (!readRegionAudio(fm, r, orig, first, kMargin)) { res.error = "Couldn't read " + r.file.getFullPathName(); return res; }
    const double rate = r.fileRate;
    // Part of the buffer to process (buffer-local samples), with context either side.
    juce::int64 a = 0, b = juce::int64(orig[0].size());
    if (!range.isEmpty()) {
        a = juce::jlimit<juce::int64>(0, b, r.srcIn - first + juce::int64((range.getStart() - r.start) * rate));
        b = juce::jlimit<juce::int64>(a, b, r.srcIn - first + juce::int64((range.getEnd() - r.start) * rate));
        if (b <= a) { res.error = "The selection doesn't overlap this region."; return res; }
    }
    const auto ctx = juce::int64(0.5 * rate);
    const auto pa = juce::jmax<juce::int64>(0, a - ctx), pb = juce::jmin<juce::int64>(juce::int64(orig[0].size()), b + ctx);
    dsp::Channels work(2);
    for (int c = 0; c < 2; ++c) work[size_t(c)].assign(orig[size_t(c)].begin() + pa, orig[size_t(c)].begin() + pb);

    juce::StringArray done;
    auto step = [&](double p) { return !progress || progress(p); };
    if (s.dc) { dsp::removeDc(work); done.add("DC removed"); }
    if (!step(0.1)) { res.error = "Cancelled."; return res; }
    if (s.hum) {
        double f = dsp::removeHum(work, rate, s.humF0, s.humHarmonics, 30);
        done.add(f > 0 ? "Hum removed at " + juce::String(f, 0) + " Hz (+" + juce::String(s.humHarmonics - 1) + " harmonics)" : "No hum found");
    }
    if (!step(0.25)) { res.error = "Cancelled."; return res; }
    if (s.declick) {
        auto st = dsp::declick(work, rate, s.declickSensitivity, s.declickMaxMs);
        done.add(juce::String(st.found) + " click(s) repaired (" + juce::String(st.samplesRepaired) + " samples)");
    }
    if (!step(0.5)) { res.error = "Cancelled."; return res; }
    if (s.decrackle) {
        auto st = dsp::decrackle(work, rate, s.crackleAmount);
        done.add(juce::String(st.found) + " crackle event(s) repaired");
    }
    if (!step(0.7)) { res.error = "Cancelled."; return res; }
    if (s.denoise) {
        if (!noise || !noise->valid()) { res.error = "Denoise needs a noise profile: select a noise-only stretch and press Learn."; return res; }
        if (std::abs(noise->rate - rate) > 0.5) { res.error = "The noise profile was learned at a different sample rate."; return res; }
        dsp::denoise(work, rate, *noise, s.reductionDb, s.sensitivity);
        done.add("Noise reduced (up to " + juce::String(s.reductionDb, 0) + " dB)");
    }
    if (done.isEmpty()) { res.error = "Nothing selected to do."; return res; }

    // Splice the processed span [a,b) back with 5 ms crossfades; everything else stays exact.
    dsp::Channels out = orig;
    const auto xf = juce::jmax<juce::int64>(1, juce::int64(0.005 * rate));
    for (int c = 0; c < 2; ++c)
        for (juce::int64 k = a; k < b; ++k) {
            double w = 1.0;
            if (range.isEmpty()) w = 1.0;
            else if (k - a < xf) w = double(k - a) / double(xf);
            else if (b - k <= xf) w = double(b - k) / double(xf);
            out[size_t(c)][size_t(k)] = orig[size_t(c)][size_t(k)] * (1 - w) + work[size_t(c)][size_t(k - pa)] * w;
        }
    if (range.isEmpty())   // whole region: take the processed context too (keeps trims smooth)
        for (int c = 0; c < 2; ++c)
            for (juce::int64 k = pa; k < pb; ++k) out[size_t(c)][size_t(k)] = work[size_t(c)][size_t(k - pa)];
    if (!step(0.9)) { res.error = "Cancelled."; return res; }
    res.file = nextFile(folder, r, "restored");
    if (!writeChannels(res.file, out, rate)) { res.error = "Couldn't write " + res.file.getFullPathName(); return res; }
    res.fileLength = juce::int64(out[0].size());
    res.srcIn = r.srcIn - first;
    res.srcOut = r.srcOut - first;
    res.report = done.joinIntoString("\n");
    res.ok = true;
    return res;
}

RestoreResult spectralEditRegion(juce::AudioFormatManager& fm, const Region& r, double t0, double t1, double f0, double f1,
                                 dsp::SpectralMode mode, double gainDb, const juce::File& folder) {
    RestoreResult res;
    dsp::Channels ch;
    juce::int64 first = 0;
    if (!readRegionAudio(fm, r, ch, first, kMargin)) { res.error = "Couldn't read " + r.file.getFullPathName(); return res; }
    const double off = double(r.srcIn - first) / r.fileRate;   // region-local t -> buffer t
    dsp::spectralEdit(ch, r.fileRate, t0 + off, t1 + off, f0, f1, mode, gainDb);
    res.file = nextFile(folder, r, "spectral");
    if (!writeChannels(res.file, ch, r.fileRate)) { res.error = "Couldn't write " + res.file.getFullPathName(); return res; }
    res.fileLength = juce::int64(ch[0].size());
    res.srcIn = r.srcIn - first;
    res.srcOut = r.srcOut - first;
    res.report = juce::String(mode == dsp::SpectralMode::Heal ? "Healed " : "Attenuated ") + juce::String(f0, 0) + "-" + juce::String(f1, 0) +
                 " Hz from " + juce::String(t0, 2) + " to " + juce::String(t1, 2) + " s";
    res.ok = true;
    return res;
}

void applyProcessed(Session& s, int id, const RestoreResult& res, const juce::String& suffix) {
    auto* r = s.findRegion(id);
    if (!r || !res.ok) return;
    r->file = res.file;
    r->fileLength = res.fileLength;
    r->srcIn = res.srcIn;
    r->srcOut = res.srcOut;
    if (!r->name.contains("(" + suffix + ")")) r->name << " (" << suffix << ")";
}

// ================================================================= RestorePanel

static void setupSlider(juce::Slider& s, double lo, double hi, double step, double v, const juce::String& suffix) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    s.setRange(lo, hi, step);
    s.setValue(v, juce::dontSendNotification);
    s.setTextValueSuffix(suffix);
}

RestorePanel::RestorePanel(const juce::String& target, bool haveProfile, bool haveSelection) {
    title_.setText("Restore: " + target, juce::dontSendNotification);
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(title_);
    dc_.setButtonText("Remove DC offset");
    hum_.setButtonText("Remove hum");
    declick_.setButtonText("Declick");
    decrackle_.setButtonText("Decrackle");
    denoise_.setButtonText("Denoise (broadband: hiss, air, preamp noise)");
    selOnly_.setButtonText("Only the destination selection (with 5 ms crossfades)");
    declick_.setToggleState(true, juce::dontSendNotification);
    selOnly_.setToggleState(haveSelection, juce::dontSendNotification);
    selOnly_.setEnabled(haveSelection);
    for (auto* t : {&dc_, &hum_, &declick_, &decrackle_, &denoise_, &selOnly_}) {
        t->setColour(juce::ToggleButton::textColourId, colours::text);
        t->setColour(juce::ToggleButton::tickColourId, colours::accent);
        addAndMakeVisible(*t);
    }
    humF0_.addItem("Auto (50 / 60 Hz)", 1);
    humF0_.addItem("50 Hz", 2);
    humF0_.addItem("60 Hz", 3);
    humF0_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(humF0_);
    setupSlider(clickSens_, 1, 10, 0.5, 5, "");
    setupSlider(clickLen_, 0.1, 5, 0.1, 1.0, " ms");
    setupSlider(crackle_, 0, 10, 0.5, 4, "");
    setupSlider(reduction_, 3, 30, 0.5, 12, " dB");
    setupSlider(sens_, 0.5, 3, 0.05, 1.2, "x");
    for (auto* s : {&clickSens_, &clickLen_, &crackle_, &reduction_, &sens_}) addAndMakeVisible(*s);
    l1_.setText("Sensitivity", juce::dontSendNotification);
    l2_.setText("Longest click", juce::dontSendNotification);
    l3_.setText("Amount", juce::dontSendNotification);
    l4_.setText("Reduction", juce::dontSendNotification);
    l5_.setText("Noise estimate", juce::dontSendNotification);
    for (auto* l : {&l1_, &l2_, &l3_, &l4_, &l5_, &profile_}) { l->setColour(juce::Label::textColourId, colours::dim); addAndMakeVisible(*l); }
    profile_.setText(haveProfile ? "Noise profile: learned" : "Noise profile: none yet (select noise only, then Learn)", juce::dontSendNotification);
    for (auto* b : {&learn_, &process_}) { styleButton(*b, b == &process_); addAndMakeVisible(*b); }
    learn_.onClick = [this] { if (onLearnNoise) onLearnNoise(); };
    process_.onClick = [this] { if (onProcess) onProcess(); };
    setSize(560, 470);
}

RestoreSettings RestorePanel::settings() const {
    RestoreSettings s;
    s.dc = dc_.getToggleState();
    s.hum = hum_.getToggleState();
    s.humF0 = humF0_.getSelectedId() == 2 ? 50 : humF0_.getSelectedId() == 3 ? 60 : 0;
    s.declick = declick_.getToggleState();
    s.declickSensitivity = clickSens_.getValue();
    s.declickMaxMs = clickLen_.getValue();
    s.decrackle = decrackle_.getToggleState();
    s.crackleAmount = crackle_.getValue();
    s.denoise = denoise_.getToggleState();
    s.reductionDb = reduction_.getValue();
    s.sensitivity = sens_.getValue();
    return s;
}

void RestorePanel::paint(juce::Graphics& g) { g.fillAll(colours::bg); }

void RestorePanel::resized() {
    auto r = getLocalBounds().reduced(12);
    title_.setBounds(r.removeFromTop(24));
    r.removeFromTop(6);
    auto line = [&](juce::Component& c, int h = 24) { c.setBounds(r.removeFromTop(h)); r.removeFromTop(2); };
    auto param = [&](juce::Label& l, juce::Component& c) {
        auto x = r.removeFromTop(24);
        x.removeFromLeft(24);
        l.setBounds(x.removeFromLeft(110));
        c.setBounds(x);
        r.removeFromTop(2);
    };
    line(dc_);
    auto h = r.removeFromTop(24);
    hum_.setBounds(h.removeFromLeft(160));
    humF0_.setBounds(h.removeFromLeft(180));
    r.removeFromTop(4);
    line(declick_);
    param(l1_, clickSens_);
    param(l2_, clickLen_);
    line(decrackle_);
    param(l3_, crackle_);
    line(denoise_);
    param(l4_, reduction_);
    param(l5_, sens_);
    auto p = r.removeFromTop(26);
    p.removeFromLeft(24);
    learn_.setBounds(p.removeFromLeft(200));
    p.removeFromLeft(8);
    profile_.setBounds(p);
    r.removeFromTop(8);
    line(selOnly_);
    process_.setBounds(r.removeFromBottom(28).removeFromRight(130));
}

// ================================================================= SpectralView

SpectralView::SpectralView(AudioEngine& e, Session& s, int id, juce::Range<double> view) : e_(e), s_(s), id_(id), view_(view) {
    for (auto* b : {&attenuate_, &heal_, &play_, &scale_}) { styleButton(*b, b == &heal_); addAndMakeVisible(*b); }
    setupSlider(gain_, -60, 0, 0.5, -24, " dB");
    addAndMakeVisible(gain_);
    info_.setColour(juce::Label::textColourId, colours::dim);
    addAndMakeVisible(info_);
    info_.setText("Drag a box around the sound to remove (time x frequency). Heal rebuilds it from its surroundings; Attenuate turns it down.",
                  juce::dontSendNotification);
    auto apply = [this](dsp::SpectralMode m) {
        if (sel_.isEmpty() || !onApply) return;
        double t0 = xToT(sel_.getX()), t1 = xToT(sel_.getRight());
        double f1 = yToF(sel_.getY()), f0 = yToF(sel_.getBottom());
        onApply(t0, t1, f0, f1, m, gain_.getValue());
    };
    attenuate_.onClick = [apply] { apply(dsp::SpectralMode::Attenuate); };
    heal_.onClick = [apply] { apply(dsp::SpectralMode::Heal); };
    play_.onClick = [this] {
        auto* r = s_.findRegion(id_);
        if (!r || !onAudition) return;
        double t0 = sel_.isEmpty() ? view_.getStart() : xToT(sel_.getX()), t1 = sel_.isEmpty() ? view_.getEnd() : xToT(sel_.getRight());
        onAudition(r->start + t0 - 0.75, r->start + t1 + 0.75);
    };
    scale_.onClick = [this] { logFreq_ = !logFreq_; render(); repaint(); };
    setSize(1000, 560);
    refresh();
}

juce::Rectangle<int> SpectralView::plot() const { return getLocalBounds().reduced(10).withTrimmedBottom(70).withTrimmedLeft(46); }

double SpectralView::xToT(float x) const {
    auto p = plot();
    return view_.getStart() + view_.getLength() * double(x - p.getX()) / juce::jmax(1, p.getWidth());
}

double SpectralView::yToF(float y) const {
    auto p = plot();
    double f = 1.0 - double(y - p.getY()) / juce::jmax(1, p.getHeight());
    double ny = rate_ / 2;
    return logFreq_ ? 20.0 * std::pow(ny / 20.0, juce::jlimit(0.0, 1.0, f)) : f * ny;
}

float SpectralView::fToY(double f) const {
    auto p = plot();
    double ny = rate_ / 2;
    double n = logFreq_ ? std::log(juce::jmax(20.0, f) / 20.0) / std::log(ny / 20.0) : f / ny;
    return float(p.getY() + (1.0 - juce::jlimit(0.0, 1.0, n)) * p.getHeight());
}

void SpectralView::refresh() {
    auto* r = s_.findRegion(id_);
    if (!r) return;
    rate_ = r->fileRate;
    render();
    repaint();
}

void SpectralView::render() {
    auto* r = s_.findRegion(id_);
    auto p = plot();
    if (!r || p.isEmpty()) return;
    dsp::Channels ch;
    juce::int64 first = 0;
    if (!readRegionAudio(e_.formats(), *r, ch, first, 0)) return;
    // Cut the view out of the region.
    auto a = juce::jlimit<juce::int64>(0, juce::int64(ch[0].size()), juce::int64(view_.getStart() * rate_));
    auto b = juce::jlimit<juce::int64>(a, juce::int64(ch[0].size()), juce::int64(view_.getEnd() * rate_));
    for (auto& c : ch) c = std::vector<double>(c.begin() + a, c.begin() + b);
    auto sg = dsp::spectrogram(ch, rate_, p.getWidth());
    img_ = juce::Image(juce::Image::RGB, juce::jmax(1, p.getWidth()), juce::jmax(1, p.getHeight()), true);
    if (sg.db.empty()) return;
    const int bins = int(sg.db[0].size());
    for (int x = 0; x < img_.getWidth(); ++x) {
        auto& col = sg.db[size_t(juce::jmin(int(sg.db.size()) - 1, x * int(sg.db.size()) / img_.getWidth()))];
        for (int y = 0; y < img_.getHeight(); ++y) {
            double f = yToF(float(p.getY() + y));
            int bin = juce::jlimit(0, bins - 1, int(f / (rate_ / sg.fftSize)));
            float v = juce::jlimit(0.0f, 1.0f, (col[size_t(bin)] + 110.0f) / 100.0f);
            // Dark blue -> purple -> orange -> pale yellow.
            auto c = v < 0.5f ? juce::Colour(0xff0b0f24).interpolatedWith(juce::Colour(0xff6a2c8a), v * 2)
                              : juce::Colour(0xff6a2c8a).interpolatedWith(juce::Colour(0xfff6c35b), (v - 0.5f) * 2).interpolatedWith(juce::Colours::white, juce::jmax(0.0f, v - 0.9f) * 4);
            img_.setPixelAt(x, y, c);
        }
    }
}

void SpectralView::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    auto p = plot();
    g.drawImageAt(img_, p.getX(), p.getY());
    g.setColour(colours::dim);
    g.setFont(10.0f);
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (f > rate_ / 2) break;
        float y = fToY(f);
        g.drawText(f >= 1000 ? juce::String(f / 1000, 0) + "k" : juce::String(f, 0), getLocalBounds().getX() + 4, int(y) - 6, 40, 12, juce::Justification::right);
        g.setColour(colours::dim.withAlpha(0.25f));
        g.drawHorizontalLine(int(y), float(p.getX()), float(p.getRight()));
        g.setColour(colours::dim);
    }
    g.drawText(msfString(view_.getStart()), p.getX(), p.getBottom() + 2, 80, 12, juce::Justification::left);
    g.drawText(msfString(view_.getEnd()), p.getRight() - 80, p.getBottom() + 2, 80, 12, juce::Justification::right);
    if (!sel_.isEmpty()) {
        g.setColour(colours::accent.withAlpha(0.2f));
        g.fillRect(sel_);
        g.setColour(colours::accent);
        g.drawRect(sel_, 1.5f);
        g.setFont(11.0f);
        g.drawText(juce::String(yToF(sel_.getBottom()), 0) + "-" + juce::String(yToF(sel_.getY()), 0) + " Hz, " +
                       juce::String((xToT(sel_.getRight()) - xToT(sel_.getX())) * 1000, 0) + " ms",
                   sel_.toNearestInt().translated(0, -14).withHeight(14).withWidth(220), juce::Justification::left);
    }
    // Playhead (if inside the view).
    if (auto* r = s_.findRegion(id_)) {
        double t = e_.positionSeconds() - r->start;
        if (view_.contains(t)) {
            float x = p.getX() + float((t - view_.getStart()) / view_.getLength() * p.getWidth());
            g.setColour(colours::text);
            g.drawVerticalLine(int(x), float(p.getY()), float(p.getBottom()));
        }
    }
}

void SpectralView::resized() {
    auto r = getLocalBounds().reduced(10);
    auto bottom = r.removeFromBottom(54);
    info_.setBounds(bottom.removeFromTop(20));
    auto b = bottom.removeFromBottom(28);
    heal_.setBounds(b.removeFromLeft(120));
    b.removeFromLeft(6);
    attenuate_.setBounds(b.removeFromLeft(100));
    gain_.setBounds(b.removeFromLeft(240));
    b.removeFromLeft(12);
    play_.setBounds(b.removeFromLeft(120));
    scale_.setBounds(b.removeFromRight(110));
    render();
}

void SpectralView::mouseDown(const juce::MouseEvent& e) {
    if (!plot().contains(e.getPosition())) return;
    dragFrom_ = e.position;
    sel_ = {};
    repaint();
}

void SpectralView::mouseDrag(const juce::MouseEvent& e) {
    auto p = plot().toFloat();
    auto to = juce::Point<float>(juce::jlimit(p.getX(), p.getRight(), e.position.x), juce::jlimit(p.getY(), p.getBottom(), e.position.y));
    sel_ = juce::Rectangle<float>(dragFrom_, to);
    repaint();
}

}  // namespace lathe
