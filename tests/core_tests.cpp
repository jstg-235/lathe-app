// Core unit tests: run with `ctest` or directly. No framework, so they build anywhere.
#include "../core/cd/CdTextEncoder.h"
#include "../core/cd/Disc.h"
#include "../core/ddp/Ddp.h"
#include "../core/dsp/Analyzer.h"
#include "../core/dsp/Meters.h"
#include "../core/util/Hash.h"

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

int main() {
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
