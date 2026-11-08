// Renderer tests: real WAV files through ProgramRenderer (gaps, gain, fades, SRC) and the
// DDP layout derived from the same session, so playback, export and PQ stay in step.
#include "../app/Source/Commands.h"
#include "../core/util/FlacTags.h"
#include "../app/Source/NullTest.h"
#include "../app/Source/ProgramRenderer.h"
#include "../app/Source/AutoMarks.h"
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
                // Compare as the Mac sees them (Control and Command are different keys there,
                // while on Linux / Windows JUCE folds Command into Control).
                auto parts = juce::StringArray::fromTokens(k.trim().toLowerCase(), "+", "");
                parts.trim();
                juce::String keyName = parts[parts.size() - 1];
                parts.remove(parts.size() - 1);
                for (auto& m : parts) m = m == "ctrl" ? "control" : m == "alt" ? "option" : m == "cmd" ? "command" : m;
                parts.sort(false);
                auto key = parts.joinIntoString("+") + "+" + juce::KeyPress::createFromDescription(keyName).getTextDescription().toLowerCase();
                if (seen.count(key)) { ++dupes; std::printf("duplicate key %s: %s / %s\n", key.toRawUTF8(), seen[key].toRawUTF8(), d.name); }
                seen[key] = d.name;
            }
        CHECK(dupes == 0 && unparsed == 0);
        // The Legacy preset: no clashes either, and the classic keys land where expected.
        std::map<juce::String, juce::String> seenL;
        int dupesL = 0;
        for (auto& d : allCommands())
            for (auto& k : presetKeys(1, d)) {
                if (!juce::KeyPress::createFromDescription(k).isValid()) { ++dupesL; std::printf("legacy unparsed: %s\n", k.toRawUTF8()); }
                auto key = canonicalKey(k);
                if (seenL.count(key)) { ++dupesL; std::printf("legacy duplicate %s: %s / %s\n", key.toRawUTF8(), seenL[key].toRawUTF8(), d.name); }
                seenL[key] = d.name;
            }
        CHECK(dupesL == 0);
        CHECK(presetKeys(1, *findCommand(cmd::playToIn)) == juce::StringArray{"F5"});
        CHECK(presetKeys(1, *findCommand(cmd::sourceIn)) == juce::StringArray{"["});
        CHECK(presetKeys(1, *findCommand(cmd::overwriteEdit)) == juce::StringArray{"F1"});
        CHECK(presetKeys(1, *findCommand(cmd::addTrackMarker)) == juce::StringArray{"F9"});
        CHECK(presetKeys(1, *findCommand(cmd::exportDdp)).isEmpty());   // its key went to Zoom to Fit
        CHECK(presetKeys(1, *findCommand(cmd::playAroundEdit)).isEmpty());
        CHECK(presetKeys(1, *findCommand(cmd::analyse)) == juce::StringArray{"command + shift + A"});   // untouched
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

        // Uneven tracks that still fit at track starts: no song is broken for better balance.
        {
            Session uneven = fakeSession(4, 600.0, 0.0);             // 4 x 10 min
            uneven.regions.getReference(3).srcOut = juce::int64(1000.0 * 44100);   // last one 16:40
            Product lpU;
            lpU.kind = ProductKind::LP12_33;
            lpU.name = "LP";
            autoSplit(uneven, lpU, nullptr);
            bool noMid = true;
            for (auto& x : lpU.units) noMid = noMid && x.fadeIn == 0 && x.fadeOut == 0;
            CHECK(lpU.units.size() == 4 && noMid && std::abs(lpU.units[3].start - 1802.0) < 1e-6);
        }

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

    // ---- Missing audio: found beside the session, or relinked by hand ----
    {
        auto moved = dir.getChildFile("moved");
        moved.getChildFile("Audio").createDirectory();
        auto t1 = makeTone(dir, "m1.wav", 48000, 1.0, 440, 0.3f);
        auto t2 = makeTone(dir, "m2.wav", 48000, 1.0, 440, 0.3f);
        Session ms;
        ms.append(regionFor(ms, fm, t1), 2.0);
        ms.append(regionFor(ms, fm, t2), 1.0);
        ms.append(regionFor(ms, fm, t1), 1.0);   // the same file used twice
        auto sf = dir.getChildFile("missing.lathe");
        juce::String err;
        CHECK(ms.save(sf, err));
        // Move the audio into moved/Audio and the session into moved/.
        CHECK(t1.moveFileTo(moved.getChildFile("Audio/m1.wav")) && t2.moveFileTo(moved.getChildFile("Audio/m2.wav")));
        Session back;
        CHECK(back.load(sf, err));
        CHECK(back.missingAudio().size() == 2);   // distinct files, not regions
        CHECK(back.findMissingAudioIn({moved, moved.getChildFile("Audio")}) == 2 && back.missingAudio().isEmpty());
        CHECK(back.regions[0].file == moved.getChildFile("Audio/m1.wav") && back.regions[2].file == back.regions[0].file);
        // Manual relink of one file changes every region that used it.
        Session again;
        CHECK(again.load(sf, err));
        CHECK(again.relinkAudio(again.regions[0].file, moved.getChildFile("Audio/m1.wav")) == 2);
        CHECK(again.missingAudio().size() == 1 && again.missingAudio()[0].getFileName() == "m2.wav");
    }

    // ---- Deactivated plugins: hidden from menus, never loaded, settings kept in the session ----
    {
        lathe::PluginHost host(false);
        lathe::PluginChain chain(host);
        juce::PluginDescription d;
        d.name = "Missing Comp";
        d.pluginFormatName = "VST3";
        d.manufacturerName = "Nobody";
        d.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/LatheTestMissingComp.vst3";
        d.uniqueId = d.deprecatedUid = 0x1a7e;
        juce::XmlElement root("LATHE_CHAIN");
        auto* e = root.createNewChildElement("SLOT");
        e->setAttribute("bypassed", true);
        e->addChildElement(d.createXml().release());
        e->setAttribute("state", juce::String("c2V0dGluZ3M="));
        const auto text = root.toString();
        const juce::MemoryBlock state(text.toRawUTF8(), text.getNumBytesAsUTF8());

        host.known().addType(d);
        host.setDeactivated(d, true);
        bool inMenu = false;
        for (auto& t : host.activeTypes()) inMenu = inMenu || t.createIdentifierString() == d.createIdentifierString();
        CHECK(host.isDeactivated(d) && !inMenu);
        juce::StringArray problems;
        chain.setState(state, problems);
        CHECK(chain.size() == 1 && chain.isDormant(0) && chain.nameAt(0) == "Missing Comp (deactivated)" && chain.isBypassed(0));
        CHECK(problems.size() == 1 && problems[0].contains("deactivated"));
        auto back = juce::XmlDocument::parse(chain.getState().toString());
        CHECK(back && back->getChildByName("SLOT") && back->getChildByName("SLOT")->getStringAttribute("state") == "c2V0dGluZ3M=");
        chain.prepare(44100, 512);
        juce::AudioBuffer<float> b(2, 512);
        for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(b.getWritePointer(ch), 0.25f, 512);
        chain.process(b, 0, 512);
        CHECK(b.getSample(1, 100) == 0.25f && chain.latencySamples() == 0);   // passes straight through

        // Switched back on: the session tries to load it again (it doesn't exist here, so that fails normally).
        host.setDeactivated(d, false);
        problems.clear();
        chain.setState(state, problems);
        CHECK(!host.isDeactivated(d) && chain.size() == 0 && problems.size() == 1 && !problems[0].contains("deactivated"));
        host.known().removeType(d);
    }
    // ---- Source / destination edits, clipboard, lane targets, SRPs ----
    {
        auto loud = makeTone(dir, "dest.wav", 44100, 10.0, 500, 0.5f);
        auto quiet = makeTone(dir, "src.wav", 44100, 10.0, 500, 0.1f);
        auto fresh = [&] {
            Session d;
            d.lanes = 1;
            Region r = regionFor(d, fm, loud);
            r.start = 0;
            d.regions.add(r);
            d.addMarker(Marker::TrackStart, 0.0, r.id);
            d.addMarker(Marker::Index, 6.0, r.id);
            return d;
        };
        Session src;
        src.lanes = 1;
        Region q = regionFor(src, fm, quiet);
        q.start = 0;
        src.regions.add(q);
        const juce::Array<int> lane0{0};
        Clip c = src.copyRange(2.0, 4.0, lane0);   // 2 s of the quiet take
        NEAR(c.length, 2.0, 1e-9);
        CHECK(c.regions.size() == 1 && c.regions[0].start == 0.0 && c.regions[0].srcIn == 88200 && c.regions[0].srcOut == 176400);

        auto levelAt = [&](const Session& ss, double t0, double len) {
            ProgramRenderer pr(fm);
            pr.setSession(ss, 44100);
            juce::AudioBuffer<float> buf(2, int(44100 * len));
            pr.seek(juce::int64(t0 * 44100));
            pr.render(buf, 0, buf.getNumSamples());
            return rms(buf, 0, buf.getNumSamples());
        };
        const double loudRms = 0.5 / std::sqrt(2.0), quietRms = 0.1 / std::sqrt(2.0);

        {   // Insert: later material (and its markers) moves right by the clip length.
            Session d = fresh();
            auto ids = d.paste(c, lane0, 5.0, PasteMode::Insert);
            CHECK(ids.size() == 1);
            NEAR(d.end(), 12.0, 1e-3);
            Marker* idx = nullptr;
            for (auto& m : d.markers) if (m.type == Marker::Index) idx = &m;
            CHECK(idx && std::abs(idx->t - 8.0) < 1e-3);
            NEAR(levelAt(d, 5.2, 1.5), quietRms, 0.003);
            NEAR(levelAt(d, 7.2, 1.0), loudRms, 0.003);
        }
        {   // Replace: overwrite from the point, nothing moves.
            Session d = fresh();
            d.paste(c, lane0, 3.0, PasteMode::Replace);
            NEAR(d.end(), 10.0, 1e-3);
            NEAR(levelAt(d, 3.1, 1.8), quietRms, 0.003);
            NEAR(levelAt(d, 5.2, 1.0), loudRms, 0.003);
        }
        {   // Replace Left: the clip ends at the Out.
            Session d = fresh();
            auto ids = d.paste(c, lane0, 0.0, PasteMode::ReplaceLeft, 8.0);
            auto* r = d.findRegion(ids[0]);
            CHECK(r && std::abs(r->start - 6.0) < 1e-9 && std::abs(r->end() - 8.0) < 1e-6);
            NEAR(levelAt(d, 6.1, 1.8), quietRms, 0.003);
            NEAR(d.end(), 10.0, 1e-3);
        }
        {   // Replace Constrained: only the destination In-Out changes, the clip is cut to fit.
            Session d = fresh();
            auto ids = d.paste(c, lane0, 4.0, PasteMode::Constrained, 5.0);
            auto* r = d.findRegion(ids[0]);
            CHECK(r && std::abs(r->length() - 1.0) < 1e-6);
            NEAR(levelAt(d, 4.1, 0.8), quietRms, 0.003);
            NEAR(levelAt(d, 5.1, 1.0), loudRms, 0.003);
            NEAR(d.end(), 10.0, 1e-3);
        }
        {   // Back-timed past zero: what would land before the start is dropped.
            Session d = fresh();
            auto ids = d.paste(c, lane0, 0.0, PasteMode::ReplaceLeft, 0.5);
            auto* r = d.findRegion(ids[0]);
            CHECK(r && r->start == 0.0 && std::abs(r->length() - 0.5) < 1e-6);
        }
        {   // Overlay: nothing removed, the regions overlap (and crossfade).
            Session d = fresh();
            d.paste(c, lane0, 2.0, PasteMode::Overlay);
            CHECK(d.regions.size() == 2);
            NEAR(d.end(), 10.0, 1e-3);
        }
        {   // Cut: copy + clear with ripple; marks inside the range go, later ones move up.
            Session d = fresh();
            d.addMarker(Marker::Index, 3.0, d.regions[0].id);
            Clip cut = d.copyRange(2.0, 4.0, lane0);
            d.clearRange(2.0, 4.0, lane0, true);
            NEAR(cut.length, 2.0, 1e-9);
            NEAR(d.end(), 8.0, 1e-3);
            int idxCount = 0;
            for (auto& m : d.markers) if (m.type == Marker::Index) { ++idxCount; NEAR(m.t, 4.0, 1e-3); }
            CHECK(idxCount == 1);
            // Paste it back where it was: the original programme returns.
            d.paste(cut, lane0, 2.0, PasteMode::Insert);
            NEAR(d.end(), 10.0, 1e-3);
            NEAR(levelAt(d, 0.5, 9.0), loudRms, 0.003);
        }
        {   // Lane targets: an untargeted lane is left alone by ripples.
            Session d = fresh();
            d.lanes = 2;
            Region r2 = regionFor(d, fm, loud);
            r2.lane = 1;
            r2.start = 6.0;
            d.regions.add(r2);
            d.lanesOff.add(1);
            CHECK(d.targetLanes() == juce::Array<int>{0});
            d.paste(c, d.targetLanes(), 1.0, PasteMode::Insert);
            CHECK(std::abs(d.findRegion(r2.id)->start - 6.0) < 1e-9);
        }
        {   // SRPs: unlocked ones ripple, locked ones stay; they and the source timeline survive a save.
            Session d = fresh();
            const int a1 = d.addSrp(7.0, "check edit");
            const int a2 = d.addSrp(8.0, "locked");
            d.findSrp(a2)->locked = true;
            d.paste(c, lane0, 1.0, PasteMode::Insert);
            NEAR(d.findSrp(a1)->t, 9.0, 1e-9);
            NEAR(d.findSrp(a2)->t, 8.0, 1e-9);
            Session srcLive = src;
            srcLive.addSrp(1.5, "take 2 best");
            d.source = &srcLive;
            d.lanesOff.add(3);
            auto f = dir.getChildFile("srp.lathe");
            juce::String err;
            CHECK(d.save(f, err));
            Session back, backSrc;
            back.source = &backSrc;
            CHECK(back.load(f, err));
            CHECK(back.srps.size() == 2 && back.srps[0].text == "locked" && back.srps[0].locked && back.srps[1].text == "check edit");
            CHECK(back.lanesOff.contains(3));
            CHECK(backSrc.regions.size() == 1 && backSrc.srps.size() == 1 && backSrc.srps[0].text == "take 2 best");
            CHECK(backSrc.regions[0].file == quiet);
            // Loaded without a live source: kept as-is and written back out unchanged.
            Session plain;
            CHECK(plain.load(f, err) && plain.sourceDoc.isObject());
            CHECK(plain.toVar()["source"].isObject());
        }
    }
    // ---- Edit Fade: custom shapes, offsets, Lock Sound, ripple until black ----
    {
        NEAR(20 * std::log10(fadeGain(FadeShape::Custom, 0.5, 3.0, 0.0)), -3.0, 1e-6);
        NEAR(20 * std::log10(fadeGain(FadeShape::Custom, 0.5, 6.0, 0.7)), -6.0, 1e-6);   // alpha keeps the midpoint
        NEAR(fadeGain(FadeShape::Custom, 0.5, 6.0206, 0.0), 0.5, 1e-4);                  // 6.02 dB = linear
        CHECK(fadeGain(FadeShape::Custom, 0.0, 3.0, 0.5) == 0.0 && fadeGain(FadeShape::Custom, 1.0, 3.0, 0.5) == 1.0);
        CHECK(fadeGain(FadeShape::Custom, 0.1, 3.0, 1.0) < fadeGain(FadeShape::Custom, 0.1, 3.0, 0.0));   // S: slower start

        auto t1 = makeTone(dir, "ef1.wav", 44100, 10.0, 500, 0.5f);
        Session e;
        e.lanes = 1;
        Region a = regionFor(e, fm, t1);
        a.srcIn = 0; a.srcOut = 44100 * 4; a.start = 0;               // 0..4 s, handle after it
        Region b = regionFor(e, fm, t1);
        b.srcIn = 44100 * 4; b.srcOut = 44100 * 8; b.start = 4.0;      // butt at 4 s, handle before it
        Region c = regionFor(e, fm, t1);
        c.srcIn = 0; c.srcOut = 44100 * 2; c.start = 8.0;              // joined to b
        Region d = regionFor(e, fm, t1);
        d.srcIn = 0; d.srcOut = 44100 * 2; d.start = 12.0;             // after a gap
        e.regions.add(a); e.regions.add(b); e.regions.add(c); e.regions.add(d);
        const int m = e.addMarker(Marker::Index, 9.0, c.id);

        // Lock Sound: a 100 ms crossfade centred on the butt; the audio stays where it was.
        e.crossfadeCentred(a.id, b.id, 0.1);
        NEAR(e.findRegion(a.id)->end(), 4.05, 1e-4);
        NEAR(e.findRegion(b.id)->start, 3.95, 1e-4);
        NEAR(e.findRegion(b.id)->srcIn, 44100 * 4 - 2205, 1);          // same audio at the same time
        NEAR(e.findRegion(c.id)->start, 8.0, 1e-9);
        auto fa = e.fadesFor(e.indexOfRegion(a.id));
        NEAR(fa.out, 0.1, 1e-4);
        // The same continuous tone through an equal-power crossfade of correlated audio: no dip
        // below the tone, a bump of at most +3 dB in the middle.
        {
            ProgramRenderer pr(fm);
            pr.setSession(e, 44100);
            juce::AudioBuffer<float> buf(2, 4410);
            pr.seek(juce::int64(3.9 * 44100));
            pr.render(buf, 0, 4410);
            NEAR(rms(buf, 0, 1000), 0.5 / std::sqrt(2.0), 0.01);
            CHECK(rms(buf, 2000, 400) > 0.5 / std::sqrt(2.0) && rms(buf, 2000, 400) < 0.5 * 1.42);
        }
        // Ripple until black: lengthening the crossfade moves b and c (joined) but not d (after a gap).
        e.setCrossfadeLength(a.id, b.id, 0.3, Session::Ripple::UntilBlack);
        NEAR(e.findRegion(b.id)->start, 3.75, 1e-4);
        NEAR(e.findRegion(c.id)->start, 7.8, 1e-4);
        NEAR(e.findMarker(m)->t, 8.8, 1e-4);                          // attached marker rode along
        NEAR(e.findRegion(d.id)->start, 12.0, 1e-9);
        // Ripple all moves d too.
        e.setCrossfadeLength(a.id, b.id, 0.2, Session::Ripple::All);
        NEAR(e.findRegion(d.id)->start, 12.1, 1e-4);

        // Manual fade length and offset: the fade-out keeps 20 ms and ends 30 ms before the region end.
        auto* ra = e.findRegion(a.id);
        ra->fadeOut = 0.02; ra->fadeOutManual = true; ra->fadeOutOffset = 0.03;
        fa = e.fadesFor(e.indexOfRegion(a.id));
        NEAR(fa.out, 0.02, 1e-9);
        CHECK(fa.gainAt(ra->length() - 0.01, ra->length()) == 0.0);      // silent after its fade
        NEAR(fa.gainAt(ra->length() - 0.06, ra->length()), 1.0, 1e-9);   // full before it
        // Saved and loaded intact.
        ra->fadeOutShape = FadeShape::Custom; ra->fadeOutDb = 4.5; ra->fadeOutAlpha = 0.25;
        juce::String err;
        Session back;
        CHECK(back.fromVar(juce::JSON::parse(juce::JSON::toString(e.toVar())), err));
        auto* rb2 = back.findRegion(a.id);
        CHECK(rb2 && rb2->fadeOutManual && rb2->fadeOutShape == FadeShape::Custom && rb2->fadeOutDb == 4.5 && rb2->fadeOutAlpha == 0.25 &&
              std::abs(rb2->fadeOutOffset - 0.03) < 1e-12);
        // Splices: the butt at c/d isn't one (gap), a/b overlaps.
        auto sp = e.splicesAround({b.id});
        CHECK(sp.contains({a.id, b.id}) && sp.contains({b.id, c.id}));
    }
    // ---- PQ: offsets, locks, auto marks, AutoSpace, track reorder, ISRC numbering ----
    {
        auto song = [&](const char* n, double sec, float amp) { return makeTone(dir, n, 44100, sec, 440, amp); };
        auto s1 = song("pq1.wav", 6, 0.4f), s2 = song("pq2.wav", 5, 0.3f), s3 = song("pq3.wav", 4, 0.2f);
        Session p;
        p.lanes = 1;
        for (auto f : {s1, s2, s3}) p.append(regionFor(p, fm, f), 2.0);   // 2-6? : 2..8, 10..15, 17..21
        auto starts = [&] { juce::Array<double> a; for (auto& m : p.markers) if (m.type == Marker::TrackStart) a.add(m.t); return a; };
        CHECK(starts().size() == 3);
        // Offsets: start 5, track 1 "same" (but the 2 s pregap floor holds), splice 2, end 3.
        p.pqOffsetFrames = 5;
        p.pqSpliceFrames = 2;   // every track start here is on a region start
        p.pqEndFrames = 3;
        auto d = p.toDisc();
        CHECK(d.tracks[0].startSector() == 150);
        CHECK(d.tracks[1].startSector() == 10 * 75 - 2);
        CHECK(d.tracks[1].indexes[0].number == 0 && d.tracks[1].indexes[0].sector == 8 * 75 + 3);
        // Per-track override, and switching offsets off.
        for (auto& m : p.markers) if (m.type == Marker::TrackStart && std::abs(m.t - 17.0) < 0.01) { m.offsetDefault = false; m.offsetFrames = 20; }
        CHECK(p.toDisc().tracks[2].startSector() == 17 * 75 - 20);
        p.pqOffsetsOn = false;
        CHECK(p.toDisc().tracks[2].startSector() == 17 * 75 && p.toDisc().tracks[1].indexes[0].sector == 8 * 75);
        p.pqOffsetsOn = true;
        // Minimum index width.
        const int ix = p.addMarker(Marker::Index, 11.0);
        p.pqMinIndexFrames = 75 * 2;
        CHECK(p.toDisc().tracks[1].indexes.size() == 2);   // 00 + 01, the index 1 s in is dropped
        p.pqMinIndexFrames = 0;
        CHECK(p.toDisc().tracks[1].indexes.size() == 3);
        p.removeMarker(ix);

        // Locks: a locked mark stays when its audio moves; the global lock holds them all.
        int t2 = 0;
        for (auto& m : p.markers) if (m.type == Marker::TrackStart && std::abs(m.t - 10.0) < 0.01) t2 = m.id;
        p.findMarker(t2)->locked = true;
        p.ripple(9.0, 1.0);
        NEAR(p.findMarker(t2)->t, 10.0, 1e-9);
        p.ripple(9.0, -1.0);
        p.findMarker(t2)->locked = false;
        p.lockMarks = true;
        p.ripple(9.0, 1.0);
        NEAR(p.findMarker(t2)->t, 10.0, 1e-9);
        p.lockMarks = false;
        p.ripple(9.0, -1.0);
        NEAR(p.findMarker(t2)->t, 9.0, 1e-9);   // regions moved back, the mark moved only once
        p.moveMarker(t2, 10.0);

        // AutoSpace to 1 s: gaps shrink, marks follow their audio.
        p.autoSpace(1.0);
        auto st = starts();
        CHECK(st.size() == 3);
        NEAR(st[1], 9.0, 1e-6);
        NEAR(st[2], 15.0, 1e-6);
        NEAR(p.end(), 19.0, 1e-6);

        // Reorder: track 3 to the front. The audio and its mark go with it; the pregap stays.
        CHECK(p.moveTrack(2, 0));
        auto st2 = starts();
        CHECK(st2.size() == 3);
        auto titleAt = [&](int k) { int n = 0; for (auto& m : p.markers) if (m.type == Marker::TrackStart && n++ == k) return juce::String(m.text.title); return juce::String(); };
        CHECK(titleAt(0) == "pq3" && titleAt(1) == "pq1" && titleAt(2) == "pq2");
        NEAR(st2[0], 2.0, 1e-6);
        NEAR(p.end(), 19.0, 1e-3);
        // The first track now plays pq3's audio (the quietest tone).
        {
            ProgramRenderer pr(fm);
            pr.setSession(p, 44100);
            juce::AudioBuffer<float> buf(2, 4410);
            pr.seek(44100 * 3);
            pr.render(buf, 0, 4410);
            NEAR(rms(buf, 0, 4410), 0.2 / std::sqrt(2.0), 0.005);
        }
        CHECK(p.moveTrack(0, 2));
        CHECK(titleAt(0) == "pq1" && titleAt(2) == "pq3");

        // ISRC numbering.
        CHECK(p.autoIsrc("US-ABC-26-00098") == 3);
        int k = 0;
        for (auto& m : p.markers)
            if (m.type == Marker::TrackStart) { CHECK(m.isrc == (k == 0 ? "USABC2600098" : k == 1 ? "USABC2600099" : "USABC2600100")); ++k; }
        CHECK(p.autoIsrc("nonsense") == 0);

        // Edited black and segments to marks on a session with no marks.
        Session q;
        q.lanes = 1;
        for (auto f : {s1, s2}) q.append(regionFor(q, fm, f), 3.0, false);
        CHECK(q.markers.isEmpty());
        CHECK(q.segmentsToStartMarks() == 2);
        CHECK(q.segmentsToStartMarks() == 0);
        q.markers.clear();
        CHECK(q.editedBlackToMarks() == 3);   // lead-in gap: start only; between: pause + start
        // Audio before the first mark.
        q.markers.clear();
        q.addMarker(Marker::TrackStart, 4.0);
        NEAR(q.audioBeforeFirstTrack(), 1.0, 1e-6);

        // Analog black: one file with a silent stretch in the middle.
        {
            auto f = dir.getChildFile("ab.wav");
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 24, {}, 0));
            os.release();
            juce::AudioBuffer<float> b(2, 44100 * 10);
            for (int i = 0; i < b.getNumSamples(); ++i) {
                const double t = i / 44100.0;
                const float v = (t > 4.0 && t < 6.5) ? 0.0f : 0.4f * float(std::sin(2 * juce::MathConstants<double>::pi * 440 * t));
                b.setSample(0, i, v);
                b.setSample(1, i, v);
            }
            w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
            w.reset();
            Session a;
            a.lanes = 1;
            a.append(regionFor(a, fm, f), 2.0);
            auto sil = findSilences(fm, a, -60.0, 1.0);
            CHECK(sil.size() == 2);   // the 2 s lead-in and the 2.5 s gap
            if (sil.size() == 2) {
                NEAR(sil[1].getStart(), 6.0, 0.05);
                NEAR(sil[1].getEnd(), 8.5, 0.05);
            }
            CHECK(analogBlackToMarks(a, sil) == 2);
            int starts2 = 0, pauses = 0;
            for (auto& m : a.markers) { starts2 += m.type == Marker::TrackStart; pauses += m.type == Marker::PauseStart; }
            CHECK(starts2 == 2 && pauses == 1);
        }
        // New PQ settings survive a save.
        juce::String err;
        Session back;
        p.pqTrack1Frames = 0;
        p.lockMarks = true;
        CHECK(back.fromVar(juce::JSON::parse(juce::JSON::toString(p.toVar())), err));
        CHECK(back.pqOffsetFrames == 5 && back.pqTrack1Frames == 0 && back.pqSpliceFrames == 2 && back.pqEndFrames == 3 && back.lockMarks);
        int own = 0;
        for (auto& m : back.markers) own += (!m.offsetDefault && m.offsetFrames == 20) ? 1 : 0;
        CHECK(own == 1);
    }
    // ---- Assembly: drop modes, segment gain, polarity ----
    {
        auto x1 = makeTone(dir, "as1.wav", 44100, 4.0, 300, 0.5f), x2 = makeTone(dir, "as2.wav", 44100, 2.0, 300, 0.25f);
        auto build = [&] {
            Session a;
            a.lanes = 1;
            Region r1 = regionFor(a, fm, x1); r1.start = 0; a.regions.add(r1);    // 0..4
            Region r2 = regionFor(a, fm, x1); r2.start = 6; a.regions.add(r2);    // 6..10
            Region r3 = regionFor(a, fm, x2); r3.start = 20; a.regions.add(r3);   // the one we drag
            a.addMarker(Marker::Index, 7.0, r2.id);
            a.addMarker(Marker::Index, 20.5, r3.id);
            return a;
        };
        {   // Insert at 2 s: r1 is split, everything after moves right by 2 s; r3's own mark rides with it.
            Session a = build();
            const int id = a.regions[2].id;
            a.dropRegion(id, 2.0, 0, Session::DropMode::Insert);
            CHECK(a.regions.size() == 4);
            NEAR(a.findRegion(id)->start, 2.0, 1e-9);
            double r2s = 0;
            for (auto& r : a.regions) if (r.id == a.regions[1].id) r2s = r.start;
            NEAR(r2s, 8.0, 1e-6);
            juce::Array<double> idx;
            for (auto& m : a.markers) idx.add(m.t);
            idx.sort();
            NEAR(idx[0], 2.5, 1e-6);   // r3's mark moved with r3
            NEAR(idx[1], 9.0, 1e-6);   // r2's mark rippled
            NEAR(a.end(), 12.0, 1e-6);
        }
        {   // Replace at 1 s: covers 1..3 of r1, nothing moves.
            Session a = build();
            const int id = a.regions[2].id;
            a.dropRegion(id, 1.0, 0, Session::DropMode::Replace);
            NEAR(a.end(), 10.0, 1e-6);
            ProgramRenderer pr(fm);
            pr.setSession(a, 44100);
            juce::AudioBuffer<float> buf(2, 44100);
            pr.seek(int(44100 * 1.5));
            pr.render(buf, 0, 22050);
            NEAR(rms(buf, 0, 22050), 0.25 / std::sqrt(2.0), 0.003);
        }
        {   // Segment gain: absolute, relative, normalize, polarity.
            Session a = build();
            juce::Array<int> ids{a.regions[0].id, a.regions[2].id};
            CHECK(applySegmentGain(a, ids, SegmentGainMode::Absolute, -3.0, -1) == 2);
            CHECK(a.regions[0].gainDb == -3.0 && a.regions[2].gainDb == -3.0);
            applySegmentGain(a, ids, SegmentGainMode::Relative, 1.5, -1);
            NEAR(a.regions[2].gainDb, -1.5, 1e-9);
            applySegmentGain(a, ids, SegmentGainMode::Normalize, -1.0, 1, [&](const Region& r) { return regionPeakDb(fm, r); });
            NEAR(a.regions[0].gainDb, -1.0 - 20 * std::log10(0.5), 0.01);    // peak 0.5 -> -1 dBFS
            NEAR(a.regions[2].gainDb, -1.0 - 20 * std::log10(0.25), 0.01);
            CHECK(a.regions[0].invert && a.regions[2].invert && !a.regions[1].invert);
            // Inverted polarity renders inverted.
            Session one;
            one.lanes = 1;
            Region q = regionFor(one, fm, x1); q.start = 0; one.regions.add(q);
            Session inv = one;
            inv.regions.getReference(0).invert = true;
            ProgramRenderer pa(fm), pb(fm);
            pa.setSession(one, 44100);
            pb.setSession(inv, 44100);
            juce::AudioBuffer<float> ba(2, 1000), bb(2, 1000);
            pa.seek(44100); pb.seek(44100);
            pa.render(ba, 0, 1000); pb.render(bb, 0, 1000);
            double worst = 0;
            for (int i = 0; i < 1000; ++i) worst = std::max(worst, double(std::abs(ba.getSample(0, i) + bb.getSample(0, i))));
            CHECK(worst < 1e-6 && rms(ba, 0, 1000) > 0.1);
            juce::String err;
            Session back;
            CHECK(back.fromVar(juce::JSON::parse(juce::JSON::toString(inv.toVar())), err) && back.regions[0].invert);
        }
    }
    // ---- Timeline gain ----
    {
        auto x = makeTone(dir, "tg.wav", 44100, 10.0, 400, 0.5f);
        Session g;
        g.lanes = 1;
        Region r = regionFor(g, fm, x); r.start = 0; g.regions.add(r);
        g.addGainNode(2.0, 0.0);
        g.addGainNode(4.0, -6.0);
        const int locked = g.addGainNode(8.0, -6.0);
        NEAR(g.timelineGainDb(1.0), 0.0, 1e-12);
        NEAR(g.timelineGainDb(3.0), -3.0, 1e-12);   // straight line in dB
        NEAR(g.timelineGainDb(9.0), -6.0, 1e-12);
        auto level = [&](const Session& ss, double t0) {
            ProgramRenderer pr(fm);
            pr.setSession(ss, 44100);
            juce::AudioBuffer<float> buf(2, 2205);
            pr.seek(juce::int64(t0 * 44100));
            pr.render(buf, 0, 2205);
            return 20 * std::log10(rms(buf, 0, 2205) / (0.5 / std::sqrt(2.0)));
        };
        NEAR(level(g, 0.5), 0.0, 0.02);
        NEAR(level(g, 5.0), -6.0, 0.02);
        NEAR(level(g, 2.975), -3.0, 0.1);
        g.gainBypass = true;
        NEAR(level(g, 5.0), 0.0, 0.02);
        g.gainBypass = false;
        // Ripple moves unlocked nodes; locked and globally locked ones stay.
        g.findGainNode(locked)->locked = true;
        g.ripple(3.0, 1.0);
        NEAR(g.gainNodes[1].t, 5.0, 1e-9);
        NEAR(g.findGainNode(locked)->t, 8.0, 1e-9);
        g.lockGain = true;
        g.ripple(3.0, 1.0);
        NEAR(g.gainNodes[1].t, 5.0, 1e-9);
        juce::String err;
        Session back;
        CHECK(back.fromVar(juce::JSON::parse(juce::JSON::toString(g.toVar())), err));
        CHECK(back.gainNodes.size() == 3 && back.lockGain && back.gainNodes[2].locked && std::abs(back.gainNodes[1].db + 6.0) < 1e-12);
        // With no nodes, the render is untouched (bit-for-bit).
        Session plain;
        plain.lanes = 1;
        plain.regions.add(r);
        ProgramRenderer pa(fm);
        pa.setSession(plain, 44100);
        juce::AudioBuffer<double> b1(2, 1000);
        pa.seek(44100);
        pa.render(b1, 0, 1000);
        plain.addGainNode(1.0, 0.0);   // a flat 0 dB node changes nothing either
        ProgramRenderer pb(fm);
        pb.setSession(plain, 44100);
        juce::AudioBuffer<double> b2(2, 1000);
        pb.seek(44100);
        pb.render(b2, 0, 1000);
        int diff = 0;
        for (int i = 0; i < 1000; ++i) diff += b1.getSample(0, i) != b2.getSample(0, i);
        CHECK(diff == 0);
    }
    // ---- Lane formats: mono, stereo, 5.1 ----
    {
        // A six-channel file: each channel a different level so the routing is visible.
        auto f6 = dir.getChildFile("six.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f6.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000, juce::AudioChannelSet::create5point1(), 24, {}, 0));
            os.release();
            juce::AudioBuffer<float> b(6, 48000);
            const float amp[6] = {0.1f, 0.2f, 0.3f, 0.05f, 0.4f, 0.5f};
            for (int c = 0; c < 6; ++c)
                for (int i = 0; i < 48000; ++i) b.setSample(c, i, amp[c] * float(std::sin(2 * juce::MathConstants<double>::pi * 300 * i / 48000.0)));
            w->writeFromAudioSampleBuffer(b, 0, 48000);
        }
        Session m;
        m.lanes = 1;
        Region r = regionFor(m, fm, f6); r.start = 0; m.regions.add(r);
        CHECK(m.programmeChannels() == 2);    // a stereo lane: the 5.1 file plays its front pair
        m.setLaneFormat(0, 6);
        CHECK(m.programmeChannels() == 6 && m.laneFormat(0) == 6);
        ProgramRenderer pr(fm);
        pr.setSession(m, 48000);
        CHECK(pr.channels() == 6);
        juce::AudioBuffer<double> six(6, 4800);
        pr.seek(4800);
        pr.render(six, 0, 4800);
        auto rmsD = [](const juce::AudioBuffer<double>& b, int c) { double s2 = 0; for (int i = 0; i < b.getNumSamples(); ++i) s2 += b.getSample(c, i) * b.getSample(c, i); return std::sqrt(s2 / b.getNumSamples()); };
        const double amps[6] = {0.1, 0.2, 0.3, 0.05, 0.4, 0.5};
        for (int c = 0; c < 6; ++c) NEAR(rmsD(six, c), amps[c] / std::sqrt(2.0), 0.002);
        // A stereo consumer gets the ITU fold-down: L = L + .707 C + .707 Ls.
        juce::AudioBuffer<float> two(2, 4800);
        pr.seek(4800);
        pr.render(two, 0, 4800);
        NEAR(rms(two, 0, 4800), (0.1 + 0.7071 * 0.3 + 0.7071 * 0.4) / std::sqrt(2.0), 0.003);
        // A mono lane averages a stereo file and feeds both sides of a stereo programme.
        auto st = makeTone(dir, "mono-src.wav", 44100, 2.0, 300, 0.4f);
        Session mono;
        mono.lanes = 1;
        Region q = regionFor(mono, fm, st); q.start = 0; mono.regions.add(q);
        mono.setLaneFormat(0, 1);
        CHECK(mono.programmeChannels() == 2);
        ProgramRenderer pm(fm);
        pm.setSession(mono, 44100);
        juce::AudioBuffer<float> mb(2, 4410);
        pm.seek(4410);
        pm.render(mb, 0, 4410);
        double d = 0;
        for (int i = 0; i < 4410; ++i) d = std::max(d, double(std::abs(mb.getSample(0, i) - mb.getSample(1, i))));
        CHECK(d < 1e-7);
        NEAR(rms(mb, 0, 4410), 0.4 / std::sqrt(2.0), 0.003);
        juce::String err;
        Session back;
        CHECK(back.fromVar(juce::JSON::parse(juce::JSON::toString(m.toVar())), err) && back.laneFormat(0) == 6 && back.laneFormat(3) == 2);
    }
    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
