#include "Exporter.h"

#include "../../core/util/FlacTags.h"
#include "../../core/util/Hash.h"

#include <CDSPResampler.h>

namespace lathe {

namespace {

// Pulls finished master audio: programme → chain → (optional) SRC. Owns the chain while alive.
class MasterStream {
public:
    MasterStream(AudioEngine& e, const Session& s, const ExportSettings& es, double outRate, double startSec = 0)
        : engine_(e), renderer_(e.formats()), runPlugins_(es.runPlugins) {
        projRate_ = s.effectiveRate();
        outRate_ = outRate;
        engine_.suspendForOffline(true);
        lock_ = std::make_unique<juce::ScopedLock>(engine_.chain().lock());
        fxLock_ = std::make_unique<juce::ScopedLock>(engine_.regionFx().lock());   // region chains belong to the export now
        if (es.runPlugins) renderer_.setRegionFx(&engine_.regionFx());
        renderer_.setSession(s, projRate_, true);
        if (startSec > 0) renderer_.seek(juce::int64(std::llround(startSec * projRate_)));
        if (runPlugins_) {
            engine_.chain().prepare(projRate_, kBlock, true);   // 64-bit for every plugin that supports it
            engine_.chain().setNonRealtime(true);
            latency_ = engine_.chain().latencySamples();
        }
        if (std::abs(projRate_ - outRate_) > 0.5)
            for (int ch = 0; ch < 2; ++ch) rs_[ch] = std::make_unique<r8b::CDSPResampler24>(projRate_, outRate_, kBlock);
        buf_.setSize(2, kBlock);
    }

    ~MasterStream() {
        if (runPlugins_) engine_.chain().setNonRealtime(false);
        engine_.regionFx().prepare(engine_.deviceRate(), 2048, false);   // back to playback settings
        fxLock_.reset();
        lock_.reset();
        engine_.suspendForOffline(false);   // re-prepares the master chain for playback
    }

    // Fills n frames per channel of output-rate audio, 64-bit float.
    bool pull(double* l, double* r, int n, juce::String& err) {
        while (int(pend_[0].size()) - pendPos_ < n) {
            if (!renderer_.render(buf_, 0, kBlock)) { err = renderer_.lastError(); return false; }
            if (runPlugins_) engine_.chain().process(buf_, 0, kBlock);
            int skip = juce::jmin(latency_, kBlock);   // plugin delay compensation
            latency_ -= skip;
            int n0 = kBlock - skip;
            for (int ch = 0; ch < 2; ++ch) {
                const double* p = buf_.getReadPointer(ch) + skip;
                if (rs_[ch]) {
                    dbl_.assign(p, p + n0);
                    double* op = nullptr;
                    int got = rs_[ch]->process(dbl_.data(), n0, op);
                    pend_[ch].insert(pend_[ch].end(), op, op + got);
                } else {
                    pend_[ch].insert(pend_[ch].end(), p, p + n0);
                }
            }
        }
        std::copy(pend_[0].begin() + pendPos_, pend_[0].begin() + pendPos_ + n, l);
        std::copy(pend_[1].begin() + pendPos_, pend_[1].begin() + pendPos_ + n, r);
        pendPos_ += n;
        if (pendPos_ > (1 << 16)) {
            for (auto& v : pend_) v.erase(v.begin(), v.begin() + pendPos_);
            pendPos_ = 0;
        }
        return true;
    }

private:
    static constexpr int kBlock = 4096;
    AudioEngine& engine_;
    ProgramRenderer renderer_;
    bool runPlugins_;
    double projRate_ = 44100, outRate_ = 44100;
    int latency_ = 0;
    std::unique_ptr<juce::ScopedLock> lock_, fxLock_;
    std::unique_ptr<r8b::CDSPResampler24> rs_[2];
    juce::AudioBuffer<double> buf_;
    std::vector<double> dbl_;
    std::vector<double> pend_[2];
    int pendPos_ = 0;
};

// Per-track loudness bookkeeping on the 44.1 kHz master.
struct QcRunner {
    QcRunner(const Session& s, double rate) {
        whole.prepare(rate, 2);
        for (auto& tr : s.trackRanges()) {
            starts.add(juce::int64(tr.getStart() * rate));
            ends.add(juce::int64(tr.getEnd() * rate));
            auto* m = per.add(new dsp::LoudnessMeter());
            m->prepare(rate, 2);
        }
    }
    void feed(const double* ld, const double* rd, int n, juce::int64 pos) {
        fl.resize(size_t(n));
        fr.resize(size_t(n));
        for (int i = 0; i < n; ++i) { fl[size_t(i)] = float(ld[i]); fr[size_t(i)] = float(rd[i]); }
        const float* l = fl.data();
        const float* r = fr.data();
        const float* ch[2] = {l, r};
        whole.process(ch, n);
        for (int i = 0; i < per.size(); ++i) {
            juce::int64 a = juce::jmax(pos, starts[i]), b = juce::jmin(pos + n, ends[i]);
            if (b <= a) continue;
            const float* c2[2] = {l + (a - pos), r + (a - pos)};
            per[i]->process(c2, int(b - a));
        }
    }
    void fill(QcReport& q) const {
        q.integratedLufs = whole.integratedLufs();
        q.truePeakDb = whole.truePeakDb();
        q.lra = whole.loudnessRange();
        q.clippedSamples = whole.clippedSamples();
        q.tracks.clear();
        for (auto* m : per) q.tracks.add({m->integratedLufs(), m->truePeakDb()});
    }
    dsp::LoudnessMeter whole;
    std::vector<float> fl, fr;
    juce::OwnedArray<dsp::LoudnessMeter> per;
    juce::Array<juce::int64> starts, ends;
};

// Writes dithered integers exactly: JUCE's integer writers take left-justified 32-bit samples.
bool writeInts(juce::AudioFormatWriter& w, const int32_t* interleaved, int n, int bits, std::vector<int> (&tmp)[2]) {
    for (auto& v : tmp) if (int(v.size()) < n) v.resize(size_t(n));
    const int shift = 32 - bits;
    for (int i = 0; i < n; ++i) {
        tmp[0][size_t(i)] = int(uint32_t(interleaved[i * 2]) << shift);
        tmp[1][size_t(i)] = int(uint32_t(interleaved[i * 2 + 1]) << shift);
    }
    const int* chans[3] = {tmp[0].data(), tmp[1].data(), nullptr};
    return w.write(chans, n);
}

juce::String fmtDb(double v, const char* unit) {
    return std::isfinite(v) ? juce::String(v, 1) + " " + unit : juce::String("--");
}

}  // namespace

juce::String ditherName(dsp::Ditherer::Shape s) {
    switch (s) {
        case dsp::Ditherer::Shape::None: return "TPDF, flat";
        case dsp::Ditherer::Shape::Light: return "TPDF, 1st-order shaped";
        case dsp::Ditherer::Shape::Strong: return "TPDF, 2nd-order shaped";
        case dsp::Ditherer::Shape::Medium: return "TPDF, 5th-order Lipshitz shaped";
        case dsp::Ditherer::Shape::Psycho: return "TPDF, 9th-order psychoacoustic shaped";
    }
    return {};
}

juce::String QcReport::toText() const {
    juce::String s;
    s << "QC (44.1 kHz master, before dither)\n";
    s << "  Integrated loudness: " << fmtDb(integratedLufs, "LUFS") << "\n";
    s << "  Loudness range:      " << juce::String(lra, 1) << " LU\n";
    s << "  True peak:           " << fmtDb(truePeakDb, "dBTP") << "\n";
    s << "  Samples at full scale: " << juce::String(clippedSamples) << "\n";
    for (int i = 0; i < tracks.size(); ++i)
        s << "  Track " << juce::String(i + 1).paddedLeft('0', 2) << ": " << fmtDb(tracks[i].lufs, "LUFS") << ", "
          << fmtDb(tracks[i].truePeakDb, "dBTP") << "\n";
    return s;
}

ddp::WriteResult Exporter::exportDdp(const Session& s, const juce::File& folder, const ExportSettings& es,
                                     const Keep& keep, QcReport& qc) {
    juce::StringArray adj;
    cd::Disc disc = s.toDisc(&adj);
    auto pre = cd::validate(disc);
    if (cd::hasErrors(pre)) {
        ddp::WriteResult r;
        r.issues = pre;
        r.error = "Fix the errors listed before exporting.";
        return r;
    }

    MasterStream ms(engine_, s, es, cd::kSampleRate);
    QcRunner qr(s, cd::kSampleRate);
    dsp::Ditherer dither;
    dither.prepare(2, 16, es.shape, 0x5EED1234u, cd::kSampleRate);
    std::vector<double> l, r;
    juce::int64 pos = 0;
    const juce::int64 total = disc.leadOutSector * cd::kSamplesPerSector;
    juce::String err;
    bool cancelled = false;

    auto pull = [&](int16_t* out, int64_t frames) -> bool {
        l.resize(size_t(frames));
        r.resize(size_t(frames));
        if (!ms.pull(l.data(), r.data(), int(frames), err)) return false;
        qr.feed(l.data(), r.data(), int(frames), pos);
        const double* ch[2] = {l.data(), r.data()};
        dither.process16(ch, int(frames), out);
        pos += frames;
        if (!keep(double(pos) / double(total))) { cancelled = true; return false; }
        return true;
    };

    ddp::WriteOptions wo;
    juce::String header;
    if (s.client.isNotEmpty()) header << "Client:   " << s.client << "\n";
    if (s.engineer.isNotEmpty()) header << "Engineer: " << s.engineer << "\n";
    header << "Created:  " << juce::Time::getCurrentTime().toString(true, true) << " by Lathe\n";
    header << "Dither:   " << ditherName(es.shape) << "\n";
    header << "Engine:   64-bit float mix and plugin path";
    if (es.runPlugins) header << " (" << juce::String(engine_.chain().countDoublePrecision()) << " plugin(s) in 64-bit)";
    header << "\n";
    wo.pqLogHeader = header.toStdString();

    auto res = ddp::writeDdp(disc, folder.getFullPathName().toStdString(), pull, wo);
    if (!res.ok && err.isNotEmpty()) res.error = err.toStdString();
    if (cancelled) res.error = "Export cancelled.";
    qr.fill(qc);
    if (res.ok) {
        // Append QC to the PQ log so the plant sheet and the engineer's numbers travel together.
        auto log = folder.getChildFile("PQ_LOG.TXT");
        log.appendText("\n" + qc.toText());
        for (auto& a : adj) log.appendText("Note: " + a + "\n");
    }
    return res;
}

juce::String Exporter::exportTrackWavs(const Session& s, const juce::File& folder, const ExportSettings& es,
                                       const Keep& keep) {
    cd::Disc disc = s.toDisc();
    if (disc.tracks.empty()) return "No tracks to export.";
    folder.createDirectory();
    MasterStream ms(engine_, s, es, cd::kSampleRate);
    dsp::Ditherer dither;
    dither.prepare(2, 16, es.shape, 0x5EED1234u, cd::kSampleRate);
    juce::WavAudioFormat wav;
    const juce::int64 total = disc.leadOutSector * cd::kSamplesPerSector;
    juce::int64 pos = 0;
    std::vector<double> l(4096), r(4096);
    std::vector<int16_t> pcm(4096 * 2);
    juce::String err;

    for (size_t t = 0; t < disc.tracks.size(); ++t) {
        // CD tracks run from index 01 to the next track's index 01, like a ripped disc.
        juce::int64 a = disc.tracks[t].startSector() * cd::kSamplesPerSector;
        juce::int64 b = (t + 1 < disc.tracks.size() ? disc.tracks[t + 1].startSector() : disc.leadOutSector) * cd::kSamplesPerSector;
        // Discard audio before this track (the pregap of track 1).
        while (pos < a) {
            int n = int(juce::jmin<juce::int64>(4096, a - pos));
            if (!ms.pull(l.data(), r.data(), n, err)) return err;
            const double* ch[2] = {l.data(), r.data()};
            dither.process16(ch, n, pcm.data());
            pos += n;
        }
        auto name = juce::String(int(t + 1)).paddedLeft('0', 2) + " " +
                    juce::File::createLegalFileName(juce::String::fromUTF8(disc.tracks[t].text.title.c_str())) + ".wav";
        auto f = folder.getChildFile(name);
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
        if (!os) return "Couldn't write " + f.getFullPathName();
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), cd::kSampleRate, 2, 16, {}, 0));
        if (!w) return "Couldn't create a WAV writer.";
        os.release();
        juce::AudioBuffer<int> ib(2, 4096);
        while (pos < b) {
            int n = int(juce::jmin<juce::int64>(4096, b - pos));
            if (!ms.pull(l.data(), r.data(), n, err)) return err;
            const double* ch[2] = {l.data(), r.data()};
            dither.process16(ch, n, pcm.data());
            for (int i = 0; i < n; ++i) {   // JUCE int writers take left-justified 32-bit samples
                ib.setSample(0, i, int(pcm[size_t(i * 2)]) << 16);
                ib.setSample(1, i, int(pcm[size_t(i * 2 + 1)]) << 16);
            }
            const int* chans[3] = {ib.getReadPointer(0), ib.getReadPointer(1), nullptr};
            if (!w->write(chans, n)) return "Disk write failed.";
            pos += n;
            if (!keep(double(pos) / double(total))) return "Export cancelled.";
        }
    }
    return {};
}

juce::String Exporter::exportProgramWav(const Session& s, const juce::File& file, int bits, const ExportSettings& es,
                                        const Keep& keep) {
    if (s.programmeChannels() == 6) return exportRangeWav(s, file, bits, es, {0.0, s.end()}, keep);
    const double rate = s.effectiveRate();
    MasterStream ms(engine_, s, es, rate);
    const juce::int64 total = juce::int64(std::ceil(s.end() * rate));
    juce::WavAudioFormat wav;
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> os(file.createOutputStream().release());
    if (!os) return "Couldn't write " + file.getFullPathName();
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate, 2, bits, {}, 0));
    if (!w) return "Couldn't create a " + juce::String(bits) + "-bit WAV writer.";
    os.release();
    std::vector<double> l(4096), r(4096);
    juce::AudioBuffer<float> fbuf(2, 4096);
    std::vector<int> ibuf[2];
    dsp::Ditherer dither;
    if (bits < 32) dither.prepare(2, bits, es.shape, 0x5EED1234u, rate);
    std::vector<int32_t> q(4096 * 2);
    juce::String err;
    for (juce::int64 pos = 0; pos < total;) {
        int n = int(juce::jmin<juce::int64>(4096, total - pos));
        if (!ms.pull(l.data(), r.data(), n, err)) return err;
        if (bits < 32) {
            // Dither from 64-bit to the target word length, then write the integers exactly.
            const double* ch[2] = {l.data(), r.data()};
            dither.process(ch, n, q.data());
            if (!writeInts(*w, q.data(), n, bits, ibuf)) return "Disk write failed.";
        } else {
            for (int i = 0; i < n; ++i) { fbuf.setSample(0, i, float(l[size_t(i)])); fbuf.setSample(1, i, float(r[size_t(i)])); }
            if (!w->writeFromAudioSampleBuffer(fbuf, 0, n)) return "Disk write failed.";
        }
        pos += n;
        if (!keep(double(pos) / double(total))) return "Export cancelled.";
    }
    return {};
}

juce::String Exporter::exportRangeWav(const Session& s, const juce::File& file, int bits, const ExportSettings& es, juce::Range<double> range,
                                      const Keep& keep) {
    const double rate = s.effectiveRate();
    range = range.getIntersectionWith({0.0, juce::jmax(s.end(), range.getEnd())});
    if (range.getLength() <= 0) return "Nothing to export there.";
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> os(file.createOutputStream().release());
    if (!os) return "Couldn't write " + file.getFullPathName();
    juce::WavAudioFormat wav;
    const juce::int64 total = juce::int64(std::llround(range.getLength() * rate));
    juce::String err;

    if (s.programmeChannels() == 6) {
        // Six channels straight from the programme (region effects apply, the stereo master chain doesn't).
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate, juce::AudioChannelSet::create5point1(), bits, {}, 0));
        if (!w) return "Couldn't create a six-channel " + juce::String(bits) + "-bit WAV writer.";
        os.release();
        engine_.suspendForOffline(true);
        juce::String result;
        {
            const juce::ScopedLock fxLock(engine_.regionFx().lock());
            ProgramRenderer pr(engine_.formats());
            if (es.runPlugins) pr.setRegionFx(&engine_.regionFx());
            pr.setSession(s, rate, true);
            pr.seek(juce::int64(std::llround(range.getStart() * rate)));
            juce::AudioBuffer<double> buf(6, 4096);
            juce::AudioBuffer<float> fbuf(6, 4096);
            dsp::Ditherer dither;
            if (bits < 32) dither.prepare(6, bits, es.shape, 0x5EED1234u, rate);
            std::vector<int32_t> q(4096 * 6);
            std::vector<int> ib[6];
            for (juce::int64 pos = 0; pos < total && result.isEmpty();) {
                const int n = int(juce::jmin<juce::int64>(4096, total - pos));
                if (!pr.render(buf, 0, n)) { result = pr.lastError(); break; }
                if (bits < 32) {
                    const double* ch[6];
                    for (int c = 0; c < 6; ++c) ch[c] = buf.getReadPointer(c);
                    dither.process(ch, n, q.data());
                    const int shift = 32 - bits;
                    const int* cp[7] = {};
                    for (int c = 0; c < 6; ++c) {
                        ib[c].resize(size_t(n));
                        for (int i = 0; i < n; ++i) ib[c][size_t(i)] = int(uint32_t(q[size_t(i * 6 + c)]) << shift);
                        cp[c] = ib[c].data();
                    }
                    if (!w->write(cp, n)) result = "Disk write failed.";
                } else {
                    for (int c = 0; c < 6; ++c)
                        for (int i = 0; i < n; ++i) fbuf.setSample(c, i, float(buf.getSample(c, i)));
                    if (!w->writeFromAudioSampleBuffer(fbuf, 0, n)) result = "Disk write failed.";
                }
                pos += n;
                if (!keep(double(pos) / double(total))) result = "Export cancelled.";
            }
        }
        engine_.regionFx().prepare(engine_.deviceRate(), 2048, false);
        engine_.suspendForOffline(false);
        return result;
    }

    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), rate, 2, bits, {}, 0));
    if (!w) return "Couldn't create a " + juce::String(bits) + "-bit WAV writer.";
    os.release();
    const double pre = juce::jmin(1.0, range.getStart());   // warm the chain up on what comes before
    MasterStream ms(engine_, s, es, rate, range.getStart() - pre);
    std::vector<double> l(4096), r(4096);
    for (juce::int64 skip = juce::int64(std::llround(pre * rate)); skip > 0;) {
        const int n = int(juce::jmin<juce::int64>(4096, skip));
        if (!ms.pull(l.data(), r.data(), n, err)) return err;
        skip -= n;
    }
    juce::AudioBuffer<float> fbuf(2, 4096);
    std::vector<int> ibuf[2];
    dsp::Ditherer dither;
    if (bits < 32) dither.prepare(2, bits, es.shape, 0x5EED1234u, rate);
    std::vector<int32_t> q(4096 * 2);
    for (juce::int64 pos = 0; pos < total;) {
        const int n = int(juce::jmin<juce::int64>(4096, total - pos));
        if (!ms.pull(l.data(), r.data(), n, err)) return err;
        if (bits < 32) {
            const double* ch[2] = {l.data(), r.data()};
            dither.process(ch, n, q.data());
            if (!writeInts(*w, q.data(), n, bits, ibuf)) return "Disk write failed.";
        } else {
            for (int i = 0; i < n; ++i) { fbuf.setSample(0, i, float(l[size_t(i)])); fbuf.setSample(1, i, float(r[size_t(i)])); }
            if (!w->writeFromAudioSampleBuffer(fbuf, 0, n)) return "Disk write failed.";
        }
        pos += n;
        if (!keep(double(pos) / double(total))) return "Export cancelled.";
    }
    return {};
}

juce::String Exporter::analyseProgram(const Session& s, const ExportSettings& es, const Keep& keep, dsp::AnalysisResult& processed,
                                      dsp::AnalysisResult* raw) {
    std::vector<std::pair<double, double>> ranges;
    for (auto& r : s.trackRanges()) ranges.push_back({r.getStart(), r.getEnd()});
    const juce::int64 total = juce::int64(std::ceil(s.end() * cd::kSampleRate));
    const int passes = raw ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        ExportSettings e = es;
        e.runPlugins = pass == 0 ? es.runPlugins : false;
        MasterStream ms(engine_, s, e, cd::kSampleRate);
        dsp::ProgramAnalyzer an;
        an.prepare(cd::kSampleRate, ranges);
        std::vector<double> l(4096), r(4096);
        juce::String err;
        for (juce::int64 pos = 0; pos < total;) {
            int n = int(juce::jmin<juce::int64>(4096, total - pos));
            if (!ms.pull(l.data(), r.data(), n, err)) return err;
            an.process(l.data(), r.data(), n);
            pos += n;
            if (!keep((pass + double(pos) / double(total)) / passes)) return "Cancelled.";
        }
        (pass == 0 ? processed : *raw) = an.finish();
    }
    return {};
}

juce::String Exporter::analyse(const Session& s, const ExportSettings& es, const Keep& keep, QcReport& qc) {
    MasterStream ms(engine_, s, es, cd::kSampleRate);
    QcRunner qr(s, cd::kSampleRate);
    const juce::int64 total = juce::int64(std::ceil(s.end() * cd::kSampleRate));
    std::vector<double> l(4096), r(4096);
    juce::String err;
    for (juce::int64 pos = 0; pos < total;) {
        int n = int(juce::jmin<juce::int64>(4096, total - pos));
        if (!ms.pull(l.data(), r.data(), n, err)) return err;
        qr.feed(l.data(), r.data(), n, pos);
        pos += n;
        if (!keep(double(pos) / double(total))) return "Cancelled.";
    }
    qr.fill(qc);
    return {};
}


// ================================================================= Batch render

namespace {

struct TrackInfo {
    juce::int64 start = 0, end = 0;   // samples at the sink's rate, [start, end)
    juce::String fileName;            // without extension
    juce::String title, performer, isrc;
    int number = 0;
};

// Writes a stream of dithered integers into one file per track, cut at the PQ marks.
class TrackSink {
public:
    enum Kind { Wav, Flac };
    TrackSink(Kind k, const juce::File& dir, double rate, int bits, std::vector<TrackInfo> tracks, const Session& s)
        : kind_(k), dir_(dir), rate_(rate), bits_(bits), tracks_(std::move(tracks)), album_(s.discTitle), albumArtist_(s.discPerformer) {
        dir_.createDirectory();
    }

    bool consume(const int32_t* interleaved, int n, juce::int64 pos, juce::String& err) {
        for (size_t t = 0; t < tracks_.size(); ++t) {
            auto& tr = tracks_[t];
            juce::int64 a = juce::jmax(pos, tr.start), b = juce::jmin(pos + n, tr.end);
            if (b <= a) continue;
            if (int(t) != cur_) {
                if (!close(err)) return false;
                if (!open(int(t), err)) return false;
            }
            if (!writeInts(*w_, interleaved + (a - pos) * 2, int(b - a), bits_, tmp_)) { err = "Disk write failed (" + file_.getFileName() + ")"; return false; }
            if (b == tr.end && !close(err)) return false;
        }
        return true;
    }

    bool finish(juce::String& err) { return close(err); }
    const juce::Array<juce::File>& files() const { return written_; }

private:
    bool open(int t, juce::String& err) {
        auto& tr = tracks_[size_t(t)];
        file_ = dir_.getChildFile(tr.fileName + (kind_ == Wav ? ".wav" : ".flac"));
        file_.deleteFile();
        std::unique_ptr<juce::OutputStream> os(file_.createOutputStream().release());
        if (!os) { err = "Couldn't write " + file_.getFullPathName(); return false; }
        juce::StringPairArray meta;
        if (kind_ == Wav) {
            meta.set(juce::WavAudioFormat::riffInfoTitle, tr.title);
            meta.set(juce::WavAudioFormat::riffInfoArtist, tr.performer);
            meta.set(juce::WavAudioFormat::riffInfoProductName, album_);
            meta.set(juce::WavAudioFormat::riffInfoTrackNumber, juce::String(tr.number));
            if (tr.isrc.isNotEmpty()) meta.set(juce::WavAudioFormat::riffInfoComment, "ISRC " + tr.isrc);
            meta.set(juce::WavAudioFormat::riffInfoSoftware, "Lathe");
            w_.reset(wav_.createWriterFor(os.get(), rate_, 2, bits_, meta, 0));
        } else {
            w_.reset(flac_.createWriterFor(os.get(), rate_, 2, bits_, meta, 5));
        }
        if (!w_) { err = "Couldn't create a " + juce::String(bits_) + "-bit writer for " + file_.getFileName(); return false; }
        os.release();
        cur_ = t;
        return true;
    }

    bool close(juce::String& err) {
        if (!w_) return true;
        w_.reset();   // flushes and finalises the header
        if (kind_ == Flac) {
            auto& tr = tracks_[size_t(cur_)];
            util::Tags tags{{"TITLE", tr.title.toStdString()},
                            {"ARTIST", tr.performer.toStdString()},
                            {"ALBUM", album_.toStdString()},
                            {"ALBUMARTIST", albumArtist_.toStdString()},
                            {"TRACKNUMBER", std::to_string(tr.number)},
                            {"TRACKTOTAL", std::to_string(tracks_.size())}};
            if (tr.isrc.isNotEmpty()) tags.push_back({"ISRC", tr.isrc.toStdString()});
            auto e = util::writeFlacTags(file_.getFullPathName().toStdString(), tags);
            if (!e.empty()) { err = "Tagging " + file_.getFileName() + ": " + juce::String(e); return false; }
        }
        written_.add(file_);
        cur_ = -1;
        return true;
    }

    Kind kind_;
    juce::File dir_, file_;
    double rate_;
    int bits_;
    std::vector<TrackInfo> tracks_;
    juce::String album_, albumArtist_;
    juce::WavAudioFormat wav_;
    juce::FlacAudioFormat flac_;
    std::unique_ptr<juce::AudioFormatWriter> w_;
    std::vector<int> tmp_[2];
    int cur_ = -1;
    juce::Array<juce::File> written_;
};

std::vector<TrackInfo> trackList(const Session& s, const cd::Disc& disc, double rate, bool cdFrames) {
    std::vector<TrackInfo> v;
    auto ranges = s.trackRanges();
    for (size_t t = 0; t < disc.tracks.size(); ++t) {
        TrackInfo ti;
        if (cdFrames) {   // exactly the CD's track boundaries
            ti.start = disc.tracks[t].startSector() * cd::kSamplesPerSector;
            ti.end = (t + 1 < disc.tracks.size() ? disc.tracks[t + 1].startSector() : disc.leadOutSector) * cd::kSamplesPerSector;
        } else {
            ti.start = juce::int64(std::llround(ranges[int(t)].getStart() * rate));
            ti.end = juce::int64(std::llround(ranges[int(t)].getEnd() * rate));
        }
        ti.number = int(t + 1);
        ti.title = juce::String::fromUTF8(disc.tracks[t].text.title.c_str());
        ti.performer = juce::String::fromUTF8(disc.tracks[t].text.performer.c_str());
        if (ti.performer.isEmpty()) ti.performer = s.discPerformer;
        ti.isrc = juce::String(disc.tracks[t].isrc);
        ti.fileName = juce::String(ti.number).paddedLeft('0', 2) + " " +
                      juce::File::createLegalFileName(ti.title.isEmpty() ? juce::String("Track") : ti.title);
        v.push_back(ti);
    }
    return v;
}

juce::String rateLabel(double r) {
    auto k = r / 1000.0;
    return std::abs(k - std::round(k)) < 0.01 ? juce::String(int(std::round(k))) : juce::String(k, 1);
}

juce::String md5Of(const juce::File& f) {
    juce::FileInputStream in(f);
    if (!in.openedOk()) return {};
    util::Md5 m;
    juce::HeapBlock<char> buf(1 << 20);
    for (;;) {
        auto n = in.read(buf.get(), 1 << 20);
        if (n <= 0) break;
        m.update(buf.get(), size_t(n));
    }
    return m.hexDigest();
}


// Renders product units from the master stream. Each unit takes its window of the programme,
// applies its own fades (after the master chain, before dither) and writes either a 16-bit
// CD image (staged to a temp file, then written as a DDP) or a hi-res side file.
class UnitSink {
public:
    UnitSink(UnitRender info, bool cd, double rate, int bits, dsp::Ditherer::Shape shape, juce::File out, uint32_t seed)
        : info_(std::move(info)), cd_(cd), rate_(rate), bits_(bits), out_(std::move(out)) {
        winStart_ = juce::int64(std::llround(info_.window.start * rate));
        winEnd_ = juce::int64(std::llround(info_.window.end * rate));
        if (cd_) {
            leadSamples_ = juce::int64(std::llround(info_.lead * rate));
            total_ = info_.disc.leadOutSector * cd::kSamplesPerSector;
        }
        dither_.prepare(2, bits, shape, seed, rate);
        meter_.prepare(rate, 2);
        info_.rate = rate;
        info_.bits = bits;
    }

    ~UnitSink() { if (tmp_.exists()) tmp_.deleteFile(); }

    bool consume(const double* l, const double* r, int n, juce::int64 pos, juce::String& err) {
        juce::int64 a = juce::jmax(pos, winStart_), b = juce::jmin(pos + n, winEnd_);
        if (b <= a || done_) return true;
        if (!opened_ && !open(err)) return false;
        const int m = int(b - a);
        gl_.resize(size_t(m));
        gr_.resize(size_t(m));
        for (int i = 0; i < m; ++i) {
            const double g = unitGain(info_.window, double(a + i) / rate_);
            gl_[size_t(i)] = l[a - pos + i] * g;
            gr_[size_t(i)] = r[a - pos + i] * g;
        }
        fl_.resize(size_t(m));
        fr_.resize(size_t(m));
        for (int i = 0; i < m; ++i) { fl_[size_t(i)] = float(gl_[size_t(i)]); fr_[size_t(i)] = float(gr_[size_t(i)]); }
        const float* fch[2] = {fl_.data(), fr_.data()};
        meter_.process(fch, m);
        const double* ch[2] = {gl_.data(), gr_.data()};
        if (cd_) {
            pcm_.resize(size_t(m * 2));
            dither_.process16(ch, m, pcm_.data());
            if (!writePcm(pcm_.data(), m)) { err = "Disk write failed (" + tmp_.getFileName() + ")"; return false; }
        } else {
            ints_.resize(size_t(m * 2));
            dither_.process(ch, m, ints_.data());
            if (!writeInts(*w_, ints_.data(), m, bits_, tmp2_)) { err = "Disk write failed (" + out_.getFileName() + ")"; return false; }
        }
        written_ += m;
        if (b == winEnd_) done_ = true;
        return true;
    }

    // Closes the side file, or writes and verifies the DDP from the staged image.
    bool finish(const juce::File& root, juce::String& err) {
        if (!opened_ && !open(err)) return false;
        info_.lufs = meter_.integratedLufs();
        info_.truePeakDb = meter_.truePeakDb();
        if (!cd_) {
            w_.reset();
            info_.path = out_.getRelativePathFrom(root);
            info_.verified = true;
            return true;
        }
        // Pad to the disc's last sector with digital silence, then hand the image to the DDP writer.
        std::vector<int16_t> zeros(8192 * 2, 0);
        while (leadSamples_ + written_ < total_) {
            int n = int(juce::jmin<juce::int64>(8192, total_ - leadSamples_ - written_));
            if (!writePcm(zeros.data(), n)) { err = "Disk write failed."; return false; }
            written_ += n;
        }
        os_.reset();
        juce::FileInputStream in(tmp_);
        if (!in.openedOk()) { err = "Couldn't read back " + tmp_.getFullPathName(); return false; }
        auto pull = [&](int16_t* dst, int64_t frames) -> bool {
            const auto bytes = size_t(frames) * 4;
            if (in.read(dst, int(bytes)) != int(bytes)) return false;
            for (int64_t i = 0; i < frames * 2; ++i) dst[i] = int16_t(juce::ByteOrder::swapIfBigEndian(uint16_t(dst[i])));
            return true;
        };
        ddp::WriteOptions wo;
        wo.pqLogHeader = header_.toStdString();
        auto res = ddp::writeDdp(info_.disc, out_.getFullPathName().toStdString(), pull, wo);
        if (!res.ok) { err = info_.longLabel + ": " + juce::String::fromUTF8(res.error.c_str()); return false; }
        auto check = ddp::readDdp(out_.getFullPathName().toStdString());
        info_.verified = check.ok && check.checksumsOk;
        info_.imageMd5 = juce::String(res.imageMd5);
        info_.path = out_.getRelativePathFrom(root);
        tmp_.deleteFile();
        return true;
    }

    void setHeader(juce::String h) { header_ = std::move(h); }
    const UnitRender& info() const { return info_; }
    bool isCd() const { return cd_; }
    juce::File output() const { return out_; }

private:
    bool open(juce::String& err) {
        opened_ = true;
        if (cd_) {
            out_.createDirectory();
            tmp_ = out_.getChildFile(".lathe-image.tmp");
            tmp_.deleteFile();
            os_ = std::make_unique<juce::FileOutputStream>(tmp_);
            if (!os_->openedOk()) { err = "Couldn't write " + tmp_.getFullPathName(); return false; }
            // Pregap: digital silence, not dithered.
            std::vector<int16_t> zeros(size_t(juce::jmax<juce::int64>(1, leadSamples_)) * 2, 0);
            if (leadSamples_ > 0 && !writePcm(zeros.data(), int(leadSamples_))) { err = "Disk write failed."; return false; }
            return true;
        }
        out_.getParentDirectory().createDirectory();
        out_.deleteFile();
        std::unique_ptr<juce::OutputStream> os(out_.createOutputStream().release());
        if (!os) { err = "Couldn't write " + out_.getFullPathName(); return false; }
        juce::StringPairArray meta;
        meta.set(juce::WavAudioFormat::riffInfoTitle, info_.longLabel);
        meta.set(juce::WavAudioFormat::riffInfoSoftware, "Lathe");
        w_.reset(wav_.createWriterFor(os.get(), rate_, 2, bits_, meta, 0));
        if (!w_) { err = "Couldn't create a " + juce::String(bits_) + "-bit writer for " + out_.getFileName(); return false; }
        os.release();
        return true;
    }

    bool writePcm(const int16_t* interleaved, int frames) {
        le_.resize(size_t(frames) * 2);
        for (int i = 0; i < frames * 2; ++i) le_[size_t(i)] = juce::ByteOrder::swapIfBigEndian(uint16_t(interleaved[i]));
        return os_->write(le_.data(), size_t(frames) * 4);
    }

    UnitRender info_;
    bool cd_;
    double rate_;
    int bits_;
    juce::File out_, tmp_;
    juce::String header_;
    juce::int64 winStart_ = 0, winEnd_ = 0, leadSamples_ = 0, total_ = 0, written_ = 0;
    bool opened_ = false, done_ = false;
    dsp::Ditherer dither_;
    dsp::LoudnessMeter meter_;
    std::unique_ptr<juce::FileOutputStream> os_;
    juce::WavAudioFormat wav_;
    std::unique_ptr<juce::AudioFormatWriter> w_;
    std::vector<double> gl_, gr_;
    std::vector<float> fl_, fr_;
    std::vector<int16_t> pcm_;
    std::vector<uint16_t> le_;
    std::vector<int32_t> ints_;
    std::vector<int> tmp2_[2];
};

// Builds the unit sinks for every product. CD units go in `cd`, side units in `hi`.
juce::String makeUnitSinks(const Session& s, const juce::File& folder, double hiRate, int hiBits, dsp::Ditherer::Shape shape,
                           const juce::String& header, std::vector<std::unique_ptr<UnitSink>>& cdOut,
                           std::vector<std::unique_ptr<UnitSink>>& hiOut) {
    for (int pi = 0; pi < s.products.size(); ++pi) {
        const auto& p = s.products.getReference(pi);
        const auto& spec = productSpec(p.kind);
        for (auto& issue : validateProduct(s, p))
            if (issue.error) return issue.message;
        const auto pname = juce::File::createLegalFileName(p.name.isEmpty() ? juce::String(spec.name) : p.name);
        for (int ui = 0; ui < p.units.size(); ++ui) {
            UnitRender info;
            info.product = pi;
            info.unit = ui;
            info.label = unitLabel(p, ui);
            info.longLabel = p.name + " " + unitLabel(p, ui, true);
            info.window = p.units[ui];
            const uint32_t seed = 0x5EED1234u + uint32_t(pi * 100 + ui);
            if (spec.cdFormat) {
                Session ps = s;
                if (p.upc.isNotEmpty()) ps.upcEan = p.upc;
                if (p.catalogue.isNotEmpty()) ps.catalogue = p.catalogue, ps.masterId = {};
                info.disc = ps.toDiscForUnit(info.window, ui + 1, p.units.size(), info.lead, &info.notes);
                auto issues = cd::validate(info.disc);
                if (cd::hasErrors(issues)) {
                    juce::String e = info.longLabel + " can't be made into a valid disc:";
                    for (auto& i : issues)
                        if (i.level == cd::Issue::Error) e << "\n  " << juce::String::fromUTF8(i.message.c_str());
                    return e;
                }
                auto dir = folder.getChildFile(pname + " DDP");
                if (p.units.size() > 1) {
                    auto cat = p.catalogue.isNotEmpty() ? p.catalogue : s.catalogue;
                    dir = dir.getChildFile(juce::File::createLegalFileName("Disc " + juce::String(ui + 1) + (cat.isNotEmpty() ? " - " + cat + "-D" + juce::String(ui + 1) : juce::String())));
                }
                auto sink = std::make_unique<UnitSink>(info, true, cd::kSampleRate, 16, shape, dir, seed);
                juce::String h = header;
                h << "Product:  " << p.name << ", " << info.label << " of " << p.units.size() << "\n";
                sink->setHeader(h);
                cdOut.push_back(std::move(sink));
            } else {
                auto title = s.discTitle.isNotEmpty() ? s.discTitle + " - " : juce::String();
                auto file = folder.getChildFile(pname + " " + spec.unitNoun + "s")
                                .getChildFile(juce::File::createLegalFileName(title + p.name + " " + unitLabel(p, ui) + ".wav"));
                hiOut.push_back(std::make_unique<UnitSink>(info, false, hiRate, hiBits, shape, file, seed));
            }
        }
    }
    return {};
}
}  // namespace

BatchResult Exporter::batchRender(const Session& s, const BatchSettings& bs, const juce::File& folder, const ExportSettings& es,
                                  const Keep& keep) {
    BatchResult res;
    juce::StringArray adj;
    cd::Disc disc = s.toDisc(&adj);
    if (disc.tracks.empty()) { res.error = "There are no track markers."; return res; }
    if (bs.ddp) {
        auto issues = cd::validate(disc);
        if (cd::hasErrors(issues)) {
            res.error = "The disc has errors that a plant would reject; fix them or untick DDP:";
            for (auto& i : issues)
                if (i.level == cd::Issue::Error) res.error << "\n  " << juce::String::fromUTF8(i.message.c_str());
            return res;
        }
    }
    if (!folder.createDirectory()) { res.error = "Couldn't create " + folder.getFullPathName(); return res; }

    const double hiRate = s.effectiveRate();
    juce::String unitHeader;
    if (s.client.isNotEmpty()) unitHeader << "Client:   " << s.client << "\n";
    if (s.engineer.isNotEmpty()) unitHeader << "Engineer: " << s.engineer << "\n";
    unitHeader << "Created:  " << juce::Time::getCurrentTime().toString(true, true) << " by Lathe (batch render)\n";
    unitHeader << "Dither:   " << ditherName(es.shape) << "\n";
    std::vector<std::unique_ptr<UnitSink>> cdUnits, hiUnits;
    if (bs.products && !s.products.isEmpty()) {
        auto e = makeUnitSinks(s, folder, hiRate, bs.hiBits >= 32 ? 24 : bs.hiBits, es.shape, unitHeader, cdUnits, hiUnits);
        if (e.isNotEmpty()) { res.error = e; return res; }
    }

    const bool passCd = bs.ddp || bs.wav16 || bs.flac16 || !cdUnits.empty();
    const bool passHi = bs.wavHi || bs.flacHi || bs.programme || !hiUnits.empty();
    const double weightCd = passCd ? (passHi ? 0.5 : 1.0) : 0.0;
    QcReport qc;
    juce::String err;
    juce::String settingsText;

    // ---- Pass 1: the CD master at 44.1 kHz / 16-bit. One render feeds the DDP and the
    // 16-bit track files, so the WAVs/FLACs are bit-identical to what's on the disc.
    if (passCd) {
        MasterStream ms(engine_, s, es, cd::kSampleRate);
        QcRunner qr(s, cd::kSampleRate);
        dsp::Ditherer dither;
        dither.prepare(2, 16, es.shape, 0x5EED1234u, cd::kSampleRate);
        std::vector<std::unique_ptr<TrackSink>> sinks;
        auto tracks = trackList(s, disc, cd::kSampleRate, true);
        if (bs.wav16) sinks.push_back(std::make_unique<TrackSink>(TrackSink::Wav, folder.getChildFile("WAV 16-bit 44.1 kHz"), cd::kSampleRate, 16, tracks, s));
        if (bs.flac16) sinks.push_back(std::make_unique<TrackSink>(TrackSink::Flac, folder.getChildFile("FLAC 16-bit 44.1 kHz"), cd::kSampleRate, 16, tracks, s));
        const juce::int64 total = disc.leadOutSector * cd::kSamplesPerSector;
        std::vector<double> l, r;
        std::vector<int32_t> ints;
        juce::int64 pos = 0;
        bool cancelled = false;

        auto pull16 = [&](int16_t* out, int64_t frames) -> bool {
            l.resize(size_t(frames));
            r.resize(size_t(frames));
            ints.resize(size_t(frames * 2));
            if (!ms.pull(l.data(), r.data(), int(frames), err)) return false;
            qr.feed(l.data(), r.data(), int(frames), pos);
            for (auto& u : cdUnits)
                if (!u->consume(l.data(), r.data(), int(frames), pos, err)) return false;
            const double* ch[2] = {l.data(), r.data()};
            dither.process(ch, int(frames), ints.data());
            for (int64_t i = 0; i < frames * 2; ++i) out[i] = int16_t(ints[size_t(i)]);
            for (auto& sk : sinks)
                if (!sk->consume(ints.data(), int(frames), pos, err)) return false;
            pos += frames;
            if (!keep(weightCd * double(pos) / double(total))) { cancelled = true; return false; }
            return true;
        };

        if (bs.ddp) {
            ddp::WriteOptions wo;
            juce::String header;
            if (s.client.isNotEmpty()) header << "Client:   " << s.client << "\n";
            if (s.engineer.isNotEmpty()) header << "Engineer: " << s.engineer << "\n";
            header << "Created:  " << juce::Time::getCurrentTime().toString(true, true) << " by Lathe (batch render)\n";
            header << "Dither:   " << ditherName(es.shape) << "\n";
            wo.pqLogHeader = header.toStdString();
            auto ddpDir = folder.getChildFile("DDP");
            auto w = ddp::writeDdp(disc, ddpDir.getFullPathName().toStdString(), pull16, wo);
            if (!w.ok) {
                res.error = cancelled ? juce::String("Cancelled.") : err.isNotEmpty() ? err : juce::String::fromUTF8(w.error.c_str());
                return res;
            }
            for (auto& f : w.files) res.files.add("DDP/" + juce::String(f));
            auto check = ddp::readDdp(ddpDir.getFullPathName().toStdString());
            res.ddpVerified = check.ok && check.checksumsOk;
        } else {
            std::vector<int16_t> scratch;
            while (pos < total) {
                auto n = juce::jmin<juce::int64>(4096, total - pos);
                scratch.resize(size_t(n * 2));
                if (!pull16(scratch.data(), n)) { res.error = cancelled ? juce::String("Cancelled.") : err; return res; }
            }
        }
        for (auto& sk : sinks) {
            if (!sk->finish(err)) { res.error = err; return res; }
            for (auto& f : sk->files()) res.files.add(f.getRelativePathFrom(folder));
        }
        for (auto& u : cdUnits) {
            if (!u->finish(folder, err)) { res.error = err; return res; }
            auto kids = u->output().findChildFiles(juce::File::findFiles, false);
            kids.sort();
            for (auto& f : kids) res.files.add(f.getRelativePathFrom(folder));
            res.delivery.units.push_back(u->info());
        }
        qr.fill(qc);
        settingsText << "CD master: 44.1 kHz / 16-bit, dither " << ditherName(es.shape) << "\n";
    }

    // ---- Pass 2: hi-res deliverables at the project rate (no sample-rate conversion).
    if (passHi) {
        MasterStream ms(engine_, s, es, hiRate);
        std::unique_ptr<QcRunner> qr;
        if (!passCd) qr = std::make_unique<QcRunner>(s, hiRate);
        dsp::Ditherer dither;
        const bool floatOut = bs.hiBits >= 32;
        if (!floatOut) dither.prepare(2, bs.hiBits, es.shape, 0x5EED1234u, hiRate);
        const int sinkBits = floatOut ? 24 : bs.hiBits;
        auto label = juce::String(sinkBits) + "-bit " + rateLabel(hiRate) + " kHz";
        std::vector<std::unique_ptr<TrackSink>> sinks;
        auto tracks = trackList(s, disc, hiRate, false);
        if (bs.wavHi) sinks.push_back(std::make_unique<TrackSink>(TrackSink::Wav, folder.getChildFile("WAV " + label), hiRate, sinkBits, tracks, s));
        if (bs.flacHi) sinks.push_back(std::make_unique<TrackSink>(TrackSink::Flac, folder.getChildFile("FLAC " + label), hiRate, sinkBits, tracks, s));
        std::unique_ptr<juce::AudioFormatWriter> prog;
        juce::File progFile;
        juce::WavAudioFormat wav;
        if (bs.programme) {
            progFile = folder.getChildFile(juce::File::createLegalFileName((s.discTitle.isEmpty() ? juce::String("Programme") : s.discTitle) + " " + label + ".wav"));
            progFile.deleteFile();
            std::unique_ptr<juce::OutputStream> os(progFile.createOutputStream().release());
            if (os) prog.reset(wav.createWriterFor(os.get(), hiRate, 2, sinkBits, {}, 0));
            if (!prog) { res.error = "Couldn't write " + progFile.getFullPathName(); return res; }
            os.release();
        }
        const juce::int64 total = juce::int64(std::ceil(s.end() * hiRate));
        std::vector<double> l(4096), r(4096);
        std::vector<int32_t> ints(4096 * 2);
        std::vector<int> tmp[2];
        for (juce::int64 pos = 0; pos < total;) {
            int n = int(juce::jmin<juce::int64>(4096, total - pos));
            if (!ms.pull(l.data(), r.data(), n, err)) { res.error = err; return res; }
            if (qr) qr->feed(l.data(), r.data(), n, pos);
            for (auto& u : hiUnits)
                if (!u->consume(l.data(), r.data(), n, pos, err)) { res.error = err; return res; }
            const double* ch[2] = {l.data(), r.data()};
            if (floatOut) {   // 32-bit float requested: deliver 24-bit integer files without dither
                for (int i = 0; i < n; ++i) {
                    ints[size_t(i * 2)] = int32_t(juce::jlimit(-8388608.0, 8388607.0, std::round(l[size_t(i)] * 8388608.0)));
                    ints[size_t(i * 2 + 1)] = int32_t(juce::jlimit(-8388608.0, 8388607.0, std::round(r[size_t(i)] * 8388608.0)));
                }
            } else {
                dither.process(ch, n, ints.data());
            }
            for (auto& sk : sinks)
                if (!sk->consume(ints.data(), n, pos, err)) { res.error = err; return res; }
            if (prog && !writeInts(*prog, ints.data(), n, sinkBits, tmp)) { res.error = "Disk write failed."; return res; }
            pos += n;
            if (!keep(weightCd + (1.0 - weightCd) * double(pos) / double(total))) { res.error = "Cancelled."; return res; }
        }
        for (auto& sk : sinks) {
            if (!sk->finish(err)) { res.error = err; return res; }
            for (auto& f : sk->files()) res.files.add(f.getRelativePathFrom(folder));
        }
        if (prog) { prog.reset(); res.files.add(progFile.getFileName()); }
        for (auto& u : hiUnits) {
            if (!u->finish(folder, err)) { res.error = err; return res; }
            res.files.add(u->info().path);
            res.delivery.units.push_back(u->info());
        }
        if (qr) qr->fill(qc);
        settingsText << "Hi-res: " << label << ", dither " << (floatOut ? juce::String("none") : ditherName(es.shape)) << "\n";
    }

    // ---- Report: settings, QC and an MD5 for every file, so the client can verify delivery.
    juce::String rep;
    rep << "LATHE RENDER REPORT\n";
    rep << (s.discTitle.isNotEmpty() ? s.discTitle : juce::String("(untitled)"));
    if (s.discPerformer.isNotEmpty()) rep << " - " << s.discPerformer;
    rep << "\n";
    if (s.client.isNotEmpty()) rep << "Client:   " << s.client << "\n";
    if (s.engineer.isNotEmpty()) rep << "Engineer: " << s.engineer << "\n";
    rep << "Rendered: " << juce::Time::getCurrentTime().toString(true, true) << "\n";
    rep << "Engine:   64-bit float mix, region effects and master chain";
    rep << " (" << engine_.chain().size() << " master insert(s))\n";
    rep << settingsText << "\n";
    if (bs.ddp) rep << "DDP: " << (res.ddpVerified ? "written and verified (all checksums match)" : "WRITTEN BUT VERIFY FAILED - re-render") << "\n\n";
    rep << qc.toText() << "\n";
    for (auto& a : adj) rep << "Note: " << a << "\n";
    if (!res.delivery.units.empty()) {
        rep << "PRODUCTS\n";
        for (auto& u : res.delivery.units) {
            rep << "  " << u.longLabel << ": " << juce::String(u.window.length() / 60.0, 2) << " min, " << fmtDb(u.lufs, "LUFS") << ", "
                << fmtDb(u.truePeakDb, "dBTP");
            if (!u.disc.tracks.empty()) rep << ", " << int(u.disc.tracks.size()) << " tracks, DDP " << (u.verified ? "verified" : "VERIFY FAILED");
            rep << "\n";
            for (auto& n : u.notes) rep << "    Note: " << n << "\n";
        }
        rep << "\n";
    }
    rep << "FILES (MD5)\n";
    for (auto& f : res.files) {
        auto file = folder.getChildFile(f);
        rep << "  " << md5Of(file) << "  " << f << "\n";
    }
    folder.getChildFile("Render Report.txt").replaceWithText(rep);
    res.files.add("Render Report.txt");
    res.report = rep;
    bool unitsOk = true;
    for (auto& u : res.delivery.units) unitsOk = unitsOk && u.verified;
    res.ok = (!bs.ddp || res.ddpVerified) && unitsOk;
    if (!res.ok) res.error = "A DDP didn't verify after writing. Re-render to a different drive.";
    auto& d = res.delivery;
    d.disc = disc;
    d.discNotes = adj;
    d.fullDdp = bs.ddp;
    d.ddpVerified = res.ddpVerified;
    d.qc = qc;
    d.files = res.files;
    d.settingsText = settingsText;
    d.dither = ditherName(es.shape);
    d.hiRate = hiRate;
    d.hiBits = bs.hiBits;
    return res;
}

}  // namespace lathe
