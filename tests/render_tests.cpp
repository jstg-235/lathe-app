// Renderer tests: real WAV files through ProgramRenderer (gaps, gain, fades, SRC) and the
// DDP layout derived from the same session, so playback, export and PQ stay in step.
#include "../app/Source/Commands.h"
#include "../core/util/FlacTags.h"
#include "../app/Source/NullTest.h"
#include "../app/Source/ProgramRenderer.h"
#include "../app/Source/Products.h"

// A test plugin: hard clip at +/-level with a fixed latency, in float and double.
class ClipDelayPlugin : public juce::AudioPluginInstance {
public:
    ClipDelayPlugin(double level, int latency)
        : AudioPluginInstance(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())),
          level_(level), lat_(latency) {}
    void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = "ClipDelay"; d.pluginFormatName = "Test"; d.fileOrIdentifier = "clipdelay"; }
    const juce::String getName() const override { return "ClipDelay"; }
    void prepareToPlay(double, int) override { setLatencySamples(lat_); reset(); }
    void reset() override { for (auto& d : delay_) { d.assign(size_t(lat_ + 1), 0.0); } pos_ = 0; }
    void releaseResources() override {}
    bool supportsDoublePrecisionProcessing() const override { return true; }
    template <typename T> void run(juce::AudioBuffer<T>& b) {
        for (int i = 0; i < b.getNumSamples(); ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                double x = juce::jlimit(-level_, level_, double(b.getSample(ch, i)));
                auto& d = delay_[size_t(ch)];
                d[size_t(pos_)] = x;
                b.setSample(ch, i, T(d[size_t((pos_ + 1) % (lat_ + 1))]));
            }
            pos_ = (pos_ + 1) % (lat_ + 1);
        }
    }
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { run(b); }
    void processBlock(juce::AudioBuffer<double>& b, juce::MidiBuffer&) override { run(b); }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
private:
    double level_;
    int lat_, pos_ = 0;
    std::vector<double> delay_[2];
};
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

    // ---- Bit transparency: a 24-bit file at the project rate, unity gain, no fades,
    // must come out of the 64-bit renderer sample-for-sample identical.
    {
        auto f = dir.getChildFile("noise24.wav");
        juce::WavAudioFormat wav;
        {
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 24, {}, 0));
            os.release();
            juce::Random rnd(42);
            juce::AudioBuffer<int> ib(2, 44100);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 44100; ++i) ib.setSample(ch, i, (rnd.nextInt(1 << 24) - (1 << 23)) << 8);
            const int* chans[3] = {ib.getReadPointer(0), ib.getReadPointer(1), nullptr};
            w->write(chans, 44100);
        }
        Session t;
        t.append(regionFor(t, fm, f), 2.0);
        ProgramRenderer rr(fm);
        rr.setSession(t, 44100);
        juce::AudioBuffer<double> o(2, int(rr.totalSamples()));
        rr.render(o, 0, o.getNumSamples());
        std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(f));
        juce::AudioBuffer<float> src(2, 44100);
        rd->read(&src, 0, 44100, 0, true, true);
        int mismatches = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 44100; ++i)
                if (o.getSample(ch, 88200 + i) != double(src.getSample(ch, i))) ++mismatches;
        CHECK(mismatches == 0);
        std::printf("bit-transparency: %d mismatches in 88200 samples\n", mismatches);
    }

    // ---- Null test: B = A delayed 123 samples, inverted, -0.5 dB. Must find all three.
    {
        auto fa = dir.getChildFile("na.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(fa.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 32, {}, 0));
            os.release();
            juce::Random rnd(99);
            juce::AudioBuffer<float> n(2, 44100 * 4);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n.getNumSamples(); ++i) n.setSample(ch, i, (rnd.nextFloat() - 0.5f) * 0.5f);
            w->writeFromAudioSampleBuffer(n, 0, n.getNumSamples());
        }
        auto fb = dir.getChildFile("nb.wav");
        {
            std::unique_ptr<juce::AudioFormatReader> ra(fm.createReaderFor(fa));
            juce::AudioBuffer<float> x(2, int(ra->lengthInSamples) + 123);
            x.clear();
            ra->read(&x, 123, int(ra->lengthInSamples), 0, true, true);
            x.applyGain(-std::pow(10.0f, -0.5f / 20.0f));
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(fb.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 32, {}, 0));
            os.release();
            w->writeFromAudioSampleBuffer(x, 0, x.getNumSamples());
        }
        auto nr = nullTest(fm, fa, fb, {}, {});
        CHECK(nr.ok);
        std::printf("null test: offset %lld inverted %d gain %.3f dB residual-after-gain %.1f dBFS\n", (long long) nr.offset,
                    int(nr.inverted), nr.gainDiffDb, 20 * std::log10(nr.residualRmsGainMatched + 1e-30));
        CHECK(nr.offset == 123);
        CHECK(nr.inverted);
        CHECK(std::abs(nr.gainDiffDb - 0.5) < 0.01);
        CHECK(nr.residualRmsGainMatched < 1e-5);
        // Identical files are reported bit-identical.
        auto same = nullTest(fm, fa, fa, {}, {});
        CHECK(same.ok && same.offset == 0 && same.residualPeak == 0.0);
    }

    // ---- Region effects: run before fades, with latency compensated exactly.
    {
        auto f = dir.getChildFile("fxsrc.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 32, {}, 0));
            os.release();
            juce::Random rnd(5);
            juce::AudioBuffer<float> n(2, 44100 * 3);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n.getNumSamples(); ++i) n.setSample(ch, i, (rnd.nextFloat() - 0.5f) * 0.8f);
            w->writeFromAudioSampleBuffer(n, 0, n.getNumSamples());
        }
        lathe::PluginHost host;
        lathe::RegionFxBank bank(host);
        Session fs;
        int id = fs.append(regionFor(fs, fm, f), 2.0);
        bank.chainFor(id, true)->addInstance(std::make_unique<ClipDelayPlugin>(0.1, 100));
        CHECK(bank.hasEffects(id));
        ProgramRenderer fr(fm);
        fr.setRegionFx(&bank);
        fr.setSession(fs, 44100, true);
        CHECK(bank.latency(id) == 100);
        juce::AudioBuffer<double> o(2, int(fr.totalSamples()));
        fr.render(o, 0, o.getNumSamples());
        std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(f));
        juce::AudioBuffer<float> src(2, int(rd->lengthInSamples));
        rd->read(&src, 0, src.getNumSamples(), 0, true, true);
        int bad = 0;
        for (int i = 0; i < src.getNumSamples(); ++i)
            if (std::abs(o.getSample(0, 88200 + i) - juce::jlimit(-0.1, 0.1, double(src.getSample(0, i)))) > 1e-12) ++bad;
        std::printf("region fx (clip + 100-sample latency): %d mismatches in %d samples\n", bad, src.getNumSamples());
        CHECK(bad == 0);

        // Fades shape the processed sound: with a 1 s fade-in, half-way gain applies AFTER the clipper.
        fs.findRegion(id)->fadeIn = 1.0;
        fs.findRegion(id)->fadeInShape = FadeShape::Linear;
        ProgramRenderer fr2(fm);
        fr2.setRegionFx(&bank);
        fr2.setSession(fs, 44100, true);
        juce::AudioBuffer<double> o2(2, int(fr2.totalSamples()));
        fr2.render(o2, 0, o2.getNumSamples());
        int k = 22050;   // 0.5 s into the region
        double expect = juce::jlimit(-0.1, 0.1, double(src.getSample(0, k))) * (double(k) / 44100.0);
        CHECK(std::abs(o2.getSample(0, 88200 + k) - expect) < 1e-9);

        // Bypass-all removes both the processing and the latency.
        bank.setBypassAll(true);
        ProgramRenderer fr3(fm);
        fr3.setRegionFx(&bank);
        fs.findRegion(id)->fadeIn = 0;
        fr3.setSession(fs, 44100, true);
        juce::AudioBuffer<double> o3(2, int(fr3.totalSamples()));
        fr3.render(o3, 0, o3.getNumSamples());
        CHECK(o3.getSample(0, 88200 + 1000) == double(src.getSample(0, 1000)));
        bank.setBypassAll(false);

        // Split copies the region's effects state; sync builds the new region's chain.
        fs.findRegion(id)->fxState = "dummy-not-loaded";   // pretend a captured state exists
        int tail = fs.split(id, 3.0);
        CHECK(fs.findRegion(tail)->fxState == fs.findRegion(id)->fxState);
    }

    // ---- FLAC tags: written and read back, audio untouched.
    {
        auto ff = dir.getChildFile("tag.flac");
        juce::FlacAudioFormat flac;
        {
            std::unique_ptr<juce::OutputStream> os(ff.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(flac.createWriterFor(os.get(), 44100, 2, 16, {}, 5));
            os.release();
            juce::AudioBuffer<float> n(2, 44100);
            for (int i = 0; i < 44100; ++i) { n.setSample(0, i, float(std::sin(i * 0.05) * 0.5)); n.setSample(1, i, 0.25f); }
            w->writeFromAudioSampleBuffer(n, 0, 44100);
        }
        std::unique_ptr<juce::AudioFormatReader> before(fm.createReaderFor(ff));
        juce::AudioBuffer<float> a(2, 44100), b(2, 44100);
        before->read(&a, 0, 44100, 0, true, true);
        before.reset();
        auto e = lathe::util::writeFlacTags(ff.getFullPathName().toStdString(),
                                            {{"TITLE", "Caf\xc3\xa9 Song"}, {"ARTIST", "Tester"}, {"ISRC", "USABC2600001"}, {"TRACKNUMBER", "3"}});
        CHECK(e.empty());
        lathe::util::Tags got;
        CHECK(lathe::util::readFlacTags(ff.getFullPathName().toStdString(), got));
        CHECK(got.size() == 4 && got[0].second == "Caf\xc3\xa9 Song" && got[2].second == "USABC2600001");
        std::unique_ptr<juce::AudioFormatReader> after(fm.createReaderFor(ff));
        CHECK(after != nullptr && after->lengthInSamples == 44100);
        after->read(&b, 0, 44100, 0, true, true);
        bool same = true;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 44100; ++i) same &= a.getSample(ch, i) == b.getSample(ch, i);
        CHECK(same);
        // Re-tagging replaces, not appends.
        CHECK(lathe::util::writeFlacTags(ff.getFullPathName().toStdString(), {{"TITLE", "New"}}).empty());
        got.clear();
        lathe::util::readFlacTags(ff.getFullPathName().toStdString(), got);
        CHECK(got.size() == 1 && got[0].second == "New");
    }

    // ---- Key commands: every default parses, and no two commands share a key.
    {
        std::map<juce::String, juce::String> seen;
        int dupes = 0, unparsed = 0;
        for (auto& d : allCommands())
            for (auto& k : juce::StringArray::fromTokens(d.keys, ";", "")) {
                if (k.trim().isEmpty()) continue;
                auto kp = juce::KeyPress::createFromDescription(k.trim());
                if (!kp.isValid()) { ++unparsed; std::printf("unparsed key: %s\n", k.toRawUTF8()); }
                auto key = kp.getTextDescription().toLowerCase();
                if (seen.count(key)) { ++dupes; std::printf("duplicate key %s: %s / %s\n", key.toRawUTF8(), seen[key].toRawUTF8(), d.name); }
                seen[key] = d.name;
            }
        CHECK(dupes == 0 && unparsed == 0);
        if (auto* out = std::getenv("LATHE_KEYGUIDE_OUT")) juce::File(out).replaceWithText(keyGuideHtml(nullptr));
    }


    // ---- Products: unit gain, auto-split, unit discs, PQ offset, persistence ----
    {
        // A fake programme: regions don't need real files for layout maths.
        auto fakeSession = [](int tracks, double trackSec, double gap) {
            Session p;
            p.discTitle = "Long Night";
            p.discPerformer = "Ensemble";
            double t = 2.0;
            for (int i = 0; i < tracks; ++i) {
                Region r;
                r.id = p.nextId++;
                r.fileRate = 44100;
                r.fileLength = juce::int64(trackSec * 44100);
                r.srcIn = 0;
                r.srcOut = r.fileLength;
                r.start = t;
                p.regions.add(r);
                Marker m;
                m.id = p.nextId++;
                m.type = Marker::TrackStart;
                m.t = t;
                m.attachedRegion = r.id;
                m.isrc = "USABC26" + juce::String(i + 1).paddedLeft('0', 5).toStdString();
                m.text.title = ("Part " + juce::String(i + 1)).toStdString();
                p.markers.add(m);
                t += trackSec + gap;
            }
            return p;
        };

        Unit u{10.0, 20.0, 2.0, 4.0, FadeShape::Linear, FadeShape::Linear};
        CHECK(unitGain(u, 9.99) == 0.0 && unitGain(u, 20.0) == 0.0);
        CHECK(std::abs(unitGain(u, 11.0) - 0.5) < 1e-9 && unitGain(u, 15.0) == 1.0 && std::abs(unitGain(u, 18.0) - 0.5) < 1e-9);

        // 12 five-minute tracks on 12" LP: 4 sides of 3 tracks, all breaks at track starts.
        Session lp = fakeSession(12, 300.0, 0.0);
        Product vinyl;
        vinyl.kind = ProductKind::LP12_33;
        vinyl.name = "LP";
        auto msg = autoSplit(lp, vinyl, nullptr);
        std::printf("%s\n", msg.toRawUTF8());
        CHECK(vinyl.units.size() == 4);
        bool atTracks = true;
        for (int i = 1; i < vinyl.units.size(); ++i) {
            atTracks = atTracks && vinyl.units[i].fadeIn == 0 && vinyl.units[i - 1].fadeOut == 0 && vinyl.units[i].start == vinyl.units[i - 1].end;
            atTracks = atTracks && std::abs(vinyl.units[i].start - (2.0 + 900.0 * i)) < 1e-6;
        }
        CHECK(atTracks);
        CHECK(validateProduct(lp, vinyl).empty());
        CHECK(unitLabel(vinyl, 2) == "Side C" && unitLabel(vinyl, 2, true) == "Record 2, Side C");
        auto ut = unitTracks(lp, vinyl.units[1]);
        CHECK(ut.size() == 3 && ut[0].number == 4 && std::abs(ut[0].start) < 1e-6 && !ut[0].continuesFromPrevious);

        // One 3-hour performance on CD: three discs, mid-music breaks with overlap and fades.
        Session live = fakeSession(1, 3 * 3600.0, 0.0);
        Product cdp;
        cdp.kind = ProductKind::CD;
        cdp.name = "CD";
        msg = autoSplit(live, cdp, nullptr);
        std::printf("%s\n", msg.toRawUTF8());
        CHECK(cdp.units.size() == 3);
        const double T = live.end();
        CHECK(cdp.units[0].start == 0 && cdp.units[2].end == T);
        CHECK(cdp.units[0].fadeOut == 5.0 && cdp.units[1].fadeIn == 5.0 && cdp.units[1].fadeOut == 5.0);
        CHECK(std::abs(cdp.units[0].end - cdp.units[1].start - 10.0) < 1e-6);   // 5 s each side of the break
        CHECK(std::abs(cdp.units[0].end - 5.0 - T / 3) < 1e-6);
        bool fits = true;
        for (auto& x : cdp.units) fits = fits && x.length() <= productSpec(ProductKind::CD).target;
        CHECK(fits && validateProduct(live, cdp).empty());

        // Same programme on LP: 9 sides needed, rounded up to 10.
        Product lp2;
        lp2.kind = ProductKind::LP12_33;
        lp2.name = "LP";
        autoSplit(live, lp2, nullptr);
        CHECK(lp2.units.size() == 10);

        // With a loudness map, the break moves to the quiet passage near the ideal point.
        dsp::LoudnessMap map;
        map.hopSec = 0.1;
        map.momentary.assign(size_t(T / 0.1) + 10, -14.0f);
        for (int i = 0; i < 30; ++i) map.momentary[size_t((3660.0 + 1.0) / 0.1) + size_t(i)] = -50.0f;   // 61:01-61:04
        Product cdq;
        cdq.kind = ProductKind::CD;
        cdq.name = "CD";
        autoSplit(live, cdq, &map);
        double brk = cdq.units[0].end - 5.0;
        std::printf("quiet break at %.1f s (dip at 3661-3664)\n", brk);
        CHECK(brk > 3661.0 && brk < 3664.0);

        // Disc 2 starts mid-track: a "(cont.)" track reusing the ISRC, after a 2 s pregap.
        double lead = 0;
        juce::StringArray notes;
        auto d2 = live.toDiscForUnit(cdp.units[1], 2, 3, lead, &notes);
        CHECK(std::abs(lead - 2.0) < 1e-9);
        CHECK(d2.tracks.size() == 1 && d2.tracks[0].text.title == "Part 1 (cont.)" && d2.tracks[0].isrc == "USABC2600001");
        CHECK(d2.tracks[0].startSector() == 150 && d2.tracks[0].firstSector() == 0);
        CHECK(d2.leadOutSector == int64_t(std::ceil((cdp.units[1].length() + 2.0) * 75 - 1e-6)));
        CHECK(!cd::hasErrors(cd::validate(d2)) && notes.size() == 1);
        CHECK(d2.text.title == "Long Night (Disc 2)");
        auto d1 = live.toDiscForUnit(cdp.units[0], 1, 3, lead, nullptr);
        CHECK(lead == 0 && d1.tracks.size() == 1 && d1.tracks[0].startSector() == 150 && d1.tracks[0].text.title == "Part 1");

        // A disc that starts on a track boundary gets a pregap but no continuation track.
        Session multi = fakeSession(30, 300.0, 2.0);
        Product cdm;
        cdm.kind = ProductKind::CD;
        cdm.name = "CD";
        autoSplit(multi, cdm, nullptr);
        CHECK(cdm.units.size() == 2 && cdm.units[0].fadeOut == 0);
        auto m2 = multi.toDiscForUnit(cdm.units[1], 2, 2, lead, nullptr);
        CHECK(std::abs(lead - 2.0) < 1e-9 && m2.tracks[0].text.title.find("(cont.)") == std::string::npos);
        CHECK(!cd::hasErrors(cd::validate(m2)));
        int onDiscs = int(m2.tracks.size()) + int(multi.toDiscForUnit(cdm.units[0], 1, 2, lead, nullptr).tracks.size());
        CHECK(onDiscs == 30);

        // Gaps between units are errors.
        Product broken = cdp;
        broken.units.getReference(1).start = broken.units[0].end + 3.0;
        bool gapFound = false;
        for (auto& i : validateProduct(live, broken)) gapFound = gapFound || (i.error && i.message.contains("Gap"));
        CHECK(gapFound);

        // Break audition: out unit's tail, flip gap, in unit's head.
        cdp.flipGap = 1.5;
        auto bp = makeBreakPlan(cdp, 0, 3.0, 3.0);
        CHECK(std::abs(bp.a1 - cdp.units[0].end) < 1e-9 && std::abs(bp.a0 - (cdp.units[0].end - 8.0)) < 1e-9);
        CHECK(std::abs(bp.length() - (8.0 + 1.5 + 8.0)) < 1e-9);
        CHECK(bp.at(8.0 + 0.7).first < 0);                                   // in the flip gap
        CHECK(std::abs(bp.at(8.0 + 1.5).first - cdp.units[1].start) < 1e-9 && bp.at(8.0 + 1.5).second < 1e-6);   // fade-in starts at 0

        // PQ offset moves track starts earlier, but never track 1 under 2 s.
        multi.pqOffsetFrames = 10;
        juce::StringArray adj;
        auto off = multi.toDisc(&adj);
        multi.pqOffsetFrames = 0;
        auto plain = multi.toDisc();
        CHECK(off.tracks[0].startSector() == 150 && plain.tracks[0].startSector() == 150);
        CHECK(off.tracks[5].startSector() == plain.tracks[5].startSector() - 10);
        CHECK(adj.joinIntoString(" ").contains("PQ offset"));

        // Persistence: products, release metadata and credits survive a save/load.
        live.products.add(cdp);
        live.products.getReference(0).id = live.nextId++;
        live.products.getReference(0).catalogue = "LN-001CD";
        live.label = "Night Label";
        live.pLine = "2026 Night Label";
        live.cdTextGenre = 5;
        live.htoa = true;
        live.pqOffsetFrames = 6;
        live.markers.getReference(0).publisher = "Night Songs";
        live.markers.getReference(0).pro = "ASCAP";
        live.markers.getReference(0).explicitContent = true;
        auto sf = dir.getChildFile("products.lathe");
        juce::String err;
        CHECK(live.save(sf, err));
        Session back;
        CHECK(back.load(sf, err));
        CHECK(back.products.size() == 1 && back.products[0].units.size() == 3 && back.products[0].catalogue == "LN-001CD");
        CHECK(std::abs(back.products[0].units[1].start - cdp.units[1].start) < 1e-9 && back.products[0].units[1].fadeIn == 5.0);
        CHECK(back.label == "Night Label" && back.cdTextGenre == 5 && back.htoa && back.pqOffsetFrames == 6 && back.pLine == "2026 Night Label");
        CHECK(back.markers[0].publisher == "Night Songs" && back.markers[0].pro == "ASCAP" && back.markers[0].explicitContent);
    }
    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
