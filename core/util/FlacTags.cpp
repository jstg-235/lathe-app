#include "FlacTags.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace lathe::util {

namespace {
struct Block {
    int type;
    std::vector<uint8_t> data;
};

bool parse(const std::vector<uint8_t>& f, std::vector<Block>& blocks, size_t& audioStart) {
    if (f.size() < 8 || f[0] != 'f' || f[1] != 'L' || f[2] != 'a' || f[3] != 'C') return false;
    size_t p = 4;
    bool last = false;
    while (!last) {
        if (p + 4 > f.size()) return false;
        last = (f[p] & 0x80) != 0;
        int type = f[p] & 0x7F;
        size_t len = (size_t(f[p + 1]) << 16) | (size_t(f[p + 2]) << 8) | f[p + 3];
        p += 4;
        if (p + len > f.size()) return false;
        blocks.push_back({type, std::vector<uint8_t>(f.begin() + long(p), f.begin() + long(p + len))});
        p += len;
    }
    audioStart = p;
    return !blocks.empty() && blocks[0].type == 0;   // STREAMINFO must be first
}

void le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
}  // namespace

std::string writeFlacTags(const std::string& path, const Tags& tags, const std::string& vendor) {
    std::vector<uint8_t> f;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "can't open " + path;
        f.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::vector<Block> blocks;
    size_t audio = 0;
    if (!parse(f, blocks, audio)) return "not a valid FLAC file";

    Block vc{4, {}};
    le32(vc.data, uint32_t(vendor.size()));
    vc.data.insert(vc.data.end(), vendor.begin(), vendor.end());
    le32(vc.data, uint32_t(tags.size()));
    for (auto& [k, v] : tags) {
        std::string c = k + "=" + v;
        le32(vc.data, uint32_t(c.size()));
        vc.data.insert(vc.data.end(), c.begin(), c.end());
    }
    if (vc.data.size() >= (1u << 24)) return "tags too large";

    std::vector<Block> out;
    out.push_back(blocks[0]);
    out.push_back(vc);
    for (size_t i = 1; i < blocks.size(); ++i)
        if (blocks[i].type != 4) out.push_back(blocks[i]);

    std::vector<uint8_t> w = {'f', 'L', 'a', 'C'};
    for (size_t i = 0; i < out.size(); ++i) {
        size_t len = out[i].data.size();
        w.push_back(uint8_t((i + 1 == out.size() ? 0x80 : 0) | out[i].type));
        w.push_back(uint8_t(len >> 16));
        w.push_back(uint8_t(len >> 8));
        w.push_back(uint8_t(len));
        w.insert(w.end(), out[i].data.begin(), out[i].data.end());
    }
    w.insert(w.end(), f.begin() + long(audio), f.end());

    std::string tmp = path + ".tagtmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return "can't write " + tmp;
        o.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
        if (!o) return "disk write failed";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) return "couldn't replace the file: " + ec.message();
    return {};
}

bool readFlacTags(const std::string& path, Tags& outTags) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> f(std::istreambuf_iterator<char>(in), {});
    std::vector<Block> blocks;
    size_t audio = 0;
    if (!parse(f, blocks, audio)) return false;
    for (auto& b : blocks) {
        if (b.type != 4) continue;
        auto rd = [&](size_t p) { return uint32_t(b.data[p]) | uint32_t(b.data[p + 1]) << 8 | uint32_t(b.data[p + 2]) << 16 | uint32_t(b.data[p + 3]) << 24; };
        size_t p = 0;
        if (b.data.size() < 8) return false;
        p += 4 + rd(0);
        uint32_t n = rd(p);
        p += 4;
        for (uint32_t i = 0; i < n && p + 4 <= b.data.size(); ++i) {
            uint32_t len = rd(p);
            p += 4;
            if (p + len > b.data.size()) return false;
            std::string c(b.data.begin() + long(p), b.data.begin() + long(p + len));
            p += len;
            auto eq = c.find('=');
            if (eq != std::string::npos) outTags.push_back({c.substr(0, eq), c.substr(eq + 1)});
        }
        return true;
    }
    return true;
}

}  // namespace lathe::util
