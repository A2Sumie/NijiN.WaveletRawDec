// SPDX-License-Identifier: Apache-2.0
#ifndef NEV2BAYER_TIFF_INTERNAL_H
#define NEV2BAYER_TIFF_INTERNAL_H

#include "../include/nev2bayer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nev2bayer {
namespace detail {

inline uint16_t rd_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t rd_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t rd_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(rd_be32(p)) << 32) | static_cast<uint64_t>(rd_be32(p + 4));
}

inline uint16_t rd_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t rd_u16_e(const uint8_t* p, bool be) {
    return be ? rd_be16(p) : rd_le16(p);
}

inline uint32_t rd_u32_e(const uint8_t* p, bool be) {
    return be ? rd_be32(p) : rd_le32(p);
}

inline float rd_be_f32(const uint8_t* p) {
    uint32_t u = rd_be32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline double rd_be_f64(const uint8_t* p) {
    uint64_t u = rd_be64(p);
    double d = 0.0;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}

inline std::string fourcc_to_str(const uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), 4);
}

inline std::string rstrip_ascii(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != '\0') {
        ++len;
    }
    while (len > 0 && p[len - 1] == ' ') {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

struct ScopedFile {
    FILE* fp = nullptr;
    explicit ScopedFile(const std::string& path) : fp(std::fopen(path.c_str(), "rb")) {}
    ~ScopedFile() {
        if (fp) {
            std::fclose(fp);
        }
    }
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
};

inline bool seek_read(FILE* fp, uint64_t off, void* dst, size_t n) {
    if (n == 0) {
        return true;
    }
    if (fseeko(fp, static_cast<off_t>(off), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(dst, 1, n, fp) == n;
}

struct TiffTagValue {
    uint16_t tag = 0;
    uint16_t dtype = 0;
    uint32_t count = 0;
    uint64_t file_offset = 0;
    std::vector<uint8_t> data;
};

bool read_tiff_ifd(
    FILE* fp,
    uint64_t tiff_base,
    uint64_t file_size,
    bool be,
    uint32_t ifd_rel_off,
    std::vector<TiffTagValue>& out_tags,
    uint32_t* next_ifd_off = nullptr);

uint32_t tag_as_u32(const TiffTagValue& tv, bool be);
bool tag_as_rational(const TiffTagValue& tv, bool be, double& out);

bool parse_sony_arw_tiff(
    FILE* fp,
    uint64_t file_size,
    bool be,
    uint32_t ifd0_next_off,
    const std::vector<TiffTagValue>& ifd0,
    const std::vector<uint32_t>& subifds,
    uint32_t exif_ifd_off,
    NevClipInfo& clip,
    NevFrameMeta& meta,
    std::string& err);

} // namespace detail
} // namespace nev2bayer

#endif // NEV2BAYER_TIFF_INTERNAL_H
