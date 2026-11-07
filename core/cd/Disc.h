// Lathe — Red Book disc model: tracks, indexes, PQ flags, ISRC, UPC/EAN, CD-Text.
// Pure C++17, no framework dependencies, so it can be unit-tested anywhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lathe::cd {

constexpr int kFramesPerSecond   = 75;     // CD sectors per second
constexpr int kSamplesPerSector  = 588;    // stereo sample frames per sector at 44.1 kHz
constexpr int kBytesPerSector    = 2352;   // 588 * 2 channels * 2 bytes
constexpr int kSampleRate        = 44100;
constexpr int64_t kMaxSectors    = (99LL * 60 + 59) * 75 + 74;  // 99:59:74

// Minutes:Seconds:Frames address. Minutes may exceed 59.
struct Msf {
    int m = 0, s = 0, f = 0;
    static Msf fromSectors(int64_t sectors);
    int64_t toSectors() const { return (int64_t(m) * 60 + s) * kFramesPerSecond + f; }
    std::string str(char sep = ':') const;   // "MM:SS:FF"
    std::string digits() const;              // "MMSSFF"
};

int64_t samplesToSectorsCeil(int64_t sampleFrames);
int64_t sectorsToSamples(int64_t sectors);

struct CdText {
    std::string title, performer, songwriter, composer, arranger, message;
    bool empty() const {
        return title.empty() && performer.empty() && songwriter.empty() &&
               composer.empty() && arranger.empty() && message.empty();
    }
};

struct Index {
    int number = 1;       // 0 = pregap, 1 = track start, 2..99 = sub-indexes
    int64_t sector = 0;   // absolute sector in the program (0 = first sector of the image)
};

struct Track {
    std::vector<Index> indexes;   // sorted by number; must contain index 1
    std::string isrc;             // 12 chars or empty
    bool preEmphasis = false;
    bool copyPermitted = false;   // DCP
    bool fourChannel = false;
    bool scms = false;
    CdText text;

    const Index* find(int number) const;
    int64_t startSector() const;      // index 1
    int64_t firstSector() const;      // index 0 if present, else index 1
    char controlNibble() const;       // hex char for the PQ C1 field
};

struct Disc {
    std::string upcEan;     // 13-digit EAN-13 or 12-digit UPC-A, or empty
    std::string masterId;   // free text, max 48 chars, not written to disc
    CdText text;            // disc-level CD-Text
    std::string discId;     // CD-Text pack 0x86 (usually the catalogue number), disc only
    int genreCode = 0;      // CD-Text pack 0x87 genre code (0 = none, 1 = not used, 2..27 = see genreName)
    std::string genreText;  // supplementary genre text in pack 0x87
    bool codesInCdText = true;  // also write UPC/EAN and ISRCs as CD-Text pack 0x8E
    std::vector<Track> tracks;
    int64_t leadOutSector = 0;  // total program length in sectors

    bool hasCdText() const;
};

struct Issue {
    enum Level { Warning, Error } level;
    std::string message;
};

// Red Book / DDP plant rules. Errors make a master unusable; warnings are worth a look.
std::vector<Issue> validate(const Disc& disc);
bool hasErrors(const std::vector<Issue>& issues);

// Helpers exposed for tests and the UI.
bool isValidIsrc(const std::string& isrc);
// CD-Text genre names for codes 0..27 (Red Book / MMC list); empty for unknown codes.
std::string genreName(int code);
constexpr int kGenreCount = 28;
bool isValidUpcEan(const std::string& code, bool* checkDigitOk = nullptr);
std::string normaliseIsrc(std::string isrc);   // strips dashes/spaces, uppercases

}  // namespace lathe::cd
