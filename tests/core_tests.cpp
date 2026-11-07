// Core unit tests: run with `ctest` or directly. No framework, so they build anywhere.
#include "../core/cd/CdTextEncoder.h"
#include "../core/cd/Disc.h"
#include "../core/ddp/Ddp.h"
#include "../core/dsp/Analyzer.h"
#include "../core/dsp/Meters.h"
#include "../core/dsp/Restoration.h"
#include "../core/util/Hash.h"
#include "../core/doc/Pdf.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lathe;
namespace fs = std::filesystem;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::abs(double(a) - double(b)) <= (tol))

static cd::Disc sampleDisc() {
    cd::Disc d;
    d.upcEan = "0123456789012";
    d.masterId = "LATHE TEST";
    d.text.title = "Test Album";
    d.text.performer = "The Testers";
    cd::Track t1;
    t1.indexes = {{0, 0}, {1, 150}};
    t1.isrc = "USABC2600001";
    t1.text.title = "First";
    cd::Track t2;
    t2.indexes = {{0, 150 + 75 * 10}, {1, 150 + 75 * 12}, {2, 150 + 75 * 15}};
    t2.isrc = "USABC2600002";
    t2.copyPermitted = true;
    t2.text.title = "Second Song — Café";   // em dash and é: must map to Latin-1
    d.tracks = {t1, t2};
    d.leadOutSector = 150 + 75 * 22;
    return d;
}

static void testMsf() {
    CHECK(cd::Msf::fromSectors(0).str() == "00:00:00");
    CHECK(cd::Msf::fromSectors(150).str() == "00:02:00");
    CHECK(cd::Msf::fromSectors(75 * 61 + 74).str() == "01:01:74");
    CHECK(cd::Msf::fromSectors(cd::kMaxSectors).str() == "99:59:74");
    CHECK((cd::Msf{1, 1, 74}).toSectors() == 75 * 61 + 74);
    CHECK(cd::samplesToSectorsCeil(588) == 1);
    CHECK(cd::samplesToSectorsCeil(589) == 2);
}

static void testValidation() {
    auto d = sampleDisc();
    CHECK(!cd::hasErrors(cd::validate(d)));

    auto shortT = d;
    shortT.leadOutSector = shortT.tracks[1].startSector() + 75 * 3;  // 3 s last track
    CHECK(cd::hasErrors(cd::validate(shortT)));

    auto noPregap = d;
    noPregap.tracks[0].indexes = {{0, 0}, {1, 100}};
    CHECK(cd::hasErrors(cd::validate(noPregap)));

    auto badIsrc = d;
    badIsrc.tracks[0].isrc = "US-ABC-26";
    CHECK(cd::hasErrors(cd::validate(badIsrc)));

    auto dcpScms = d;
    dcpScms.tracks[1].scms = true;
    CHECK(cd::hasErrors(cd::validate(dcpScms)));

    CHECK(cd::isValidIsrc("GBAYE0000351"));
    CHECK(cd::normaliseIsrc("us-abc-26-00001") == "USABC2600001");
    bool ok = false;
    CHECK(cd::isValidUpcEan("4006381333931", &ok) && ok);   // valid EAN-13
    CHECK(cd::isValidUpcEan("036000291452", &ok) && ok);    // valid UPC-A
    CHECK(cd::isValidUpcEan("4006381333932", &ok) && !ok);  // wrong check digit
}

static void testHashes() {
    util::Md5 m;
    m.update("", 0);
    CHECK(m.hexDigest() == "d41d8cd98f00b204e9800998ecf8427e");
    m.reset();
    std::string q = "The quick brown fox jumps over the lazy dog";
    m.update(q.data(), q.size());
    CHECK(m.hexDigest() == "9e107d9d372bb6826bd81d3542a419d6");
    m.reset();
    std::string big(1000001, 'a');   // crosses many block boundaries in odd chunk sizes
    for (size_t i = 0; i < big.size(); i += 777) m.update(big.data() + i, std::min<size_t>(777, big.size() - i));
    util::Md5 m2;
    m2.update(big.data(), big.size());
    CHECK(m.hexDigest() == m2.hexDigest());

    util::Crc32 c;
    c.update("123456789", 9);
    CHECK(c.value() == 0xCBF43926u);
    CHECK(util::crc16Ccitt(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x31C3);
}

static void testCdText() {
    auto d = sampleDisc();
    auto r = cd::encodeCdText(d);
    CHECK(r.error.empty());
    CHECK(r.packs.size() % 18 == 0);
    CHECK(!r.packs.empty());
    // Every pack's CRC must verify; sequence numbers count up from 0.
    bool crcOk = true, seqOk = true;
    for (size_t i = 0; i < r.packs.size() / 18; ++i) {
        auto* p = r.packs.data() + i * 18;
        uint16_t crc = uint16_t(~util::crc16Ccitt(p, 16));
        crcOk &= p[16] == (crc >> 8) && p[17] == (crc & 0xFF);
        seqOk &= p[2] == i;
    }
    CHECK(crcOk);
    CHECK(seqOk);
    CHECK(r.packs[r.packs.size() - 18] == 0x8F);   // size info last
    // Character outside Latin-1 is refused with a clear message.
    auto bad = d;
    bad.tracks[0].text.title = "東京";
    CHECK(!cd::encodeCdText(bad).error.empty());
}

static void testDdpRoundTrip() {
    auto d = sampleDisc();
    fs::path dir = fs::temp_directory_path() / "lathe_ddp_test";
    fs::remove_all(dir);

    // Deterministic ramp so the image content can be checked byte-for-byte.
    int64_t sample = 0;
    auto pull = [&](int16_t* buf, int64_t frames) {
        for (int64_t i = 0; i < frames; ++i, ++sample) {
            buf[i * 2] = int16_t(sample & 0x7FFF);
            buf[i * 2 + 1] = int16_t(-(sample & 0x7FFF));
        }
        return true;
    };
    auto w = ddp::writeDdp(d, dir.string(), pull);
    if (!w.ok) std::printf("write error: %s\n", w.error.c_str());
    CHECK(w.ok);
    CHECK(fs::file_size(dir / "DDPID") == 128);
    CHECK(fs::file_size(dir / "DDPMS") == 128 * 3);
    CHECK(fs::file_size(dir / "IMAGE.DAT") == uint64_t(d.leadOutSector) * 2352);
    CHECK(fs::file_size(dir / "PQDESCR") % 64 == 0);

    // Field positions from the published layout.
    std::ifstream f(dir / "DDPMS", std::ios::binary);
    std::string ms((std::istreambuf_iterator<char>(f)), {});
    CHECK(ms.substr(0, 6) == "VVVMS0");
    CHECK(ms.substr(30, 8) == "CDTEXT  ");
    CHECK(ms.substr(256 + 4, 2) == "D0");
    CHECK(ms.substr(256 + 38, 4) == "DA70");
    CHECK(ms.substr(256 + 46, 4) == " 150");
    CHECK(ms.substr(256 + 74, 9) == "IMAGE.DAT");

    std::ifstream pqf(dir / "PQDESCR", std::ios::binary);
    std::string pq((std::istreambuf_iterator<char>(pqf)), {});
    CHECK(pq.substr(0, 18) == "VVVS0000  00000001");
    CHECK(pq.substr(64, 18) == "VVVS0100  00000001");   // track 1 index 0
    CHECK(pq.substr(64 + 20, 12) == "USABC2600001");     // ISRC rides on the track's first packet (index 00)
    CHECK(pq.substr(128 + 20, 12) == std::string(12, ' '));

    auto r = ddp::readDdp(dir.string());
    if (!r.ok) std::printf("read error: %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(r.checksumsChecked && r.checksumsOk);
    CHECK(r.disc.tracks.size() == 2);
    CHECK(r.disc.upcEan == d.upcEan);
    CHECK(r.disc.leadOutSector == d.leadOutSector);
    CHECK(r.disc.tracks[1].isrc == "USABC2600002");
    CHECK(r.disc.tracks[1].copyPermitted);
    CHECK(r.disc.tracks[1].indexes.size() == 3);
    CHECK(r.disc.tracks[1].startSector() == d.tracks[1].startSector());
    CHECK(r.disc.text.title == "Test Album");
    CHECK(r.disc.tracks[1].text.title == "Second Song - Café");

    // Corrupt one byte of audio: verification must catch it.
    {
        std::fstream img(dir / "IMAGE.DAT", std::ios::binary | std::ios::in | std::ios::out);
        img.seekp(5000);
        img.put('\x55');
    }
    auto r2 = ddp::readDdp(dir.string());
    CHECK(r2.ok && !r2.checksumsOk);

    // A disc with errors must refuse to export.
    auto bad = d;
    bad.tracks[0].indexes = {{1, 0}};
    sample = 0;
    CHECK(!ddp::writeDdp(bad, (dir / "bad").string(), pull).ok);
    fs::remove_all(dir);
}

static void testLoudness() {
    // EBU Tech 3341: a 1 kHz sine at -23 dBFS peak in both channels reads -23.0 LUFS.
    const double fs = 48000;
    dsp::LoudnessMeter m;
    m.prepare(fs, 2);
    const int n = int(fs * 20);
    std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
    double a = std::pow(10.0, -23.0 / 20.0);
    for (int i = 0; i < n; ++i) l[size_t(i)] = r[size_t(i)] = float(a * std::sin(2 * M_PI * 1000 * i / fs));
    const float* ch[2] = {l.data(), r.data()};
    for (int off = 0; off < n; off += 512) {
        const float* p[2] = {ch[0] + off, ch[1] + off};
        m.process(p, std::min(512, n - off));
    }
    NEAR(m.integratedLufs(), -23.0, 0.1);
    NEAR(m.shortTermLufs(), -23.0, 0.1);
    NEAR(m.momentaryLufs(), -23.0, 0.1);
    CHECK(m.loudnessRange() < 0.2);

    // 44.1 kHz too (re-derived filter coefficients).
    dsp::LoudnessMeter m44;
    m44.prepare(44100, 2);
    std::vector<float> s(44100 * 10);
    for (size_t i = 0; i < s.size(); ++i) s[i] = float(a * std::sin(2 * M_PI * 1000 * double(i) / 44100));
    const float* p44[2] = {s.data(), s.data()};
    m44.process(p44, int(s.size()));
    NEAR(m44.integratedLufs(), -23.0, 0.1);
}

static void testTruePeak() {
    // fs/4 sine with 45° phase: samples peak at 0.707, true peak is 1.0 (+3 dB over sample peak).
    dsp::TruePeak tp;
    tp.prepare(44100);
    std::vector<float> x(4410);
    for (size_t i = 0; i < x.size(); ++i) x[i] = float(0.5 * std::sin(M_PI / 2 * double(i) + M_PI / 4));
    tp.process(x.data(), int(x.size()));
    double db = 20 * std::log10(tp.maxLinear() / 0.5);
    NEAR(db, 0.0, 0.5);   // within half a dB of the real peak

    // Below that, a plain sample-peak meter would read 3 dB low.
    float samplePeak = 0;
    for (float v : x) samplePeak = std::max(samplePeak, std::abs(v));
    CHECK(20 * std::log10(samplePeak / 0.5) < -2.5);
}

static void testDither() {
    dsp::Ditherer d;
    d.prepare(2, 16, dsp::Ditherer::Shape::None);
    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = float(0.25 * std::sin(2 * M_PI * 997 * double(i) / 44100));
    std::vector<int16_t> out(l.size() * 2);
    const float* p[2] = {l.data(), r.data()};
    d.process16(p, int(l.size()), out.data());
    double errSq = 0, errMean = 0;
    for (size_t i = 0; i < l.size(); ++i) {
        double e = out[i * 2] - l[i] * 32768.0;
        errSq += e * e;
        errMean += e;
    }
    errSq /= double(l.size());
    errMean /= double(l.size());
    NEAR(errMean, 0.0, 0.05);   // unbiased
    NEAR(errSq, 0.25, 0.08);    // TPDF + rounding: 1/12 + 2/12 ≈ 0.25 LSB²

    // Already-16-bit audio and digital silence pass through untouched (every shape).
    for (auto sh : {dsp::Ditherer::Shape::None, dsp::Ditherer::Shape::Psycho}) {
        dsp::Ditherer d2;
        d2.prepare(2, 16, sh, 7, 44100);
        std::vector<float> a16(4096), b16(4096);
        for (size_t i = 0; i < a16.size(); ++i) { a16[i] = float(int((i * 37) % 2001) - 1000) / 32768.0f; b16[i] = 0.0f; }
        const float* p16[2] = {a16.data(), b16.data()};
        std::vector<int16_t> o16(8192);
        d2.process16(p16, 4096, o16.data());
        bool exact = true;
        for (size_t i = 0; i < a16.size(); ++i) exact &= o16[i * 2] == int(std::lround(a16[i] * 32768.0)) && o16[i * 2 + 1] == 0;
        CHECK(exact);
    }

    // Full-scale input must clamp, never wrap.
    std::vector<float> hot(64, 1.5f), neg(64, -1.5f);
    const float* hp[2] = {hot.data(), neg.data()};
    std::vector<int16_t> o(128);
    d.process16(hp, 64, o.data());
    CHECK(o[0] == 32767 && o[1] == -32768);
}

// Noise power of the requantisation error in the band below ~4 kHz, via a 4th-order lowpass.
static double lowBandNoise(dsp::Ditherer::Shape shape) {
    dsp::Ditherer d;
    d.prepare(1, 16, shape, 12345, 44100);
    const int n = 1 << 17;
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[size_t(i)] = 0.001 * std::sin(2 * M_PI * 1000.0 * i / 44100.0);   // -60 dBFS tone
    std::vector<int32_t> q(static_cast<size_t>(n));
    const double* ch[1] = {x.data()};
    d.process(ch, n, q.data());
    // Butterworth-ish lowpass at 4 kHz (two cascaded 2nd-order sections, bilinear).
    auto lp = [](std::vector<double>& s) {
        const double K = std::tan(M_PI * 4000.0 / 44100.0), Q = 0.7071;
        const double a0 = 1 + K / Q + K * K;
        const double b0 = K * K / a0, b1 = 2 * b0, b2 = b0, a1 = 2 * (K * K - 1) / a0, a2 = (1 - K / Q + K * K) / a0;
        double z1 = 0, z2 = 0;
        for (auto& v : s) { double y = b0 * v + z1; z1 = b1 * v - a1 * y + z2; z2 = b2 * v - a2 * y; v = y; }
    };
    std::vector<double> e(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) e[size_t(i)] = double(q[size_t(i)]) - x[size_t(i)] * 32768.0;
    lp(e); lp(e);
    double p = 0;
    for (int i = 4096; i < n; ++i) p += e[size_t(i)] * e[size_t(i)];
    return p / (n - 4096);
}

static void testNoiseShaping() {
    double flat = lowBandNoise(dsp::Ditherer::Shape::None);
    double light = lowBandNoise(dsp::Ditherer::Shape::Light);
    double medium = lowBandNoise(dsp::Ditherer::Shape::Medium);
    double psycho = lowBandNoise(dsp::Ditherer::Shape::Psycho);
    auto db = [&](double v) { return 10 * std::log10(v / flat); };
    std::printf("noise below 4 kHz vs flat TPDF: light %.1f dB, lipshitz %.1f dB, psycho %.1f dB\n", db(light), db(medium), db(psycho));
    CHECK(db(light) < -3);
    CHECK(db(medium) < -8);
    CHECK(db(psycho) < -8);
    // Stays bounded on a full-scale square wave (clipping must not make the 9-tap shaper ring).
    dsp::Ditherer d;
    d.prepare(1, 16, dsp::Ditherer::Shape::Psycho, 1, 44100);
    std::vector<double> sq(44100);
    for (size_t i = 0; i < sq.size(); ++i) sq[i] = ((i / 50) % 2) ? 0.999 : -0.999;
    std::vector<int32_t> out(sq.size());
    const double* ch[1] = {sq.data()};
    d.process(ch, int(sq.size()), out.data());
    double worst = 0;
    for (size_t i = 0; i < sq.size(); ++i) worst = std::max(worst, std::abs(out[i] - sq[i] * 32768.0));
    CHECK(worst < 64);   // within a few dozen LSB of the input even at the rails
}

static void testAnalyzer() {
    // Two 5 s tracks of a 1 kHz tone at -20 dBFS, with planted faults:
    //  2.000 s: 10 full-scale samples (clip + true-peak over)      track 1
    //  6.000 s: 50 ms of digital silence (dropout)                 track 2
    //  7-8 s:   right channel inverted (phase)                     track 2
    //  track 2: +1 % DC offset
    const double fs = 44100;
    const int n = int(fs * 10);
    std::vector<double> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
    const double a = std::pow(10.0, -20.0 / 20.0);
    for (int i = 0; i < n; ++i) {
        double t = i / fs, v = a * std::sin(2 * M_PI * 1000 * t);
        l[size_t(i)] = v;
        r[size_t(i)] = (t >= 7.0 && t < 8.0) ? -v : v;
        if (t >= 5.0) { l[size_t(i)] += 0.01; r[size_t(i)] += 0.01; }
    }
    for (int i = 0; i < 10; ++i) l[size_t(int(2.0 * fs) + i)] = r[size_t(int(2.0 * fs) + i)] = 1.0;
    for (int i = 0; i < int(0.05 * fs); ++i) l[size_t(int(6.0 * fs) + i)] = r[size_t(int(6.0 * fs) + i)] = 0.0;

    dsp::ProgramAnalyzer an;
    an.prepare(fs, {{0.0, 5.0}, {5.0, 10.0}});
    for (int off = 0; off < n; off += 1000) an.process(l.data() + off, r.data() + off, std::min(1000, n - off));
    auto res = an.finish();

    CHECK(res.tracks.size() == 2);
    NEAR(res.tracks[0].lufs, -20.0, 0.3);
    CHECK(std::abs(res.map.shortTerm.size() - 100.0) <= 1);
    NEAR(res.map.hopSec, 0.1, 1e-9);
    auto has = [&](dsp::Problem::Kind k, double at, int track) {
        for (auto& p : res.problems)
            if (p.kind == k && at >= p.start - 0.15 && at <= p.end + 0.15 && p.track == track) return true;
        return false;
    };
    CHECK(has(dsp::Problem::Clip, 2.0, 1));
    int clips = 0;
    for (auto& p : res.problems) clips += p.kind == dsp::Problem::Clip;
    CHECK(clips == 1);
    CHECK(has(dsp::Problem::TruePeak, 2.0, 1));
    CHECK(has(dsp::Problem::Dropout, 6.0, 2));
    CHECK(has(dsp::Problem::Phase, 7.5, 2));
    CHECK(has(dsp::Problem::DcOffset, 7.0, 2));
    bool spurious = false;   // nothing should be flagged in track 1 apart from the clip
    for (auto& p : res.problems) if (p.track == 1 && (p.start < 1.8 || p.start > 2.3)) spurious = true;
    CHECK(!spurious);
    NEAR(res.tracks[1].dcPercent, 1.0, 0.05);
    CHECK(res.tracks[1].minCorrelation < -0.9);
    NEAR(res.tracks[0].minCorrelation, 1.0, 0.01);
    std::printf("analyser: %d problems found\n%s", int(res.problems.size()), res.summary().c_str());
}

// Program-like test signal: a few non-harmonic partials with slow amplitude movement.
static std::vector<double> music(int n, double fs, unsigned seed) {
    std::vector<double> x(static_cast<size_t>(n));
    const double fr[5] = {221.0, 347.3, 523.1, 880.7, 1777.0};
    for (int i = 0; i < n; ++i) {
        double t = i / fs, v = 0;
        for (int k = 0; k < 5; ++k) v += (0.06 / (k + 1)) * (1.0 + 0.3 * std::sin(2 * M_PI * (0.3 + k * 0.17) * t + seed)) * std::sin(2 * M_PI * fr[k] * t + k);
        x[size_t(i)] = v;
    }
    return x;
}

static double rmsDiff(const std::vector<double>& a, const std::vector<double>& b, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(s / double(to - from));
}

static void testRestoration() {
    const double fs = 44100;
    const int n = int(fs * 4);

    // Declick: 40 clicks of 1-12 samples added to music; repair should take the error down a lot.
    {
        auto clean = music(n, fs, 1);
        dsp::Channels ch{clean, clean};
        std::srand(3);
        int planted = 0;
        for (int c = 0; c < 40; ++c) {
            int at = 4000 + (std::rand() % (n - 8000)), w = 1 + std::rand() % 12;
            double amp = (std::rand() % 2 ? 1 : -1) * (0.2 + 0.4 * (std::rand() % 100) / 100.0);
            for (int k = 0; k < w; ++k) { ch[0][size_t(at + k)] += amp * (1 - k / double(w)); ch[1][size_t(at + k)] += amp * 0.8 * (1 - k / double(w)); }
            ++planted;
        }
        double before = rmsDiff(ch[0], clean, 0, size_t(n));
        auto st = dsp::declick(ch, fs, 6, 1.0);
        double after = rmsDiff(ch[0], clean, 0, size_t(n));
        std::printf("declick: %d planted, %d repaired, error %.1f dB -> %.1f dB\n", planted, st.found,
                    20 * std::log10(before), 20 * std::log10(after + 1e-15));
        CHECK(st.found >= 35);
        CHECK(20 * std::log10(after / before) < -20);   // at least 20 dB less error
        // Clean audio is left alone.
        dsp::Channels c2{clean, clean};
        auto st2 = dsp::declick(c2, fs, 6, 1.0);
        CHECK(st2.found <= 2);
        CHECK(rmsDiff(c2[0], clean, 0, size_t(n)) < 1e-4);
    }

    // Denoise: white noise at -50 dBFS RMS over music; learn from a noise-only stretch.
    {
        auto clean = music(n, fs, 2);
        std::srand(11);
        std::vector<double> noiseL(static_cast<size_t>(n)), noiseR(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            noiseL[size_t(i)] = (std::rand() / double(RAND_MAX) - 0.5) * 0.0109;
            noiseR[size_t(i)] = (std::rand() / double(RAND_MAX) - 0.5) * 0.0109;
        }
        dsp::Channels noisy{clean, clean};
        for (int i = 0; i < n; ++i) { noisy[0][size_t(i)] += noiseL[size_t(i)]; noisy[1][size_t(i)] += noiseR[size_t(i)]; }
        dsp::Channels noiseOnly{std::vector<double>(noiseL.begin(), noiseL.begin() + n / 4), std::vector<double>(noiseR.begin(), noiseR.begin() + n / 4)};
        auto np = dsp::learnNoise(noiseOnly, fs);
        CHECK(np.valid());
        double before = rmsDiff(noisy[0], clean, 8192, size_t(n - 8192));
        dsp::denoise(noisy, fs, np, 18, 1.5);
        double after = rmsDiff(noisy[0], clean, 8192, size_t(n - 8192));
        std::printf("denoise: error %.1f dB -> %.1f dB\n", 20 * std::log10(before), 20 * std::log10(after));
        CHECK(20 * std::log10(after / before) < -6);
        // Music level preserved within 1 dB.
        double sc = 0, sp = 0;
        for (int i = 8192; i < n - 8192; ++i) { sc += clean[size_t(i)] * clean[size_t(i)]; sp += noisy[0][size_t(i)] * noisy[0][size_t(i)]; }
        CHECK(std::abs(10 * std::log10(sp / sc)) < 1.0);
    }

    // Hum: 60 Hz + harmonics, auto-detected and removed; music around it survives.
    {
        auto clean = music(n, fs, 3);
        dsp::Channels ch{clean, clean};
        for (int i = 0; i < n; ++i)
            for (int h = 1; h <= 4; ++h) for (auto& x : ch) x[size_t(i)] += (0.01 / h) * std::sin(2 * M_PI * 60.0 * h * i / fs);
        double before = rmsDiff(ch[0], clean, 0, size_t(n));
        double f0 = dsp::removeHum(ch, fs, 0, 8, 30);
        double after = rmsDiff(ch[0], clean, size_t(fs * 0.5), size_t(n - fs * 0.5));
        std::printf("hum: detected %.0f Hz, error %.1f dB -> %.1f dB\n", f0, 20 * std::log10(before), 20 * std::log10(after));
        CHECK(f0 == 60.0);
        CHECK(20 * std::log10(after / before) < -20);
        dsp::Channels quiet{clean, clean};
        CHECK(dsp::removeHum(quiet, fs, 0, 8, 30) == 0.0);   // no hum: nothing done
    }

    // Spectral attenuate: a 0.3 s 6 kHz tone burst removed; outside the box unchanged.
    {
        auto clean = music(n, fs, 4);
        dsp::Channels ch{clean, clean};
        for (int i = int(fs * 1.5); i < int(fs * 1.8); ++i) for (auto& x : ch) x[size_t(i)] += 0.05 * std::sin(2 * M_PI * 6000.0 * i / fs);
        dsp::Channels edited = ch;
        dsp::spectralEdit(edited, fs, 1.45, 1.85, 5500, 6500, dsp::SpectralMode::Attenuate, -60);
        double burstBefore = rmsDiff(ch[0], clean, size_t(fs * 1.55), size_t(fs * 1.75));
        double burstAfter = rmsDiff(edited[0], clean, size_t(fs * 1.55), size_t(fs * 1.75));
        std::printf("spectral attenuate: burst %.1f dB -> %.1f dB\n", 20 * std::log10(burstBefore), 20 * std::log10(burstAfter));
        CHECK(20 * std::log10(burstAfter / burstBefore) < -25);
        bool untouched = true;
        for (size_t i = 0; i < size_t(fs * 1.0); ++i) untouched &= edited[0][i] == ch[0][i];
        for (size_t i = size_t(fs * 2.5); i < size_t(n); ++i) untouched &= edited[0][i] == ch[0][i];
        CHECK(untouched);   // bit-exact outside the edited span
        // Heal does the same job without a hole.
        dsp::Channels healed = ch;
        dsp::spectralEdit(healed, fs, 1.45, 1.85, 5500, 6500, dsp::SpectralMode::Heal, 0);
        double healAfter = rmsDiff(healed[0], clean, size_t(fs * 1.55), size_t(fs * 1.75));
        std::printf("spectral heal: burst %.1f dB -> %.1f dB\n", 20 * std::log10(burstBefore), 20 * std::log10(healAfter));
        CHECK(20 * std::log10(healAfter / burstBefore) < -15);
        auto sg = dsp::spectrogram(ch, fs, 500);
        CHECK(!sg.db.empty() && sg.db[0].size() == size_t(sg.fftSize / 2 + 1));
    }

    // DC removal.
    {
        dsp::Channels ch{std::vector<double>(1000, 0.02), std::vector<double>(1000, -0.01)};
        dsp::removeDc(ch);
        CHECK(std::abs(ch[0][500]) < 1e-12 && std::abs(ch[1][10]) < 1e-12);
    }
}


static void testCdTextExtras() {
    using namespace lathe;
    cd::Disc d;
    d.upcEan = "0123456789012";
    d.text.title = "Album";
    d.discId = "CAT-001";
    d.genreCode = 14;
    d.genreText = "Modal";
    cd::Track t;
    t.indexes = {{0, 0}, {1, 150}};
    t.isrc = "USABC2600001";
    t.text.title = "Song";
    d.tracks.push_back(t);
    d.leadOutSector = 75 * 60;
    auto r = cd::encodeCdText(d);
    CHECK(r.error.empty() && r.packs.size() % 18 == 0);
    int n86 = 0, n87 = 0, n8e = 0;
    std::string s86, s8e;
    std::vector<uint8_t> g;
    for (size_t i = 0; i + 18 <= r.packs.size(); i += 18) {
        uint8_t type = r.packs[i];
        if (type == 0x86) { ++n86; for (int k = 4; k < 16; ++k) s86 += char(r.packs[i + k]); }
        if (type == 0x87) { ++n87; for (int k = 4; k < 16; ++k) g.push_back(r.packs[i + k]); }
        if (type == 0x8E) { ++n8e; for (int k = 4; k < 16; ++k) s8e += char(r.packs[i + k]); }
    }
    CHECK(n86 == 1 && s86.substr(0, 7) == "CAT-001");
    CHECK(n87 == 1 && g[0] == 0 && g[1] == 14 && std::string(g.begin() + 2, g.begin() + 7) == "Modal");
    CHECK(n8e == 3 && s8e.find("0123456789012") == 0 && s8e.find("USABC2600001") == 14);
    // Size pack counts the new types.
    size_t sizeAt = r.packs.size() - 3 * 18;
    CHECK(r.packs[sizeAt] == 0x8F && r.packs[sizeAt + 4 + 4 + 6] == 1 && r.packs[sizeAt + 4 + 4 + 7] == 1);
    CHECK(cd::genreName(14) == "Jazz" && cd::genreName(99).empty());
    d.codesInCdText = false;
    auto r2 = cd::encodeCdText(d);
    bool none = true;
    for (size_t i = 0; i < r2.packs.size(); i += 18) none = none && r2.packs[i] != 0x8E;
    CHECK(none);
}

static void testPdf() {
    using namespace lathe;
    doc::Pdf pdf;
    pdf.setInfo("PQ (Sheet)", "Studio");
    pdf.setFont(true, 12);
    pdf.text(40, 60, "Caf\xC3\xA9 \xE2\x80\x94 Side A (cont.)");
    CHECK(doc::Pdf::toWinAnsi("Caf\xC3\xA9 \xE2\x80\x94 \xE2\x84\x97") == std::string("Caf\xE9 \x97 (P)"));
    CHECK(std::abs(doc::Pdf::textWidth("AV", false, 10) - (6.67 + 6.67)) < 0.01);
    pdf.setFont(false, 10);
    CHECK(pdf.textWidth(pdf.fit("A very long title that will not fit", 60)) <= 60.0);
    auto lines = pdf.wrap("one two three four five six seven eight nine ten", 60);
    CHECK(lines.size() > 2);
    pdf.line(40, 70, 300, 70);
    pdf.rect(40, 80, 100, 20, true, true);
    pdf.newPage();
    pdf.text(40, 60, "Page two");
    CHECK(pdf.addJpeg({1, 2, 3}) == -1);
    auto bytes = pdf.finish();
    std::string s(bytes.begin(), bytes.end());
    CHECK(s.rfind("%PDF-1.4", 0) == 0 && s.find("/Count 2") != std::string::npos && s.find("(PQ \\(Sheet\\))") != std::string::npos);
    // Every xref offset must point at its object header.
    size_t sx = s.rfind("startxref");
    size_t xref = size_t(std::stoll(s.substr(sx + 10)));
    CHECK(s.compare(xref, 4, "xref") == 0);
    std::istringstream in(s.substr(xref));
    std::string word;
    int first = 0, count = 0;
    in >> word >> first >> count;
    bool ok = count > 5;
    for (int i = 0; i < count; ++i) {
        std::string off, gen, kind;
        in >> off >> gen >> kind;
        if (i == 0) continue;
        size_t o = size_t(std::stoll(off));
        ok = ok && s.compare(o, std::to_string(i).size() + 6, std::to_string(i) + " 0 obj") == 0;
    }
    CHECK(ok);
}

int main() {
    testRestoration();
    testAnalyzer();
    testNoiseShaping();
    testMsf();
    testValidation();
    testHashes();
    testCdText();
    testDdpRoundTrip();
    testLoudness();
    testTruePeak();
    testDither();
    testCdTextExtras();
    testPdf();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
