// End-to-end batch render: DDP + WAV/FLAC 16/44.1 + hi-res, then checks that the 16-bit
// track files are bit-identical to the DDP image and the tags are present.
#include "../app/Source/Capture.h"
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

// A test plugin: fixed gain with a fixed latency.
class GainDelayPlugin : public juce::AudioPluginInstance {
public:
    GainDelayPlugin(float gain, int latency)
        : AudioPluginInstance(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())),
          gain_(gain), lat_(latency) {}
    void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = "GainDelay"; d.pluginFormatName = "Test"; d.fileOrIdentifier = "gaindelay"; }
    const juce::String getName() const override { return "GainDelay"; }
    void prepareToPlay(double, int) override { setLatencySamples(lat_); reset(); }
    void reset() override { for (auto& d : delay_) d.assign(size_t(lat_ + 1), 0.0f); pos_ = 0; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int i = 0; i < b.getNumSamples(); ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                auto& d = delay_[size_t(ch)];
                d[size_t(pos_)] = b.getSample(ch, i) * gain_;
                b.setSample(ch, i, d[size_t((pos_ + 1) % (lat_ + 1))]);
            }
            pos_ = (pos_ + 1) % (lat_ + 1);
        }
    }
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
    float gain_;
    int lat_, pos_ = 0;
    std::vector<float> delay_[2];
};

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
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // keep output if a test crashes
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
    // (No interface from here on: a real one would pull blocks while the tests feed their own.)
    engine.devices().closeAudioDevice();
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


    // ---- Compare Unprocessed (A / B / C): time-aligned, instant, level matched by measurement.
    // A real device (a Mac has one even on a build machine) would pull audio from the engine at
    // the same time as the test: close it, so the test owns every block.
    engine.devices().closeAudioDevice();
    for (int withRegionFx = 0; withRegionFx < 2; ++withRegionFx) {
        auto a = noiseFile(dir, "cmp.wav", 44100, 8.0, 21);
        engine.prepareToPlay(512, 44100);
        engine.chain().prepare(44100, 512);
        juce::StringArray problems;
        engine.chain().setState({}, problems);
        engine.chain().addInstance(std::make_unique<GainDelayPlugin>(2.0f, 300));   // master: +6 dB, 300 samples late
        Session s;
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(a));
        const int id = s.append(s.makeRegion(a, r->sampleRate, r->lengthInSamples), 0.0);
        if (withRegionFx) {
            engine.regionFx().prepare(44100, 2048, false);
            engine.regionFx().chainFor(id, true, "cmp")->addInstance(std::make_unique<GainDelayPlugin>(0.5f, 700));   // region: -6 dB, 700 late
            engine.regionFx().syncAndCapture(s);
            CHECK(s.findRegion(id)->fxState.isNotEmpty());
        }
        engine.loadSession(s);
        engine.resetMatch();
        // Plays the first `blocks` blocks from the top and returns the output.
        auto run = [&](bool dry, bool match, int blocks) {
            engine.stop();
            engine.setPositionSeconds(2.0);   // the programme starts after the 2 s pre-gap
            engine.setHearUnprocessed(dry);
            engine.setLevelMatch(match);
            engine.play();
            juce::AudioBuffer<float> all(2, blocks * 512);
            for (int b = 0; b < blocks; ++b) {
                CHECK(engine.waitForReadAhead(5000));   // CI machines can be slow: never play an underrun
                juce::AudioBuffer<float> out(2, 512);
                juce::AudioSourceChannelInfo info(&out, 0, 512);
                engine.getNextAudioBlock(info);
                for (int ch = 0; ch < 2; ++ch) all.copyFrom(ch, b * 512, out, ch, 0, 512);
            }
            return all;
        };
        const int blocks = 40;
        run(false, false, 100);   // let any earlier level-match gain glide back to unity
        auto A = run(false, false, blocks);
        auto B = run(true, false, blocks);
        // Region effects (when present) are -6 dB and the master +6 dB, so A = 2x B (no region
        // effects) or A = B; either way the same samples at the same time.
        const float expect = withRegionFx ? 1.0f : 2.0f;
        double err = 0, ref = 0;
        for (int i = 20 * 512; i < blocks * 512; ++i) {
            const double d = A.getSample(0, i) - expect * B.getSample(0, i);
            err += d * d;
            ref += double(A.getSample(0, i)) * A.getSample(0, i);
        }
        const double nullDb = 10.0 * std::log10(juce::jmax(1e-30, err / juce::jmax(1e-30, ref)));
        std::printf("compare%s: A against B nulls to %.1f dB (time-aligned)\n", withRegionFx ? " with region effects" : "", nullDb);
        CHECK(nullDb < -100.0);
        // Level match: about 3 s of play measures the difference, then C sits at B's level.
        engine.resetMatch();
        auto C = run(false, true, 400);
        const double expectDiff = withRegionFx ? 0.0 : 6.0206;
        std::printf("  measured A against B %.2f dB, cut %.2f dB (match %s)\n", engine.matchDifferenceDb(), engine.matchCutDb(),
                    engine.matchMeasured() ? "measured" : "not measured");
        CHECK(engine.matchMeasured());
        CHECK(std::abs(engine.matchDifferenceDb() - expectDiff) < 0.05);
        CHECK(std::abs(engine.matchCutDb() + expectDiff) < 0.05);
        auto Bl = run(true, false, 400);
        const double rc = C.getRMSLevel(0, 370 * 512, 25 * 512), rb = Bl.getRMSLevel(0, 370 * 512, 25 * 512);
        std::printf("  C against B level %.2f dB\n", 20.0 * std::log10(rc / rb));
        CHECK(std::abs(20.0 * std::log10(rc / rb)) < 0.1);
        // The meters read A, even while C or B is heard.
        run(false, true, 30);   // (locating clears the meter feed, so it holds just this run)
        juce::AudioBuffer<float> tapped(2, 30 * 512);
        const int got = engine.meterTap().pull(tapped);
        CHECK(got == 30 * 512);
        if (got == 30 * 512) {
            const double rt = tapped.getRMSLevel(0, 20 * 512, 10 * 512), ra = A.getRMSLevel(0, 20 * 512, 10 * 512);
            std::printf("  meters against A %.2f dB\n", 20.0 * std::log10(rt / ra));
            CHECK(std::abs(20.0 * std::log10(rt / ra)) < 0.05);
        }
        engine.stop();
        engine.setHearUnprocessed(false);
        engine.setLevelMatch(false);
        engine.chain().setState({}, problems);
        engine.regionFx().clear();
        engine.loadSession(Session());
        engine.releaseResources();
    }

    // ---- Audio Setup settings, capture planning and landing (no audio) ----
    {
        IoSettings io;
        io.monitorOut = 2;
        io.monitorSix = true;
        io.defaultInput = 4;
        io.nextToSession = false;
        io.folder = dir.getChildFile("takes").getFullPathName();
        io.bits = 32;
        io.inputMonitoring = true;
        io.captureOut = 6;
        io.captureIn = 8;
        io.captureMute = true;
        io.capturePostRoll = 5.0;
        const auto key = IoSettings::delayKey("Interface A", "Interface A", 6, 8, 96000.0, 256);
        io.delays[key] = 613;
        IoSettings back;
        back.fromVar(juce::JSON::parse(juce::JSON::toString(io.toVar())));
        CHECK(back.monitorOut == 2 && back.monitorSix && back.defaultInput == 4 && !back.nextToSession && back.folder == io.folder);
        CHECK(back.bits == 32 && back.inputMonitoring && back.captureOut == 6 && back.captureIn == 8 && back.captureMute && back.capturePostRoll == 5.0);
        CHECK(back.delay(key) == 613);
        // A measurement belongs to its sockets, sample rate and buffer: anything else is "not measured".
        CHECK(back.delay(IoSettings::delayKey("Interface A", "Interface A", 6, 8, 96000.0, 512)) == -1);
        CHECK(back.delay(IoSettings::delayKey("Interface A", "Interface A", 6, 10, 96000.0, 256)) == -1);
        CHECK(back.delay(IoSettings::delayKey("Interface A", "Interface B", 6, 8, 96000.0, 256)) == -1);
        // Recordings go next to the session by default, or in the chosen folder.
        auto sessionFile = dir.getChildFile("Album.lathe");
        sessionFile.replaceWithText("{}");
        IoSettings def;
        CHECK(def.nextToSession && def.bits == 24 && !def.inputMonitoring);
        CHECK(recordingsFolder(def, sessionFile) == dir.getChildFile("Album Recordings"));
        CHECK(recordingsFolder(io, sessionFile) == dir.getChildFile("takes"));
        CHECK(recordingName(sessionFile, "Capture", juce::Time(2026, 10, 10, 16, 11, 5)) == "Album Capture 2026-11-10 161105");
        CHECK(recordingName(juce::File(), "Print", juce::Time(2026, 0, 2, 3, 4, 5)).startsWith("Untitled Print 2026-01-02"));
        CHECK(recordingBytes(10.0, 48000.0, 2, 24) == 48000 * 10 * 2 * 3 + 4096);

        // Planning: the In-Out or the whole programme; the lane type; the lowest lane heard.
        Session s;
        auto add = [&](int lane, double start, double len) {
            Region r = s.makeRegion(dir.getChildFile("x.wav"), 48000, juce::int64(len * 48000));
            r.lane = lane;
            r.start = start;
            s.regions.add(r);
            s.lanes = juce::jmax(s.lanes, lane + 1);
            return r.id;
        };
        const int a = add(0, 2.0, 10.0), b = add(1, 4.0, 4.0), c = add(2, 20.0, 5.0);
        auto whole = planCapture(s, {});
        CHECK(whole.whole && whole.range.getStart() == 0.0 && whole.range.getEnd() == 25.0 && whole.channels == 2 && whole.underLane == 2 && whole.sources.size() == 3);
        auto part = planCapture(s, {5.0, 7.0});
        CHECK(!part.whole && part.valid() && part.underLane == 1 && part.sources.size() == 2 && part.sources.contains(a) && part.sources.contains(b));
        CHECK(!planCapture(s, {13.0, 19.0}).valid());   // silence between the In and Out
        s.setLaneFormat(0, 1);
        CHECK(planCapture(s, {2.5, 3.5}).channels == 1);   // only the mono lane is heard there
        CHECK(planCapture(s, {5.0, 7.0}).channels == 2);   // mono and stereo together
        s.setLaneFormat(2, 6);
        CHECK(planCapture(s, {5.0, 7.0}).channels == 6);   // a 5.1 programme is captured as 5.1
        s.setLaneFormat(2, 2);
        s.findRegion(b)->mute = true;
        CHECK(planCapture(s, {5.0, 7.0}).underLane == 0);   // a muted region isn't part of it
        s.findRegion(b)->mute = false;

        // Landing: a new lane directly under the material, in sync, the round trip taken off.
        s.lanesOff.add(2);
        CaptureTake take;
        take.file = dir.getChildFile("take.wav");
        take.rate = 48000;
        take.startSample = 3 * 48000 - 3;            // the file began 2 s (less the read-ahead) before the In point
        take.roundTrip = 700;
        take.length = (2 + 2 + 1) * 48000 + 700;     // pre-roll, the range, a tail
        part = planCapture(s, {5.0, 7.0});
        const int id = placeCapture(s, part, take, true, "Take");
        CHECK(id != 0);
        if (auto* r = s.findRegion(id)) {
            CHECK(r->lane == 2 && s.lanes == 4);
            CHECK(std::abs(r->start - 5.0) < 1e-12 && r->srcIn == 2 * 48000 + 3 + 700 && r->srcOut - r->srcIn == 2 * 48000);
            CHECK(r->name == "Take" && !r->mute);
            CHECK(s.laneFormat(2) == 2 && s.laneFormat(0) == 1);
        }
        CHECK(s.findRegion(c)->lane == 3 && s.lanesOff.contains(3) && !s.lanesOff.contains(2));   // what was below moved down
        // Mute the original: only the captured stretch goes quiet.
        int mutedPieces = 0;
        double mutedLength = 0;
        for (auto& r : s.regions)
            if (r.mute) { ++mutedPieces; mutedLength += r.length(); CHECK(r.start >= 5.0 - 1e-9 && r.end() <= 7.0 + 1e-9); }
        CHECK(mutedPieces == 2 && std::abs(mutedLength - 4.0) < 1e-6);
        double heardLane0 = 0;
        for (auto& r : s.regions)
            if (r.lane == 0 && !r.mute) heardLane0 += r.length();
        CHECK(std::abs(heardLane0 - 8.0) < 1e-6);
        // Stopped early: what was recorded is kept; nothing usable gives no region.
        Session e;
        Region er = e.makeRegion(dir.getChildFile("x.wav"), 48000, 480000);
        er.start = 2.0;
        e.regions.add(er);
        auto ep = planCapture(e, {5.0, 7.0});
        take.length = 2 * 48000 + 3 + 700 + 24000;   // half a second into the range
        Session e1 = e;
        const int eid = placeCapture(e1, ep, take, false, "Early");
        CHECK(eid != 0 && e1.findRegion(eid)->srcOut - e1.findRegion(eid)->srcIn == 24000 && e1.findRegion(eid)->lane == 1);
        take.length = 48000;                          // stopped during the pre-roll
        Session e2 = e;
        CHECK(placeCapture(e2, ep, take, false, "None") == 0 && e2.regions.size() == 1 && e2.lanes == e.lanes);
        // The capture on its own, for switching against what was sent.
        auto alone = captureAlone(e1, eid);
        CHECK(alone.regions.size() == 1 && alone.regions[0].lane == 0 && alone.regions[0].start == e1.findRegion(eid)->start);
    }

    // ---- Renaming an audio file on disk: the session follows ----
    {
        auto folder = dir.getChildFile("rename");
        folder.createDirectory();
        auto a = noiseFile(folder, "Album Capture 2026-11-10 161105.wav", 44100, 1.0, 5);
        auto other = noiseFile(folder, "Taken.wav", 44100, 1.0, 6);
        Session s;
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(a));
        const int id = s.append(s.makeRegion(a, r->sampleRate, r->lengthInSamples), 0.0);
        r.reset();
        s.split(id, 2.5);
        s.references.add({a, "ref", -1000, 0});
        Session srcSide;
        srcSide.regions.add(s.regions[0]);
        s.source = &srcSide;
        juce::File now;
        CHECK(renameAudioFile(s, a, "  ", now).isNotEmpty());                       // no name
        CHECK(renameAudioFile(s, a, "Taken", now).isNotEmpty() && a.existsAsFile());  // never over another file
        CHECK(renameAudioFile(s, folder.getChildFile("nope.wav"), "x", now).isNotEmpty());
        CHECK(renameAudioFile(s, a, "Track 3: through the Fairchild.wav", now).isEmpty());
        CHECK(now.existsAsFile() && !a.existsAsFile() && now.getParentDirectory() == folder && now.getFileExtension() == ".wav");
        CHECK(now.getFileNameWithoutExtension().startsWith("Track 3") && !now.getFileName().containsChar(':'));   // made legal, the extension not doubled
        bool all = true;
        for (auto& reg : s.regions) all &= reg.file == now && reg.name == now.getFileNameWithoutExtension();
        CHECK(all && s.regions.size() == 2 && s.references[0].file == now && srcSide.regions[0].file == now);
        CHECK(s.missingAudio().isEmpty());
        s.source = nullptr;
    }

    // ---- Inputs, routing and Capture through a simulated loop (the engine's own blocks) ----
    {
        const int N = 512;
        engine.prepareToPlay(N, 44100);
        engine.chain().prepare(44100, N);
        juce::StringArray problems;
        engine.chain().setState({}, problems);
        auto src = noiseFile(dir, "cap.wav", 44100, 8.0, 77);
        std::unique_ptr<juce::AudioFormatReader> sr(fm.createReaderFor(src));
        juce::AudioBuffer<float> noise(2, int(sr->lengthInSamples));
        sr->read(&noise, 0, noise.getNumSamples(), 0, true, true);
        Session s;
        const int rid = s.append(s.makeRegion(src, sr->sampleRate, sr->lengthInSamples), 0.0);
        const juce::int64 regionStart = juce::int64(std::llround(s.findRegion(rid)->start * 44100));   // 2 s
        engine.loadSession(s);

        // Input meters: armed inputs reach the input feed (and only that), overs are counted.
        {
            engine.setInputSelection(0, 2);
            juce::AudioBuffer<float> io(2, N);
            auto fill = [&](float l, float r) { for (int i = 0; i < N; ++i) { io.setSample(0, i, l); io.setSample(1, i, r); } };
            juce::AudioBuffer<float> got(2, 4 * N);
            engine.inputTap().clear();
            engine.meterTap().clear();
            fill(0.25f, -0.5f);
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
            CHECK(!engine.inputsLive() && engine.inputTap().pull(got) == 0);   // not armed: nothing is measured
            CHECK(io.getMagnitude(0, N) == 0.0f);                               // and no input leaks to the outputs
            engine.setArmed(true);
            engine.resetInputOvers();
            fill(0.25f, -0.5f);
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
            CHECK(engine.inputsLive() && engine.inputTap().pull(got) == N);
            CHECK(got.getSample(0, 100) == 0.25f && got.getSample(1, 100) == -0.5f);
            CHECK(io.getMagnitude(0, N) == 0.0f);                               // input monitoring is off by default
            CHECK(engine.meterTap().pull(got) == 0);                            // the output meters don't see the inputs
            CHECK(engine.inputOvers() == 0 && std::abs(engine.inputMax() - 0.5f) < 1e-6f);
            fill(0.25f, -0.5f);
            io.setSample(1, 7, 1.0f);
            io.setSample(0, 9, -1.2f);
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
            CHECK(engine.inputOvers() == 2 && engine.inputMax() >= 1.2f);
            engine.resetInputOvers();
            CHECK(engine.inputOvers() == 0);
            // Input monitoring: the armed pair is played on the monitor pair.
            engine.setInputMonitoring(true);
            fill(0.25f, -0.5f);
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
            CHECK(io.getSample(0, 5) == 0.25f && io.getSample(1, 5) == -0.5f);
            // A mono input is shown on both bars.
            engine.setInputSelection(1, 1);
            engine.inputTap().clear();
            fill(0.25f, -0.5f);
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
            CHECK(engine.inputTap().pull(got) == N && got.getSample(0, 3) == -0.5f && got.getSample(1, 3) == -0.5f);
            engine.setInputMonitoring(false);
            engine.setArmed(false);
            engine.setInputSelection(0, 2);
        }

        // Output pair: playback goes to the chosen pair, nothing else is touched by it.
        {
            engine.setMonitorOutput(2);
            engine.setPositionSeconds(3.0);
            engine.play();
            juce::AudioBuffer<float> io(4, N);
            bool onPair = true, quietElsewhere = true;
            for (int b = 0; b < 30; ++b) {
                CHECK(engine.waitForReadAhead(5000));
                for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < N; ++i) io.setSample(ch, i, 0.9f);   // "inputs" in the device's buffer
                engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                quietElsewhere &= io.getMagnitude(0, 0, N) == 0.0f && io.getMagnitude(1, 0, N) == 0.0f;
                if (b > 4) onPair &= io.getRMSLevel(2, 0, N) > 0.01f && io.getRMSLevel(3, 0, N) > 0.01f && io.getMagnitude(2, 0, N) < 0.9f;
            }
            CHECK(onPair && quietElsewhere);
            engine.stop();
            engine.setMonitorOutput(-1);
        }

        // One capture through a loop of `trip` samples with `gearGain`. Returns the placed session.
        struct Run { Session after; CapturePlan plan; CaptureTake take; int id = 0; bool complete = false; juce::AudioBuffer<float> file; int fileChannels = 0; };
        auto capture = [&](const Session& from, juce::Range<double> selection, int trip, float gearGain, int stopAfterBlocks, const juce::String& name,
                           int bufferChannels = 2, bool mute = false) {
            Run run;
            run.plan = planCapture(from, selection);
            AudioEngine::CaptureRequest rq;
            rq.file = dir.getChildFile(name + ".wav");
            rq.channels = run.plan.channels;
            rq.bits = 32;
            rq.from = run.plan.range.getStart();
            rq.to = run.plan.range.getEnd();
            rq.preRoll = 2.0;
            rq.postRoll = 0.5;
            juce::String err;
            CHECK(engine.startCapture(rq, from, err));
            CHECK(engine.captureActive() && engine.captureRecording() && engine.inputsLive());
            std::vector<std::vector<float>> sent{size_t(bufferChannels), std::vector<float>()};
            juce::AudioBuffer<float> io(bufferChannels, N);
            const int maxBlocks = int((run.plan.range.getLength() + 4.0) * 44100 / N) + 50;
            int blocks = 0;
            for (; blocks < maxBlocks && !engine.captureComplete(); ++blocks) {
                if (stopAfterBlocks >= 0 && blocks >= stopAfterBlocks) break;
                CHECK(engine.waitForReadAhead(5000));
                // The gear: what was sent `trip` samples ago comes back on the same channel numbers.
                for (int ch = 0; ch < bufferChannels; ++ch)
                    for (int i = 0; i < N; ++i) {
                        const int k = blocks * N + i - trip;
                        io.setSample(ch, i, k >= 0 ? sent[size_t(ch)][size_t(k)] * gearGain : 0.0f);
                    }
                engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                for (int ch = 0; ch < bufferChannels; ++ch)
                    for (int i = 0; i < N; ++i) sent[size_t(ch)].push_back(io.getSample(ch, i));
            }
            run.complete = engine.captureComplete();
            run.take.file = rq.file;
            run.take.startSample = engine.recordStartSample();
            run.take.roundTrip = trip + engine.masterLatency();
            engine.stopCapture();
            CHECK(!engine.captureActive() && !engine.isRecording());
            if (std::unique_ptr<juce::AudioFormatReader> r{fm.createReaderFor(rq.file)}) {
                run.take.rate = r->sampleRate;
                run.take.length = r->lengthInSamples;
                run.fileChannels = int(r->numChannels);
                run.file.setSize(int(r->numChannels), int(r->lengthInSamples));
                r->read(&run.file, 0, int(r->lengthInSamples), 0, true, true);
            }
            run.after = from;
            run.id = placeCapture(run.after, run.plan, run.take, mute, name);
            engine.loadSession(from);
            return run;
        };
        // How far the captured region is from the programme: the worst sample difference with the
        // region read `slip` samples off its place (0 = where it was put).
        auto worst = [&](const Run& run, float scale, int slip) {
            const auto* r = run.after.findRegion(run.id);
            if (!r) return 1.0e9;
            const juce::int64 t0 = juce::int64(std::llround(r->start * 44100));
            double w = 0;
            for (juce::int64 k = 2; k < r->srcOut - r->srcIn - 2; ++k) {
                const juce::int64 n = t0 + k - regionStart;   // sample of the source file the programme plays at t0 + k
                const float want = n >= 0 && n < noise.getNumSamples() ? noise.getSample(0, int(n)) * scale : 0.0f;
                w = juce::jmax(w, double(std::abs(run.file.getSample(0, int(r->srcIn + k + slip)) - want)));
            }
            return w;
        };

        const int trip = N + 317;   // a round trip is never shorter than a buffer
        // The In-Out, with pre-roll: lands on a new lane under the original, on the sample.
        {
            auto run = capture(s, {3.0, 5.0}, trip, 1.0f, -1, "cap-range");
            CHECK(run.complete && run.id != 0 && run.fileChannels == 2);
            const auto* r = run.after.findRegion(run.id);
            CHECK(r && r->lane == 1 && run.after.lanes == 3 && std::abs(r->start - 3.0) < 1e-12 && r->srcOut - r->srcIn == 2 * 44100);
            const double exact = worst(run, 1.0f, 0);
            std::printf("capture In-Out: worst sample error %.3g in place, %.3g one sample early, %.3g one late\n", exact, worst(run, 1.0f, -1), worst(run, 1.0f, 1));
            CHECK(exact == 0.0);                                             // bit for bit, +/- 0 samples
            CHECK(worst(run, 1.0f, -1) > 0.05 && worst(run, 1.0f, 1) > 0.05);
            // After the Out point the programme is silent, so the tail holds only the gear's own tail.
            bool tailSilent = true;
            for (juce::int64 i = r->srcOut; i < run.take.length; ++i) tailSilent &= run.file.getSample(0, int(i)) == 0.0f;
            CHECK(tailSilent && run.take.length - r->srcOut >= juce::int64(0.5 * 44100) - trip - N);
            CHECK(!run.after.findRegion(rid)->mute);                         // the original is left as it was
        }
        // Nothing selected: the whole programme, from the very first sample.
        {
            auto run = capture(s, {}, trip, 1.0f, -1, "cap-whole", 2, true);
            CHECK(run.complete && run.plan.whole && run.id != 0);
            const auto* r = run.after.findRegion(run.id);
            CHECK(r && r->start == 0.0 && r->srcOut - r->srcIn == juce::int64(std::llround(s.end() * 44100)));
            const double exact = worst(run, 1.0f, 0);
            std::printf("capture whole programme: worst sample error %.3g\n", exact);
            CHECK(exact == 0.0);
            CHECK(run.after.findRegion(rid)->mute);                          // "mute the original" asked for
        }
        // Through master plugins that add delay: the capture still lands in sync.
        {
            engine.chain().addInstance(std::make_unique<GainDelayPlugin>(0.5f, 300));
            auto run = capture(s, {4.0, 5.5}, trip, 1.0f, -1, "cap-chain");
            CHECK(run.complete && run.id != 0 && run.take.roundTrip == trip + 300);
            const double exact = worst(run, 0.5f, 0);
            std::printf("capture through a 300-sample master chain: worst sample error %.3g\n", exact);
            CHECK(exact < 1e-7);
            // Switching between what was sent and the capture: in step (they null), and level
            // matching turns down whichever is louder.
            engine.loadSessionWithAlternate(s, captureAlone(run.after, run.id));
            CHECK(engine.hasAlternate());
            auto listen = [&](bool hearCapture, bool match, int blocks, double from = 4.2) {
                engine.stop();
                engine.setPositionSeconds(from);
                engine.setHearUnprocessed(hearCapture);
                engine.setLevelMatch(match);
                engine.play();
                juce::AudioBuffer<float> all(2, blocks * N);
                for (int b = 0; b < blocks; ++b) {
                    CHECK(engine.waitForReadAhead(5000));
                    juce::AudioBuffer<float> out(2, N);
                    engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&out, 0, N));
                    for (int ch = 0; ch < 2; ++ch) all.copyFrom(ch, b * N, out, ch, 0, N);
                }
                engine.stop();
                return all;
            };
            listen(false, false, 60);
            auto A = listen(false, false, 40), B = listen(true, false, 40);
            double errSum = 0, refSum = 0;
            for (int i = 20 * N; i < 40 * N; ++i) { const double d = A.getSample(0, i) - B.getSample(0, i); errSum += d * d; refSum += double(A.getSample(0, i)) * A.getSample(0, i); }
            const double nullDb = 10.0 * std::log10(juce::jmax(1e-30, errSum / juce::jmax(1e-30, refSum)));
            std::printf("  sent against capture nulls to %.1f dB\n", nullDb);
            CHECK(refSum > 0 && nullDb < -100.0);
            engine.setHearUnprocessed(false);
            engine.setLevelMatch(false);
            engine.loadSession(s);
            CHECK(!engine.hasAlternate());
            // Gear that adds 6 dB: the capture is louder, so level matching turns the capture down.
            auto hot = capture(s, {3.0, 7.5}, trip, 2.0f, -1, "cap-hot");
            CHECK(hot.id != 0 && worst(hot, 1.0f, 0) < 1e-7);
            engine.loadSessionWithAlternate(s, captureAlone(hot.after, hot.id));
            engine.resetMatch();
            auto sentLevel = listen(false, true, 330, 3.2);                  // (about 3.8 s: long enough to measure)
            std::printf("  +6 dB gear: sent against capture measures %.2f dB\n", engine.matchDifferenceDb());
            CHECK(engine.matchMeasured() && std::abs(engine.matchDifferenceDb() + 6.0206) < 0.05);
            CHECK(std::abs(engine.matchCutDb()) < 1e-9);                     // what was sent isn't touched
            auto captureLevel = listen(true, true, 330, 3.2);
            const double ra = sentLevel.getRMSLevel(0, 300 * N, 25 * N), rb = captureLevel.getRMSLevel(0, 300 * N, 25 * N);
            std::printf("  level matched, capture against sent %.2f dB\n", 20.0 * std::log10(rb / ra));
            CHECK(std::abs(20.0 * std::log10(rb / ra)) < 0.1);
            engine.setHearUnprocessed(false);
            engine.setLevelMatch(false);
            engine.chain().setState({}, problems);
            engine.loadSession(s);
        }
        // Stopped early: what was recorded is kept and placed.
        {
            const int blocksToStop = int((2.0 + 0.75) * 44100 / N);   // pre-roll and three quarters of a second
            auto run = capture(s, {3.0, 5.0}, trip, 1.0f, blocksToStop, "cap-early");
            CHECK(!run.complete && run.id != 0);
            const auto* r = run.after.findRegion(run.id);
            const juce::int64 kept = r ? r->srcOut - r->srcIn : 0;
            std::printf("capture stopped early: kept %.2f s\n", double(kept) / 44100.0);
            CHECK(kept > juce::int64(0.6 * 44100) && kept < juce::int64(0.8 * 44100) && worst(run, 1.0f, 0) == 0.0);
        }
        // The send on another output pair: the programme leaves on it, the monitor pair stays quiet
        // (or carries the return, with input monitoring on), and the capture is the same.
        {
            engine.setInputMonitoring(true);
            auto plan = planCapture(s, {3.0, 3.5});
            AudioEngine::CaptureRequest rq;
            rq.file = dir.getChildFile("cap-route.wav");
            rq.firstOutput = 2;
            rq.firstInput = 2;
            rq.channels = 2;
            rq.bits = 32;
            rq.from = 3.0;
            rq.to = 3.5;
            rq.preRoll = 0.5;
            rq.postRoll = 0.1;
            juce::String err;
            CHECK(engine.startCapture(rq, s, err));
            juce::AudioBuffer<float> io(4, N);
            std::vector<float> sent;
            bool monitorIsReturn = true, heardSomething = false;
            for (int b = 0; b < 400 && !engine.captureComplete(); ++b) {
                CHECK(engine.waitForReadAhead(5000));
                io.clear();
                for (int i = 0; i < N; ++i) {
                    const int k = b * N + i - trip;
                    const float v = k >= 0 ? sent[size_t(k)] : 0.0f;
                    io.setSample(2, i, v);
                    io.setSample(3, i, v);
                }
                juce::AudioBuffer<float> in(2, N);
                in.copyFrom(0, 0, io, 2, 0, N);
                engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                for (int i = 0; i < N; ++i) {
                    sent.push_back(io.getSample(2, i));
                    monitorIsReturn &= io.getSample(0, i) == in.getSample(0, i);   // outputs 1-2: the return, not the send
                    heardSomething |= io.getSample(2, i) != 0.0f;
                }
            }
            CHECK(engine.captureComplete() && monitorIsReturn && heardSomething);
            CaptureTake take;
            take.file = rq.file;
            take.startSample = engine.recordStartSample();
            take.roundTrip = trip;
            engine.stopCapture();
            engine.setInputMonitoring(false);
            Run run;
            run.plan = plan;
            if (std::unique_ptr<juce::AudioFormatReader> r{fm.createReaderFor(rq.file)}) {
                take.rate = r->sampleRate;
                take.length = r->lengthInSamples;
                run.file.setSize(2, int(r->lengthInSamples));
                r->read(&run.file, 0, int(r->lengthInSamples), 0, true, true);
            }
            run.after = s;
            run.id = placeCapture(run.after, plan, take, false, "route");
            CHECK(run.id != 0 && worst(run, 1.0f, 0) == 0.0);
            engine.loadSession(s);
        }
        // The channels follow the lane type: a mono lane is captured as a mono file onto a mono lane.
        {
            Session mono = s;
            mono.setLaneFormat(0, 1);
            auto run = capture(mono, {3.0, 4.0}, trip, 1.0f, -1, "cap-mono");
            CHECK(run.complete && run.plan.channels == 1 && run.fileChannels == 1 && run.id != 0);
            CHECK(run.after.laneFormat(1) == 1 && run.after.laneFormat(0) == 1);
            const auto* r = run.after.findRegion(run.id);
            CHECK(r && r->srcOut - r->srcIn == 44100 && run.file.getRMSLevel(0, int(r->srcIn), 44100) > 0.01f);
        }
        // ... and a 5.1 programme as six channels, each on its own socket.
        {
            auto f6 = dir.getChildFile("cap6.wav");
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> os(f6.createOutputStream().release());
                std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 44100, juce::AudioChannelSet::create5point1(), 24, {}, 0));
                os.release();
                juce::AudioBuffer<float> b(6, 44100 * 3);
                for (int c = 0; c < 6; ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(c, i, 0.1f * float(c + 1) * float(std::sin(i * 0.05)));
                w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
            }
            Session sur;
            std::unique_ptr<juce::AudioFormatReader> r6(fm.createReaderFor(f6));
            sur.append(sur.makeRegion(f6, r6->sampleRate, r6->lengthInSamples), 2.0);
            sur.setLaneFormat(0, 6);
            auto run = capture(sur, {3.0, 4.0}, trip, 1.0f, -1, "cap-51", 6);
            CHECK(run.complete && run.plan.channels == 6 && run.fileChannels == 6 && run.id != 0 && run.after.laneFormat(1) == 6);
            if (auto* r = run.after.findRegion(run.id); r && run.fileChannels == 6) {
                bool each = true;
                for (int c = 0; c < 6; ++c) {
                    const int k = 1000;   // timeline 3 s + 1000 samples = 1 s + 1000 into the file
                    const float want = 0.1f * float(c + 1) * float(std::sin((44100 + k) * 0.05));
                    each &= std::abs(run.file.getSample(c, int(r->srcIn) + k) - want) < 1e-4f;
                }
                CHECK(each);
            }
        }
        // Level check: routed and measured, nothing recorded, and it doesn't stop by itself.
        {
            AudioEngine::CaptureRequest rq;
            rq.levelCheck = true;
            rq.from = 3.0;
            rq.to = 3.2;
            rq.preRoll = 0.0;
            juce::String err;
            CHECK(engine.startCapture(rq, s, err));
            CHECK(engine.captureActive() && !engine.captureRecording() && !engine.isRecording() && engine.inputsLive());
            juce::AudioBuffer<float> io(2, N);
            bool played = false;
            for (int b = 0; b < 60; ++b) {
                CHECK(engine.waitForReadAhead(5000));
                io.clear();
                engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                if (b > 40) played |= io.getRMSLevel(0, 0, N) > 0.01f;   // past the Out point: still playing (the window loops it)
            }
            CHECK(played && !engine.captureComplete());
            engine.stopCapture();
            engine.loadSession(s);
        }
        // The old Record window's print is placed from the same exact start.
        {
            auto pf = dir.getChildFile("print.wav");
            juce::String err;
            engine.setPositionSeconds(2.5);
            CHECK(engine.startRecording(pf, 0, 32, true, err));
            juce::AudioBuffer<float> io(2, N);
            std::vector<float> sent;
            for (int b = 0; b < 80; ++b) {
                CHECK(engine.waitForReadAhead(5000));
                for (int i = 0; i < N; ++i) {
                    const int k = b * N + i - trip;
                    io.setSample(0, i, k >= 0 ? sent[size_t(k)] : 0.0f);
                    io.setSample(1, i, 0.0f);
                }
                engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                for (int i = 0; i < N; ++i) sent.push_back(io.getSample(0, i));
            }
            const juce::int64 start = engine.recordStartSample();
            CHECK(std::abs(engine.recordStartPosition() * 44100.0 - double(start)) < 1e-6);
            engine.stopRecording();
            std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(pf));
            CHECK(r != nullptr);
            if (r) {
                juce::AudioBuffer<float> got(2, int(r->lengthInSamples));
                r->read(&got, 0, got.getNumSamples(), 0, true, true);
                // File sample i holds programme sample start + i - trip.
                bool same = true;
                for (int i = trip + 100; i < trip + 20000; ++i) {
                    const juce::int64 n = start + i - trip - regionStart;
                    same &= n >= 0 && got.getSample(0, i) == noise.getSample(0, int(n));
                }
                CHECK(same);
            }
        }
        engine.stop();
        engine.loadSession(Session());
        engine.releaseResources();
    }

    // ---- External Gear insert and Print, through a simulated loop on outputs / inputs 3-4 ----
    {
        const int N = 512, trip = N + 233;
        engine.prepareToPlay(N, 44100);
        engine.chain().prepare(44100, N);
        juce::StringArray problems;
        engine.chain().setState({}, problems);
        auto src = noiseFile(dir, "gear.wav", 44100, 8.0, 91);
        std::unique_ptr<juce::AudioFormatReader> sr(fm.createReaderFor(src));
        juce::AudioBuffer<float> noise(2, int(sr->lengthInSamples));
        sr->read(&noise, 0, noise.getNumSamples(), 0, true, true);
        Session s;
        const int rid = s.append(s.makeRegion(src, sr->sampleRate, sr->lengthInSamples), 0.0);
        const juce::int64 regionStart = juce::int64(std::llround(s.findRegion(rid)->start * 44100));
        // Master chain: a plugin (-6 dB, 300 samples), the gear, a plugin (x3, 100 samples).
        engine.chain().addInstance(std::make_unique<GainDelayPlugin>(0.5f, 300));
        CHECK(engine.addExternalGear().isEmpty());
        auto* gear = dynamic_cast<ExternalGear*>(engine.chain().pluginAt(1));
        CHECK(gear != nullptr && engine.chain().builtinAt(1) != nullptr && engine.chain().builtinCount() == 1);
        engine.chain().addInstance(std::make_unique<GainDelayPlugin>(3.0f, 100));
        if (gear) {
            const int id = gear->anchorId();
            CHECK(id != 0 && engine.chain().slotOfAnchor(id) == 1 && engine.chain().lastLiveHardwareSlot() == 1);
            CHECK(gear->sendOutput() == 2);                          // the first pair that isn't the monitor pair
            CHECK(gear->setRoute(0, 2).isNotEmpty() && gear->sendOutput() == 2);   // the monitor pair is refused
            CHECK(gear->setRoute(2, 2).isEmpty() && gear->returnInput() == 2);
            gear->setPing(trip);
            CHECK(gear->getLatencySamples() == trip && engine.chain().latencySamples() == 300 + trip + 100);
            CHECK(engine.chain().latencyThrough(id) == 300 + trip);
            engine.loadSession(s);

            // Plays `ses` from `from` through the loop (the gear returns what it was sent, `trip`
            // samples later, times gearGain). Returns what reaches the monitor pair.
            struct Played { juce::AudioBuffer<float> out; std::vector<float> sent; bool monitorClean = true; };
            auto play = [&](double from, int blocks, float gearGain, const std::function<bool()>& done = {}) {
                Played p;
                p.out.setSize(2, blocks * N);
                p.out.clear();
                engine.stop();
                if (!engine.printActive()) { engine.setPositionSeconds(from); engine.play(); }
                juce::AudioBuffer<float> io(4, N);
                for (int b = 0; b < blocks; ++b) {
                    if (done && done()) { p.out.setSize(2, b * N, true); break; }
                    CHECK(engine.waitForReadAhead(5000));
                    io.clear();
                    for (int i = 0; i < N; ++i) {
                        const int k = b * N + i - trip;
                        const float v = k >= 0 ? p.sent[size_t(k)] * gearGain : 0.0f;
                        io.setSample(2, i, v);
                        io.setSample(3, i, v);
                    }
                    engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                    for (int i = 0; i < N; ++i) p.sent.push_back(io.getSample(2, i));
                    for (int ch = 0; ch < 2; ++ch) p.out.copyFrom(ch, b * N, io, ch, 0, N);
                }
                engine.stop();
                return p;
            };
            // What the monitors should carry `late` samples after a start at 3 s: the programme times `gain`.
            auto worstAgainst = [&](const juce::AudioBuffer<float>& out, int late, float gain, int fromSample, int toSample) {
                const juce::int64 p0 = juce::int64(3.0 * 44100);
                double w = 0;
                for (int k = fromSample; k < juce::jmin(toSample, out.getNumSamples()); ++k) {
                    const juce::int64 n = p0 + k - late - regionStart;
                    const float want = n >= 0 && n < noise.getNumSamples() ? noise.getSample(0, int(n)) * gain : 0.0f;
                    w = juce::jmax(w, double(std::abs(out.getSample(0, k) - want)));
                }
                return w;
            };
            const int total = 300 + trip + 100;
            // In the chain: the send leaves on 3-4 (never on the monitor pair), the return carries on
            // down the chain, and everything is late by exactly the reported latency.
            {
                auto p = play(3.0, 60, 1.0f);
                const double w = worstAgainst(p.out, total, 1.5f, 4000, 60 * N);
                std::printf("external gear in the chain: worst sample error %.3g (latency %d reported)\n", w, engine.chain().lastLatency());
                CHECK(w < 1e-6 && engine.chain().lastLatency() == total);
                // the send is the signal after the first plugin: -6 dB, 300 late
                double ws = 0;
                for (int k = 2000; k < 20000; ++k) {
                    const juce::int64 n = juce::int64(3.0 * 44100) + k - 300 - regionStart;
                    ws = juce::jmax(ws, double(std::abs(p.sent[size_t(k)] - noise.getSample(0, int(n)) * 0.5f)));
                }
                CHECK(ws < 1e-7);
                CHECK(!gear->sendBlocked() && !gear->returnSilent() && !gear->returnClipped());
            }
            // Bypassed: the signal passes, just as late, so switching doesn't jump.
            {
                engine.chain().setBypassed(1, true);
                CHECK(engine.chain().latencySamples() == total && engine.chain().lastLiveHardwareSlot() == -1);
                auto p = play(3.0, 40, 0.0f);   // (nothing comes back: it isn't needed)
                CHECK(worstAgainst(p.out, total, 1.5f, 4000, 40 * N) < 1e-6);
                engine.chain().setBypassed(1, false);
            }
            // Dry / wet lines up (half and half through a cable is the signal again); polarity flips the return.
            {
                gear->setMix(0.5);
                auto p = play(3.0, 40, 1.0f);
                CHECK(worstAgainst(p.out, total, 1.5f, 4000, 40 * N) < 1e-6);
                gear->setInverted(true);
                auto q = play(3.0, 40, 1.0f);
                CHECK(q.out.getMagnitude(0, 4000, 30 * N) < 1e-6f);   // dry and inverted wet cancel
                gear->setInverted(false);
                gear->setMix(1.0);
                // Send and return levels.
                gear->setSendDb(-6.0206);
                gear->setReturnDb(6.0206);
                auto r = play(3.0, 40, 1.0f);
                CHECK(worstAgainst(r.out, total, 1.5f, 4000, 40 * N) < 1e-5);
                gear->setSendDb(0.0);
                gear->setReturnDb(0.0);
            }
            // Warnings: nothing coming back, and a return that clips.
            {
                play(3.0, 120, 0.0f);
                CHECK(gear->returnSilent());
                gear->clearLights();
                play(3.0, 30, 60.0f);
                CHECK(gear->returnClipped() && !gear->returnSilent());
                gear->clearLights();
            }
            // Saved with the chain and brought back: the same insert, route, ping and settings.
            {
                gear->setSendDb(-3.0);
                auto state = engine.chain().getState();
                auto before = id;
                engine.chain().setState(state, problems);
                CHECK(engine.chain().size() == 1);   // (the two test plugins aren't installed plugins, so only the insert comes back)
                problems.clear();
                auto* g2 = dynamic_cast<ExternalGear*>(engine.chain().pluginAt(0));
                CHECK(g2 && g2->anchorId() == before && g2->sendOutput() == 2 && g2->returnInput() == 2 && g2->ping() == trip && std::abs(g2->sendDb() + 3.0) < 1e-9);
                // Rebuild the chain as it was for the rest of the test.
                engine.chain().setState({}, problems);
                engine.chain().addInstance(std::make_unique<GainDelayPlugin>(0.5f, 300));
                CHECK(engine.addExternalGear().isEmpty());
                gear = dynamic_cast<ExternalGear*>(engine.chain().pluginAt(1));
                juce::MemoryBlock gs;
                if (g2) { g2->setSendDb(0.0); }
                engine.chain().addInstance(std::make_unique<GainDelayPlugin>(3.0f, 100));
                CHECK(gear != nullptr);
            }
        }
        if (gear) {
            CHECK(gear->setRoute(2, 2).isEmpty());
            gear->setPing(trip);
            const int id = gear->anchorId();
            const int total = 300 + trip + 100;
            Exporter ex(engine);
            ExportSettings es;
            // An export never leaves the gear out silently: with live hardware it is refused.
            auto refused = dir.getChildFile("refused.wav");
            auto msg = ex.exportProgramWav(s, refused, 32, es, [](double) { return true; });
            CHECK(msg == hardwareExportMessage());
            CHECK(needsHardware(s, engine.chain().lastLiveHardwareSlot(), [&](int a) { return engine.chain().slotOfAnchor(a); }));

            // Print the whole programme through the insert.
            auto runPrint = [&](const Session& from, juce::Range<double> selection, const juce::String& name) {
                auto plan = planCapture(from, selection);
                plan.sources = sourcesThrough(from, plan, engine.chain().slotOfAnchor(id), [&](int a) { return engine.chain().slotOfAnchor(a); });
                plan.channels = 2;
                AudioEngine::PrintRequest rq;
                rq.file = dir.getChildFile(name + ".wav");
                rq.gear = id;
                rq.bits = 32;
                rq.from = plan.range.getStart();
                rq.to = plan.range.getEnd();
                rq.preRoll = 2.0;
                rq.postRoll = 0.25;
                juce::String err;
                CHECK(engine.startPrint(rq, from, err));
                CHECK(engine.printActive() && engine.captureRecording() && !engine.inputsLive());
                juce::AudioBuffer<float> io(4, N);
                std::vector<float> sent;
                const int maxBlocks = int((plan.range.getLength() + 5.0) * 44100 / N) + 50;
                for (int b = 0; b < maxBlocks && !engine.captureComplete(); ++b) {
                    CHECK(engine.waitForReadAhead(5000));
                    io.clear();
                    for (int i = 0; i < N; ++i) {
                        const int k = b * N + i - trip;
                        const float v = k >= 0 ? sent[size_t(k)] : 0.0f;
                        io.setSample(2, i, v);
                        io.setSample(3, i, v);
                    }
                    engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                    for (int i = 0; i < N; ++i) sent.push_back(io.getSample(2, i));
                }
                CHECK(engine.captureComplete());
                CaptureTake take;
                take.file = rq.file;
                take.startSample = engine.recordStartSample();
                take.roundTrip = engine.chain().latencyThrough(id);
                engine.stopCapture();
                CHECK(!engine.printActive());
                juce::AudioBuffer<float> file;
                if (std::unique_ptr<juce::AudioFormatReader> r{fm.createReaderFor(rq.file)}) {
                    take.rate = r->sampleRate;
                    take.length = r->lengthInSamples;
                    file.setSize(2, int(r->lengthInSamples));
                    r->read(&file, 0, int(r->lengthInSamples), 0, true, true);
                }
                Session after = from;
                juce::Array<int> muted;
                const int pid = placeCapture(after, plan, take, true, name, &muted);
                CHECK(pid != 0 && !muted.isEmpty());
                if (auto* r = after.findRegion(pid)) {
                    r->afterGear = id;
                    r->printOf = muted;
                    // The print holds the programme as it left the insert (after the first plugin), in place.
                    const juce::int64 t0 = juce::int64(std::llround(r->start * 44100));
                    double w = 0;
                    for (juce::int64 k = 2; k < r->srcOut - r->srcIn - 2; ++k) {
                        const juce::int64 n = t0 + k - regionStart;
                        const float want = n >= 0 && n < noise.getNumSamples() ? noise.getSample(0, int(n)) * 0.5f : 0.0f;
                        w = juce::jmax(w, double(std::abs(file.getSample(0, int(r->srcIn + k)) - want)));
                    }
                    std::printf("print %s: worst sample error %.3g against what left the insert\n", name.toRawUTF8(), w);
                    CHECK(w < 1e-7);
                }
                engine.loadSession(from);
                return std::make_pair(after, pid);
            };
            // First a stretch of it: the rest still needs the hardware, so exports stay refused.
            auto [part, partId] = runPrint(s, {3.0, 5.0}, "print-part");
            CHECK(part.printAnchors().size() == 1 && part.printAnchors()[0] == id);
            CHECK(needsHardware(part, engine.chain().lastLiveHardwareSlot(), [&](int a) { return engine.chain().slotOfAnchor(a); }));
            CHECK(ex.exportProgramWav(part, refused, 32, es, [](double) { return true; }) == hardwareExportMessage());
            // Playing it: the printed stretch skips the plugin above the insert (nothing is applied
            // twice) and still passes the one below it; the rest goes through the gear live. One
            // continuous signal, the same as the live chain.
            {
                engine.loadSession(part);
                juce::AudioBuffer<float> io(4, N);
                std::vector<float> sent;
                engine.setPositionSeconds(3.0);
                engine.play();
                const int blocks = int(3.0 * 44100 / N);   // through the end of the print and beyond it
                juce::AudioBuffer<float> out(2, blocks * N);
                for (int b = 0; b < blocks; ++b) {
                    CHECK(engine.waitForReadAhead(5000));
                    io.clear();
                    for (int i = 0; i < N; ++i) {
                        const int k = b * N + i - trip;
                        const float v = k >= 0 ? sent[size_t(k)] : 0.0f;
                        io.setSample(2, i, v);
                        io.setSample(3, i, v);
                    }
                    engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                    for (int i = 0; i < N; ++i) sent.push_back(io.getSample(2, i));
                    out.copyFrom(0, b * N, io, 0, 0, N);
                    out.copyFrom(1, b * N, io, 1, 0, N);
                }
                engine.stop();
                const juce::int64 p0 = juce::int64(3.0 * 44100);
                double w = 0;
                for (int k = 4000; k < blocks * N; ++k) {
                    const juce::int64 n = p0 + k - total - regionStart;
                    w = juce::jmax(w, double(std::abs(out.getSample(0, k) - noise.getSample(0, int(n)) * 1.5f)));
                }
                std::printf("print and live gear together: worst sample error %.3g across the join\n", w);
                CHECK(w < 1e-6);
                // During the printed stretch nothing is sent to the gear (it is all printed there).
                double sentInPrint = 0;
                for (int k = 6000; k < int(1.5 * 44100); ++k) sentInPrint = juce::jmax(sentInPrint, double(std::abs(sent[size_t(k)])));
                CHECK(sentInPrint == 0.0);
                // Live against print at the insert: both run in step, so they null.
                Session both = part;
                both.usePrints(id, true);
                for (int srcId : part.findRegion(partId)->printOf) both.findRegion(srcId)->mute = false;
                engine.loadSession(both);
                engine.chain().setCompareAt(id);
                juce::AudioBuffer<float> heard[2];
                for (int side = 0; side < 2; ++side) {
                    engine.chain().setHearPrint(side == 1);
                    engine.stop();
                    engine.setPositionSeconds(3.0);
                    engine.play();
                    sent.clear();
                    heard[side].setSize(2, 60 * N);
                    for (int b = 0; b < 60; ++b) {
                        CHECK(engine.waitForReadAhead(5000));
                        io.clear();
                        for (int i = 0; i < N; ++i) {
                            const int k = b * N + i - trip;
                            const float v = k >= 0 ? sent[size_t(k)] : 0.0f;
                            io.setSample(2, i, v);
                            io.setSample(3, i, v);
                        }
                        engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&io, 0, N));
                        for (int i = 0; i < N; ++i) sent.push_back(io.getSample(2, i));
                        heard[side].copyFrom(0, b * N, io, 0, 0, N);
                    }
                    engine.stop();
                }
                double err = 0, ref = 0;
                for (int k = 20 * N; k < 60 * N; ++k) {
                    const double d = heard[0].getSample(0, k) - heard[1].getSample(0, k);
                    err += d * d;
                    ref += double(heard[0].getSample(0, k)) * heard[0].getSample(0, k);
                }
                const double nullDb = 10.0 * std::log10(juce::jmax(1e-30, err / juce::jmax(1e-30, ref)));
                std::printf("live gear against its print nulls to %.1f dB\n", nullDb);
                CHECK(ref > 0 && nullDb < -100.0);
                engine.chain().setCompareAt(0);
                engine.chain().setHearPrint(false);
                engine.loadSession(s);
            }
            // Switching back and forth: the print off and its sources back, and again.
            {
                Session t = part;
                CHECK(t.usePrints(id, false) == 1 && t.findRegion(partId)->mute && t.printAnchors().isEmpty());
                double heard = 0;
                for (auto& r : t.regions) heard += r.mute ? 0.0 : r.length();
                CHECK(std::abs(heard - 8.0) < 1e-6);
                CHECK(t.usePrints(id, true) == 1 && !t.findRegion(partId)->mute);
                // ... and it survives a save.
                auto f = dir.getChildFile("prints.lathe");
                juce::String e;
                CHECK(t.save(f, e));
                Session back;
                CHECK(back.load(f, e));
                CHECK(back.findRegion(partId) && back.findRegion(partId)->afterGear == id && back.findRegion(partId)->printOf == t.findRegion(partId)->printOf);
            }
            // The whole programme printed: nothing needs the hardware, and the export (at full
            // speed, no loop) is the live result: the print through the plugin below the insert only.
            auto [whole, wholeId] = runPrint(s, {}, "print-whole");
            juce::ignoreUnused(wholeId);
            CHECK(!needsHardware(whole, engine.chain().lastLiveHardwareSlot(), [&](int a) { return engine.chain().slotOfAnchor(a); }));
            auto outFile = dir.getChildFile("printed.wav");
            CHECK(ex.exportProgramWav(whole, outFile, 32, es, [](double) { return true; }).isEmpty());
            if (std::unique_ptr<juce::AudioFormatReader> r{fm.createReaderFor(outFile)}) {
                juce::AudioBuffer<float> got(2, int(r->lengthInSamples));
                r->read(&got, 0, got.getNumSamples(), 0, true, true);
                double w = 0;
                for (int k = int(regionStart) + 10; k < int(regionStart) + noise.getNumSamples() - 10; ++k)
                    w = juce::jmax(w, double(std::abs(got.getSample(0, k) - noise.getSample(0, k - int(regionStart)) * 1.5f)));
                std::printf("export from the print (no hardware): worst sample error %.3g\n", w);
                CHECK(w < 1e-6);
            } else {
                CHECK(false);
            }
            // With the insert bypassed instead, the unprinted session exports too (without the gear, as asked).
            engine.chain().setBypassed(1, true);
            CHECK(ex.exportProgramWav(s, outFile, 32, es, [](double) { return true; }).isEmpty());
            engine.chain().setBypassed(1, false);
        }
        engine.stop();
        engine.chain().setState({}, problems);
        engine.loadSession(Session());
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
    // ---- Segment-only export, and a 5.1 programme as a six-channel WAV ----
    {
        auto a = noiseFile(dir, "seg.wav", 48000, 6.0, 7);
        Session s;
        std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(a));
        s.append(s.makeRegion(a, rd->sampleRate, rd->lengthInSamples), 2.0);
        Exporter ex(engine);
        ExportSettings es;
        auto f = dir.getChildFile("segment.wav");
        auto err = ex.exportRangeWav(s, f, 24, es, {3.0, 4.5}, [](double) { return true; });
        CHECK(err.isEmpty());
        std::unique_ptr<juce::AudioFormatReader> back(fm.createReaderFor(f));
        CHECK(back && back->lengthInSamples == 72000 && back->numChannels == 2 && back->bitsPerSample == 24);
        // The segment is the same audio as that stretch of the full programme.
        if (back) {
            auto full = dir.getChildFile("full.wav");
            CHECK(ex.exportProgramWav(s, full, 32, es, [](double) { return true; }).isEmpty());
            std::unique_ptr<juce::AudioFormatReader> fr(fm.createReaderFor(full));
            juce::AudioBuffer<float> x(2, 1000), y(2, 1000);
            back->read(&x, 0, 1000, 1000, true, true);
            fr->read(&y, 0, 1000, 48000 * 3 + 1000, true, true);
            double worst = 0;
            for (int i = 0; i < 1000; ++i) worst = std::max(worst, double(std::abs(x.getSample(0, i) - y.getSample(0, i))));
            CHECK(worst < 2.0 / 8388608.0 * 4);   // 24-bit vs float: within dither
        }
        // 5.1: a six-channel file on a 5.1 lane exports as six channels.
        auto f6 = dir.getChildFile("six.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f6.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000, juce::AudioChannelSet::create5point1(), 24, {}, 0));
            os.release();
            juce::AudioBuffer<float> b(6, 48000 * 2);
            for (int c = 0; c < 6; ++c)
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(c, i, 0.1f * float(c + 1) * float(std::sin(i * 0.05)));
            w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
        }
        Session sur;
        std::unique_ptr<juce::AudioFormatReader> r6(fm.createReaderFor(f6));
        sur.append(sur.makeRegion(f6, r6->sampleRate, r6->lengthInSamples), 2.0);
        sur.setLaneFormat(0, 6);
        auto out6 = dir.getChildFile("surround.wav");
        CHECK(ex.exportProgramWav(sur, out6, 24, es, [](double) { return true; }).isEmpty());
        std::unique_ptr<juce::AudioFormatReader> b6(fm.createReaderFor(out6));
        CHECK(b6 && b6->numChannels == 6 && b6->lengthInSamples == 48000 * 4);
        if (b6) {
            juce::AudioBuffer<float> x(6, 4800);
            b6->read(&x, 0, 4800, 48000 * 2 + 4800, true, true);
            for (int c = 0; c < 6; ++c) {
                const float want = 0.1f * float(c + 1) * float(std::sin((4800 + 100) * 0.05));
                CHECK(std::abs(x.getSample(c, 100) - want) < 1e-4f);
            }
        }
    }
    dir.deleteRecursively();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
