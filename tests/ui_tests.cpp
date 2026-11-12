// Windows and views: opening and closing them in the states that used to go wrong, plus the
// timeline and counter logic that needs real components.
#include "../app/Source/DeliveryPanels.h"
#include "../app/Source/EditPanels.h"
#include "../app/Source/MarkInfo.h"
#include "../app/Source/Metering.h"
#include "../app/Source/SegmentList.h"
#include "../app/Source/TimecodeStrip.h"

#include <cstdio>
#include <cstdlib>

using namespace lathe;

static int failures = 0, checks = 0;

// With LATHE_UI_SNAP set to a folder, views are also saved there as PNGs (to look at them).
static void snap(juce::Component& c, const juce::String& name) {
    if (auto* dir = std::getenv("LATHE_UI_SNAP")) {
        auto img = c.createComponentSnapshot(c.getLocalBounds(), true, 1.5f);
        juce::File f(juce::File(dir).getChildFile(name + ".png"));
        f.deleteFile();
        juce::FileOutputStream os(f);
        juce::PNGImageFormat().writeImageToStream(img, os);
    }
}
#define CHECK(c)                                                                     \
    do {                                                                             \
        ++checks;                                                                    \
        if (!(c)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

namespace {
Session demoSession(const juce::File& dir) {
    Session s;
    // three regions on silent files, each with a track marker
    for (int i = 0; i < 3; ++i) {
        auto f = dir.getChildFile("t" + juce::String(i) + ".wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 24, {}, 0));
            os.release();
            juce::AudioBuffer<float> b(2, 44100 * 3);
            b.clear();
            w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
        }
        auto r = s.makeRegion(f, 44100, 44100 * 3);
        r.name = "Song " + juce::String(i + 1);
        s.append(r, i == 0 ? 2.0 : 1.0);
    }
    for (auto& r : s.regions) s.addMarker(Marker::TrackStart, r.start);
    return s;
}

// Puts the panel in a window (on screen when there is one), starts editing the first editable
// cell, then closes the window with the edit still open: the case that crashed.
template <typename Panel>
bool closeWhileEditing(std::unique_ptr<Panel> panel, const char* name) {
    auto* raw = panel.get();
    raw->setSize(900, 500);
    auto window = std::make_unique<juce::DocumentWindow>(name, juce::Colours::black, juce::DocumentWindow::closeButton);
    window->setContentOwned(panel.release(), true);
   #if JUCE_MAC
    // On the Mac the window really opens, with its accessibility handlers (where the crash was).
    const bool screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr;
   #else
    const bool screen = false;
   #endif
    if (screen) window->setVisible(true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    juce::Label* edited = nullptr;
    std::function<void(juce::Component&)> find = [&](juce::Component& c) {
        for (auto* ch : c.getChildren()) {
            if (edited) return;
            if (auto* l = dynamic_cast<juce::Label*>(ch); l && l->isEditableOnDoubleClick() && l->isVisible()) { edited = l; return; }
            find(*ch);
        }
    };
    find(*raw);
    if (edited) {
        edited->showEditor();
        if (auto* ed = edited->getCurrentTextEditor()) ed->setText("12", false);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    window.reset();   // must not crash
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    std::printf("closed %s while editing a cell (%s, %s)\n", name, edited ? "cell found" : "no editable cell", screen ? "on screen" : "off screen");
    return true;
}
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lathe-ui-tests");
    dir.deleteRecursively();
    dir.createDirectory();
    {
        auto s = demoSession(dir);
        CHECK(closeWhileEditing(std::make_unique<MarkInfoPanel>(s), "Mark Info"));
        CHECK(closeWhileEditing(std::make_unique<TrackCreditsPanel>(s), "Credits"));
        CHECK(closeWhileEditing(std::make_unique<ProductsPanel>(s, [] { return static_cast<const dsp::LoudnessMap*>(nullptr); }), "Products"));
        CHECK(closeWhileEditing(std::make_unique<SegmentListPanel>(s), "Text View"));
    }
    // Edit Fade: finding fades, and editing a region's own fade-in or fade-out.
    {
        Session s;
        auto f = dir.getChildFile("fade.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 24, {}, 0));
            os.release();
            juce::AudioBuffer<float> b(2, 44100 * 10);
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, 0.5f), b.setSample(1, i, 0.5f);
            w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
        }
        // A (2..5 s) crossfades into B (4.9..8 s); C (10..12 s) stands alone. All with handles.
        auto mk = [&](double start, double len, juce::int64 in) {
            auto r = s.makeRegion(f, 44100, 44100 * 10);
            r.srcIn = in;
            r.srcOut = in + juce::int64(len * 44100);
            r.start = start;
            s.regions.add(r);
            return r.id;
        };
        const int A = mk(2.0, 3.0, 44100), B = mk(4.9, 3.1, 44100 * 2), C = mk(10.0, 2.0, 44100);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        ThumbPool pool(fm);
        Timeline tl(s, pool);
        CHECK(tl.fadeNear(4.95) == std::make_pair(A, B));     // the edit
        CHECK(tl.fadeNear(2.1) == std::make_pair(0, A));      // A's fade-in
        CHECK(tl.fadeNear(7.9) == std::make_pair(B, 0));      // B's fade-out
        CHECK(tl.fadeNear(10.2) == std::make_pair(0, C));     // C's fade-in
        CHECK(tl.fadeNear(11.9) == std::make_pair(C, 0));     // C's fade-out
        CHECK(tl.fadeNear(4.95, C) == std::make_pair(0, C));  // only C's
        CHECK(tl.fadeNear(5.0, B).first == A);                // B's nearest: the edit
        // A take being recorded: drawn as it grows, with the markers dropped on it.
        {
            s.lanes = juce::jmax(s.lanes, 2);
            Timeline live(s, pool);
            live.setSize(1200, 300);
            live.zoomToFit();
            Timeline::LiveTake t;
            t.active = true;
            t.lane = 0;
            t.underLane = true;
            t.start = 2.5;
            t.end = 9.0;
            t.peaksT0 = 2.0;
            t.peakDt = 0.01;
            t.peakCount = [] { return 700; };
            t.peak = [](int i) { return 0.2f + 0.6f * float(std::abs(std::sin(i * 0.02))); };
            t.name = "Album Capture 2026-11-12 101500";
            t.marks.add({Marker::TrackStart, 3.0});
            t.marks.add({Marker::Index, 6.2});
            live.setLiveTake(t);
            CHECK(live.liveTake().active && live.liveTake().marks.size() == 2);
            snap(live, "live-take");
            live.clearLiveTake();
            CHECK(!live.liveTake().active);
        }
        Session empty;
        Timeline te(empty, pool);
        CHECK(te.fadeNear(1.0) == std::make_pair(0, 0));

        FadeEditorPrefs prefs;
        auto lib = defaultFadeLibrary();
        {
            CrossfadeEditor ed(s, pool, fm, prefs, lib, 0, C);   // C's fade-in
            CHECK(ed.single() && ed.stillValid());
            int changes = 0;
            ed.onChange = [&] { ++changes; };
            // Set it to 250 ms, logarithmic, from the window's own controls.
            juce::Slider* len = nullptr;
            juce::ComboBox* shape = nullptr;
            int sliders = 0;
            for (auto* c : ed.getChildren()) {
                if (!c->isVisible()) continue;
                if (auto* sl = dynamic_cast<juce::Slider*>(c); sl && sliders++ == 0) len = sl;      // the side's Length (crossfade length is hidden)
                if (auto* cb = dynamic_cast<juce::ComboBox*>(c); cb && !shape && cb->getNumItems() == kNumFadeShapes) shape = cb;
            }
            CHECK(len && shape);
            if (len && shape) {
                len->setValue(250.0, juce::sendNotificationSync);
                shape->setSelectedId(int(FadeShape::Logarithmic) + 1, juce::sendNotificationSync);
                auto* r = s.findRegion(C);
                CHECK(std::abs(r->fadeIn - 0.25) < 1e-9 && r->fadeInManual && r->fadeInShape == FadeShape::Logarithmic && changes >= 2);
                CHECK(r->fadeOut == 0.0);   // the other end is left alone
                const auto fades = s.fadesFor(s.indexOfRegion(C));
                CHECK(std::abs(fades.gainAt(0.125, r->length()) - fadeGain(FadeShape::Logarithmic, 0.5)) < 1e-9);
            }
            ed.setSize(980, 680);
            auto img = ed.createComponentSnapshot(ed.getLocalBounds());
            CHECK(img.isValid());
            juce::Thread::sleep(300);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            snap(ed, "edit-fade-in");
            // A library slot recalled onto a single fade: its length and the in shape.
            FadePreset p;
            p.length = 0.04;
            p.inShape = FadeShape::SCurve;
            p.used = true;
            lib[3] = p;
            ed.recall(3);
            CHECK(std::abs(s.findRegion(C)->fadeIn - 0.04) < 1e-9 && s.findRegion(C)->fadeInShape == FadeShape::SCurve);
        }
        {
            CrossfadeEditor ed(s, pool, fm, prefs, lib, B, 0);   // B's fade-out
            ed.setSize(980, 680);
            CHECK(ed.single() && ed.createComponentSnapshot(ed.getLocalBounds()).isValid());
            applySingleFadePreset(s, B, false, lib[3]);
            CHECK(std::abs(s.findRegion(B)->fadeOut - 0.04) < 1e-9 && s.findRegion(B)->fadeOutShape == lib[3].outShape);
            s.regions.remove(s.indexOfRegion(B));
            CHECK(!ed.stillValid());   // its region is gone (an undo): the window closes
        }
    }

    // Timecode strip: every unit formats and reads back; typing sets In, Out and the length.
    {
        using U = TimecodeStrip::Units;
        const double t = 3723.5;   // 1 h 2 min 3.5 s
        CHECK(TimecodeStrip::format(t, U::CdFrames, 44100) == "62:03:38");
        CHECK(TimecodeStrip::format(t, U::Hms, 44100) == "1:02:03.500");
        CHECK(TimecodeStrip::format(t, U::Samples, 48000) == "178728000");
        CHECK(TimecodeStrip::format(t, U::Seconds, 44100) == "3723.500");
        CHECK(TimecodeStrip::format(t, U::Smpte25, 44100) == "01:02:03:12");
        CHECK(TimecodeStrip::format(t, U::Smpte30, 44100) == "01:02:03:15");
        CHECK(TimecodeStrip::format(t, U::Smpte24, 44100) == "01:02:03:12");
        for (int k = 0; k < TimecodeStrip::kNumUnits; ++k) {
            const auto u = U(k);
            for (double x : {0.0, 1.0 / 75 * 37, 59.96, 125.2, 3723.5}) {
                double back = -1;
                const double step = u == U::CdFrames ? 1.0 / 75 : u == U::Smpte24 ? 1.0 / 24 : u == U::Smpte25 ? 1.0 / 25 : u == U::Smpte30 ? 1.0 / 30 : 0.001;
                const bool ok = TimecodeStrip::parse(TimecodeStrip::format(x, u, 48000), u, 48000, back);
                CHECK(ok && std::abs(back - x) <= step + 1e-9);
            }
        }
        double v = 0;
        CHECK(TimecodeStrip::parse("02:30:00", U::CdFrames, 44100, v) && std::abs(v - 150.0) < 1e-9);
        CHECK(TimecodeStrip::parse("2:30.5", U::CdFrames, 44100, v) && std::abs(v - 150.5) < 1e-9);
        CHECK(TimecodeStrip::parse("96000", U::Samples, 48000, v) && std::abs(v - 2.0) < 1e-12);
        CHECK(!TimecodeStrip::parse("abc", U::Seconds, 44100, v) && !TimecodeStrip::parse("", U::Hms, 44100, v));

        TimecodeStrip strip;
        strip.setSize(1100, 64);
        TimecodeStrip::Values vals;
        vals.main = 125.4;
        vals.in = 62.0;
        vals.out = 190.5;
        vals.left = 478.9;
        vals.right = 1535.5;
        vals.leftWhat = "3 segments";
        strip.setValues(vals);
        snap(strip, "timecode-strip");
        double gotIn = -1, gotOut = -1, gotDur = -1, gotMain = -1;
        strip.onSetIn = [&](double x) { gotIn = x; };
        strip.onSetOut = [&](double x) { gotOut = x; };
        strip.onSetDuration = [&](double x) { gotDur = x; };
        strip.onSetMain = [&](double x) { gotMain = x; };
        auto type = [&](int field, const juce::String& text) {
            strip.startEditing(field);
            juce::TextEditor* ed = nullptr;
            for (auto* c : strip.getChildren()) if (auto* e = dynamic_cast<juce::TextEditor*>(c)) ed = e;
            CHECK(ed != nullptr);
            if (!ed) return;
            ed->setText(text, false);
            ed->onReturnKey();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        };
        type(1, "01:00:00");
        type(2, "03:00:00");
        type(3, "00:30:00");
        type(0, "10:00:00");
        CHECK(std::abs(gotIn - 60.0) < 1e-9 && std::abs(gotOut - 180.0) < 1e-9 && std::abs(gotDur - 30.0) < 1e-9 && std::abs(gotMain - 600.0) < 1e-9);
        gotIn = -1;
        type(1, "not a time");
        CHECK(gotIn == -1);   // nothing moves on a typo
    }

    // Meter Bridge: pausing must not shrink the meters (each repaint used to take a strip off them).
    {
        MeterHub out, in;
        out.prepare(48000);
        in.prepare(48000);
        MeterBridge bridge(out, &in);
        bridge.setSize(900, 560);
        bridge.setView(MeterBridge::View::Loudness);
        auto first = bridge.createComponentSnapshot(bridge.getLocalBounds());
        out.paused = true;
        juce::Image a, b;
        for (int i = 0; i < 6; ++i) a = bridge.createComponentSnapshot(bridge.getLocalBounds());
        for (int i = 0; i < 6; ++i) b = bridge.createComponentSnapshot(bridge.getLocalBounds());
        bool same = true;
        for (int y = 0; y < a.getHeight() && same; y += 3)
            for (int x = 0; x < a.getWidth(); x += 3)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) { same = false; break; }
        CHECK(same);
        juce::ignoreUnused(first);
    }
    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
