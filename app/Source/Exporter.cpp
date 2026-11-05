#include "Exporter.h"

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
        renderer_.setSession(s, projRate_);
        engine_.suspendForOffline(true);
        lock_ = std::make_unique<juce::ScopedLock>(engine_.chain().lock());
        if (runPlugins_) {
            engine_.chain().prepare(projRate_, kBlock);
            engine_.chain().setNonRealtime(true);
            latency_ = engine_.chain().latencySamples();
        }
        if (std::abs(projRate_ - outRate_) > 0.5)
            for (int ch = 0; ch < 2; ++ch) rs_[ch] = std::make_unique<r8b::CDSPResampler24>(projRate_, outRate_, kBlock);
        buf_.setSize(2, kBlock);
    }

    ~MasterStream() {
        if (runPlugins_) engine_.chain().setNonRealtime(false);
        lock_.reset();
        engine_.suspendForOffline(false);   // re-prepares the chain for playback
    }

    // Fills n frames per channel of output-rate audio.
    bool pull(float* l, float* r, int n, juce::String& err) {
        while (int(pend_[0].size()) - pendPos_ < n) {
            if (!renderer_.render(buf_, 0, kBlock)) { err = renderer_.lastError(); return false; }
            if (runPlugins_) engine_.chain().process(buf_, 0, kBlock);
            int skip = juce::jmin(latency_, kBlock);   // plugin delay compensation
            latency_ -= skip;
            int n0 = kBlock - skip;
            for (int ch = 0; ch < 2; ++ch) {
                const float* p = buf_.getReadPointer(ch) + skip;
                if (rs_[ch]) {
                    dbl_.assign(p, p + n0);
                    double* op = nullptr;
                    int got = rs_[ch]->process(dbl_.data(), n0, op);
                    for (int i = 0; i < got; ++i) pend_[ch].push_back(float(op[i]));
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
    std::unique_ptr<juce::ScopedLock> lock_;
    std::unique_ptr<r8b::CDSPResampler24> rs_[2];
    juce::AudioBuffer<float> buf_;
    std::vector<double> dbl_;
    std::vector<float> pend_[2];
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
    void feed(const float* l, const float* r, int n, juce::int64 pos) {
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
    juce::OwnedArray<dsp::LoudnessMeter> per;
    juce::Array<juce::int64> starts, ends;
};

juce::String fmtDb(double v, const char* unit) {
    return std::isfinite(v) ? juce::String(v, 1) + " " + unit : juce::String("--");
}

}  // namespace

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
    dither.prepare(2, 16, es.shape);
    std::vector<float> l, r;
    juce::int64 pos = 0;
    const juce::int64 total = disc.leadOutSector * cd::kSamplesPerSector;
    juce::String err;
    bool cancelled = false;

    auto pull = [&](int16_t* out, int64_t frames) -> bool {
        l.resize(size_t(frames));
        r.resize(size_t(frames));
        if (!ms.pull(l.data(), r.data(), int(frames), err)) return false;
        qr.feed(l.data(), r.data(), int(frames), pos);
        const float* ch[2] = {l.data(), r.data()};
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
    header << "Dither:   TPDF" << (es.shape == dsp::Ditherer::Shape::None ? "" : es.shape == dsp::Ditherer::Shape::Light ? " + light shaping" : " + strong shaping") << "\n";
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
    dither.prepare(2, 16, es.shape);
    juce::WavAudioFormat wav;
    const juce::int64 total = disc.leadOutSector * cd::kSamplesPerSector;
    juce::int64 pos = 0;
    std::vector<float> l(4096), r(4096);
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
            const float* ch[2] = {l.data(), r.data()};
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
            const float* ch[2] = {l.data(), r.data()};
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
    juce::AudioBuffer<float> buf(2, 4096);
    dsp::Ditherer dither;
    if (bits < 32) dither.prepare(2, bits, es.shape);
    std::vector<int32_t> q(4096 * 2);
    juce::String err;
    for (juce::int64 pos = 0; pos < total;) {
        int n = int(juce::jmin<juce::int64>(4096, total - pos));
        if (!ms.pull(buf.getWritePointer(0), buf.getWritePointer(1), n, err)) return err;
        if (bits < 32) {
            // Dither to the target word length, then hand JUCE exact values.
            const float* ch[2] = {buf.getReadPointer(0), buf.getReadPointer(1)};
            dither.process(ch, n, q.data());
            const float scale = 1.0f / float(1 << (bits - 1));
            for (int i = 0; i < n; ++i) {
                buf.setSample(0, i, float(q[size_t(i * 2)]) * scale);
                buf.setSample(1, i, float(q[size_t(i * 2 + 1)]) * scale);
            }
        }
        if (!w->writeFromAudioSampleBuffer(buf, 0, n)) return "Disk write failed.";
        pos += n;
        if (!keep(double(pos) / double(total))) return "Export cancelled.";
    }
    return {};
}

juce::String Exporter::analyse(const Session& s, const ExportSettings& es, const Keep& keep, QcReport& qc) {
    MasterStream ms(engine_, s, es, cd::kSampleRate);
    QcRunner qr(s, cd::kSampleRate);
    const juce::int64 total = juce::int64(std::ceil(s.end() * cd::kSampleRate));
    std::vector<float> l(4096), r(4096);
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

}  // namespace lathe
