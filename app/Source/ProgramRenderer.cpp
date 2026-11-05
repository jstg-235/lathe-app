#include "ProgramRenderer.h"

#include <CDSPResampler.h>

namespace lathe {

// One open source file, positioned and resampled to the programme rate.
class ClipStream {
public:
    ClipStream(juce::AudioFormatManager& fm, const Region& c, const EffectiveFades& fades, double targetRate, juce::int64 localStart)
        : clip_(c), fades_(fades), rate_(targetRate), local_(localStart) {
        reader_.reset(fm.createReaderFor(c.file));
        if (!reader_) return;
        ratio_ = c.fileRate / targetRate;
        srcPos_ = c.srcIn + juce::int64(std::floor(double(localStart) * ratio_));
        srcEnd_ = c.srcOut;
        if (std::abs(ratio_ - 1.0) > 1e-9)
            for (int ch = 0; ch < 2; ++ch)
                rs_[ch] = std::make_unique<r8b::CDSPResampler24>(c.fileRate, targetRate, kChunk);
        in_.setSize(2, kChunk);
        for (auto& v : pending_) v.reserve(size_t(kChunk * 8));
        lengthOut_ = juce::int64(std::floor(c.length() * targetRate));
        gain_ = juce::Decibels::decibelsToGain(c.gainDb);
    }

    bool ok() const { return reader_ != nullptr; }

    // Adds n frames of this clip, starting at the clip-local sample local_, into out.
    template <typename T>
    void addTo(juce::AudioBuffer<T>& out, int offset, int n) {
        while (int(pending_[0].size()) - pendingPos_ < n && !drained_) pull();
        const int avail = juce::jmin(n, int(pending_[0].size()) - pendingPos_);
        const double fadeIn = fades_.in * rate_, fadeOut = fades_.out * rate_;
        const bool env = !clip_.envelope.isEmpty();
        for (int i = 0; i < avail; ++i) {
            juce::int64 k = local_ + i;
            if (k >= lengthOut_) break;
            double g = gain_;
            if (env) g *= juce::Decibels::decibelsToGain(clip_.envelopeDb(double(k) / rate_), -150.0);
            if (fadeIn > 0 && double(k) < fadeIn) g *= fadeGain(fades_.inShape, double(k) / fadeIn);
            double toEnd = double(lengthOut_ - 1 - k);
            if (fadeOut > 0 && toEnd < fadeOut) g *= fadeGain(fades_.outShape, juce::jmax(0.0, toEnd) / fadeOut);
            for (int ch = 0; ch < 2; ++ch)
                out.getWritePointer(ch)[offset + i] += T(pending_[size_t(ch)][size_t(pendingPos_ + i)] * g);
        }
        pendingPos_ += avail;
        local_ += n;
        if (pendingPos_ > kChunk * 4) {   // compact occasionally
            for (auto& v : pending_) v.erase(v.begin(), v.begin() + pendingPos_);
            pendingPos_ = 0;
        }
    }

private:
    static constexpr int kChunk = 8192;

    void pull() {
        int n = kChunk;
        bool pastEnd = srcPos_ >= srcEnd_;
        if (!pastEnd) n = int(juce::jmin<juce::int64>(kChunk, srcEnd_ - srcPos_));
        in_.clear();
        if (!pastEnd) {
            reader_->read(&in_, 0, n, srcPos_, true, true);
            if (reader_->numChannels == 1) in_.copyFrom(1, 0, in_, 0, 0, n);
            srcPos_ += n;
        } else {
            n = kChunk;   // flush the resampler with silence
            if (++flushPasses_ > 64 || !rs_[0]) drained_ = true;
        }
        for (int ch = 0; ch < 2; ++ch) {
            auto* src = in_.getReadPointer(ch);
            dbl_.resize(size_t(n));
            for (int i = 0; i < n; ++i) dbl_[size_t(i)] = src[i];
            double* op = dbl_.data();
            int got = rs_[ch] ? rs_[ch]->process(dbl_.data(), n, op) : n;
            pending_[size_t(ch)].insert(pending_[size_t(ch)].end(), op, op + got);
        }
        if (pastEnd && !rs_[0]) drained_ = true;
    }

    Region clip_;
    EffectiveFades fades_;
    double rate_, ratio_ = 1.0, gain_ = 1.0;
    std::unique_ptr<juce::AudioFormatReader> reader_;
    std::unique_ptr<r8b::CDSPResampler24> rs_[2];
    juce::AudioBuffer<float> in_;
    std::vector<double> dbl_;
    std::vector<double> pending_[2];
    int pendingPos_ = 0;
    juce::int64 local_ = 0, srcPos_ = 0, srcEnd_ = 0, lengthOut_ = 0;
    int flushPasses_ = 0;
    bool drained_ = false;
};

ProgramRenderer::ProgramRenderer(juce::AudioFormatManager& f) : formats_(f) {}
ProgramRenderer::~ProgramRenderer() = default;

void ProgramRenderer::setSession(const Session& s, double targetRate) {
    regions_.clear();
    fades_.clear();
    for (int i = 0; i < s.regions.size(); ++i) {
        if (s.regions.getReference(i).mute) continue;
        regions_.add(s.regions.getReference(i));
        fades_.add(s.fadesFor(i));
    }
    rate_ = targetRate;
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
            st = std::make_unique<ClipStream>(formats_, regions_.getReference(i), fades_.getReference(i), rate_, from - starts_[i]);
            if (!st->ok()) {
                error_ = "Couldn't open " + regions_.getReference(i).file.getFullPathName();
                ok = false;
                st.reset();
                continue;
            }
        }
        int offset = int(from - a);
        int len = int(juce::jmin(b, ends_[i]) - from);
        if (out.getNumChannels() >= 2 && len > 0) st->addTo(out, start + offset, len);
    }
    pos_ = b;
    return ok;
}

}  // namespace lathe
