#include "CdTextEncoder.h"

#include "../util/Hash.h"

#include <array>

namespace lathe::cd {

bool utf8ToLatin1(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size();) {
        uint8_t c = uint8_t(in[i]);
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else return false;
        if (i + len > in.size()) return false;
        for (int k = 1; k < len; ++k) {
            uint8_t cc = uint8_t(in[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Common typographic characters that players can't show: map to plain ASCII.
        if (cp == 0x2018 || cp == 0x2019) cp = '\'';
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        if (cp > 0xFF || cp == 0) return false;
        out += char(cp);
        i += size_t(len);
    }
    return true;
}

namespace {
const std::string& field(const CdText& t, int type) {
    switch (type) {
        case 0x80: return t.title;
        case 0x81: return t.performer;
        case 0x82: return t.songwriter;
        case 0x83: return t.composer;
        case 0x84: return t.arranger;
        default:   return t.message;
    }
}
}  // namespace

CdTextResult encodeCdText(const Disc& disc) {
    CdTextResult res;
    if (!disc.hasCdText()) return res;

    std::array<int, 16> packCount{};   // per type 0x80..0x8F
    std::vector<std::array<uint8_t, 16>> raw;  // header + payload, CRC added at the end

    // Text packs: 0x80-0x85 (disc + per-track strings), 0x86 disc ID and 0x87 genre (disc
    // only), 0x8E UPC/EAN + ISRC (disc + per-track).
    const int types[] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x8E};
    for (int type : types) {
        std::string bytes;
        std::vector<int> owner;
        std::vector<int> posInString;
        auto addRaw = [&](const std::string& latin, int trackNo) {
            for (size_t i = 0; i < latin.size(); ++i) {
                bytes += latin[i];
                owner.push_back(trackNo);
                posInString.push_back(int(i));
            }
        };
        auto add = [&](const std::string& utf8, int trackNo, const char* what) -> bool {
            std::string latin;
            if (!utf8ToLatin1(utf8, latin)) {
                res.error = std::string(what) + " contains a character CD-Text can't show (Latin-1 only).";
                return false;
            }
            latin += '\0';
            addRaw(latin, trackNo);
            return true;
        };

        if (type <= 0x85) {
            bool present = !field(disc.text, type).empty();
            for (auto& t : disc.tracks) present = present || !field(t.text, type).empty();
            if (!present) continue;
            // Concatenate the disc string then each track string, NUL-terminated, remembering
            // which track owns each byte so the header bytes can be filled in.
            if (!add(field(disc.text, type), 0, "Disc text")) return res;
            for (size_t ti = 0; ti < disc.tracks.size(); ++ti)
                if (!add(field(disc.tracks[ti].text, type), int(ti + 1), ("Track " + std::to_string(ti + 1) + " text").c_str()))
                    return res;
        } else if (type == 0x86) {
            if (disc.discId.empty()) continue;
            if (!add(disc.discId, 0, "Disc ID")) return res;
        } else if (type == 0x87) {
            if (disc.genreCode <= 0 && disc.genreText.empty()) continue;
            std::string latin;
            if (!utf8ToLatin1(disc.genreText, latin)) { res.error = "Genre text contains a character CD-Text can't show (Latin-1 only)."; return res; }
            int code = disc.genreCode > 0 ? disc.genreCode : 1;
            std::string g;
            g += char((code >> 8) & 0xFF);
            g += char(code & 0xFF);
            g += latin;
            g += '\0';
            addRaw(g, 0);
        } else {   // 0x8E
            if (!disc.codesInCdText) continue;
            bool present = !disc.upcEan.empty();
            for (auto& t : disc.tracks) present = present || !t.isrc.empty();
            if (!present) continue;
            addRaw(disc.upcEan + std::string(1, '\0'), 0);
            for (size_t ti = 0; ti < disc.tracks.size(); ++ti) addRaw(disc.tracks[ti].isrc + std::string(1, '\0'), int(ti + 1));
        }

        for (size_t off = 0; off < bytes.size(); off += 12) {
            std::array<uint8_t, 16> p{};
            p[0] = uint8_t(type);
            p[1] = uint8_t(owner[off]);
            p[2] = 0;  // sequence, filled later
            p[3] = uint8_t(std::min(posInString[off], 15));
            for (size_t k = 0; k < 12; ++k) p[4 + k] = off + k < bytes.size() ? uint8_t(bytes[off + k]) : 0;
            raw.push_back(p);
            packCount[type - 0x80]++;
        }
    }

    // Size information: three packs of type 0x8F.
    packCount[0x0F] = 3;
    int total = int(raw.size()) + 3;
    if (total > 256) {
        res.error = "CD-Text is too long: " + std::to_string(total) + " packs (limit 256). Shorten titles or credits.";
        return res;
    }
    std::array<uint8_t, 36> size{};
    size[0] = 0x00;  // ISO 8859-1
    size[1] = 1;
    size[2] = uint8_t(disc.tracks.size());
    size[3] = 0;
    for (int i = 0; i < 16; ++i) size[4 + i] = uint8_t(packCount[i]);
    size[20] = uint8_t(total - 1);  // last sequence number of block 0
    size[28] = 0x09;                // English
    for (int k = 0; k < 3; ++k) {
        std::array<uint8_t, 16> p{};
        p[0] = 0x8F;
        p[1] = uint8_t(k);
        for (int b = 0; b < 12; ++b) p[4 + b] = size[k * 12 + b];
        raw.push_back(p);
    }

    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i][2] = uint8_t(i);
        uint16_t crc = uint16_t(~util::crc16Ccitt(raw[i].data(), 16));
        res.packs.insert(res.packs.end(), raw[i].begin(), raw[i].end());
        res.packs.push_back(uint8_t(crc >> 8));
        res.packs.push_back(uint8_t(crc & 0xFF));
    }
    return res;
}

}  // namespace lathe::cd
