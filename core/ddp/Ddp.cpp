#include "Ddp.h"

#include "../cd/CdTextEncoder.h"
#include "../util/Hash.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace lathe::ddp {

namespace {

// Fixed-width text field: left-aligned, space padded, truncated to width.
std::string text(const std::string& s, size_t width) {
    std::string r = s.substr(0, width);
    r.resize(width, ' ');
    return r;
}

// Number field, right-aligned. zeroPad=true matches HOFA/Sonoris; false matches ddptools.
std::string num(int64_t v, size_t width, bool zeroPad = true) {
    std::string d = std::to_string(v);
    if (d.size() > width) d = d.substr(d.size() - width);
    return std::string(width - d.size(), zeroPad ? '0' : ' ') + d;
}

std::string blank(size_t width) { return std::string(width, ' '); }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

int64_t parseNum(const std::string& s) {
    std::string t = trim(s);
    if (t.empty()) return 0;
    try { return std::stoll(t); } catch (...) { return -1; }
}

bool writeFile(const fs::path& p, const std::string& data, std::string& md5, uint32_t& crc) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), std::streamsize(data.size()));
    util::Md5 m; m.update(data.data(), data.size()); md5 = m.hexDigest();
    util::Crc32 c; c.update(data.data(), data.size()); crc = c.value();
    return bool(f);
}

struct Pq {
    std::string tk, ix;
    int64_t sector;
    char control;
    char adr;      // '1' or 'S' (SCMS)
    std::string isrc, upc;
};

}  // namespace

std::string makeDdpId(const cd::Disc& d) {
    std::string r;
    r += "DDP 2.00";
    r += text(d.upcEan, 13);
    r += blank(8);              // MSS
    r += blank(8);              // MSL
    r += blank(1);              // MED
    r += text(d.masterId, 48);  // MID
    r += blank(1);              // BK
    r += "CD";                  // TYPE
    r += blank(4);              // NSIDE SIDE NLAYER LAYER
    r += blank(2);              // SIZ (no user text)
    r += blank(33);             // TXT
    return r;  // 128
}

std::string makeMapPacket(const std::string& dst, int64_t dsl, const std::string& sub, bool audio,
                          int64_t pregap, const std::string& fileName, bool cdtext) {
    std::string r;
    r += "VVVM";
    r += dst;                                   // DST
    r += blank(8);                              // DSP
    r += num(dsl, 8);                           // DSL
    r += audio ? num(0, 8) : blank(8);          // DSS
    r += text(sub, 8);                          // SUB
    r += audio ? "DA" : "  ";                   // CDM
    r += audio ? "7" : " ";                     // SSM
    r += audio ? "0" : " ";                     // SCR
    r += blank(4);                              // PRE1
    r += audio ? num(pregap, 4, false) : blank(4);  // PRE2
    r += blank(4);                              // PST
    r += blank(1);                              // MED
    r += cdtext ? "00" : "  ";                  // TRK
    r += blank(2);                              // IDX
    r += blank(12);                             // ISRC
    r += num(17, 3);                            // SIZ
    r += text(fileName, 17);                    // DSI
    r += blank(1);                              // NEW
    r += blank(4);                              // PRE1NXT
    r += blank(8);                              // PAUSEADD
    r += blank(9);                              // OFS
    r += blank(15);                             // PAD
    return r;  // 128
}

static std::vector<Pq> pqEntries(const cd::Disc& d) {
    std::vector<Pq> out;
    out.push_back({"00", "00", 0, '0', '1', "", d.upcEan});
    for (size_t ti = 0; ti < d.tracks.size(); ++ti) {
        const auto& t = d.tracks[ti];
        char tk[3];
        std::snprintf(tk, sizeof tk, "%02d", int(ti + 1));
        bool first = true;
        for (auto& ix : t.indexes) {
            char ixs[3];
            std::snprintf(ixs, sizeof ixs, "%02d", ix.number);
            out.push_back({tk, ixs, ix.sector, t.controlNibble(), t.scms ? 'S' : '1', first ? t.isrc : "", ""});
            first = false;
        }
    }
    Pq lo{"AA", "01", d.leadOutSector, '0', '1', "", ""};
    out.push_back(lo);
    out.push_back(lo);  // every studied writer repeats the lead-out packet
    return out;
}

std::string makePqDescriptor(const cd::Disc& d) {
    std::string r;
    for (auto& p : pqEntries(d)) {
        r += "VVVS";
        r += p.tk;
        r += p.ix;
        r += "  " + cd::Msf::fromSectors(p.sector).digits();  // 8-char A-Time
        r += p.control;
        r += p.adr;
        r += blank(2);
        r += text(p.isrc, 12);
        r += text(p.upc, 13);
        r += blank(19);
    }
    return r;
}

static std::string cueEscape(const std::string& s) {
    std::string r;
    for (char c : s) r += (c == '"') ? '\'' : c;
    return r;
}

std::string makeCueSheet(const cd::Disc& d, const std::string& imageName) {
    std::ostringstream o;
    auto textLines = [&](const cd::CdText& t, const char* indent) {
        auto line = [&](const char* k, const std::string& v) {
            std::string l;
            if (!v.empty() && cd::utf8ToLatin1(v, l)) o << indent << k << " \"" << cueEscape(v) << "\"\n";
        };
        line("TITLE", t.title);
        line("PERFORMER", t.performer);
        line("SONGWRITER", t.songwriter);
        line("COMPOSER", t.composer);
        line("ARRANGER", t.arranger);
        line("MESSAGE", t.message);
    };
    if (!d.upcEan.empty()) o << "CATALOG " << (d.upcEan.size() == 12 ? "0" + d.upcEan : d.upcEan) << "\n";
    textLines(d.text, "");
    o << "FILE \"" << imageName << "\" BINARY\n";
    for (size_t ti = 0; ti < d.tracks.size(); ++ti) {
        const auto& t = d.tracks[ti];
        char tk[8];
        std::snprintf(tk, sizeof tk, "%02d", int(ti + 1));
        o << "  TRACK " << tk << " AUDIO\n";
        textLines(t.text, "    ");
        if (!t.isrc.empty()) o << "    ISRC " << t.isrc << "\n";
        std::string flags;
        if (t.preEmphasis) flags += " PRE";
        if (t.copyPermitted) flags += " DCP";
        if (t.fourChannel) flags += " 4CH";
        if (t.scms) flags += " SCMS";
        if (!flags.empty()) o << "    FLAGS" << flags << "\n";
        for (auto& ix : t.indexes) {
            char b[64];
            std::snprintf(b, sizeof b, "    INDEX %02d %s\n", ix.number, cd::Msf::fromSectors(ix.sector).str().c_str());
            o << b;
        }
    }
    return o.str();
}

std::string makePqLog(const cd::Disc& d, const std::string& header) {
    std::ostringstream o;
    o << "LATHE PQ LOG\n";
    if (!header.empty()) o << header << (header.back() == '\n' ? "" : "\n");
    o << "\n";
    if (!d.text.title.empty()) o << "Title:      " << d.text.title << "\n";
    if (!d.text.performer.empty()) o << "Performer:  " << d.text.performer << "\n";
    o << "UPC/EAN:    " << (d.upcEan.empty() ? "(none)" : d.upcEan) << "\n";
    o << "Tracks:     " << d.tracks.size() << "\n";
    o << "Total time: " << cd::Msf::fromSectors(d.leadOutSector).str() << "  (" << d.leadOutSector << " sectors)\n\n";
    o << "TRK IDX   START      LENGTH     PAUSE    ISRC          FLAGS  TITLE / PERFORMER\n";
    o << "--- --- ---------- ---------- -------- ------------ ------ ------------------------------\n";
    char b[512];
    for (size_t ti = 0; ti < d.tracks.size(); ++ti) {
        const auto& t = d.tracks[ti];
        int64_t next = ti + 1 < d.tracks.size() ? d.tracks[ti + 1].firstSector() : d.leadOutSector;
        int64_t len = next - t.startSector();
        int64_t pause = t.startSector() - t.firstSector();
        std::string flags;
        flags += t.preEmphasis ? 'P' : '-';
        flags += t.copyPermitted ? 'C' : '-';
        flags += t.scms ? 'S' : '-';
        flags += t.fourChannel ? '4' : '-';
        std::string who = t.text.title;
        if (!t.text.performer.empty()) who += " / " + t.text.performer;
        for (auto& ix : t.indexes) {
            bool main = ix.number == 1;
            std::snprintf(b, sizeof b, "%3d  %02d  %s  %s  %s  %-12s  %-4s  %s\n", int(ti + 1), ix.number,
                          cd::Msf::fromSectors(ix.sector).str().c_str(),
                          main ? cd::Msf::fromSectors(len).str().c_str() : "        ",
                          main ? cd::Msf::fromSectors(pause).str().c_str() : "        ",
                          main ? t.isrc.c_str() : "", main ? flags.c_str() : "", main ? who.c_str() : "");
            o << b;
        }
    }
    std::snprintf(b, sizeof b, " AA  01  %s  (lead-out)\n", cd::Msf::fromSectors(d.leadOutSector).str().c_str());
    o << b;
    return o.str();
}

WriteResult writeDdp(const cd::Disc& disc, const std::string& directory, const AudioPull& audio,
                     const WriteOptions& opts, const Progress& progress) {
    WriteResult res;
    res.issues = cd::validate(disc);
    if (cd::hasErrors(res.issues)) {
        res.error = "The disc has errors that would be rejected by a plant; fix them before exporting.";
        return res;
    }

    cd::CdTextResult cdt = cd::encodeCdText(disc);
    if (!cdt.error.empty()) { res.error = cdt.error; return res; }

    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) { res.error = "Couldn't create the folder: " + ec.message(); return res; }
    fs::path dir(directory);

    struct Sum { std::string name, md5; uint32_t crc; };
    std::vector<Sum> sums;
    const bool hasText = !cdt.packs.empty();
    const std::string pq = makePqDescriptor(disc);

    std::string ms;
    if (hasText) ms += makeMapPacket("S0", int64_t(cdt.packs.size()), "CDTEXT", false, 0, "CDTEXT.BIN", true);
    ms += makeMapPacket("S0", int64_t(pq.size()), "PQ DESCR", false, 0, "PQDESCR", false);
    ms += makeMapPacket("D0", disc.leadOutSector, "", true, disc.tracks[0].startSector(), "IMAGE.DAT", false);

    auto put = [&](const std::string& name, const std::string& data) -> bool {
        Sum s{name, {}, 0};
        if (!writeFile(dir / name, data, s.md5, s.crc)) { res.error = "Couldn't write " + name; return false; }
        sums.push_back(s);
        res.files.push_back(name);
        return true;
    };

    if (!put("DDPID", makeDdpId(disc))) return res;
    if (!put("DDPMS", ms)) return res;
    if (hasText && !put("CDTEXT.BIN", std::string(cdt.packs.begin(), cdt.packs.end()))) return res;
    if (!put("PQDESCR", pq)) return res;

    // Audio image: stream sector-aligned chunks, hash as we go.
    {
        std::ofstream f(dir / "IMAGE.DAT", std::ios::binary | std::ios::trunc);
        if (!f) { res.error = "Couldn't write IMAGE.DAT"; return res; }
        util::Md5 md5; util::Crc32 crc;
        const int64_t chunkSectors = 75 * 4;  // 4 seconds at a time
        std::vector<int16_t> pcm(size_t(chunkSectors * cd::kSamplesPerSector * 2));
        std::vector<uint8_t> le(pcm.size() * 2);
        for (int64_t s = 0; s < disc.leadOutSector; s += chunkSectors) {
            int64_t n = std::min(chunkSectors, disc.leadOutSector - s);
            int64_t frames = n * cd::kSamplesPerSector;
            if (!audio(pcm.data(), frames)) { res.error = "Export cancelled or audio couldn't be read."; return res; }
            for (int64_t i = 0; i < frames * 2; ++i) {  // force little-endian regardless of host
                uint16_t v = uint16_t(pcm[size_t(i)]);
                le[size_t(i * 2)] = uint8_t(v & 0xFF);
                le[size_t(i * 2 + 1)] = uint8_t(v >> 8);
            }
            size_t bytes = size_t(frames * 4);
            f.write(reinterpret_cast<const char*>(le.data()), std::streamsize(bytes));
            if (!f) { res.error = "Disk write failed while writing IMAGE.DAT (disk full?)."; return res; }
            md5.update(le.data(), bytes);
            crc.update(le.data(), bytes);
            if (progress) progress(double(s + n) / double(disc.leadOutSector));
        }
        res.imageMd5 = md5.hexDigest();
        sums.push_back({"IMAGE.DAT", res.imageMd5, crc.value()});
        res.files.push_back("IMAGE.DAT");
    }

    // Companion files (not listed in DDPMS).
    {
        std::string md5txt, crctxt = "[CRC32 Checksum]\n";
        for (auto& s : sums) {
            md5txt += s.md5 + " *" + s.name + "\n";
            char b[16];
            std::snprintf(b, sizeof b, "%08X", s.crc);
            crctxt += s.name + "=" + b + "\n";
        }
        std::string dummy; uint32_t dc;
        if (!writeFile(dir / "CHECKSUM.MD5", md5txt, dummy, dc)) { res.error = "Couldn't write CHECKSUM.MD5"; return res; }
        if (!writeFile(dir / "CHECKSUM.TXT", crctxt, dummy, dc)) { res.error = "Couldn't write CHECKSUM.TXT"; return res; }
        res.files.push_back("CHECKSUM.MD5");
        res.files.push_back("CHECKSUM.TXT");
        if (opts.writeCueSheet && writeFile(dir / "IMAGE.cue", makeCueSheet(disc, "IMAGE.DAT"), dummy, dc))
            res.files.push_back("IMAGE.cue");
        if (opts.writePqLog && writeFile(dir / "PQ_LOG.TXT", makePqLog(disc, opts.pqLogHeader), dummy, dc))
            res.files.push_back("PQ_LOG.TXT");
    }

    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------------

namespace {

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// DDP file names are case-insensitive in practice; find the real one.
fs::path findCaseless(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string a = e.path().filename().string(), b = name;
        std::transform(a.begin(), a.end(), a.begin(), ::toupper);
        std::transform(b.begin(), b.end(), b.begin(), ::toupper);
        if (a == b) return e.path();
    }
    return {};
}

std::string hashFile(const fs::path& p, const Progress& progress, uint64_t& done, uint64_t total) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    util::Md5 m;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        auto n = f.gcount();
        if (n <= 0) break;
        m.update(buf.data(), size_t(n));
        done += uint64_t(n);
        if (progress && total) progress(double(done) / double(total));
    }
    return m.hexDigest();
}

// Decodes CDTEXT.BIN packs back into disc/track strings (block 0, Latin-1 → UTF-8).
void decodeCdText(const std::string& bin, cd::Disc& d, std::vector<std::string>& notes) {
    std::map<int, std::string> stream;  // type → concatenated payload
    size_t bad = 0;
    for (size_t off = 0; off + 18 <= bin.size(); off += 18) {
        auto* p = reinterpret_cast<const uint8_t*>(bin.data() + off);
        uint16_t crc = uint16_t(~util::crc16Ccitt(p, 16));
        if (p[16] != (crc >> 8) || p[17] != (crc & 0xFF)) ++bad;
        int type = p[0];
        if ((p[3] & 0x70) != 0) continue;  // only block 0
        if (type >= 0x80 && type <= 0x85) stream[type].append(reinterpret_cast<const char*>(p + 4), 12);
    }
    if (bad) notes.push_back("CD-Text: " + std::to_string(bad) + " pack(s) have a bad CRC.");
    auto toUtf8 = [](const std::string& l) {
        std::string u;
        for (unsigned char c : l) {
            if (c < 0x80) u += char(c);
            else { u += char(0xC0 | (c >> 6)); u += char(0x80 | (c & 0x3F)); }
        }
        return u;
    };
    for (auto& [type, s] : stream) {
        std::vector<std::string> strs;
        std::string cur;
        for (char c : s) {
            if (c == '\0') { strs.push_back(cur); cur.clear(); }
            else cur += c;
        }
        for (size_t i = 0; i < strs.size() && i <= d.tracks.size(); ++i) {
            std::string v = strs[i];
            if (v == "\t" && i > 1) v = strs[i - 1];
            cd::CdText& t = i == 0 ? d.text : d.tracks[i - 1].text;
            std::string& dst = type == 0x80 ? t.title : type == 0x81 ? t.performer : type == 0x82 ? t.songwriter
                             : type == 0x83 ? t.composer : type == 0x84 ? t.arranger : t.message;
            dst = toUtf8(v);
        }
    }
}

}  // namespace

ReadResult readDdp(const std::string& directory, const Progress& progress) {
    ReadResult r;
    fs::path dir(directory);
    std::string id, ms;
    auto idPath = findCaseless(dir, "DDPID"), msPath = findCaseless(dir, "DDPMS");
    if (idPath.empty() || !readAll(idPath, id)) { r.error = "No DDPID file in this folder; it isn't a DDP fileset."; return r; }
    if (msPath.empty() || !readAll(msPath, ms)) { r.error = "No DDPMS file in this folder; it isn't a DDP fileset."; return r; }
    if (id.size() < 128 || id.compare(0, 6, "DDP 2.") != 0) {
        r.error = "DDPID doesn't identify a DDP 2.0x fileset.";
        return r;
    }
    if (ms.size() % 128 != 0) r.notes.push_back("DDPMS isn't a whole number of 128-byte packets.");
    r.disc.upcEan = trim(id.substr(8, 13));
    r.disc.masterId = trim(id.substr(38, 48));

    fs::path pqPath, textPath;
    int64_t pregap = -1;
    for (size_t off = 0; off + 128 <= ms.size(); off += 128) {
        std::string p = ms.substr(off, 128);
        if (p.compare(0, 4, "VVVM") != 0) { r.notes.push_back("A DDPMS packet has no VVVM marker."); continue; }
        std::string dst = p.substr(4, 2), sub = trim(p.substr(30, 8)), cdm = p.substr(38, 2);
        std::string file = trim(p.substr(74, 17));
        fs::path fp = findCaseless(dir, file);
        if (fp.empty()) { r.error = "DDPMS lists " + file + " but it's missing from the folder."; return r; }
        if (dst == "S0" && sub == "PQ DESCR") pqPath = fp;
        else if (dst == "S0" && sub == "CDTEXT") textPath = fp;
        else if (dst == "D0" && cdm == "DA") {
            if (!r.imageFile.empty()) { r.error = "Multi-stream audio images aren't supported yet."; return r; }
            r.imageFile = fp.string();
            r.imageSectors = parseNum(p.substr(14, 8));
            pregap = parseNum(p.substr(46, 4));
            std::error_code ec;
            auto size = fs::file_size(fp, ec);
            if (!ec && int64_t(size) != r.imageSectors * cd::kBytesPerSector)
                r.notes.push_back("IMAGE size (" + std::to_string(size) + " bytes) doesn't match DDPMS length of " +
                                  std::to_string(r.imageSectors) + " sectors.");
        }
    }
    if (pqPath.empty()) { r.error = "No PQ descriptor stream listed in DDPMS."; return r; }
    if (r.imageFile.empty()) { r.error = "No audio (D0 DA) stream listed in DDPMS."; return r; }

    std::string pq;
    readAll(pqPath, pq);
    int leadOuts = 0;
    for (size_t off = 0; off + 64 <= pq.size(); off += 64) {
        std::string p = pq.substr(off, 64);
        if (p.compare(0, 4, "VVVS") != 0) { r.notes.push_back("A PQ packet has no VVVS marker."); continue; }
        std::string tk = p.substr(4, 2);
        int ix = int(parseNum(p.substr(6, 2)));
        std::string t = trim(p.substr(8, 8));
        while (t.size() < 6) t = "0" + t;
        cd::Msf msf{std::stoi(t.substr(0, t.size() - 4)), std::stoi(t.substr(t.size() - 4, 2)), std::stoi(t.substr(t.size() - 2))};
        int64_t sector = msf.toSectors();
        if (tk == "00") {
            std::string upc = trim(p.substr(32, 13));
            if (!upc.empty()) r.disc.upcEan = upc;
            continue;
        }
        if (tk == "AA") { r.disc.leadOutSector = sector; ++leadOuts; continue; }
        int tn = int(parseNum(tk));
        if (tn < 1 || tn > 99) { r.notes.push_back("Bad track number in PQ: " + tk); continue; }
        while (int(r.disc.tracks.size()) < tn) r.disc.tracks.emplace_back();
        cd::Track& tr = r.disc.tracks[size_t(tn - 1)];
        tr.indexes.push_back({ix, sector});
        char c = p[16], adr = p[17];
        int ctl = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        tr.preEmphasis = ctl & 1;
        tr.copyPermitted = ctl & 2;
        tr.fourChannel = ctl & 8;
        tr.scms = adr == 'S';
        std::string isrc = trim(p.substr(20, 12));
        if (!isrc.empty()) tr.isrc = isrc;
    }
    if (leadOuts == 0) { r.error = "PQ descriptor has no lead-out."; return r; }
    if (r.disc.leadOutSector != r.imageSectors)
        r.notes.push_back("Lead-out (" + std::to_string(r.disc.leadOutSector) + ") doesn't equal the image length (" +
                          std::to_string(r.imageSectors) + " sectors).");
    if (!r.disc.tracks.empty() && pregap >= 0 && pregap != r.disc.tracks[0].startSector())
        r.notes.push_back("DDPMS pregap doesn't match track 1's index 01.");

    if (!textPath.empty()) {
        std::string bin;
        readAll(textPath, bin);
        decodeCdText(bin, r.disc, r.notes);
    }

    for (auto& issue : cd::validate(r.disc))
        r.notes.push_back(std::string(issue.level == cd::Issue::Error ? "Error: " : "Warning: ") + issue.message);

    // Checksums: accept the common file names.
    fs::path sumPath;
    for (const char* n : {"CHECKSUM.MD5", "MD5_CHECKSUM.MD5", "MD5-Checksum.md5", "MD5.MD5", "CHECKSUMS.MD5"}) {
        sumPath = findCaseless(dir, n);
        if (!sumPath.empty()) break;
    }
    if (sumPath.empty()) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".md5") { sumPath = e.path(); break; }
        }
    }
    if (!sumPath.empty()) {
        std::string sums;
        readAll(sumPath, sums);
        std::vector<std::pair<std::string, fs::path>> todo;
        uint64_t total = 0;
        std::istringstream in(sums);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() < 34) continue;
            std::string hash = line.substr(0, 32);
            std::string name = trim(line.substr(32));
            if (!name.empty() && name[0] == '*') name.erase(0, 1);
            std::transform(hash.begin(), hash.end(), hash.begin(), ::tolower);
            fs::path fp = findCaseless(dir, name);
            if (fp.empty()) { r.notes.push_back("Checksum file lists " + name + " but it's missing."); continue; }
            std::error_code ec;
            total += fs::file_size(fp, ec);
            todo.push_back({hash, fp});
        }
        r.checksumsChecked = !todo.empty();
        r.checksumsOk = r.checksumsChecked;
        uint64_t done = 0;
        for (auto& [hash, fp] : todo) {
            std::string got = hashFile(fp, progress, done, total);
            if (got != hash) {
                r.checksumsOk = false;
                r.notes.push_back("MD5 MISMATCH: " + fp.filename().string() + " is damaged or was changed.");
            }
        }
        if (r.checksumsOk) r.notes.push_back("All " + std::to_string(todo.size()) + " MD5 checksums match.");
    } else {
        r.notes.push_back("No MD5 checksum file found; file integrity couldn't be confirmed.");
    }

    r.ok = true;
    return r;
}

}  // namespace lathe::ddp
