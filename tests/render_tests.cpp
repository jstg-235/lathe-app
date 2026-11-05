// Renderer tests: real WAV files through ProgramRenderer (gaps, gain, fades, SRC) and the
// DDP layout derived from the same session, so playback, export and PQ stay in step.
#include "../app/Source/ProgramRenderer.h"
#include "../core/ddp/Ddp.h"

#include <cmath>
#include <cstdio>

using namespace lathe;

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, t) CHECK(std::abs(double(a) - double(b)) <= (t))

static juce::File makeTone(const juce::File& dir, const juce::String& name, double rate, double sec, double hz, float amp) {
    auto f = dir.getChildFile(name);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate, 2, 24, {}, 0));
    os.release();
    const int n = int(rate * sec);
    juce::AudioBuffer<float> b(2, n);
    for (int i = 0; i < n; ++i) {
        float v = amp * float(std::sin(2 * juce::MathConstants<double>::pi * hz * i / rate));
        b.setSample(0, i, v);
        b.setSample(1, i, v);
    }
    w->writeFromAudioSampleBuffer(b, 0, n);
    return f;
}

static Region regionFor(Session& s, juce::AudioFormatManager& fm, const juce::File& f) {
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    return s.makeRegion(f, r->sampleRate, r->lengthInSamples);
}

static double rms(const juce::AudioBuffer<float>& b, int a, int n) {
    double s = 0;
    for (int i = a; i < a + n; ++i) s += double(b.getSample(0, i)) * b.getSample(0, i);
    return std::sqrt(s / n);
}

int main() {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lathe_render_test");
    dir.deleteRecursively();
    dir.createDirectory();

    // Two clips at 96 kHz rendered to 44.1 kHz: 2 s lead-in, 5 s tone, 1.5 s gap, 6 s tone at -6 dB gain.
    auto a = makeTone(dir, "a.wav", 96000, 5.0, 1000, 0.5f);
    auto b = makeTone(dir, "b.wav", 96000, 6.0, 1000, 0.5f);
    Session s;
    s.append(regionFor(s, fm, a), 2.0);
    Region rb = regionFor(s, fm, b);
    rb.gainDb = -6.0;
    int idB = s.append(rb, 1.5);

    ProgramRenderer r(fm);
    r.setSession(s, 44100);
    const int total = int(r.totalSamples());
    NEAR(total, 44100 * (2.0 + 5.0 + 1.5 + 6.0), 2);
    juce::AudioBuffer<float> out(2, total);
    for (int p = 0; p < total; p += 1000) CHECK(r.render(out, p, juce::jmin(1000, total - p)));

    const double sine = 0.5 / std::sqrt(2.0);
    NEAR(rms(out, 0, 44100), 0.0, 1e-6);                         // lead-in silence
    NEAR(rms(out, int(44100 * 3), 44100), sine, 0.002);          // clip A level preserved through SRC
    NEAR(rms(out, int(44100 * 7.2), 4410), 0.0, 1e-3);           // gap is silent
    NEAR(rms(out, int(44100 * 10), 44100), sine * 0.5012, 0.002);// clip B at -6 dB

    // The first non-silent sample should land on the clip start (SRC latency is compensated).
    int first = -1;
    for (int i = 0; i < total; ++i) if (std::abs(out.getSample(0, i)) > 1e-3f) { first = i; break; }
    NEAR(first, 2 * 44100, 3);

    // Seeking into the middle gives the same audio as playing through.
    juce::AudioBuffer<float> seg(2, 4410);
    r.seek(int(44100 * 3.5));
    r.render(seg, 0, 4410);
    double diff = 0;
    for (int i = 0; i < 4410; ++i) diff = std::max(diff, double(std::abs(seg.getSample(0, i) - out.getSample(0, int(44100 * 3.5) + i))));
    CHECK(diff < 0.01);

    // Crossfade: negative gap overlaps the clips; fades make the sum smooth (no hole, no +6 dB bump).
    Session x = s;
    x.findRegion(idB)->gainDb = 0;
    x.setCrossfadeLength(x.regions[0].id, idB, 1.0);   // overlap of 1 s: automatic equal-power crossfade
    CHECK(std::abs(x.findRegion(idB)->start - 6.0) < 1e-9);
    CHECK(std::abs(x.markers[x.markers.size() - 1].t - 6.0) < 1e-9);   // attached track marker moved with it
    ProgramRenderer rx(fm);
    rx.setSession(x, 44100);
    juce::AudioBuffer<float> ox(2, int(rx.totalSamples()));
    rx.render(ox, 0, ox.getNumSamples());
    double mid = rms(ox, int(44100 * 6.4), 2205);   // centre of the overlap (equal-power: ~ +3 dB for correlated tones)
    CHECK(mid > sine * 0.9 && mid < sine * 1.5);

    // PQ positions derived from the same session.
    auto disc = s.toDisc();
    CHECK(disc.tracks.size() == 2);
    CHECK(disc.tracks[0].startSector() == 150);
    CHECK(disc.tracks[1].startSector() == int64_t(std::llround((2.0 + 5.0 + 1.5) * 75)));
    CHECK(disc.tracks[1].firstSector() == int64_t(std::llround((2.0 + 5.0) * 75)));   // pause marker at end of track 1
    CHECK(!cd::hasErrors(cd::validate(disc)));

    // Session save/load round trip.
    juce::String err;
    auto sf = dir.getChildFile("t.lathe");
    CHECK(s.save(sf, err));
    Session s2;
    CHECK(s2.load(sf, err));
    CHECK(s2.regions.size() == 2 && std::abs(s2.regions[1].gainDb + 6.0) < 1e-9);
    CHECK(s2.markers.size() == s.markers.size());

    // Editing operations.
    {
        Session e = s;
        int idA = e.regions[0].id;
        int tail = e.split(idA, 4.0);
        CHECK(tail != 0 && e.regions.size() == 3);
        CHECK(std::abs(e.findRegion(tail)->start - 4.0) < 1e-4 && std::abs(e.findRegion(idA)->end() - 4.0) < 1e-4);
        auto discBefore = e.toDisc();
        CHECK(discBefore.tracks.size() == 2);   // splitting doesn't change PQ

        // 4-point replace: swap 1 s of destination (5..6) for 2 s of source; later material ripples +1 s.
        double bStart = e.findRegion(idB)->start;
        e.sourceEdit(a, 96000, 96000 * 5, 0, 96000 * 2, 0, 5.0, 6.0, Session::EditMode::Replace);
        CHECK(std::abs(e.findRegion(idB)->start - (bStart + 1.0)) < 1e-6);
        CHECK(std::abs(e.end() - (s.end() + 1.0)) < 1e-6);

        // Overwrite keeps length.
        Session o = s;
        o.sourceEdit(a, 96000, 96000 * 5, 0, 96000, 0, 3.0, -1, Session::EditMode::Overwrite);
        CHECK(std::abs(o.end() - s.end()) < 1e-6);
        CHECK(o.regions.size() == 4);   // A split around the overwrite + new region + B

        // Insert ripples by the source length.
        Session in = s;
        in.sourceEdit(a, 96000, 96000 * 5, 0, 96000, 0, 3.0, -1, Session::EditMode::Insert);
        CHECK(std::abs(in.end() - (s.end() + 1.0)) < 1e-6);

        // Delete with ripple closes the hole and removes the region's markers.
        Session d = s;
        d.deleteRegion(idA, true);
        CHECK(d.regions.size() == 1 && d.toDisc().tracks.size() == 1);

        // Slip moves audio under fixed edges.
        Session sl = s;
        sl.findRegion(idA)->srcIn = 96000; sl.findRegion(idA)->srcOut = 96000 * 4;
        sl.slip(idA, -0.5);
        CHECK(sl.findRegion(idA)->srcIn == 48000 && sl.findRegion(idA)->srcOut == 96000 * 4 - 48000);

        // Envelope: -6 dB midpoint.
        Region env;
        env.envelope.add({0.0, 0.0});
        env.envelope.add({2.0, -12.0});
        CHECK(std::abs(env.envelopeDb(1.0) + 6.0) < 1e-9);
    }

    // v1 session migration.
    {
        auto v1 = dir.getChildFile("old.lathe");
        v1.replaceWithText(R"({"format":"lathe-session","version":1,"clips":[
            {"file":"/x/a.wav","name":"A","fileRate":44100,"fileLength":441000,"gapBefore":2,"pause":2,"text":{"title":"Song A"}},
            {"file":"/x/b.wav","name":"B","fileRate":44100,"fileLength":441000,"gapBefore":1.5,"pause":1.5,"isrc":"USABC2600002","subIndexes":[3.0]}]})");
        Session m;
        CHECK(m.load(v1, err));
        CHECK(m.regions.size() == 2);
        auto md = m.toDisc();
        CHECK(md.tracks.size() == 2 && md.tracks[1].isrc == "USABC2600002" && md.tracks[1].indexes.size() == 3);
        CHECK(md.tracks[0].text.title == "Song A");
    }

    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
