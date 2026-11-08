#include "ProgramRenderer.h"

#include <CDSPResampler.h>

namespace lathe {

// One open source file, positioned and resampled to the programme rate.
// Streams up to six channels: what it carries depends on the lane's format (mono: one channel,
// a stereo file averaged; stereo: two, a mono file doubled; 5.1: the file's six).
class ClipStream {
public:
    ClipStream(juce::AudioFormatManager& fm, const Region& c, const EffectiveFades& fades, double targetRate, juce::int64 localStart,
               RegionFxBank* fx, int laneFormat)
        : clip_(c), fades_(fades), rate_(targetRate), local_(localStart), format_(laneFormat) {
        reader_.reset(fm.createReaderFor(c.file));
        if (!reader_) return;
        fileCh_ = juce::jlimit(1, kMaxCh, int(reader_->numChannels));
        nCh_ = format_ == 1 ? 1 : format_ == 6 ? (fileCh_ >= 6 ? 6 : 2) : 2;
        if (fx && fx->hasEffects(c.id) && nCh_ == 2) {   // region effects are stereo
            fx_ = fx;
            fx_->reset(c.id);
            skip_ = fx_->latency(c.id);   // plugin delay compensation for this region
            fxBuf_.setSize(2, kChunk * 4);
        }
        ratio_ = c.fileRate / targetRate;
        srcPos_ = c.srcIn + juce::int64(std::floor(double(localStart) * ratio_));
        srcEnd_ = c.srcOut;
        if (std::abs(ratio_ - 1.0) > 1e-9)
            for (int ch = 0; ch < nCh_; ++ch)
                rs_[ch] = std::make_unique<r8b::CDSPResampler24>(c.fileRate, targetRate, kChunk);
        in_.setSize(juce::jmax(2, fileCh_), kChunk);
        for (auto& v : pending_) v.reserve(size_t(kChunk * 8));
        lengthOut_ = juce::int64(std::floor(c.length() * targetRate));
        gain_ = juce::Decibels::decibelsToGain(c.gainDb) * (c.invert ? -1.0 : 1.0);
        flushNeeded_ = (rs_[0] ? 64 : 0) + (skip_ + kChunk - 1) / kChunk;
    }

    bool ok() const { return reader_ != nullptr; }

    int channels() const { return nCh_; }

    // Adds n frames of this clip, starting at the clip-local sample local_, into out.
    // `map[c]` is the output channel for stream channel c (mono goes to every listed channel).
    template <typename T>
    void addTo(juce::AudioBuffer<T>& out, int offset, int n, const juce::Array<int>& map) {
        while (int(pending_[0].size()) - pendingPos_ < n && !drained_) pull();
        const int avail = juce::jmin(n, int(pending_[0].size()) - pendingPos_);
        const double lenSec = double(lengthOut_ - 1) / rate_;
        const bool env = !clip_.envelope.isEmpty();
        for (int i = 0; i < avail; ++i) {
            juce::int64 k = local_ + i;
            if (k >= lengthOut_) break;
            double g = gain_;
            if (env) g *= juce::Decibels::decibelsToGain(clip_.envelopeDb(double(k) / rate_), -150.0);
            g *= fades_.gainAt(double(k) / rate_, lenSec);
            for (int m = 0; m < map.size(); ++m) {
                const int src = juce::jmin(m, nCh_ - 1);
                out.getWritePointer(map[m])[offset + i] += T(pending_[size_t(src)][size_t(pendingPos_ + i)] * g);
            }
        }
        pendingPos_ += avail;
        local_ += n;
        if (pendingPos_ > kChunk * 4) {   // compact occasionally
            for (int ch = 0; ch < nCh_; ++ch) pending_[ch].erase(pending_[ch].begin(), pending_[ch].begin() + pendingPos_);
            pendingPos_ = 0;
        }
    }

private:
    static constexpr int kChunk = 8192;
    static constexpr int kMaxCh = 6;

    void pull() {
        int n = kChunk;
        bool pastEnd = srcPos_ >= srcEnd_;
        if (!pastEnd) n = int(juce::jmin<juce::int64>(kChunk, srcEnd_ - srcPos_));
        in_.clear();
        if (!pastEnd) {
            reader_->read(in_.getArrayOfWritePointers(), fileCh_, srcPos_, n);
            if (fileCh_ == 1) in_.copyFrom(1, 0, in_, 0, 0, n);
            if (nCh_ == 1 && fileCh_ >= 2) {   // a stereo file on a mono lane: average the pair
                in_.addFrom(0, 0, in_, 1, 0, n);
                in_.applyGain(0, 0, n, 0.5f);
            }
            srcPos_ += n;
        } else {
            n = kChunk;   // flush the resampler and plugin latency with silence
            if (++flushPasses_ > flushNeeded_) drained_ = true;
        }
        double* outs[kMaxCh] = {};
        int got = n;
        for (int ch = 0; ch < nCh_; ++ch) {
            auto* src = in_.getReadPointer(ch);
            dbl_[ch].resize(size_t(n));
            for (int i = 0; i < n; ++i) dbl_[ch][size_t(i)] = src[i];
            outs[ch] = dbl_[ch].data();
            if (rs_[ch]) got = rs_[ch]->process(dbl_[ch].data(), n, outs[ch]);
        }
        if (fx_ && got > 0) {
            // Region effects run at the programme rate, before gain, envelope and fades.
            if (fxBuf_.getNumSamples() < got) fxBuf_.setSize(2, got);
            for (int ch = 0; ch < 2; ++ch) std::copy(outs[ch], outs[ch] + got, fxBuf_.getWritePointer(ch));
            fx_->process(clip_.id, fxBuf_, got);
            for (int ch = 0; ch < 2; ++ch) outs[ch] = fxBuf_.getWritePointer(ch);
        }
        int drop = juce::jmin(skip_, got);
        skip_ -= drop;
        for (int ch = 0; ch < nCh_; ++ch)
            pending_[size_t(ch)].insert(pending_[size_t(ch)].end(), outs[ch] + drop, outs[ch] + got);
    }

    Region clip_;
    EffectiveFades fades_;
    double rate_, ratio_ = 1.0, gain_ = 1.0;
    int format_ = 2, fileCh_ = 2, nCh_ = 2;
    std::unique_ptr<juce::AudioFormatReader> reader_;
    std::unique_ptr<r8b::CDSPResampler24> rs_[kMaxCh];
    juce::AudioBuffer<float> in_;
    std::vector<double> dbl_[kMaxCh];
    std::vector<double> pending_[kMaxCh];
    RegionFxBank* fx_ = nullptr;
    juce::AudioBuffer<double> fxBuf_;
    int skip_ = 0, flushNeeded_ = 0;
    int pendingPos_ = 0;
    juce::int64 local_ = 0, srcPos_ = 0, srcEnd_ = 0, lengthOut_ = 0;
    int flushPasses_ = 0;
    bool drained_ = false;
};

ProgramRenderer::ProgramRenderer(juce::AudioFormatManager& f) : formats_(f) {}
ProgramRenderer::~ProgramRenderer() = default;

void ProgramRenderer::setSession(const Session& s, double targetRate, bool offline) {
    if (fx_) fx_->prepare(targetRate, 2048, offline);
    regions_.clear();
    fades_.clear();
    for (int i = 0; i < s.regions.size(); ++i) {
        if (s.regions.getReference(i).mute) continue;
        regions_.add(s.regions.getReference(i));
        fades_.add(s.fadesFor(i));
    }
    rate_ = targetRate;
    channels_ = s.programmeChannels();
    laneFormats_.clear();
    for (auto& r : regions_) laneFormats_.add(s.laneFormat(r.lane));
    gain_.clear();
    if (s.gainActive()) gain_ = s.gainNodes;
    starts_.clear();
    ends_.clear();
    for (auto& r : regions_) {
        auto st = juce::int64(std::llround(r.start * rate_));
        starts_.add(st);
        ends_.add(st + juce::int64(std::floor(r.length() * rate_)));
    }
    total_ = juce::int64(std::ceil(s.end() * rate_));
    streams_.clear();
    streams_.resize(size_t(regions_.size()));
    seek(0);
}

void ProgramRenderer::seek(juce::int64 p) {
    pos_ = juce::jlimit<juce::int64>(0, juce::jmax<juce::int64>(0, total_), p);
    for (auto& s : streams_) s.reset();
}

bool ProgramRenderer::render(juce::AudioBuffer<float>& out, int start, int n) { return renderT(out, start, n); }
bool ProgramRenderer::render(juce::AudioBuffer<double>& out, int start, int n) { return renderT(out, start, n); }

template <typename T>
bool ProgramRenderer::renderT(juce::AudioBuffer<T>& out, int start, int n) {
    for (int ch = 0; ch < out.getNumChannels(); ++ch) out.clear(ch, start, n);
    // Mix straight into `out` when it has the programme's channels; otherwise mix the programme
    // in a scratch buffer and fold it down (5.1 heard or written as stereo).
    const bool direct = out.getNumChannels() >= channels_;
    if (!direct && (mix_.getNumChannels() < channels_ || mix_.getNumSamples() < n)) mix_.setSize(channels_, n, false, false, true);
    if (!direct) mix_.clear(0, n);
    bool ok = true;
    const juce::int64 a = pos_, b = pos_ + n;
    for (int i = 0; i < regions_.size(); ++i) {
        if (ends_[i] <= a || starts_[i] >= b) {
            if (ends_[i] <= a) streams_[size_t(i)].reset();   // finished: release the file
            continue;
        }
        auto& st = streams_[size_t(i)];
        juce::int64 from = juce::jmax(a, starts_[i]);
        if (!st) {
            st = std::make_unique<ClipStream>(formats_, regions_.getReference(i), fades_.getReference(i), rate_, from - starts_[i], fx_,
                                              laneFormats_[i]);
            if (!st->ok()) {
                error_ = "Couldn't open " + regions_.getReference(i).file.getFullPathName();
                ok = false;
                st.reset();
                continue;
            }
        }
        int offset = int(from - a);
        int len = int(juce::jmin(b, ends_[i]) - from);
        if (len <= 0) continue;
        // Where this clip's channels go in the programme.
        juce::Array<int> map;
        if (st->channels() == 6 && channels_ == 6) map = {0, 1, 2, 3, 4, 5};
        else if (st->channels() == 1 && channels_ == 6 && laneFormats_[i] == 1) map = {2};   // a mono lane in 5.1: centre
        else map = {0, 1};
        if (direct) {
            if (out.getNumChannels() >= 2) st->addTo(out, start + offset, len, map);
        } else {
            st->addTo(mix_, offset, len, map);
        }
    }
    if (!direct) {
        // ITU-R BS.775 fold-down: C and the surrounds at -3 dB into the fronts, LFE left out.
        const double k = 0.7071067811865476;
        for (int ch = 0; ch < juce::jmin(2, out.getNumChannels()); ++ch) {
            const double* f = mix_.getReadPointer(ch);
            const double* c = mix_.getReadPointer(2);
            const double* sr = mix_.getReadPointer(ch == 0 ? 4 : 5);
            T* d = out.getWritePointer(ch, start);
            for (int i = 0; i < n; ++i) d[i] = T(f[i] + k * c[i] + k * sr[i]);
        }
    }
    if (!gain_.isEmpty()) {
        // Timeline gain, straight lines in dB between nodes.
        int k = 0;
        for (int i = 0; i < n; ++i) {
            const double t = double(a + i) / rate_;
            double db;
            if (t <= gain_.getFirst().t) db = gain_.getFirst().db;
            else if (t >= gain_.getLast().t) db = gain_.getLast().db;
            else {
                while (k + 1 < gain_.size() && gain_.getReference(k + 1).t < t) ++k;
                const auto& x = gain_.getReference(k);
                const auto& y = gain_.getReference(juce::jmin(k + 1, gain_.size() - 1));
                db = y.t > x.t ? x.db + (y.db - x.db) * (t - x.t) / (y.t - x.t) : y.db;
            }
            if (db == 0.0) continue;
            const T g = T(std::pow(10.0, db / 20.0));
            for (int ch = 0; ch < out.getNumChannels(); ++ch) out.getWritePointer(ch)[start + i] *= g;
        }
    }
    pos_ = b;
    return ok;
}

}  // namespace lathe
