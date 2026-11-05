#include "Hash.h"

#include <algorithm>
#include <cstring>

namespace lathe::util {

namespace {
constexpr uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

uint32_t crcTable[256];
bool crcInit = [] {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[i] = c;
    }
    return true;
}();
}  // namespace

void Md5::reset() {
    a_ = 0x67452301; b_ = 0xefcdab89; c_ = 0x98badcfe; d_ = 0x10325476;
    bytes_ = 0; used_ = 0;
}

void Md5::block(const uint8_t* p) {
    uint32_t M[16];
    for (int i = 0; i < 16; ++i)
        M[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
    uint32_t A = a_, B = b_, C = c_, D = d_;
    for (int i = 0; i < 64; ++i) {
        uint32_t F; int g;
        if (i < 16)      { F = (B & C) | (~B & D); g = i; }
        else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
        else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) % 16; }
        else             { F = C ^ (B | ~D);       g = (7 * i) % 16; }
        F = F + A + K[i] + M[g];
        A = D; D = C; C = B;
        B = B + rotl(F, R[i]);
    }
    a_ += A; b_ += B; c_ += C; d_ += D;
}

void Md5::update(const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    bytes_ += len;
    if (used_) {
        size_t take = std::min(len, size_t(64) - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take; p += take; len -= take;
        if (used_ == 64) { block(buf_); used_ = 0; }
    }
    while (len >= 64) { block(p); p += 64; len -= 64; }
    if (len) { std::memcpy(buf_, p, len); used_ = len; }
}

std::string Md5::hexDigest() {
    uint64_t bits = bytes_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (8 * i));
    update(len, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint32_t v : {a_, b_, c_, d_})
        for (int i = 0; i < 4; ++i) {
            uint8_t b = uint8_t(v >> (8 * i));
            out += hex[b >> 4];
            out += hex[b & 15];
        }
    return out;
}

void Crc32::update(const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) crc_ = crcTable[(crc_ ^ p[i]) & 0xFF] ^ (crc_ >> 8);
}

uint16_t crc16Ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= uint16_t(data[i]) << 8;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

}  // namespace lathe::util
