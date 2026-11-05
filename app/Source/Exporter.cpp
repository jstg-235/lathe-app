#include "Exporter.h"

#include "../../core/util/FlacTags.h"
#include "../../core/util/Hash.h"

#include <CDSPResampler.h>

namespace lathe {

namespace {

// Pulls finished master audio: programme → chain → (optional) SRC. Owns the chain while alive.
class MasterStream {
public:
    MasterStream(AudioEngine& e, const Session& s, const ExportSettings& es, double outRate)
        : engine_(e), renderer_(e.formats()), runPlugins_(es.runPlugins) {
        projRate_ = s.effectiveRate();
        outRate_ = outRate;
        engine_.suspendForOffline(true);
        lock_ = std::make_unique<juce::ScopedLock>(engine_.chain().lock());
        fxLock_ = std::make_unique<juce::ScopedLock>(engine_.regionFx().lock());   // region chains belong to the export now
        if (es.runPlugins) renderer_.setRegionFx(&engine_.regionFx());
        renderer_.setSession(s, projRate_, true);
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

    const bool passCd = bs.ddp || bs.wav16 || bs.flac16;
    const bool passHi = bs.wavHi || bs.flacHi || bs.programme;
    const double hiRate = s.effectiveRate();
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
    rep << "FILES (MD5)\n";
    for (auto& f : res.files) {
        auto file = folder.getChildFile(f);
        rep << "  " << md5Of(file) << "  " << f << "\n";
    }
    folder.getChildFile("Render Report.txt").replaceWithText(rep);
    res.files.add("Render Report.txt");
    res.report = rep;
    res.ok = !bs.ddp || res.ddpVerified;
    if (!res.ok) res.error = "The DDP didn't verify after writing. Re-render to a different drive.";
    return res;
}

}  // namespace lathe
