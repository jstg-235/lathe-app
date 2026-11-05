// End-to-end batch render: DDP + WAV/FLAC 16/44.1 + hi-res, then checks that the 16-bit
// track files are bit-identical to the DDP image and the tags are present.
#include "../app/Source/Exporter.h"
#include "../core/util/FlacTags.h"

#include <cstdio>

using namespace lathe;

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static juce::File noiseFile(const juce::File& dir, const juce::String& name, double rate, double sec, int seed) {
    auto f = dir.getChildFile(name);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate, 2, 24, {}, 0));
    os.release();
    juce::Random rnd(seed);
    juce::AudioBuffer<float> b(2, int(rate * sec));
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(ch, i, (rnd.nextFloat() - 0.5f) * 0.5f);
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    return f;
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lathe_batch_test");
    dir.deleteRecursively();
    dir.createDirectory();

    AudioEngine engine;   // no audio device needed for offline renders
    auto& fm = engine.formats();
    for (double rate : {44100.0, 96000.0}) {
        auto a = noiseFile(dir, "a" + juce::String(int(rate)) + ".wav", rate, 5.0, 1);
        auto b = noiseFile(dir, "b" + juce::String(int(rate)) + ".wav", rate, 6.0, 2);
        Session s;
        s.discTitle = "Batch Test";
        s.discPerformer = "Lathe";
        for (auto& f : {a, b}) {
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
            int id = s.append(s.makeRegion(f, r->sampleRate, r->lengthInSamples), 2.0);
            for (auto& m : s.markers)
                if (m.attachedRegion == id && m.type == Marker::TrackStart) m.isrc = id == s.regions[0].id ? "USABC2600001" : "USABC2600002";
        }
        BatchSettings bs;
        bs.ddp = bs.wav16 = bs.flac16 = bs.wavHi = bs.flacHi = bs.programme = true;
        ExportSettings es;
        es.shape = dsp::Ditherer::Shape::Psycho;
        Exporter ex(engine);
        auto out = dir.getChildFile("out" + juce::String(int(rate)));
        auto res = ex.batchRender(s, bs, out, es, [](double) { return true; });
        if (!res.ok) std::printf("batch error: %s\n", res.error.toRawUTF8());
        CHECK(res.ok && res.ddpVerified);
        std::printf("%.0f Hz project: %d files\n", rate, res.files.size());

        // 16-bit track WAVs are exactly the DDP image between the PQ marks.
        juce::MemoryBlock image;
        out.getChildFile("DDP/IMAGE.DAT").loadFileAsData(image);
        auto disc = s.toDisc();
        auto wavDir = out.getChildFile("WAV 16-bit 44.1 kHz");
        auto wavs = wavDir.findChildFiles(juce::File::findFiles, false, "*.wav");
        wavs.sort();
        CHECK(wavs.size() == 2);
        int mismatched = 0;
        for (int t = 0; t < wavs.size(); ++t) {
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(wavs[t]));
            auto start = disc.tracks[size_t(t)].startSector() * 588;
            auto end = (t + 1 < int(disc.tracks.size()) ? disc.tracks[size_t(t + 1)].startSector() : disc.leadOutSector) * 588;
            CHECK(r->lengthInSamples == end - start);
            CHECK(r->metadataValues.getValue(juce::WavAudioFormat::riffInfoTitle, "") == wavs[t].getFileNameWithoutExtension().substring(3));
            juce::AudioBuffer<int> ib(2, int(r->lengthInSamples));
            int* chans[2] = {ib.getWritePointer(0), ib.getWritePointer(1)};
            r->read(chans, 2, 0, int(r->lengthInSamples), false);
            auto* img = static_cast<const uint8_t*>(image.getData());
            for (juce::int64 i = 0; i < r->lengthInSamples; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    auto off = size_t((start + i) * 4 + ch * 2);
                    int16_t v = int16_t(img[off] | (img[off + 1] << 8));
                    if ((ib.getSample(ch, int(i)) >> 16) != v) ++mismatched;
                }
        }
        std::printf("  16-bit WAVs vs DDP image: %d mismatched samples\n", mismatched);
        CHECK(mismatched == 0);

        // FLAC 16 decodes to the same audio as the WAV, and carries tags.
        auto flacs = out.getChildFile("FLAC 16-bit 44.1 kHz").findChildFiles(juce::File::findFiles, false, "*.flac");
        flacs.sort();
        CHECK(flacs.size() == 2);
        if (flacs.size() == 2) {
            util::Tags tags;
            CHECK(util::readFlacTags(flacs[1].getFullPathName().toStdString(), tags));
            bool isrc = false, total = false;
            for (auto& [k, v] : tags) { isrc |= k == "ISRC" && v == "USABC2600002"; total |= k == "TRACKTOTAL" && v == "2"; }
            CHECK(isrc && total);
            std::unique_ptr<juce::AudioFormatReader> rf(fm.createReaderFor(flacs[0])), rw(fm.createReaderFor(wavs[0]));
            CHECK(rf && rw && rf->lengthInSamples == rw->lengthInSamples);
            juce::AudioBuffer<float> x(2, 44100), y(2, 44100);
            rf->read(&x, 0, 44100, 22050, true, true);
            rw->read(&y, 0, 44100, 22050, true, true);
            bool same = true;
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 44100; ++i) same &= x.getSample(ch, i) == y.getSample(ch, i);
            CHECK(same);
        }

        // Hi-res files at the project rate, 24-bit, one per track; programme WAV; report with MD5s.
        auto hiLabel = juce::String("24-bit ") + (rate == 44100.0 ? "44.1" : "96") + " kHz";
        auto hi = out.getChildFile("WAV " + hiLabel).findChildFiles(juce::File::findFiles, false, "*.wav");
        CHECK(hi.size() == 2);
        if (hi.size() == 2) {
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(hi[0]));
            CHECK(r->sampleRate == rate && r->bitsPerSample == 24);
        }
        CHECK(out.getChildFile("FLAC " + hiLabel).findChildFiles(juce::File::findFiles, false, "*.flac").size() == 2);
        CHECK(out.getChildFile("Batch Test " + hiLabel + ".wav").existsAsFile());
        auto report = out.getChildFile("Render Report.txt").loadFileAsString();
        CHECK(report.contains("written and verified") && report.contains("IMAGE.DAT") && report.contains("Integrated loudness"));
    }
    // ---- Programme analysis through the real master path (processed + unprocessed).
    {
        auto a = noiseFile(dir, "an.wav", 44100, 6.0, 9);
        Session s;
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(a));
        s.append(s.makeRegion(a, r->sampleRate, r->lengthInSamples), 2.0);
        s.regions.getReference(0).gainDb = -6.0;
        Exporter ex(engine);
        dsp::AnalysisResult proc, raw;
        auto err = ex.analyseProgram(s, {}, [](double) { return true; }, proc, &raw);
        CHECK(err.isEmpty());
        CHECK(proc.tracks.size() == 1 && std::isfinite(proc.lufs));
        CHECK(std::abs(proc.lufs - raw.lufs) < 0.01);   // no plugins: processed == unprocessed
        CHECK(std::abs(proc.length - 8.0) < 0.05);
        CHECK(proc.map.shortTerm.size() >= 79);
        std::printf("analysis: %.2f LUFS, %d hops, %d problems\n", proc.lufs, int(proc.map.shortTerm.size()), int(proc.problems.size()));
    }

    // ---- Reference A/B in the audio callback: crossfades to the reference, at its gain.
    {
        auto ref = noiseFile(dir, "ref.wav", 44100, 3.0, 11);
        engine.prepareToPlay(512, 44100);
        Session empty;
        engine.loadSession(empty);
        CHECK(engine.loadReference(ref));
        engine.setReferenceGainDb(-6.0206);
        engine.setListenToReference(true);
        engine.play();
        juce::AudioBuffer<float> out(2, 512);
        double sumOut = 0;
        for (int b = 0; b < 40; ++b) {   // ~0.46 s; the 10 ms ramp is long done
            out.clear();
            juce::AudioSourceChannelInfo info(&out, 0, 512);
            engine.getNextAudioBlock(info);
            if (b > 10) sumOut += out.getRMSLevel(0, 0, 512);
        }
        std::unique_ptr<juce::AudioFormatReader> rr(fm.createReaderFor(ref));
        juce::AudioBuffer<float> src(2, 44100);
        rr->read(&src, 0, 44100, 0, true, true);
        double srcRms = src.getRMSLevel(0, 0, 44100);
        double ratio = (sumOut / 29.0) / srcRms;
        std::printf("reference A/B level ratio %.3f (expect 0.500)\n", ratio);
        CHECK(std::abs(ratio - 0.5) < 0.05);
        engine.setListenToReference(false);
        for (int b = 0; b < 4; ++b) { out.clear(); juce::AudioSourceChannelInfo info(&out, 0, 512); engine.getNextAudioBlock(info); }
        out.clear();
        juce::AudioSourceChannelInfo info(&out, 0, 512);
        engine.getNextAudioBlock(info);
        CHECK(out.getMagnitude(0, 0, 512) < 1e-6f);   // back to the (silent) master
        engine.stop();
        engine.clearReference();
        engine.releaseResources();
    }

    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
