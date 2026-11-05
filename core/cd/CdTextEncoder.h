// Encodes disc + track CD-Text into lead-in packs (18 bytes each), block 0, ISO 8859-1.
#pragma once

#include "Disc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lathe::cd {

struct CdTextResult {
    std::vector<uint8_t> packs;   // n * 18 bytes, ready for CDTEXT.BIN
    std::string error;            // non-empty if the text can't be encoded
};

// Converts UTF-8 to ISO 8859-1; returns false if a character can't be represented.
bool utf8ToLatin1(const std::string& in, std::string& out);

CdTextResult encodeCdText(const Disc& disc);

}  // namespace lathe::cd
