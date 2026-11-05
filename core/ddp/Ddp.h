// DDP 2.00 audio-CD fileset writer and verifier.
//
// Field layouts follow the openly documented, MIT-licensed reverse-engineering notes at
// github.com/ddp-reverse-eng/ddp-reverse-eng (spec/ddp2.md), which match the filesets
// written by ddptools, HOFA and Sonoris. Where those writers disagree, Lathe follows the
// HOFA/Sonoris conventions (zero-padded numbers, SCR 0, PQ file named PQDESCR).
#pragma once

#include "../cd/Disc.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lathe::ddp {

// Supplies the program's audio as interleaved stereo 16-bit samples, starting at sector 0.
// Called repeatedly with consecutive chunks. Return false to abort (e.g. on cancel).
using AudioPull = std::function<bool(int16_t* interleaved, int64_t frames)>;

using Progress = std::function<void(double fraction)>;

struct WriteOptions {
    bool writeCueSheet = true;     // IMAGE.cue beside the fileset (handy for players/burners)
    bool writePqLog = true;        // human-readable PQ sheet, PQ_LOG.TXT
    std::string pqLogHeader;       // e.g. client / project / engineer lines
};

struct WriteResult {
    bool ok = false;
    std::string error;
    std::vector<cd::Issue> issues;   // validation results (errors stop the write)
    std::vector<std::string> files;  // written file names
    std::string imageMd5;
};

WriteResult writeDdp(const cd::Disc& disc, const std::string& directory, const AudioPull& audio,
                     const WriteOptions& opts = {}, const Progress& progress = {});

// ---- Reading / verification -------------------------------------------------

struct ReadResult {
    bool ok = false;
    std::string error;
    cd::Disc disc;                     // reconstructed from DDPID + PQ descriptor (+ CD-Text)
    std::string imageFile;             // path to the audio image
    int64_t imageSectors = 0;
    std::vector<std::string> notes;    // checksum results, oddities found
    bool checksumsChecked = false;
    bool checksumsOk = false;
};

// Parses a DDP folder and, if a checksum file is present, verifies every listed file.
ReadResult readDdp(const std::string& directory, const Progress& progress = {});

// The text blocks, exposed for tests.
std::string makeDdpId(const cd::Disc& disc);
std::string makeMapPacket(const std::string& dst, int64_t dsl, const std::string& sub, bool audio,
                          int64_t pregap, const std::string& fileName, bool cdtext);
std::string makePqDescriptor(const cd::Disc& disc);
std::string makeCueSheet(const cd::Disc& disc, const std::string& imageName);
std::string makePqLog(const cd::Disc& disc, const std::string& header);

}  // namespace lathe::ddp
