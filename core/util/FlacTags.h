// Writes a VORBIS_COMMENT block (FLAC's standard tags: TITLE, ARTIST, ALBUM, ISRC, ...)
// into an existing .flac file, replacing any tags already there. Audio frames are copied
// untouched.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lathe::util {

using Tags = std::vector<std::pair<std::string, std::string>>;   // key (ASCII), value (UTF-8)

// Returns an empty string on success, or a reason it failed.
std::string writeFlacTags(const std::string& path, const Tags& tags, const std::string& vendor = "Lathe");
// Reads the tags back (for tests and verification).
bool readFlacTags(const std::string& path, Tags& out);

}  // namespace lathe::util
