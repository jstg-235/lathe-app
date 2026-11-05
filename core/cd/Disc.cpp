#include "Disc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace lathe::cd {

Msf Msf::fromSectors(int64_t sectors) {
    if (sectors < 0) sectors = 0;
    Msf r;
    r.f = int(sectors % kFramesPerSecond);
    int64_t secs = sectors / kFramesPerSecond;
    r.s = int(secs % 60);
    r.m = int(secs / 60);
    return r;
}

std::string Msf::str(char sep) const {
    char b[32];
    std::snprintf(b, sizeof b, "%02d%c%02d%c%02d", m, sep, s, sep, f);
    return b;
}

std::string Msf::digits() const {
    char b[32];
    std::snprintf(b, sizeof b, "%02d%02d%02d", m, s, f);
    return b;
}

int64_t samplesToSectorsCeil(int64_t n) { return (n + kSamplesPerSector - 1) / kSamplesPerSector; }
int64_t sectorsToSamples(int64_t s) { return s * kSamplesPerSector; }

const Index* Track::find(int number) const {
    for (auto& i : indexes)
        if (i.number == number) return &i;
    return nullptr;
}

int64_t Track::startSector() const {
    auto* i = find(1);
    return i ? i->sector : 0;
}

int64_t Track::firstSector() const {
    auto* i = find(0);
    return i ? i->sector : startSector();
}

char Track::controlNibble() const {
    int v = (preEmphasis ? 1 : 0) | (copyPermitted ? 2 : 0) | (fourChannel ? 8 : 0);
    return "0123456789ABCDEF"[v];
}

bool Disc::hasCdText() const {
    if (!text.empty()) return true;
    for (auto& t : tracks)
        if (!t.text.empty()) return true;
    return false;
}

std::string normaliseIsrc(std::string s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool isValidIsrc(const std::string& isrc) {
    // CC XXX YY NNNNN: 2 letters, 3 alphanumerics, 7 digits.
    if (isrc.size() != 12) return false;
    for (int i = 0; i < 2; ++i)
        if (!std::isupper(static_cast<unsigned char>(isrc[i]))) return false;
    for (int i = 2; i < 5; ++i)
        if (!(std::isupper(static_cast<unsigned char>(isrc[i])) || std::isdigit(static_cast<unsigned char>(isrc[i]))))
            return false;
    for (int i = 5; i < 12; ++i)
        if (!std::isdigit(static_cast<unsigned char>(isrc[i]))) return false;
    return true;
}

bool isValidUpcEan(const std::string& code, bool* checkDigitOk) {
    if (code.size() != 12 && code.size() != 13) return false;
    for (char c : code)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    std::string ean = code.size() == 12 ? "0" + code : code;
    int sum = 0;
    for (int i = 0; i < 12; ++i) sum += (ean[i] - '0') * (i % 2 ? 3 : 1);
    int check = (10 - sum % 10) % 10;
    if (checkDigitOk) *checkDigitOk = (check == ean[12] - '0');
    return true;
}

static std::string trackLabel(size_t i) { return "Track " + std::to_string(i + 1); }

std::vector<Issue> validate(const Disc& d) {
    std::vector<Issue> out;
    auto err = [&](std::string m) { out.push_back({Issue::Error, std::move(m)}); };
    auto warn = [&](std::string m) { out.push_back({Issue::Warning, std::move(m)}); };

    if (d.tracks.empty()) err("The disc has no tracks.");
    if (d.tracks.size() > 99) err("A CD holds at most 99 tracks.");
    if (d.leadOutSector > kMaxSectors) err("Program is longer than 99:59:74.");
    else if (d.leadOutSector > 80LL * 60 * 75)
        warn("Program is longer than 80 minutes; many plants and players won't accept it.");
    if (d.masterId.size() > 48) err("Master ID is longer than 48 characters.");

    if (!d.upcEan.empty()) {
        bool ok = true;
        if (!isValidUpcEan(d.upcEan, &ok)) err("UPC/EAN must be 12 or 13 digits.");
        else if (!ok) warn("UPC/EAN check digit doesn't match; double-check the barcode number.");
    }

    int64_t prev = -1;
    for (size_t ti = 0; ti < d.tracks.size(); ++ti) {
        const Track& t = d.tracks[ti];
        if (!t.find(1)) { err(trackLabel(ti) + " has no start (index 01)."); continue; }
        if (t.indexes.size() > 100) err(trackLabel(ti) + " has more than 99 sub-indexes.");

        int expect = t.find(0) ? 0 : 1;
        for (auto& ix : t.indexes) {
            if (ix.number != expect) {
                err(trackLabel(ti) + ": indexes must be numbered consecutively.");
                break;
            }
            ++expect;
            if (ix.sector <= prev) err(trackLabel(ti) + ": index " + std::to_string(ix.number) +
                                       " is not after the previous marker.");
            if (ix.sector >= d.leadOutSector)
                err(trackLabel(ti) + ": index " + std::to_string(ix.number) + " is at or past the end of the program.");
            prev = ix.sector;
        }

        int64_t next = (ti + 1 < d.tracks.size()) ? d.tracks[ti + 1].startSector() : d.leadOutSector;
        if (next - t.startSector() < 4 * kFramesPerSecond)
            err(trackLabel(ti) + " is shorter than 4 seconds (Red Book minimum).");

        if (!t.isrc.empty() && !isValidIsrc(t.isrc))
            err(trackLabel(ti) + ": ISRC \"" + t.isrc + "\" isn't 12 characters (CC-XXX-YY-NNNNN).");
        if (t.copyPermitted && t.scms) err(trackLabel(ti) + ": copy-permitted and SCMS can't both be set.");
        if (t.fourChannel) warn(trackLabel(ti) + ": four-channel flag is set; this is almost never intended.");
    }

    if (!d.tracks.empty() && d.tracks[0].find(1)) {
        const Track& t1 = d.tracks[0];
        if (t1.firstSector() != 0) err("Track 1 must start at 00:00:00 (index 00 at the top of the program).");
        if (t1.startSector() < 2 * kFramesPerSecond)
            err("Track 1 needs at least 2 seconds of pregap before index 01.");
    }

    // Duplicate ISRCs are legal only for a deliberately repeated recording.
    for (size_t a = 0; a < d.tracks.size(); ++a)
        for (size_t b = a + 1; b < d.tracks.size(); ++b)
            if (!d.tracks[a].isrc.empty() && d.tracks[a].isrc == d.tracks[b].isrc)
                warn("Tracks " + std::to_string(a + 1) + " and " + std::to_string(b + 1) + " share an ISRC.");

    auto latin1Check = [&](const std::string& s, const std::string& where) {
        if (s.size() > 160) warn(where + " CD-Text is very long; some players truncate it.");
    };
    latin1Check(d.text.title, "Disc title");
    return out;
}

bool hasErrors(const std::vector<Issue>& v) {
    return std::any_of(v.begin(), v.end(), [](const Issue& i) { return i.level == Issue::Error; });
}

}  // namespace lathe::cd
