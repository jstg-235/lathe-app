// Checksums used by DDP delivery: MD5 (CHECKSUM.MD5), CRC-32 (CHECKSUM.TXT),
// and CRC-16/CCITT (CD-Text packs). Streaming, allocation-free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lathe::util {

class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::string hexDigest();   // finalises; call reset() to reuse
private:
    void block(const uint8_t* p);
    uint32_t a_, b_, c_, d_;
    uint64_t bytes_;
    uint8_t buf_[64];
    size_t used_;
};

class Crc32 {
public:
    void update(const void* data, size_t len);
    uint32_t value() const { return ~crc_; }
private:
    uint32_t crc_ = 0xFFFFFFFFu;
};

// CRC-16/CCITT, polynomial 0x1021, initial value 0 (CD-Text inverts the result).
uint16_t crc16Ccitt(const uint8_t* data, size_t len);

}  // namespace lathe::util
