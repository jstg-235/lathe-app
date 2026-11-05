#include "NullTest.h"

#include <complex>

namespace lathe {

namespace {

using cplx = std::complex<double>;

void fft(std::vector<cplx>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * juce::MathConstants<double>::pi / double(len) * (inverse ? 1 : -1);
        cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                cplx u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= double(n);
}

// Mono (L+R) samples [start, start+n) of a file; zeros outside the file.
std::vector<double> readMono(juce::AudioFormatReader& r, juce::int64 start, int n) {
    std::vector<double> out(size_t(n), 0.0);
    juce::AudioBuffer<float> buf(2, n);
    buf.clear();
    juce::int64 a = juce::jmax<juce::int64>(0, start), b = juce::jmin<juce::int64>(r.lengthInSamples, start + n);
    if (b > a) {
        r.read(&buf, int(a - start), int(b - a), a, true, true);
        if (r.numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, n);
    }
    for (int i = 0; i < n; ++i) out[size_t(i)] = double(buf.getSample(0, i)) + double(buf.getSample(1, i));
    return out;
}

juce::String db(double lin) {
    if (lin <= 0) return "-inf dBFS";
    return juce::String(20.0 * std::log10(lin), 1) + " dBFS";
}

}  // namespace

NullResult nullTest(juce::AudioFormatManager& fm, const juce::File& fa, const juce::File& fb, const juce::File& diffFile,
                    const std::function<bool(double)>& progress) {
    NullResult r;
    std::unique_ptr<juce::AudioFormatReader> A(fm.createReaderFor(fa)), B(fm.createReaderFor(fb));
    if (!A || !B) { r.error = "Couldn't read " + (!A ? fa.getFileName() : fb.getFileName()); return r; }
    r.rateA = A->sampleRate;
    r.rateB = B->sampleRate;
    r.bitsA = int(A->bitsPerSample);
    r.bitsB = int(B->bitsPerSample);
    r.lengthA = A->lengthInSamples;
    r.lengthB = B->lengthInSamples;
    if (std::abs(r.rateA - r.rateB) > 0.5) {
        r.error = "The files are at different sample rates (" + juce::String(r.rateA) + " vs " + juce::String(r.rateB) +
                  " Hz). Render both at the same rate to compare them sample for sample.";
        return r;
    }

    // ---- Alignment by cross-correlation around the first loud passage of A.
    const int N = 1 << 17;
    const int maxLag = int(juce::jmin(2.0 * r.rateA, 65536.0));
    juce::int64 startA = 0;
    {
        juce::AudioBuffer<float> scan(2, 65536);
        const float thresh = juce::Decibels::decibelsToGain(-40.0f);
        bool found = false;
        for (juce::int64 p = 0; p < A->lengthInSamples && !found; p += 65536) {
            int n = int(juce::jmin<juce::int64>(65536, A->lengthInSamples - p));
            A->read(&scan, 0, n, p, true, true);
            for (int i = 0; i < n; ++i)
                if (std::abs(scan.getSample(0, i)) > thresh) { startA = juce::jmax<juce::int64>(0, p + i - juce::int64(r.rateA * 0.25)); found = true; break; }
        }
    }
    {
        auto a = readMono(*A, startA, N);
        auto b = readMono(*B, startA - maxLag, N + 2 * maxLag);
        size_t M = 1;
        while (M < a.size() + b.size()) M <<= 1;
        std::vector<cplx> fa2(M), fb2(M);
        for (size_t i = 0; i < a.size(); ++i) fa2[i] = a[i];
        for (size_t i = 0; i < b.size(); ++i) fb2[i] = b[i];
        fft(fa2, false);
        fft(fb2, false);
        for (size_t i = 0; i < M; ++i) fa2[i] = std::conj(fa2[i]) * fb2[i];
        fft(fa2, true);
        int best = maxLag;
        double bestV = 0;
        for (int k = 0; k <= 2 * maxLag; ++k) {
            double v = fa2[size_t(k)].real();
            if (std::abs(v) > std::abs(bestV)) { bestV = v; best = k; }
        }
        r.offset = best - maxLag;
        r.inverted = bestV < 0;
    }
    if (progress && !progress(0.05)) { r.error = "Cancelled."; return r; }

    // ---- Sample-by-sample comparison over the overlap.
    const juce::int64 from = juce::jmax<juce::int64>(0, -r.offset);
    const juce::int64 to = juce::jmin(r.lengthA, r.lengthB - r.offset);
    if (to <= from) { r.error = "The files don't overlap after alignment."; return r; }
    std::unique_ptr<juce::AudioFormatWriter> w;
    if (diffFile != juce::File()) {
        diffFile.deleteFile();
        juce::WavAudioFormat wav;
        if (auto os = diffFile.createOutputStream()) {
            w.reset(wav.createWriterFor(os.get(), r.rateA, 2, 32, {}, 0));
            if (w) os.release();
        }
    }
    const int blk = 65536;
    juce::AudioBuffer<float> ba(2, blk), bb(2, blk), bd(2, blk);
    double sumAA = 0, sumBB = 0, sumAB = 0, sumDD = 0;
    const double pol = r.inverted ? -1.0 : 1.0;
    for (juce::int64 p = from; p < to; p += blk) {
        int n = int(juce::jmin<juce::int64>(blk, to - p));
        A->read(&ba, 0, n, p, true, true);
        B->read(&bb, 0, n, p + r.offset, true, true);
        if (A->numChannels == 1) ba.copyFrom(1, 0, ba, 0, 0, n);
        if (B->numChannels == 1) bb.copyFrom(1, 0, bb, 0, 0, n);
        for (int ch = 0; ch < 2; ++ch) {
            const float* x = ba.getReadPointer(ch);
            const float* y = bb.getReadPointer(ch);
            float* d = bd.getWritePointer(ch);
            for (int i = 0; i < n; ++i) {
                double xa = x[i], yb = pol * y[i];
                double diff = xa - yb;
                d[i] = float(diff);
                sumAA += xa * xa;
                sumBB += yb * yb;
                sumAB += xa * yb;
                sumDD += diff * diff;
                r.peakA = juce::jmax(r.peakA, std::abs(xa));
                r.peakB = juce::jmax(r.peakB, std::abs(yb));
                r.residualPeak = juce::jmax(r.residualPeak, std::abs(diff));
                if (diff == 0.0) ++r.identical;
            }
        }
        if (w) w->writeFromAudioSampleBuffer(bd, 0, n);
        if (progress && !progress(0.05 + 0.95 * double(p - from + n) / double(to - from))) { r.error = "Cancelled."; return r; }
    }
    r.compared = to - from;
    const double count = double(r.compared) * 2.0;
    r.signalRms = std::sqrt(sumAA / count);
    r.residualRms = std::sqrt(sumDD / count);
    if (sumBB > 0) {
        double g = sumAB / sumBB;
        r.gainDiffDb = g > 0 ? 20.0 * std::log10(g) : 0.0;
        double res = juce::jmax(0.0, sumAA - 2 * g * sumAB + g * g * sumBB);
        r.residualRmsGainMatched = std::sqrt(res / count);
    }
    r.ok = true;
    return r;
}

juce::String NullResult::toText(const juce::String& nameA, const juce::String& nameB) const {
    juce::String s;
    if (!ok) return "Null test failed: " + error;
    s << "NULL TEST\n";
    s << "  A: " << nameA << "  (" << juce::String(rateA) << " Hz, " << bitsA << "-bit, " << juce::String(double(lengthA) / rateA, 2) << " s)\n";
    s << "  B: " << nameB << "  (" << juce::String(rateB) << " Hz, " << bitsB << "-bit, " << juce::String(double(lengthB) / rateB, 2) << " s)\n\n";
    s << "Alignment\n";
    s << "  Offset:    B is " << juce::String(std::abs(offset)) << " samples " << (offset >= 0 ? "later" : "earlier") << " than A ("
      << juce::String(1000.0 * double(std::abs(offset)) / rateA, 3) << " ms)\n";
    s << "  Polarity:  " << (inverted ? "INVERTED" : "same") << "\n";
    s << "  Compared:  " << juce::String(double(compared) / rateA, 2) << " s\n\n";
    s << "Levels\n";
    s << "  Peak A " << db(peakA) << ", peak B " << db(peakB) << "\n";
    s << "  Best gain match: B needs " << juce::String(gainDiffDb, 4) << " dB\n\n";
    s << "Residual (A - B)\n";
    s << "  RMS:   " << db(residualRms) << "   (null depth " << (signalRms > 0 && residualRms > 0 ? juce::String(20 * std::log10(residualRms / signalRms), 1) + " dB" : juce::String("total")) << ")\n";
    s << "  Peak:  " << db(residualPeak) << "\n";
    s << "  After gain match, RMS: " << db(residualRmsGainMatched) << "\n";
    s << "  Identical samples: " << juce::String(100.0 * double(identical) / juce::jmax(1.0, double(compared) * 2), 3) << " %\n\n";

    s << "Verdict\n";
    double resDb = residualRms > 0 ? 20 * std::log10(residualRms) : -999;
    if (residualPeak == 0.0) s << "  BIT-IDENTICAL. The two files contain exactly the same audio.\n";
    else if (resDb < -138) s << "  Identical within 24-bit precision. Any difference heard is not in the files.\n";
    else if (resDb < -90) s << "  Differ only at dither/word-length level (consistent with different dither or rounding).\n";
    else if (residualRmsGainMatched < residualRms * 0.1) s << "  Mostly a level difference of " << juce::String(gainDiffDb, 3) << " dB; after matching gain the residual is small.\n";
    else s << "  Audibly different processing (EQ, dynamics, SRC filter, timing or fades). Listen to the difference file.\n";
    if (std::abs(offset) > 0) s << "  Note: a pure delay was removed before comparing. Sub-sample timing differences (e.g. from SRC) won't null fully.\n";
    return s;
}

}  // namespace lathe
