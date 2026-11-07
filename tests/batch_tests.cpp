// End-to-end batch render: DDP + WAV/FLAC 16/44.1 + hi-res, then checks that the 16-bit
// track files are bit-identical to the DDP image and the tags are present.
#include "../app/Source/ControlSurface.h"
#include "../app/Source/DdpFormat.h"
#include "../app/Source/Exporter.h"
#include "../app/Source/Paperwork.h"
#include "../app/Source/Restore.h"
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

// Records which commands a control surface triggers.
struct CommandSpy : juce::ApplicationCommandTarget {
    juce::Array<juce::CommandID> fired;
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& ids) override { for (auto& d : allCommands()) ids.add(d.id); }
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override {
        if (auto* d = findCommand(id)) info.setInfo(d->name, d->description, d->category, 0);
    }
    bool perform(const InvocationInfo& i) override { fired.add(i.commandID); return true; }
};

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
    // ---- Region restoration: clicks in the selection repaired, outside untouched, undo restores.
    {
        auto f = noiseFile(dir, "clicky.wav", 44100, 6.0, 31);
        // Quieter base so planted clicks stand out: rewrite at -30 dB with clicks at 2 s and 4 s.
        {
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
            juce::AudioBuffer<float> x(2, int(r->lengthInSamples));
            r->read(&x, 0, x.getNumSamples(), 0, true, true);
            r.reset();
            for (int ch = 0; ch < 2; ++ch) {
                // smooth it (one-pole) so it's predictable, like music
                float z = 0;
                for (int i = 0; i < x.getNumSamples(); ++i) { z = 0.97f * z + 0.03f * x.getSample(ch, i); x.setSample(ch, i, z * 0.3f); }
                for (int at : {88200, 176400}) for (int k = 0; k < 6; ++k) x.setSample(ch, at + k, 0.6f);
            }
            juce::WavAudioFormat wav;
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 32, {}, 0));
            os.release();
            w->writeFromAudioSampleBuffer(x, 0, x.getNumSamples());
        }
        Session s;
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
        int id = s.append(s.makeRegion(f, r->sampleRate, r->lengthInSamples), 2.0);
        auto before = juce::JSON::toString(s.toVar());
        RestoreSettings rs;   // declick on by default
        // Only 3.5..4.5 s of the region (timeline 5.5..6.5): the click at 4 s is fixed, the one at 2 s isn't.
        auto res = restoreRegion(fm, *s.findRegion(id), {5.5, 6.5}, rs, nullptr, dir.getChildFile("Processed"), {});
        CHECK(res.ok);
        applyProcessed(s, id, res, "restored");
        auto* reg = s.findRegion(id);
        CHECK(reg->file == res.file && reg->name.contains("(restored)") && reg->srcOut - reg->srcIn == r->lengthInSamples);
        std::unique_ptr<juce::AudioFormatReader> pr(fm.createReaderFor(res.file));
        juce::AudioBuffer<float> orig(2, int(r->lengthInSamples)), proc(2, int(r->lengthInSamples));
        r->read(&orig, 0, orig.getNumSamples(), 0, true, true);
        pr->read(&proc, 0, proc.getNumSamples(), reg->srcIn, true, true);
        CHECK(proc.getSample(0, 88202) == orig.getSample(0, 88202));                 // outside: click kept, untouched
        CHECK(std::abs(proc.getSample(0, 176402)) < 0.2f);                            // inside: click repaired
        bool exactOutside = true;
        for (int i = 0; i < 44100 * 3; ++i) exactOutside &= proc.getSample(1, i) == orig.getSample(1, i);
        CHECK(exactOutside);
        std::printf("restore: %s; click at 4 s %.3f -> %.3f\n", res.report.replace("\n", "; ").toRawUTF8(), orig.getSample(0, 176402), proc.getSample(0, 176402));
        // "Undo" = the previous session state: the original file again.
        Session undone;
        juce::String e2;
        undone.fromVar(juce::JSON::parse(before), e2);
        CHECK(undone.findRegion(id)->file == f);
        // Spectral edit on the same region writes a new file and keeps the length.
        auto sres = spectralEditRegion(fm, *s.findRegion(id), 1.0, 1.5, 3000, 6000, dsp::SpectralMode::Attenuate, -30, dir.getChildFile("Processed"));
        CHECK(sres.ok && sres.srcOut - sres.srcIn == r->lengthInSamples);
    }

    // ---- Control surface: Mackie defaults, jog, MIDI learn, HUI zone/port learn.
    {
        CommandSpy spy;
        juce::ApplicationCommandManager cm;
        cm.registerAllCommandsForTarget(&spy);
        cm.setFirstCommandTarget(&spy);
        juce::PropertiesFile::Options po;
        po.applicationName = "LatheTest";
        po.filenameSuffix = ".settings";
        juce::PropertiesFile props(dir.getChildFile("surface.settings"), po);
        ControlSurface cs(cm, props);
        double jogged = 0;
        cs.jog = [&](double d) { jogged += d; };
        cs.isPlaying = [] { return true; };
        cs.handle(juce::MidiMessage::noteOn(1, 0x5E, juce::uint8(127)));   // Play
        cs.handle(juce::MidiMessage::noteOn(1, 0x5E, juce::uint8(0)));     // release: ignored
        cs.handle(juce::MidiMessage::noteOn(1, 0x54, juce::uint8(127)));   // Marker
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x3C, 3));         // jog +3
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x3C, 0x42));      // jog -2
        CHECK(spy.fired.size() == 2 && spy.fired[0] == cmd::playStop && spy.fired[1] == cmd::addTrackMarker);
        CHECK(std::abs(jogged - 1.0 / 75.0) < 1e-9);
        cs.learn(cmd::analyse);
        cs.handle(juce::MidiMessage::controllerEvent(2, 20, 127));         // learned, not fired
        CHECK(spy.fired.size() == 2 && cs.mappingFor(cmd::analyse).contains("CC 20"));
        cs.handle(juce::MidiMessage::controllerEvent(2, 20, 127));
        CHECK(spy.fired.getLast() == cmd::analyse);
        // HUI: zone 0x0E, port 3 press (0x43) / release (0x03).
        cs.configure({}, {}, ControlSurface::Protocol::Hui);
        cs.learn(cmd::exportDdp);
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x0F, 0x0E));
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x2F, 0x43));
        CHECK(cs.mappingFor(cmd::exportDdp).contains("zone 14 port 3"));
        int before = spy.fired.size();
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x0F, 0x0E));
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x2F, 0x03));      // release: nothing
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x0F, 0x0E));
        cs.handle(juce::MidiMessage::controllerEvent(1, 0x2F, 0x43));      // press: export
        CHECK(spy.fired.size() == before + 1 && spy.fired.getLast() == cmd::exportDdp);
        // Mappings persist.
        ControlSurface cs2(cm, props);
        CHECK(cs2.mappingFor(cmd::analyse).contains("CC 20"));
        std::printf("control surface: %d commands fired\n", spy.fired.size());
    }

    // ---- DDP round trip: render -> open as session -> re-export is bit-identical, PQ preserved.
    {
        auto a = noiseFile(dir, "rt_a.wav", 44100, 5.0, 21);
        auto b = noiseFile(dir, "rt_b.wav", 44100, 6.0, 22);
        Session s;
        s.discTitle = "Round Trip";
        s.upcEan = "0123456789012";
        for (auto& f : {a, b}) {
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
            s.append(s.makeRegion(f, r->sampleRate, r->lengthInSamples), 2.0);
        }
        s.addMarker(Marker::Index, 4.0);
        s.markers.getReference(0).isrc = "USABC2600009";
        Exporter ex(engine);
        BatchSettings bs;
        bs.ddp = true; bs.wav16 = bs.flac16 = bs.wavHi = bs.flacHi = bs.programme = false;
        ExportSettings es;
        es.shape = dsp::Ditherer::Shape::Psycho;
        auto first = dir.getChildFile("rt1");
        CHECK(ex.batchRender(s, bs, first, es, [](double) { return true; }).ok);

        Session imported;
        juce::String err;
        juce::StringArray notes;
        CHECK(sessionFromDdp(first.getChildFile("DDP"), imported, err, notes));
        auto d1 = s.toDisc(), d2 = imported.toDisc();
        CHECK(d2.tracks.size() == d1.tracks.size());
        bool pqSame = d1.leadOutSector == d2.leadOutSector && d2.upcEan == d1.upcEan;
        for (size_t t = 0; t < d1.tracks.size() && t < d2.tracks.size(); ++t) {
            pqSame &= d1.tracks[t].indexes.size() == d2.tracks[t].indexes.size() && d1.tracks[t].isrc == d2.tracks[t].isrc;
            for (size_t k = 0; k < d1.tracks[t].indexes.size() && k < d2.tracks[t].indexes.size(); ++k)
                pqSame &= d1.tracks[t].indexes[k].sector == d2.tracks[t].indexes[k].sector && d1.tracks[t].indexes[k].number == d2.tracks[t].indexes[k].number;
        }
        CHECK(pqSame);
        // The image plays through the DDP format reader.
        std::unique_ptr<juce::AudioFormatReader> img(fm.createReaderFor(first.getChildFile("DDP/IMAGE.DAT")));
        CHECK(img && img->lengthInSamples == d1.leadOutSector * 588);

        auto second = dir.getChildFile("rt2");
        CHECK(ex.batchRender(imported, bs, second, es, [](double) { return true; }).ok);
        juce::MemoryBlock m1, m2;
        first.getChildFile("DDP/IMAGE.DAT").loadFileAsData(m1);
        second.getChildFile("DDP/IMAGE.DAT").loadFileAsData(m2);
        CHECK(m1.getSize() == m2.getSize() && m1 == m2);
        std::printf("DDP round trip: %s, %d tracks\n", m1 == m2 ? "bit-identical re-export" : "IMAGE DIFFERS", int(d2.tracks.size()));
    }

    // ---- Recording: input in the callback buffer lands in the file, sample for sample.
    {
        engine.prepareToPlay(512, 44100);
        auto rf = dir.getChildFile("rec.wav");
        juce::String err;
        CHECK(engine.startRecording(rf, 0, 32, false, err));
        juce::AudioBuffer<float> io(2, 512);
        for (int b = 0; b < 20; ++b) {
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) io.setSample(ch, i, float((b * 512 + i) % 1000) / 1000.0f * (ch ? -0.5f : 0.5f));
            juce::AudioSourceChannelInfo info(&io, 0, 512);
            engine.getNextAudioBlock(info);
        }
        engine.stopRecording();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(rf));
        CHECK(r && r->lengthInSamples == 20 * 512);
        if (r) {
            juce::AudioBuffer<float> got(2, 10240);
            r->read(&got, 0, 10240, 0, true, true);
            bool same = true;
            for (int i = 0; i < 10240; ++i) same &= got.getSample(0, i) == float(i % 1000) / 1000.0f * 0.5f && got.getSample(1, i) == float(i % 1000) / 1000.0f * -0.5f;
            CHECK(same);
        }

        // Loopback latency test finds a simulated 300-sample round trip.
        engine.startLatencyTest(0);
        std::vector<float> line(300 + 512, 0.0f);
        std::vector<float> history;
        for (int b = 0; b < 120 && engine.latencyTestResult() < 0; ++b) {
            juce::AudioBuffer<float> blk(2, 512);
            for (int i = 0; i < 512; ++i) {
                int k = int(history.size()) + i - 300;
                float v = k >= 0 && k < int(history.size()) ? history[size_t(k)] : 0.0f;
                blk.setSample(0, i, v);
                blk.setSample(1, i, v);
            }
            juce::AudioSourceChannelInfo info(&blk, 0, 512);
            engine.getNextAudioBlock(info);
            for (int i = 0; i < 512; ++i) history.push_back(blk.getSample(0, i));
        }
        std::printf("latency test measured %d samples (simulated 300)\n", engine.latencyTestResult());
        CHECK(engine.latencyTestResult() == 300);
        engine.releaseResources();
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
        int measured = 0;
        for (int b = 0; b < 60; ++b) {
            // Pace the blocks like a real device so the read-ahead thread keeps up.
            juce::Thread::sleep(b < 8 ? 60 : 5);
            out.clear();
            juce::AudioSourceChannelInfo info(&out, 0, 512);
            engine.getNextAudioBlock(info);
            if (b > 20) { sumOut += out.getRMSLevel(0, 0, 512); ++measured; }
        }
        std::unique_ptr<juce::AudioFormatReader> rr(fm.createReaderFor(ref));
        juce::AudioBuffer<float> src(2, 44100);
        rr->read(&src, 0, 44100, 0, true, true);
        double srcRms = src.getRMSLevel(0, 0, 44100);
        double ratio = (sumOut / measured) / srcRms;
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


    // ---- Products: per-disc DDPs and LP side files with their own fades, plus paperwork ----
    {
        // 16-bit sources, so unity-gain samples pass dither untouched and can be compared exactly.
        auto make16 = [&](const juce::String& name, double sec, int seed) {
            auto f = dir.getChildFile(name);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, 2, 16, {}, 0));
            os.release();
            juce::Random rnd(seed);
            std::vector<int> l(size_t(44100 * sec)), r(l.size());
            for (size_t i = 0; i < l.size(); ++i) { l[i] = (rnd.nextInt(20000) - 10000) << 16; r[i] = (rnd.nextInt(20000) - 10000) << 16; }
            const int* ch[3] = {l.data(), r.data(), nullptr};
            w->write(ch, int(l.size()));
            return f;
        };
        auto a = make16("pa.wav", 5.0, 11);
        auto b = make16("pb.wav", 12.0, 12);
        Session s;
        s.discTitle = "Split Test";
        s.discPerformer = "Lathe Ensemble";
        s.label = "Test Records";
        s.catalogue = "TR-001";
        s.upcEan = "0123456789012";
        s.releaseDate = "2026-12-01";
        s.pLine = "2026 Test Records";
        s.cLine = "2026 Test Records";
        s.engineer = "A. Engineer";
        s.client = "Test Client";
        s.cdTextGenre = 14;
        for (auto& f : {a, b}) {
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
            s.append(s.makeRegion(f, r->sampleRate, r->lengthInSamples), 2.0);
        }
        s.sortMarkers();
        int tn = 0;
        for (auto& m : s.markers)
            if (m.type == Marker::TrackStart) {
                ++tn;
                m.text.title = tn == 1 ? "Overture" : "Long Movement";
                m.isrc = tn == 1 ? "USABC2600011" : "USABC2600012";
                m.text.songwriter = "C. Omposer";
                m.publisher = "Test Music";
                m.pro = "BMI";
                m.explicitContent = tn == 2;
            }
        // a: 2..7 s, b: 9..21 s.
        Product cdp;
        cdp.id = s.nextId++;
        cdp.kind = ProductKind::CD;
        cdp.name = "CD";
        cdp.catalogue = "TR-001CD";
        cdp.units.add({0.0, 15.0, 0.0, 1.0, FadeShape::EqualPower, FadeShape::Linear});
        cdp.units.add({14.0, 21.0, 1.0, 0.0, FadeShape::Linear, FadeShape::EqualPower});
        s.products.add(cdp);
        Product lp;
        lp.id = s.nextId++;
        lp.kind = ProductKind::LP12_33;
        lp.name = "LP";
        lp.catalogue = "TR-001LP";
        lp.units.add({0.0, 9.0});
        lp.units.add({9.0, 21.0});
        s.products.add(lp);

        BatchSettings bs;
        bs.ddp = true;
        bs.wav16 = bs.flac16 = bs.wavHi = bs.flacHi = false;
        bs.programme = true;
        ExportSettings es;
        es.shape = dsp::Ditherer::Shape::None;
        Exporter ex(engine);
        auto out = dir.getChildFile("products");
        auto res = ex.batchRender(s, bs, out, es, [](double) { return true; });
        if (!res.ok) std::printf("products error: %s\n", res.error.toRawUTF8());
        std::printf("products: %d units, %d files\n", int(res.delivery.units.size()), res.files.size());
        CHECK(res.ok && res.delivery.units.size() == 4);

        auto full = out.getChildFile("DDP/IMAGE.DAT");
        auto disc1 = out.getChildFile("CD DDP/Disc 1 - TR-001CD-D1/IMAGE.DAT");
        auto disc2 = out.getChildFile("CD DDP/Disc 2 - TR-001CD-D2/IMAGE.DAT");
        CHECK(full.existsAsFile() && disc1.existsAsFile() && disc2.existsAsFile());
        juce::MemoryBlock mf, m1, m2;
        full.loadFileAsData(mf);
        disc1.loadFileAsData(m1);
        disc2.loadFileAsData(m2);
        auto sample = [](const juce::MemoryBlock& m, juce::int64 frame, int ch) {
            auto* p = static_cast<const uint8_t*>(m.getData()) + (frame * 2 + ch) * 2;
            return int16_t(p[0] | (p[1] << 8));
        };
        // Disc 1: identical to the full programme up to its fade-out (14 s), faded after, silent padding at the end.
        int diff1 = 0;
        for (juce::int64 i = 0; i < 14 * 44100; ++i) diff1 += sample(m1, i, 0) != sample(mf, i, 0) || sample(m1, i, 1) != sample(mf, i, 1);
        CHECK(diff1 == 0);
        juce::int64 mid = juce::int64(14.5 * 44100);
        double ratio = double(sample(m1, mid, 0)) / double(sample(mf, mid, 0));
        CHECK(std::abs(ratio - 0.5) < 0.01);   // linear fade-out half way
        CHECK(m1.getSize() == size_t(std::ceil(15.0 * 75)) * 2352);
        // Disc 2: 2 s of digital silence, then the programme from 14 s with a 1 s fade-in.
        bool silentPregap = true;
        for (juce::int64 i = 0; i < 2 * 44100; ++i) silentPregap = silentPregap && sample(m2, i, 0) == 0 && sample(m2, i, 1) == 0;
        CHECK(silentPregap);
        int diff2 = 0;
        for (juce::int64 i = 44100; i < 6 * 44100; ++i) diff2 += sample(m2, 2 * 44100 + i, 0) != sample(mf, 14 * 44100 + i, 0);
        CHECK(diff2 == 0);
        CHECK(m2.getSize() == size_t(std::ceil(9.0 * 75)) * 2352);
        auto rd = ddp::readDdp(out.getChildFile("CD DDP/Disc 2 - TR-001CD-D2").getFullPathName().toStdString());
        CHECK(rd.ok && rd.checksumsOk && rd.disc.tracks.size() == 1 && rd.disc.tracks[0].isrc == "USABC2600012" && rd.disc.tracks[0].text.title == "Long Movement (cont.)");
        CHECK(rd.disc.masterId == "TR-001CD-D2");

        // LP sides: hi-res files whose samples are exactly the programme's (unity gain, 16-bit source).
        auto sideB = out.getChildFile("LP Sides/Split Test - LP Side B.wav");
        auto prog = out.getChildFile("Split Test 24-bit 44.1 kHz.wav");
        std::unique_ptr<juce::AudioFormatReader> rb(fm.createReaderFor(sideB)), rp(fm.createReaderFor(prog));
        CHECK(rb && rp && rb->lengthInSamples == 12 * 44100 && rb->bitsPerSample == 24);
        if (rb && rp) {
            juce::AudioBuffer<float> bb(2, 44100), pb(2, 44100);
            rb->read(&bb, 0, 44100, 0, true, true);
            rp->read(&pb, 0, 44100, 9 * 44100, true, true);
            int d = 0;
            for (int i = 0; i < 44100; ++i) d += bb.getSample(0, i) != pb.getSample(0, i);
            CHECK(d == 0);
        }
        CHECK(res.report.contains("PRODUCTS") && res.report.contains("CD Disc 2"));

        // Paperwork with a logo.
        auto logo = dir.getChildFile("logo.png");
        {
            juce::Image img(juce::Image::ARGB, 300, 100, true);
            juce::Graphics g(img);
            g.setColour(juce::Colours::darkorange);
            g.fillEllipse(10, 10, 80, 80);
            g.setColour(juce::Colours::black);
            g.setFont(40.0f);
            g.drawText("STUDIO", 100, 20, 190, 60, juce::Justification::centredLeft);
            juce::FileOutputStream fo(logo);
            juce::PNGImageFormat().writeImageToStream(img, fo);
        }
        StudioProfile st;
        st.name = "Lathe Test Mastering";
        st.address = "1 Groove Street\nNashville, TN 37203";
        st.phone = "+1 615 555 0100";
        st.email = "studio@example.com";
        st.website = "example.com";
        st.logo = logo;
        CHECK(!logoJpeg(logo).empty());
        auto pw = writePaperwork(s, res.delivery, st, out);
        if (pw.error.isNotEmpty()) std::printf("paperwork error: %s\n", pw.error.toRawUTF8());
        CHECK(pw.error.isEmpty());
        for (auto& n : {"PQ Sheet - Full Programme - TR-001.pdf", "PQ Sheet - CD.pdf", "Cutting Sheet - LP.pdf", "Label Copy.pdf", "Delivery Manifest.pdf", "Track Metadata.csv", "Delivery.json"})
            CHECK(out.getChildFile("Paperwork").getChildFile(n).existsAsFile());
        juce::MemoryBlock pdf;
        out.getChildFile("Paperwork/PQ Sheet - CD.pdf").loadFileAsData(pdf);
        std::string pdfText(static_cast<const char*>(pdf.getData()), pdf.getSize());
        CHECK(pdfText.rfind("%PDF-1.4", 0) == 0 && pdfText.find("/DCTDecode") != std::string::npos && pdfText.find("Long Movement \\(cont.\\)") != std::string::npos);
        auto json = juce::JSON::parse(out.getChildFile("Paperwork/Delivery.json"));
        CHECK(json["products"].size() == 2 && json["tracks"].size() == 2 && json["files"].size() > 10);
        CHECK(json["files"][0]["md5"].toString().length() == 32);
        auto csv = out.getChildFile("Paperwork/Track Metadata.csv").loadFileAsString();
        CHECK(csv.contains("Long Movement") && csv.contains("BMI") && csv.contains("Side B"));
        // A product named like the full programme's sheet doesn't overwrite it.
        {
            Session same = s;
            same.catalogue = {};
            auto pw2 = writePaperwork(same, res.delivery, st, dir.getChildFile("pw2"));
            CHECK(pw2.error.isEmpty() && pw2.files.contains("Paperwork/PQ Sheet - Full Programme.pdf") && pw2.files.contains("Paperwork/PQ Sheet - CD.pdf"));
        }
        if (auto* keep = std::getenv("LATHE_KEEP_PAPERWORK")) out.getChildFile("Paperwork").copyDirectoryTo(juce::File(keep));
    }
    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
