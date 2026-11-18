// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC.h"
#include "../src/focus_meta_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <vector>

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::fprintf(stderr, "CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__, #expr); \
            std::abort();                                                                   \
        }                                                                                   \
    } while (0)

namespace {

std::string temp_file_path(const char* name) {
    const char* td = std::getenv("TMPDIR");
#ifdef _WIN32
    if (!td || !*td) td = std::getenv("TEMP");
    if (!td || !*td) td = std::getenv("TMP");
    std::string base = (td && *td) ? td : ".";
#else
    std::string base = (td && *td) ? td : "/tmp";
#endif
    while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    return base + "/" + name;
}

bool file_exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && st.st_size > 0;
}

uint16_t rd_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void put_le16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
    buf[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void put_le32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    buf[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), fp) == data.size();
    if (std::fclose(fp) != 0) ok = false;
    return ok;
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    CHECK(fp != nullptr);
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    CHECK(sz > 0);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    size_t rd = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    CHECK(rd == out.size());
    return out;
}

void test_negative_containers_and_codestreams() {
    std::string err;
    nijinwvdc::NevClipInfo clip;
    CHECK(!nijinwvdc::parse_nev_clip(temp_file_path("does_not_exist_nijin_123456.nev"), clip, err));

    nijinwvdc::TicoRawInfo jxs;
    std::vector<uint16_t> out;
    uint8_t tiny[8] = {0xff, 0x10, 0xff, 0x50, 0, 0, 0, 0};
    CHECK(!nijinwvdc::probe_ticoraw_header(nullptr, 0, jxs, err));
    CHECK(!nijinwvdc::probe_ticoraw_header(tiny, sizeof(tiny), jxs, err));
    CHECK(!nijinwvdc::decode_ticoraw_bayer(tiny, sizeof(tiny), 1, out, jxs, err));

    std::vector<uint8_t> junk(256, 0xAA);
    junk[0] = 0xFF; junk[1] = 0x10;
    junk[2] = 0xFF; junk[3] = 0x50;
    CHECK(!nijinwvdc::probe_ticoraw_header(junk.data(), junk.size(), jxs, err));
    CHECK(!nijinwvdc::decode_ticoraw_bayer(junk.data(), junk.size(), 2, out, jxs, err));

    // Malformed ISOBMFF file (ftyp only, no moov)
    std::string bad_nev = temp_file_path("nijin_bad_ftyp.nev");
    std::vector<uint8_t> ftyp_only = {
        0, 0, 0, 16, 'f', 't', 'y', 'p', 'n', 'i', 'k', 'o', 0, 0, 0, 1
    };
    CHECK(write_bytes(bad_nev, ftyp_only));
    CHECK(!nijinwvdc::parse_nev_clip(bad_nev, clip, err));
    std::remove(bad_nev.c_str());

    // Synthetic TIFF with Compression=34713 SubIFD whose strip is NOT a TicoRAW 0xFF10 SOC
    std::string legacy_nef = temp_file_path("nijin_legacy_34713.nef");
    std::vector<uint8_t> tiff(256, 0);
    tiff[0] = 'I'; tiff[1] = 'I'; put_le16(tiff, 2, 42); put_le32(tiff, 4, 8);
    // IFD0 @ 8: 1 entry (SubIFDs=330 -> 30)
    put_le16(tiff, 8, 1);
    put_le16(tiff, 10, 330); put_le16(tiff, 12, 4); put_le32(tiff, 14, 1); put_le32(tiff, 18, 30);
    // SubIFD @ 30: 4 entries (Compression=259:34713, Photometric=262:32803, StripOffsets=273:140, StripByteCounts=279:32)
    put_le16(tiff, 30, 4);
    auto write_ifd_ent = [&](size_t idx, uint16_t tag, uint16_t type, uint32_t cnt, uint32_t val) {
        size_t o = 32 + idx * 12;
        put_le16(tiff, o + 0, tag);
        put_le16(tiff, o + 2, type);
        put_le32(tiff, o + 4, cnt);
        put_le32(tiff, o + 8, val);
    };
    write_ifd_ent(0, 259, 3, 1, 34713);
    write_ifd_ent(1, 262, 3, 1, 32803);
    write_ifd_ent(2, 273, 4, 1, 140);
    write_ifd_ent(3, 279, 4, 1, 32);
    tiff[140] = 0x12; tiff[141] = 0x34;
    CHECK(write_bytes(legacy_nef, tiff));
    CHECK(!nijinwvdc::parse_nev_clip(legacy_nef, clip, err));
    CHECK(err.find("legacy Nikon") != std::string::npos);
    std::remove(legacy_nef.c_str());

    // Synthetic Sony ARW SubIFD using unsupported Compression=32767 (legacy compressed/uncompressed)
    std::string legacy_arw = temp_file_path("nijin_legacy_32767.arw");
    std::vector<uint8_t> arw(320, 0);
    arw[0] = 'I'; arw[1] = 'I'; put_le16(arw, 2, 42); put_le32(arw, 4, 8);
    // IFD0 @ 8: Make="SONY", SubIFDs=330 -> 60
    put_le16(arw, 8, 2);
    put_le16(arw, 10, 271); put_le16(arw, 12, 2); put_le32(arw, 14, 5); put_le32(arw, 18, 40);
    put_le16(arw, 22, 330); put_le16(arw, 24, 4); put_le32(arw, 26, 1); put_le32(arw, 30, 60);
    std::memcpy(&arw[40], "SONY", 5);
    put_le16(arw, 60, 5);
    auto write_arw_ent = [&](size_t idx, uint16_t tag, uint16_t type, uint32_t cnt, uint32_t val) {
        size_t o = 62 + idx * 12;
        put_le16(arw, o + 0, tag);
        put_le16(arw, o + 2, type);
        put_le32(arw, o + 4, cnt);
        put_le32(arw, o + 8, val);
    };
    write_arw_ent(0, 259, 3, 1, 32767);
    write_arw_ent(1, 262, 3, 1, 32803);
    write_arw_ent(2, 273, 4, 1, 160);
    write_arw_ent(3, 279, 4, 1, 32);
    write_arw_ent(4, 0x7000, 3, 1, 1);
    CHECK(write_bytes(legacy_arw, arw));
    CHECK(!nijinwvdc::parse_nev_clip(legacy_arw, clip, err));
    CHECK(err.find("Unsupported Sony ARW") != std::string::npos);
    std::remove(legacy_arw.c_str());

    // Negative ARW6 codestream and strip probes
    nijinwvdc::Arw6Info ainfo;
    std::vector<uint16_t> aout;
    uint8_t bad_a000[16] = {'B', 'A', 'D', '0', 0};
    CHECK(!nijinwvdc::probe_arw6_codestream(nullptr, 0, ainfo, err));
    CHECK(!nijinwvdc::probe_arw6_codestream(bad_a000, sizeof(bad_a000), ainfo, err));
    CHECK(!nijinwvdc::decode_arw6_tile(bad_a000, sizeof(bad_a000), 1, 0, 0, 16, 16, aout, ainfo, err));

    for (uint32_t bad_tc : {0u, 2u, 5u}) {
        std::vector<uint8_t> bad_strip(0x80, 0);
        put_le32(bad_strip, 0, bad_tc);
        CHECK(!nijinwvdc::probe_arw6_strip(bad_strip.data(), bad_strip.size(), ainfo, err));
        CHECK(!nijinwvdc::decode_arw6_bayer(bad_strip.data(), bad_strip.size(), 1, aout, ainfo, err));
    }
}

void test_cfa_crop_and_colormatrix() {
    uint16_t src[16] = {
         1,  2,  3,  4,
         5,  6,  7,  8,
         9, 10, 11, 12,
        13, 14, 15, 16
    };
    const uint8_t rggb[4] = {0, 1, 1, 2};
    std::vector<uint16_t> dst;
    uint8_t dcfa[4] = {0, 0, 0, 0};

    // Even/even keeps RGGB
    uint16_t c00[4] = {0, 0, 2, 2};
    CHECK(nijinwvdc::apply_bayer_crop(src, 4, 4, c00, rggb, dst, dcfa));
    CHECK(dcfa[0] == 0 && dcfa[1] == 1 && dcfa[2] == 1 && dcfa[3] == 2);

    // Odd X, even Y -> GRBG [1, 0, 2, 1]
    uint16_t c10[4] = {1, 0, 2, 2};
    CHECK(nijinwvdc::apply_bayer_crop(src, 4, 4, c10, rggb, dst, dcfa));
    CHECK(dst.size() == 4 && dst[0] == 2 && dst[1] == 3 && dst[2] == 6 && dst[3] == 7);
    CHECK(dcfa[0] == 1 && dcfa[1] == 0 && dcfa[2] == 2 && dcfa[3] == 1);

    // Even X, odd Y -> GBRG [1, 2, 0, 1]
    uint16_t c01[4] = {0, 1, 2, 2};
    CHECK(nijinwvdc::apply_bayer_crop(src, 4, 4, c01, rggb, dst, dcfa));
    CHECK(dst[0] == 5 && dst[3] == 10);
    CHECK(dcfa[0] == 1 && dcfa[1] == 2 && dcfa[2] == 0 && dcfa[3] == 1);

    // Odd X, odd Y -> BGGR [2, 1, 1, 0]
    uint16_t c11[4] = {1, 1, 2, 2};
    CHECK(nijinwvdc::apply_bayer_crop(src, 4, 4, c11, rggb, dst, dcfa));
    CHECK(dcfa[0] == 2 && dcfa[1] == 1 && dcfa[2] == 1 && dcfa[3] == 0);

    // Out-of-bounds crop rejected
    uint16_t bad_crop[4] = {3, 0, 2, 2};
    CHECK(!nijinwvdc::apply_bayer_crop(src, 4, 4, bad_crop, rggb, dst, dcfa));

    double cm[9] = {0};
    const char* known_models[] = {
        "NIKON Z 8", "NIKON Z 9", "NIKON Z6_3", "NIKON ZR", "NIKON Z f", "NIKON Z50_2", "NIKON Z5_2"
    };
    for (const char* m : known_models) {
        CHECK(nijinwvdc::lookup_nikon_d65_colormatrix(m, cm));
        CHECK(cm[0] > 0.8 && cm[4] > 1.0 && cm[8] > 0.5);
    }
    CHECK(!nijinwvdc::lookup_nikon_d65_colormatrix("UNKNOWN CAMERA", cm));
    CHECK(cm[0] > 1.0 && cm[4] > 1.0);

    const char* sony_models[] = {
        "ILCE-7M5", "ILCE-7RM5", "ILCE-7M4", "ILCE-7CR", "ILCE-1M2", "ILCE-9M3", "ILME-FX3"
    };
    for (const char* m : sony_models) {
        CHECK(nijinwvdc::lookup_camera_d65_colormatrix("SONY", m, cm));
        CHECK(cm[0] > 0.6 && cm[4] > 1.0 && cm[8] > 0.4);
    }
    CHECK(!nijinwvdc::lookup_camera_d65_colormatrix("SONY", "UNKNOWN MODEL", cm));
    CHECK(cm[0] > 0.8 && cm[4] > 1.0);
}

struct BitWriter {
    std::vector<uint8_t> bytes;
    uint8_t cur = 0;
    int bit_pos = 0;

    void push_bit(uint32_t b) {
        cur = static_cast<uint8_t>(cur | ((b & 1u) << (7 - bit_pos)));
        ++bit_pos;
        if (bit_pos == 8) {
            bytes.push_back(cur);
            cur = 0;
            bit_pos = 0;
        }
    }

    void push_bits(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            push_bit((v >> i) & 1u);
        }
    }

    std::vector<uint8_t> finish_padded(size_t align) {
        if (bit_pos > 0) {
            bytes.push_back(cur);
            cur = 0;
            bit_pos = 0;
        }
        while (bytes.empty() || (bytes.size() % align) != 0) {
            bytes.push_back(0);
        }
        return std::move(bytes);
    }
};

std::vector<uint8_t> build_synth_arw6_res_stream(
    int n_bands,
    int chunk_count,
    int32_t dc_val,
    size_t zero_chunk_bytes = 8) {
    std::vector<std::vector<uint8_t>> chunks(static_cast<size_t>(chunk_count));
    std::vector<uint8_t> payload;
    for (int ci = 0; ci < chunk_count; ++ci) {
        if (ci == 0 && dc_val != 0) {
            BitWriter bw;
            bw.push_bits(2u, 2); // '10' -> increment k by unary_zeros + 1
            for (int z = 0; z < 11; ++z) bw.push_bit(0);
            bw.push_bit(1); // k = 12
            uint32_t mag0 = static_cast<uint32_t>(dc_val < 0 ? -dc_val : dc_val);
            bw.push_bits(mag0, 12);
            bw.push_bits(0, 12);
            bw.push_bits(0, 12);
            bw.push_bits(0, 12);
            bw.push_bit(dc_val < 0 ? 1u : 0u);
            chunks[ci] = bw.finish_padded(std::max<size_t>(8u, zero_chunk_bytes));
        } else {
            chunks[ci].assign(zero_chunk_bytes, 0);
        }
        payload.insert(payload.end(), chunks[ci].begin(), chunks[ci].end());
    }
    while ((payload.size() % 16u) != 0u) {
        payload.push_back(0);
    }

    const uint32_t dir_blocks = 1;
    const uint32_t payload_blocks = static_cast<uint32_t>(payload.size() / 16u);

    BitWriter hdr;
    hdr.push_bits(dir_blocks, 16);
    hdr.push_bits(payload_blocks, 24);
    hdr.push_bits(4, 4); // magic4
    hdr.push_bits(0, 4); // zero4
    hdr.push_bits(static_cast<uint32_t>(n_bands), 2);
    hdr.push_bits(0, 6);
    hdr.push_bits(static_cast<uint32_t>(chunk_count), 16);
    hdr.push_bits(0x10u, 8);
    hdr.push_bits(0x00u, 8);
    hdr.push_bits(0, 32);
    hdr.push_bits(0, 8);
    std::vector<uint8_t> hbytes = hdr.finish_padded(16);

    BitWriter dir;
    for (int ci = 0; ci < chunk_count; ++ci) {
        dir.push_bits(static_cast<uint32_t>(chunks[ci].size()), 16);
        for (int b = 0; b < n_bands; ++b) {
            dir.push_bits(0, 4);
        }
    }
    std::vector<uint8_t> dbytes = dir.finish_padded(16);

    std::vector<uint8_t> stream;
    stream.reserve(hbytes.size() + dbytes.size() + payload.size());
    stream.insert(stream.end(), hbytes.begin(), hbytes.end());
    stream.insert(stream.end(), dbytes.begin(), dbytes.end());
    stream.insert(stream.end(), payload.begin(), payload.end());
    return stream;
}

std::vector<uint8_t> build_synth_arw6_wx16(
    uint16_t width_px,
    int32_t gl_dc,
    int32_t r_dc,
    int32_t b_dc,
    size_t zero_chunk_bytes = 8) {
    std::vector<uint8_t> streams[5][3];
    const int32_t dcs[3] = {gl_dc, r_dc, b_dc};
    uint32_t grp_bytes[4] = {0, 0, 0, 0};

    for (int r = 0; r < 4; ++r) {
        int nb = (r == 0) ? 1 : 3;
        int nchunks = (r == 0) ? 1 : 2;
        for (int c = 0; c < 3; ++c) {
            streams[r][c] = build_synth_arw6_res_stream(
                nb, nchunks, (r == 0) ? dcs[c] : 0, zero_chunk_bytes);
            grp_bytes[r] += static_cast<uint32_t>(streams[r][c].size());
        }
    }
    streams[4][0] = build_synth_arw6_res_stream(1, 2, 0, zero_chunk_bytes);

    std::vector<uint8_t> cs(128, 0);
    std::memcpy(cs.data(), "A000", 4);
    auto put_be16 = [&](size_t off, uint16_t v) {
        cs[off + 0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        cs[off + 1] = static_cast<uint8_t>(v & 0xFFu);
    };
    auto put_be24 = [&](size_t off, uint32_t v) {
        cs[off + 0] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        cs[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        cs[off + 2] = static_cast<uint8_t>(v & 0xFFu);
    };

    put_be16(8, width_px); // w
    put_be16(10, 8);       // h / 2
    put_be16(12, 14 << 4);
    cs[14] = (3u << 5) | (3u << 2) | 0x02u;
    cs[15] = 0x00u;

    for (int g = 0; g < 4; ++g) {
        put_be24(16 + static_cast<size_t>(g) * 3, grp_bytes[g] >> 4);
    }
    for (int g = 0; g < 5; ++g) {
        size_t rec = 48 + static_cast<size_t>(g) * 16;
        uint8_t cnt = (g < 4) ? 3u : 1u;
        cs[rec] = cnt;
        for (uint8_t j = 0; j < cnt; ++j) {
            put_be24(rec + 1 + static_cast<size_t>(j) * 3,
                     static_cast<uint32_t>(streams[g][j].size() >> 4));
        }
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            cs.insert(cs.end(), streams[r][c].begin(), streams[r][c].end());
        }
    }
    cs.insert(cs.end(), streams[4][0].begin(), streams[4][0].end());
    return cs;
}

std::vector<uint8_t> build_synth_arw6_16x16(int32_t gl_dc, int32_t r_dc, int32_t b_dc) {
    return build_synth_arw6_wx16(16, gl_dc, r_dc, b_dc, 8);
}

void test_arw6_synthetic_1tile_and_4tile() {
    std::string err;

    // 1) 16x16 linear-region synthetic tile: G_L=-700 (u=1348), R=+20 (u=1388), B=-20 (u=1308)
    auto cs_lin = build_synth_arw6_16x16(-700, 20, -20);
    nijinwvdc::Arw6Info info1, info4;
    CHECK(nijinwvdc::probe_arw6_codestream(cs_lin.data(), cs_lin.size(), info1, err));
    CHECK(info1.width == 16 && info1.height == 16 && info1.coded_depth == 14);

    std::vector<uint8_t> strip1(0x68, 0);
    put_le32(strip1, 0, 1); // tile_cnt = 1
    put_le32(strip1, 8, 0x68);
    put_le32(strip1, 16, 0); put_le32(strip1, 20, 0);
    put_le32(strip1, 24, 16); put_le32(strip1, 28, 16);
    strip1.insert(strip1.end(), cs_lin.begin(), cs_lin.end());

    std::vector<uint16_t> b1, b4;
    CHECK(nijinwvdc::decode_arw6_bayer(strip1.data(), strip1.size(), 1, b1, info1, err));
    CHECK(nijinwvdc::decode_arw6_bayer(strip1.data(), strip1.size(), 4, b4, info4, err));
    CHECK(b1.size() == 16u * 16u && b1 == b4);
    for (uint32_t r = 0; r < 8; ++r) {
        for (uint32_t c = 0; c < 8; ++c) {
            CHECK(b1[(r * 2) * 16 + c * 2] == 694);
            CHECK(b1[(r * 2) * 16 + c * 2 + 1] == 674);
            CHECK(b1[(r * 2 + 1) * 16 + c * 2] == 674);
            CHECK(b1[(r * 2 + 1) * 16 + c * 2 + 1] == 654);
        }
    }

    // Wide 64x16 (w_comp=32) negative-DC tile: exercises AVX2 16-quad main loop with negative sums
    auto cs_wide = build_synth_arw6_wx16(64, -700, 20, -20, 64);
    std::vector<uint16_t> wide1, wide4;
    CHECK(nijinwvdc::decode_arw6_tile(cs_wide.data(), cs_wide.size(), 1, 0, 0, 64, 16, wide1, info1, err));
    CHECK(nijinwvdc::decode_arw6_tile(cs_wide.data(), cs_wide.size(), 4, 0, 0, 64, 16, wide4, info4, err));
    CHECK(wide1.size() == 64u * 16u && wide1 == wide4);
    for (uint32_t r = 0; r < 8; ++r) {
        for (uint32_t c = 0; c < 32; ++c) {
            CHECK(wide1[(r * 2) * 64 + c * 2] == 694);
            CHECK(wide1[(r * 2) * 64 + c * 2 + 1] == 674);
            CHECK(wide1[(r * 2 + 1) * 64 + c * 2] == 674);
            CHECK(wide1[(r * 2 + 1) * 64 + c * 2 + 1] == 654);
        }
    }

    // 2) Nonlinear upper-LUT tile: G_L=+400 (u=2448), R=+100, B=-100
    auto cs_hi = build_synth_arw6_16x16(400, 100, -100);
    std::vector<uint16_t> hi1, hi4;
    CHECK(nijinwvdc::decode_arw6_tile(cs_hi.data(), cs_hi.size(), 1, 0, 0, 16, 16, hi1, info1, err));
    CHECK(nijinwvdc::decode_arw6_tile(cs_hi.data(), cs_hi.size(), 4, 0, 0, 16, 16, hi4, info4, err));
    CHECK(hi1 == hi4);
    CHECK(hi1[0] > hi1[1] && hi1[1] == hi1[16] && hi1[16] > hi1[17]);
    CHECK(hi1[1] > 1224);

    // 3) 4-tile 32x32 TIFF ARW6 -> decode -> crop -> DNG roundtrip
    auto cs_q1 = build_synth_arw6_16x16(-750, 10, -10);
    auto cs_q2 = build_synth_arw6_16x16(-650, 15, -15);
    auto cs_q3 = build_synth_arw6_16x16(-620, 20, -20);
    const std::vector<uint8_t>* qcs[4] = {&cs_lin, &cs_q1, &cs_q2, &cs_q3};
    const uint32_t qxy[4][2] = {{0, 0}, {16, 0}, {0, 16}, {16, 16}};
    std::vector<uint8_t> strip4(0x68, 0);
    put_le32(strip4, 0, 4);
    uint32_t cur = 0x68;
    for (int i = 0; i < 4; ++i) {
        size_t rec = 8 + static_cast<size_t>(i) * 24;
        put_le32(strip4, rec + 0, cur);
        put_le32(strip4, rec + 8, qxy[i][0]);
        put_le32(strip4, rec + 12, qxy[i][1]);
        put_le32(strip4, rec + 16, 16);
        put_le32(strip4, rec + 20, 16);
        cur += static_cast<uint32_t>(qcs[i]->size());
    }
    for (int i = 0; i < 4; ++i) {
        strip4.insert(strip4.end(), qcs[i]->begin(), qcs[i]->end());
    }

    // Wrap strip4 in minimal ARW6 TIFF container
    const char make_str[] = "SONY";
    const char model_str[] = "ILCE-7M5";
    std::vector<uint8_t> arw(512 + strip4.size(), 0);
    arw[0] = 'I'; arw[1] = 'I'; put_le16(arw, 2, 42); put_le32(arw, 4, 8);
    // IFD0 @ 8 (3 entries: Make=271, Model=272, SubIFDs=330 -> 64)
    put_le16(arw, 8, 3);
    auto put_ifd0 = [&](size_t idx, uint16_t tag, uint16_t dt, uint32_t cnt, uint32_t val) {
        size_t o = 10 + idx * 12;
        put_le16(arw, o + 0, tag); put_le16(arw, o + 2, dt);
        put_le32(arw, o + 4, cnt); put_le32(arw, o + 8, val);
    };
    put_ifd0(0, 271, 2, 5, 48);
    put_ifd0(1, 272, 2, 9, 54);
    put_ifd0(2, 330, 4, 1, 64);
    std::memcpy(&arw[48], make_str, 5);
    std::memcpy(&arw[54], model_str, 9);

    // SubIFD @ 64 (12 entries)
    const size_t strip_off = 512;
    const size_t data_pool = 240;
    put_le16(arw, 64, 12);
    auto put_sub = [&](size_t idx, uint16_t tag, uint16_t dt, uint32_t cnt, uint32_t val) {
        size_t o = 66 + idx * 12;
        put_le16(arw, o + 0, tag); put_le16(arw, o + 2, dt);
        put_le32(arw, o + 4, cnt); put_le32(arw, o + 8, val);
    };
    put_sub(0, 256, 4, 1, 32);
    put_sub(1, 257, 4, 1, 32);
    put_sub(2, 258, 3, 1, 14);
    put_sub(3, 259, 3, 1, 32766);
    put_sub(4, 262, 3, 1, 32803);
    put_sub(5, 273, 4, 1, static_cast<uint32_t>(strip_off));
    put_sub(6, 279, 4, 1, static_cast<uint32_t>(strip4.size()));
    put_sub(7, 33422, 1, 4, 0x02010100u); // RGGB
    put_sub(8, 0x7000, 3, 1, 6);
    put_sub(9, 0x7310, 3, 4, static_cast<uint32_t>(data_pool));
    put_sub(10, 50719, 4, 2, static_cast<uint32_t>(data_pool + 8));
    put_sub(11, 50720, 4, 2, static_cast<uint32_t>(data_pool + 16));
    for (size_t i = 0; i < 4; ++i) put_le16(arw, data_pool + i * 2, 512);
    put_le32(arw, data_pool + 8, 2);  put_le32(arw, data_pool + 12, 2);
    put_le32(arw, data_pool + 16, 28); put_le32(arw, data_pool + 20, 28);
    std::memcpy(&arw[strip_off], strip4.data(), strip4.size());

    std::string arw_path = temp_file_path("nijin_synth_4tile.arw");
    std::string dng_path = temp_file_path("nijin_synth_4tile.dng");
    CHECK(write_bytes(arw_path, arw));

    nijinwvdc::NevClipInfo clip;
    CHECK(nijinwvdc::parse_nev_clip(arw_path, clip, err));
    CHECK(clip.major_brand == "ARW");
    CHECK(clip.tracks.size() == 1 && clip.tracks[0].format == "ARW6");

    nijinwvdc::NevFrameMeta meta;
    std::vector<uint8_t> loaded_strip;
    CHECK(nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, meta, &loaded_strip, err));
    CHECK(meta.has_crop && meta.crop[2] == 28 && meta.crop[3] == 28);

    std::vector<uint16_t> fb1, fb4;
    nijinwvdc::Arw6Info finfo1, finfo4;
    CHECK(nijinwvdc::decode_arw6_bayer(loaded_strip.data(), loaded_strip.size(), 1, fb1, finfo1, err));
    CHECK(nijinwvdc::decode_arw6_bayer(loaded_strip.data(), loaded_strip.size(), 4, fb4, finfo4, err));
    CHECK(finfo4.tiles.size() == 4 && finfo4.width == 32 && finfo4.height == 32);
    CHECK(fb1 == fb4);
    CHECK(fb1[0 * 32 + 1] == 674);
    CHECK(fb1[0 * 32 + 17] == 649);
    CHECK(fb1[16 * 32 + 1] == 699);
    CHECK(fb1[16 * 32 + 17] == 714);

    nijinwvdc::DngWriteOptions dopts;
    nijinwvdc::build_dng_options_from_clip_and_frame(clip, meta, finfo4, true, 1, dopts);
    std::vector<uint16_t> cropped;
    CHECK(nijinwvdc::apply_bayer_crop(fb4.data(), finfo4.width, finfo4.height, meta.crop, meta.cfa_pattern, cropped, dopts.cfa_pattern));
    dopts.width = meta.crop[2];
    dopts.height = meta.crop[3];
    dopts.has_crop = false;
    CHECK(nijinwvdc::write_cfa_dng(dng_path, cropped.data(), dopts, err));
    auto dng_bytes = read_bytes(dng_path);
    CHECK(dng_bytes.size() > 28u * 28u * 2u);
    std::remove(arw_path.c_str());
    std::remove(dng_path.c_str());
}

void test_dng_roundtrip_synthetic() {
    std::string err;
    const uint32_t w = 64, h = 48;
    std::vector<uint16_t> pixels(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint16_t>(1008u + (i % 15000u));
    }

    auto verify_dng_case = [&](const char* fname, const uint16_t blk[4], bool expect_scalar_black) {
        std::string p = temp_file_path(fname);
        nijinwvdc::DngWriteOptions opts;
        opts.width = w;
        opts.height = h;
        opts.bit_depth = 14;
        opts.white = 16383;
        std::memcpy(opts.black, blk, sizeof(opts.black));
        opts.has_crop = true;
        opts.crop_origin[0] = 2; opts.crop_origin[1] = 2;
        opts.crop_size[0] = 60; opts.crop_size[1] = 44;
        opts.make = "NIKON CORPORATION";
        opts.model = "NIKON Z 8";
        opts.unique_model = "NIKON Z 8";
        CHECK(nijinwvdc::write_cfa_dng(p, pixels.data(), opts, err));

        std::vector<uint8_t> raw = read_bytes(p);
        CHECK(raw.size() >= 16);
        CHECK(raw[0] == 'I' && raw[1] == 'I' && rd_le16(&raw[2]) == 42);
        uint32_t ifd_off = rd_le32(&raw[4]);
        uint16_t cnt = rd_le16(&raw[ifd_off]);
        CHECK(cnt >= 16);

        uint16_t prev_tag = 0;
        bool saw_50713 = false;
        bool saw_50714 = false;
        uint32_t strip_off = 0, strip_cnt = 0;
        for (uint16_t i = 0; i < cnt; ++i) {
            const uint8_t* ent = &raw[ifd_off + 2 + static_cast<size_t>(i) * 12];
            uint16_t tag = rd_le16(ent + 0);
            uint16_t type = rd_le16(ent + 2);
            uint32_t count = rd_le32(ent + 4);
            uint32_t vo = rd_le32(ent + 8);
            CHECK(i == 0 || tag > prev_tag);
            prev_tag = tag;
            if (tag == 273) strip_off = vo;
            if (tag == 279) strip_cnt = vo;
            if (tag == 50713) saw_50713 = true;
            if (tag == 50714) {
                saw_50714 = true;
                CHECK(type == 5);
                if (expect_scalar_black) {
                    CHECK(count == 1);
                    CHECK(rd_le32(&raw[vo]) == blk[0] && rd_le32(&raw[vo + 4]) == 1u);
                } else {
                    CHECK(count == 4);
                    for (int c = 0; c < 4; ++c) {
                        CHECK(rd_le32(&raw[vo + 8 * c]) == blk[c]);
                    }
                }
            }
        }
        CHECK(saw_50714);
        CHECK(saw_50713 == !expect_scalar_black);
        CHECK(strip_cnt == w * h * 2u);
        CHECK(strip_off + strip_cnt == raw.size());
        CHECK(std::memcmp(&raw[strip_off], pixels.data(), strip_cnt) == 0);
        std::remove(p.c_str());
    };

    const uint16_t uniform_blk[4] = {1008, 1008, 1008, 1008};
    const uint16_t split_blk[4] = {1008, 1009, 1009, 1010};
    verify_dng_case("nijin_selftest_uniform.dng", uniform_blk, true);
    verify_dng_case("nijin_selftest_split.dng", split_blk, false);
}

void write_be32_u8(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

std::vector<uint8_t> make_box(const char type[4], const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(8u + payload.size());
    write_be32_u8(out, static_cast<uint32_t>(8u + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> build_synth_arw_4tile_tiff() {
    auto cs0 = build_synth_arw6_16x16(-700, 20, -20);
    auto cs1 = build_synth_arw6_16x16(-750, 10, -10);
    auto cs2 = build_synth_arw6_16x16(-650, 15, -15);
    auto cs3 = build_synth_arw6_16x16(-620, 20, -20);
    const std::vector<uint8_t>* qcs[4] = {&cs0, &cs1, &cs2, &cs3};
    const uint32_t qxy[4][2] = {{0, 0}, {16, 0}, {0, 16}, {16, 16}};
    std::vector<uint8_t> strip4(0x68, 0);
    put_le32(strip4, 0, 4);
    uint32_t cur = 0x68;
    for (int i = 0; i < 4; ++i) {
        size_t rec = 8 + static_cast<size_t>(i) * 24;
        put_le32(strip4, rec + 0, cur);
        put_le32(strip4, rec + 8, qxy[i][0]);
        put_le32(strip4, rec + 12, qxy[i][1]);
        put_le32(strip4, rec + 16, 16);
        put_le32(strip4, rec + 20, 16);
        cur += static_cast<uint32_t>(qcs[i]->size());
    }
    for (int i = 0; i < 4; ++i) {
        strip4.insert(strip4.end(), qcs[i]->begin(), qcs[i]->end());
    }

    const char make_str[] = "SONY";
    const char model_str[] = "ILCE-7M5";
    std::vector<uint8_t> arw(512 + strip4.size(), 0);
    arw[0] = 'I'; arw[1] = 'I'; put_le16(arw, 2, 42); put_le32(arw, 4, 8);
    put_le16(arw, 8, 3);
    auto put_ifd0 = [&](size_t idx, uint16_t tag, uint16_t dt, uint32_t cnt, uint32_t val) {
        size_t o = 10 + idx * 12;
        put_le16(arw, o + 0, tag); put_le16(arw, o + 2, dt);
        put_le32(arw, o + 4, cnt); put_le32(arw, o + 8, val);
    };
    put_ifd0(0, 271, 2, 5, 48);
    put_ifd0(1, 272, 2, 9, 54);
    put_ifd0(2, 330, 4, 1, 64);
    std::memcpy(&arw[48], make_str, 5);
    std::memcpy(&arw[54], model_str, 9);

    const size_t strip_off = 512;
    const size_t data_pool = 240;
    put_le16(arw, 64, 12);
    auto put_sub = [&](size_t idx, uint16_t tag, uint16_t dt, uint32_t cnt, uint32_t val) {
        size_t o = 66 + idx * 12;
        put_le16(arw, o + 0, tag); put_le16(arw, o + 2, dt);
        put_le32(arw, o + 4, cnt); put_le32(arw, o + 8, val);
    };
    put_sub(0, 256, 4, 1, 32);
    put_sub(1, 257, 4, 1, 32);
    put_sub(2, 258, 3, 1, 14);
    put_sub(3, 259, 3, 1, 32766);
    put_sub(4, 262, 3, 1, 32803);
    put_sub(5, 273, 4, 1, static_cast<uint32_t>(strip_off));
    put_sub(6, 279, 4, 1, static_cast<uint32_t>(strip4.size()));
    put_sub(7, 33422, 1, 4, 0x02010100u);
    put_sub(8, 0x7000, 3, 1, 6);
    put_sub(9, 0x7310, 3, 4, static_cast<uint32_t>(data_pool));
    put_sub(10, 50719, 4, 2, static_cast<uint32_t>(data_pool + 8));
    put_sub(11, 50720, 4, 2, static_cast<uint32_t>(data_pool + 16));
    for (size_t i = 0; i < 4; ++i) put_le16(arw, data_pool + i * 2, 512);
    put_le32(arw, data_pool + 8, 2);  put_le32(arw, data_pool + 12, 2);
    put_le32(arw, data_pool + 16, 28); put_le32(arw, data_pool + 20, 28);
    std::memcpy(&arw[strip_off], strip4.data(), strip4.size());
    return arw;
}

void test_session_reader_postproc_and_threading() {
    std::string err;
    CHECK(std::strcmp(nijinwvdc::version_string(), NIJINWVDC_VERSION_STRING) == 0);
    const uint32_t caps = nijinwvdc::simd_capabilities();
#if defined(__aarch64__) || defined(_M_ARM64)
    CHECK((caps & NIJIN_SIMD_ARM_NEON) != 0u);
#elif defined(__x86_64__) || defined(_M_X64)
    CHECK((caps & NIJIN_SIMD_X86_SSE2) != 0u);
#else
    (void)caps;
#endif

    // 1) MediaReader open_memory + IoCallbacks (small bounded reads) + bare A000 + DecoderSession reuse/strided/ROI
    auto cs_tile = build_synth_arw6_16x16(-700, 20, -20);
    auto cs_tile_hi = build_synth_arw6_16x16(400, 100, -100);
    auto arw_tiff = build_synth_arw_4tile_tiff();

    nijinwvdc::MediaReader r_mem;
    CHECK(r_mem.open_memory(arw_tiff.data(), arw_tiff.size(), err));
    CHECK(r_mem.container_format() == nijinwvdc::ContainerFormat::TiffArw);
    CHECK(r_mem.codec_family() == nijinwvdc::CodecFamily::Arw6);
    CHECK(r_mem.frame_count() == 1u);

    struct ChunkedMem {
        const std::vector<uint8_t>* buf;
    } cctx{&arw_tiff};
    nijinwvdc::IoCallbacks cb;
    cb.user_data = &cctx;
    cb.file_size = arw_tiff.size();
    cb.read_at = [](void* ud, uint64_t off, void* dst, size_t sz, size_t* got) -> bool {
        const auto* ctx = static_cast<const ChunkedMem*>(ud);
        if (off > ctx->buf->size() || sz > ctx->buf->size() - off) return false;
        size_t pos = 0;
        while (pos < sz) {
            size_t step = std::min<size_t>(37u, sz - pos);
            std::memcpy(static_cast<uint8_t*>(dst) + pos, ctx->buf->data() + off + pos, step);
            pos += step;
        }
        *got = sz;
        return true;
    };
    nijinwvdc::MediaReader r_cb;
    CHECK(r_cb.open_callbacks(cb, err));

    nijinwvdc::MediaReader r_a000;
    CHECK(r_a000.open_memory(cs_tile.data(), cs_tile.size(), err));
    CHECK(r_a000.container_format() == nijinwvdc::ContainerFormat::RawCodestream);
    CHECK(r_a000.codec_family() == nijinwvdc::CodecFamily::Arw6);

    nijinwvdc::DecoderSession sess(2);
    std::vector<uint16_t> b_mem, b_cb, b_tile, b_hi;
    nijinwvdc::DecodedFrameInfo di_mem, di_cb, di_tile, di_hi;
    nijinwvdc::DecodeOptions dopts;
    CHECK(sess.decode_frame(r_mem, 0, dopts, b_mem, di_mem, err));
    CHECK(sess.decode_frame(r_cb, 0, dopts, b_cb, di_cb, err));
    CHECK(b_mem == b_cb);
    CHECK(sess.decode_frame(r_a000, 0, dopts, b_tile, di_tile, err));
    CHECK(di_tile.coded_width == 16u && di_tile.coded_height == 16u && b_tile[0] == 694u);
    CHECK(sess.decode_codestream(cs_tile_hi.data(), cs_tile_hi.size(), dopts, b_hi, di_hi, err));
    CHECK(b_hi[0] > b_hi[1]);

    // Padded strided decode with canary guard words
    const size_t pad_u16_stride = 40u; // 80 bytes (> 32 * 2)
    std::vector<uint16_t> strided_buf(16u + 32u * pad_u16_stride + 16u, 0xBEEFu);
    nijinwvdc::DecodedFrameInfo di_strided;
    CHECK(sess.decode_frame_into(
        r_mem, 0, dopts, strided_buf.data() + 16u, pad_u16_stride * sizeof(uint16_t), 32u, di_strided, err));
    for (size_t i = 0; i < 16u; ++i) {
        CHECK(strided_buf[i] == 0xBEEFu);
        CHECK(strided_buf[16u + 32u * pad_u16_stride + i] == 0xBEEFu);
    }
    for (uint32_t y = 0; y < 32u; ++y) {
        for (uint32_t x = 0; x < 32u; ++x) {
            CHECK(strided_buf[16u + y * pad_u16_stride + x] == b_mem[y * 32u + x]);
        }
        for (uint32_t x = 32u; x < pad_u16_stride; ++x) {
            CHECK(strided_buf[16u + y * pad_u16_stride + x] == 0xBEEFu);
        }
    }

    // Active crop + arbitrary odd ROI parity
    nijinwvdc::DecodeOptions dopts_crop;
    dopts_crop.apply_active_crop = true;
    std::vector<uint16_t> b_crop, b_crop_ref;
    nijinwvdc::DecodedFrameInfo di_crop;
    CHECK(sess.decode_frame(r_mem, 0, dopts_crop, b_crop, di_crop, err));
    uint8_t ref_cfa[4] = {0};
    CHECK(nijinwvdc::apply_bayer_crop(
        b_mem.data(), 32u, 32u, di_mem.meta.crop, di_mem.meta.cfa_pattern, b_crop_ref, ref_cfa));
    CHECK(di_crop.output_width == 28u && di_crop.output_height == 28u && b_crop == b_crop_ref);

    nijinwvdc::DecodeOptions dopts_roi;
    dopts_roi.has_roi = true;
    dopts_roi.want_quant_profile = true;
    dopts_roi.roi[0] = 3; dopts_roi.roi[1] = 3; dopts_roi.roi[2] = 22; dopts_roi.roi[3] = 20;
    std::vector<uint16_t> b_roi, b_roi_ref;
    nijinwvdc::DecodedFrameInfo di_roi;
    CHECK(sess.decode_frame(r_mem, 0, dopts_roi, b_roi, di_roi, err));
    const uint16_t roi16[4] = {3, 3, 22, 20};
    CHECK(nijinwvdc::apply_bayer_crop(b_mem.data(), 32u, 32u, roi16, di_mem.meta.cfa_pattern, b_roi_ref, ref_cfa));
    CHECK(di_roi.output_width == 22u && di_roi.output_height == 20u);
    CHECK(std::memcmp(di_roi.cfa_pattern, ref_cfa, 4) == 0);
    CHECK(b_roi == b_roi_ref);
    CHECK(di_roi.quant_profile.available);
    CHECK(di_roi.quant_profile.codec == nijinwvdc::CodecFamily::Arw6);
    CHECK(di_roi.quant_profile.row_step == 8u);
    CHECK(di_roi.quant_profile.num_rows == 3u);
    {
        float wsum = 0.0f, psum = 0.0f;
        for (int k = 0; k < 16; ++k) wsum += di_roi.quant_profile.walsh16_luma_norm[k];
        for (int k = 0; k < 6; ++k) psum += di_roi.quant_profile.luma_scale_psd[k];
        CHECK(wsum > 15.5f && wsum < 16.5f);
        CHECK(psum > 0.99f && psum < 1.01f);
        CHECK(di_roi.quant_profile.nlt_slope_lut[240] > di_roi.quant_profile.nlt_slope_lut[4]);
        auto qp_cal = di_roi.quant_profile;
        nijinwvdc::recalibrate_quant_profile_range(qp_cal, 512.0f, 16383.0f);
        CHECK(qp_cal.black_ref == 512.0f && qp_cal.white_ref == 16383.0f);
        CHECK(qp_cal.nlt_slope_lut[240] > qp_cal.nlt_slope_lut[4]);
    }

    // Concurrent multi-thread decode sharing one MediaReader
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&r_mem, &b_mem]() {
            nijinwvdc::DecoderSession tsess(2);
            nijinwvdc::DecodeOptions topts;
            std::vector<uint16_t> tb;
            nijinwvdc::DecodedFrameInfo tinfo;
            std::string terr;
            for (int iter = 0; iter < 8; ++iter) {
                CHECK(tsess.decode_frame(r_mem, 0, topts, tb, tinfo, terr));
                CHECK(tb == b_mem);
            }
        });
    }
    for (auto& w : workers) w.join();

    // 2) SIMD split_bayer_cfa_planes + normalize_bayer_to_float
    const uint32_t sw = 100, sh = 6, s_stride_u16 = 108;
    std::vector<uint16_t> s_pix(sh * s_stride_u16, 0x5555u);
    for (uint32_t y = 0; y < sh; ++y) {
        for (uint32_t x = 0; x < sw; ++x) {
            uint16_t v = static_cast<uint16_t>((y * 1021u + x * 673u) & 0xFFFFu);
            if (x == 0 && y == 0) v = 0xFFFFu;
            if (x == 1 && y == 0) v = 0x8000u;
            if (x == 2 && y == 0) v = 0x7FFFu;
            if (x == 3 && y == 0) v = 0x0000u;
            s_pix[y * s_stride_u16 + x] = v;
        }
    }
    const size_t half_w = sw / 2, half_h = sh / 2, p_stride_u16 = 56;
    std::vector<uint16_t> p00(half_h * p_stride_u16, 0), p01(half_h * p_stride_u16, 0),
                          p10(half_h * p_stride_u16, 0), p11(half_h * p_stride_u16, 0);
    CHECK(nijinwvdc::split_bayer_cfa_planes(
        s_pix.data(), s_stride_u16 * 2u, sw, sh,
        p00.data(), p01.data(), p10.data(), p11.data(), p_stride_u16 * 2u));
    for (uint32_t py = 0; py < half_h; ++py) {
        for (uint32_t px = 0; px < half_w; ++px) {
            CHECK(p00[py * p_stride_u16 + px] == s_pix[(2u * py) * s_stride_u16 + 2u * px]);
            CHECK(p01[py * p_stride_u16 + px] == s_pix[(2u * py) * s_stride_u16 + 2u * px + 1u]);
            CHECK(p10[py * p_stride_u16 + px] == s_pix[(2u * py + 1u) * s_stride_u16 + 2u * px]);
            CHECK(p11[py * p_stride_u16 + px] == s_pix[(2u * py + 1u) * s_stride_u16 + 2u * px + 1u]);
        }
    }

    const uint32_t nw = 38, nh = 3;
    std::vector<uint16_t> nsrc(nw * nh);
    for (size_t i = 0; i < nsrc.size(); ++i) nsrc[i] = static_cast<uint16_t>((i * 457u) % 16384u);
    const float fblk[4] = {100.0f, 200.0f, 300.0f, 400.0f};
    const float fwhite = 10000.0f;
    std::vector<float> fclamp(nw * nh, -1.0f), funclamp(nw * nh, -1.0f);
    CHECK(nijinwvdc::normalize_bayer_to_float(nsrc.data(), 0, nw, nh, fblk, fwhite, true, fclamp.data(), 0));
    CHECK(nijinwvdc::normalize_bayer_to_float(nsrc.data(), 0, nw, nh, fblk, fwhite, false, funclamp.data(), 0));
    for (uint32_t y = 0; y < nh; ++y) {
        for (uint32_t x = 0; x < nw; ++x) {
            const uint32_t ph = ((y & 1u) * 2u) | (x & 1u);
            const float ref = (static_cast<float>(nsrc[y * nw + x]) - fblk[ph]) / (fwhite - fblk[ph]);
            const float ref_c = (ref < 0.0f) ? 0.0f : ((ref > 1.0f) ? 1.0f : ref);
            CHECK(std::fabs(funclamp[y * nw + x] - ref) < 1e-5f);
            CHECK(std::fabs(fclamp[y * nw + x] - ref_c) < 1e-5f);
        }
    }

    // 3) Synthetic MOV (vide + BE soun lpcm split across chunks + drop-frame tmcd) -> timing + audio + WAV
    std::vector<uint8_t> pcm_data;
    const uint32_t total_aframes = 512;
    for (uint32_t i = 0; i < total_aframes; ++i) {
        int16_t l = static_cast<int16_t>(1000 + i);
        int16_t r = static_cast<int16_t>(-2000 - i);
        pcm_data.push_back(static_cast<uint8_t>((static_cast<uint16_t>(l) >> 8) & 0xFFu));
        pcm_data.push_back(static_cast<uint8_t>(static_cast<uint16_t>(l) & 0xFFu));
        pcm_data.push_back(static_cast<uint8_t>((static_cast<uint16_t>(r) >> 8) & 0xFFu));
        pcm_data.push_back(static_cast<uint8_t>(static_cast<uint16_t>(r) & 0xFFu));
    }
    const uint32_t tmcd_vals[3] = {1799u, 1800u, 17982u};
    std::vector<uint8_t> tmcd_raw;
    for (uint32_t tv : tmcd_vals) write_be32_u8(tmcd_raw, tv);

    auto make_stco_single = [](uint32_t off) {
        std::vector<uint8_t> p(12, 0);
        put_le32(p, 0, 0);
        p[7] = 1;
        p[8] = static_cast<uint8_t>((off >> 24) & 0xFFu);
        p[9] = static_cast<uint8_t>((off >> 16) & 0xFFu);
        p[10] = static_cast<uint8_t>((off >> 8) & 0xFFu);
        p[11] = static_cast<uint8_t>(off & 0xFFu);
        return make_box("stco", p);
    };

    // Reserve known mdat offset >= 4096 inside a synthetic MOV
    std::vector<uint8_t> ftyp_pay = {'n', 'i', 'k', 'o', 0, 0, 0, 1};
    auto box_ftyp = make_box("ftyp", ftyp_pay);
    std::vector<uint8_t> mdat_pay(32 + pcm_data.size() + tmcd_raw.size(), 0);
    const size_t pcm_rel0 = 32;
    const size_t pcm_rel1 = 32 + 300 * 4;
    const size_t tc_rel = 32 + pcm_data.size();
    std::memcpy(&mdat_pay[pcm_rel0], pcm_data.data(), pcm_data.size());
    std::memcpy(&mdat_pay[tc_rel], tmcd_raw.data(), tmcd_raw.size());

    auto build_mov = [&](uint32_t mdat_abs_off) {
        std::vector<uint8_t> mvhd_p(24, 0);
        mvhd_p[14] = 0x75; mvhd_p[15] = 0x30; // timescale = 30000

        // vide trak
        std::vector<uint8_t> v_mdhd(24, 0); v_mdhd[14] = 0x75; v_mdhd[15] = 0x30;
        std::vector<uint8_t> v_hdlr(12, 0); std::memcpy(&v_hdlr[8], "vide", 4);
        std::vector<uint8_t> v_stsd(94, 0); v_stsd[7] = 1; v_stsd[11] = 86;
        std::memcpy(&v_stsd[12], "NRAW", 4);
        v_stsd[40] = 0; v_stsd[41] = 16; v_stsd[42] = 0; v_stsd[43] = 16;
        std::vector<uint8_t> v_stts(24, 0);
        v_stts[7] = 2;
        v_stts[11] = 2; v_stts[14] = 0x03; v_stts[15] = 0xE9; // 2 * 1001
        v_stts[19] = 1; v_stts[22] = 0x07; v_stts[23] = 0xD2; // 1 * 2002
        std::vector<uint8_t> v_stsc(20, 0); v_stsc[7] = 1; v_stsc[11] = 1; v_stsc[15] = 3;
        std::vector<uint8_t> v_stsz(12, 0); v_stsz[7] = 8; v_stsz[11] = 3;
        auto v_stbl = make_box("stbl", [&]() {
            std::vector<uint8_t> all;
            for (auto& b : {make_box("stsd", v_stsd), make_box("stts", v_stts),
                            make_box("stsc", v_stsc), make_box("stsz", v_stsz),
                            make_stco_single(mdat_abs_off)}) {
                all.insert(all.end(), b.begin(), b.end());
            }
            return all;
        }());
        std::vector<uint8_t> v_trak_body;
        for (auto& b : {make_box("mdhd", v_mdhd), make_box("hdlr", v_hdlr), make_box("minf", v_stbl)}) {
            v_trak_body.insert(v_trak_body.end(), b.begin(), b.end());
        }

        // soun trak (lpcm v2 big-endian 16-bit 2ch 48kHz across 2 chunks: 300, 212 frames)
        std::vector<uint8_t> s_mdhd(24, 0); s_mdhd[14] = 0xBB; s_mdhd[15] = 0x80; // 48000
        std::vector<uint8_t> s_hdlr(12, 0); std::memcpy(&s_hdlr[8], "soun", 4);
        std::vector<uint8_t> s_stsd(80, 0); s_stsd[7] = 1; s_stsd[11] = 72;
        std::memcpy(&s_stsd[12], "lpcm", 4);
        s_stsd[25] = 2; // ver=2 @ +16 (+8 header = 24)
        const double sr = 48000.0;
        uint64_t sr_u = 0; std::memcpy(&sr_u, &sr, 8);
        for (int b = 0; b < 8; ++b) s_stsd[48 + b] = static_cast<uint8_t>((sr_u >> ((7 - b) * 8)) & 0xFFu);
        s_stsd[59] = 2;  // ch
        s_stsd[67] = 16; // bits
        s_stsd[71] = 0x0Eu; // flags: big-endian(2) | signed(4) | packed(8)
        s_stsd[75] = 4;  // bytesPerPacket
        std::vector<uint8_t> s_stsc(32, 0); s_stsc[7] = 2;
        s_stsc[11] = 1; s_stsc[14] = 0x01; s_stsc[15] = 0x2C; // chunk 1: 300
        s_stsc[23] = 2; s_stsc[27] = 212;                      // chunk 2: 212
        std::vector<uint8_t> s_stsz(12, 0); s_stsz[7] = 4; s_stsz[10] = 0x02; s_stsz[11] = 0x00; // 512
        std::vector<uint8_t> s_stco(16, 0); s_stco[7] = 2;
        uint32_t co0 = mdat_abs_off + static_cast<uint32_t>(pcm_rel0);
        uint32_t co1 = mdat_abs_off + static_cast<uint32_t>(pcm_rel1);
        for (int b = 0; b < 4; ++b) {
            s_stco[8 + b] = static_cast<uint8_t>((co0 >> ((3 - b) * 8)) & 0xFFu);
            s_stco[12 + b] = static_cast<uint8_t>((co1 >> ((3 - b) * 8)) & 0xFFu);
        }
        auto s_stbl = make_box("stbl", [&]() {
            std::vector<uint8_t> all;
            for (auto& b : {make_box("stsd", s_stsd), make_box("stsc", s_stsc),
                            make_box("stsz", s_stsz), make_box("stco", s_stco)}) {
                all.insert(all.end(), b.begin(), b.end());
            }
            return all;
        }());
        std::vector<uint8_t> s_trak_body;
        for (auto& b : {make_box("mdhd", s_mdhd), make_box("hdlr", s_hdlr), make_box("minf", s_stbl)}) {
            s_trak_body.insert(s_trak_body.end(), b.begin(), b.end());
        }

        // tmcd trak (drop-frame 29.97, 30 nf)
        std::vector<uint8_t> t_hdlr(12, 0); std::memcpy(&t_hdlr[8], "tmcd", 4);
        std::vector<uint8_t> t_stsd(44, 0); t_stsd[7] = 1; t_stsd[11] = 36;
        std::memcpy(&t_stsd[12], "tmcd", 4);
        t_stsd[31] = 1; // flags=1 (drop-frame) @ +20 (+8=28..31)
        t_stsd[40] = 30; // nf @ +32 (+8=40)
        std::vector<uint8_t> t_stsc(20, 0); t_stsc[7] = 1; t_stsc[11] = 1; t_stsc[15] = 3;
        std::vector<uint8_t> t_stsz(12, 0); t_stsz[7] = 4; t_stsz[11] = 3;
        auto t_stbl = make_box("stbl", [&]() {
            std::vector<uint8_t> all;
            for (auto& b : {make_box("stsd", t_stsd), make_box("stsc", t_stsc),
                            make_box("stsz", t_stsz),
                            make_stco_single(mdat_abs_off + static_cast<uint32_t>(tc_rel))}) {
                all.insert(all.end(), b.begin(), b.end());
            }
            return all;
        }());
        std::vector<uint8_t> t_trak_body;
        for (auto& b : {make_box("hdlr", t_hdlr), make_box("minf", t_stbl)}) {
            t_trak_body.insert(t_trak_body.end(), b.begin(), b.end());
        }

        std::vector<uint8_t> moov_body;
        for (auto& b : {make_box("mvhd", mvhd_p), make_box("trak", v_trak_body),
                        make_box("trak", s_trak_body), make_box("trak", t_trak_body)}) {
            moov_body.insert(moov_body.end(), b.begin(), b.end());
        }
        auto box_moov = make_box("moov", moov_body);
        auto box_mdat = make_box("mdat", mdat_pay);
        std::vector<uint8_t> mov;
        mov.insert(mov.end(), box_ftyp.begin(), box_ftyp.end());
        mov.insert(mov.end(), box_moov.begin(), box_moov.end());
        mov.insert(mov.end(), box_mdat.begin(), box_mdat.end());
        return mov;
    };

    auto mov_probe = build_mov(0);
    const uint32_t mdat_pay_off = static_cast<uint32_t>(mov_probe.size() - mdat_pay.size());
    auto mov = build_mov(mdat_pay_off);

    nijinwvdc::MediaReader r_mov;
    CHECK(r_mov.open_memory(mov.data(), mov.size(), err));
    CHECK(r_mov.container_format() == nijinwvdc::ContainerFormat::Mp4Nev);
    CHECK(r_mov.frame_count() == 3u);

    nijinwvdc::VideoFrameTiming ft0, ft1, ft2;
    CHECK(r_mov.get_frame_timing(0, ft0, err));
    CHECK(r_mov.get_frame_timing(1, ft1, err));
    CHECK(r_mov.get_frame_timing(2, ft2, err));
    CHECK(ft0.pts_ticks == 0 && ft0.duration_ticks == 1001 && ft0.has_timecode && ft0.drop_frame);
    CHECK(std::strcmp(ft0.timecode_str, "00:00:59;29") == 0);
    CHECK(ft1.pts_ticks == 1001 && std::strcmp(ft1.timecode_str, "00:01:00;02") == 0);
    CHECK(ft2.pts_ticks == 2002 && ft2.duration_ticks == 2002 && std::strcmp(ft2.timecode_str, "00:10:00;00") == 0);

    const auto ainfo = r_mov.audio_info();
    CHECK(ainfo.available && ainfo.channels == 2u && ainfo.bits_per_channel == 16u);
    CHECK(ainfo.is_big_endian && ainfo.is_signed && !ainfo.is_float && ainfo.total_frames == total_aframes);

    std::vector<uint8_t> pcm_slice;
    CHECK(r_mov.read_audio_pcm(250, 128, pcm_slice, err));
    CHECK(pcm_slice.size() == 128u * 4u);
    CHECK(std::memcmp(pcm_slice.data(), &pcm_data[250u * 4u], pcm_slice.size()) == 0);

    std::string wav_path = temp_file_path("nijin_synth_audio.wav");
    CHECK(r_mov.export_audio_wav(wav_path, err));
    auto wav = read_bytes(wav_path);
    std::remove(wav_path.c_str());
    CHECK(wav.size() == 44u + pcm_data.size());
    CHECK(std::memcmp(wav.data(), "RIFF", 4) == 0 && std::memcmp(&wav[8], "WAVE", 4) == 0);
    CHECK(rd_le16(&wav[20]) == 1u && rd_le16(&wav[22]) == 2u && rd_le32(&wav[24]) == 48000u);
    CHECK(rd_le32(&wav[40]) == pcm_data.size());
    CHECK(static_cast<int16_t>(rd_le16(&wav[44])) == 1000 && static_cast<int16_t>(rd_le16(&wav[46])) == -2000);

    // 4) encode_cfa_dng_memory exact parity with write_cfa_dng
    nijinwvdc::DngWriteOptions mopts;
    mopts.width = 32; mopts.height = 32;
    std::vector<uint8_t> mem_dng;
    CHECK(nijinwvdc::encode_cfa_dng_memory(b_mem.data(), mopts, mem_dng, err));
    std::string file_dng = temp_file_path("nijin_mem_cmp.dng");
    CHECK(nijinwvdc::write_cfa_dng(file_dng, b_mem.data(), mopts, err));
    auto file_dng_bytes = read_bytes(file_dng);
    std::remove(file_dng.c_str());
    CHECK(mem_dng == file_dng_bytes);
}

void test_optional_real_corpus() {
    std::string err;
    const char* env_nev = std::getenv("NIJINWVDC_TEST_NEV");
    if (env_nev && *env_nev && file_exists(env_nev)) {
        nijinwvdc::NevClipInfo clip;
        CHECK(nijinwvdc::parse_nev_clip(env_nev, clip, err));
        CHECK(clip.major_brand == "niko");
        CHECK(!clip.video_samples.empty());

        nijinwvdc::NevFrameMeta m0, m_oob;
        std::vector<uint8_t> cs0;
        CHECK(nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, m0, &cs0, err));
        CHECK(!nijinwvdc::read_nev_frame_meta_and_codestream(
            clip, static_cast<uint32_t>(clip.video_samples.size()), m_oob, nullptr, err));
        CHECK(m0.width > 0 && m0.height > 0 && cs0.size() > 256);

        std::vector<uint16_t> bad_bayer;
        nijinwvdc::TicoRawInfo bad_info;
        std::vector<uint8_t> trunc_cs(cs0.begin(), cs0.begin() + 256);
        CHECK(!nijinwvdc::decode_ticoraw_bayer(trunc_cs.data(), trunc_cs.size(), 1, bad_bayer, bad_info, err));

        // Corrupt the EOC marker bytes -> clean structural failure
        std::vector<uint8_t> corrupt_eoc = cs0;
        corrupt_eoc[corrupt_eoc.size() - 1] ^= 0xFFu;
        CHECK(!nijinwvdc::decode_ticoraw_bayer(corrupt_eoc.data(), corrupt_eoc.size(), 2, bad_bayer, bad_info, err));

        // Corrupt a data payload byte inside the middle precinct: decoder must stay memory-safe
        std::vector<uint8_t> fuzz_mid = cs0;
        fuzz_mid[fuzz_mid.size() / 2] ^= 0xFFu;
        (void)nijinwvdc::decode_ticoraw_bayer(fuzz_mid.data(), fuzz_mid.size(), 2, bad_bayer, bad_info, err);

        std::vector<uint16_t> b1, b4;
        nijinwvdc::TicoRawInfo info1, info4;
        CHECK(nijinwvdc::decode_ticoraw_bayer(cs0.data(), cs0.size(), 1, b1, info1, err));
        CHECK(nijinwvdc::decode_ticoraw_bayer(cs0.data(), cs0.size(), 4, b4, info4, err));
        CHECK(b1.size() == static_cast<size_t>(info1.width) * info1.height);
        CHECK(b1 == b4);

        std::string tmp_dng = temp_file_path("nijin_corpus_nev.dng");
        nijinwvdc::DngWriteOptions dopts;
        nijinwvdc::build_dng_options_from_clip_and_frame(clip, m0, info4, false, 0, dopts);
        CHECK(nijinwvdc::write_cfa_dng(tmp_dng, b4.data(), dopts, err));
        std::remove(tmp_dng.c_str());

        nijinwvdc::MediaReader mr;
        CHECK(mr.open_file(env_nev, err));
        nijinwvdc::DecoderSession ds;
        nijinwvdc::DecodeOptions dopts_s;
        dopts_s.want_quant_profile = true;
        dopts_s.num_threads = 1;
        std::vector<uint16_t> bs;
        nijinwvdc::DecodedFrameInfo ds_info;
        CHECK(ds.decode_frame(mr, 0, dopts_s, bs, ds_info, err));
        CHECK(bs == b1);
        CHECK(ds_info.quant_profile.available && ds_info.quant_profile.mean_luma_code_std > 0.0f);
        CHECK(ds_info.quant_profile.nlt_slope_lut[240] > ds_info.quant_profile.nlt_slope_lut[4]);
        nijinwvdc::DecodeOptions dopts_m = dopts_s;
        dopts_m.num_threads = 4;
        nijinwvdc::DecodedFrameInfo ds_info4;
        std::vector<uint16_t> bs4;
        CHECK(ds.decode_frame(mr, 0, dopts_m, bs4, ds_info4, err));
        CHECK(ds_info4.quant_profile.q_luma_code_row == ds_info.quant_profile.q_luma_code_row);
        CHECK(ds_info4.quant_profile.mean_luma_code_std == ds_info.quant_profile.mean_luma_code_std);
        const auto ainfo = mr.audio_info();
        if (ainfo.available) {
            std::string wav_p = temp_file_path("nijin_corpus_nev.wav");
            CHECK(mr.export_audio_wav(wav_p, err));
            CHECK(file_exists(wav_p));
            std::remove(wav_p.c_str());
        }
    }

    const char* env_nef = std::getenv("NIJINWVDC_TEST_NEF");
    if (env_nef && *env_nef && file_exists(env_nef)) {
        nijinwvdc::NevClipInfo clip;
        CHECK(nijinwvdc::parse_nev_clip(env_nef, clip, err));
        CHECK(clip.major_brand == "NEF");
        CHECK(clip.video_samples.size() == 1);

        nijinwvdc::NevFrameMeta m0, m_oob;
        std::vector<uint8_t> cs0;
        CHECK(nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, m0, &cs0, err));
        CHECK(!nijinwvdc::read_nev_frame_meta_and_codestream(clip, 1, m_oob, nullptr, err));

        std::vector<uint16_t> b1, b4;
        nijinwvdc::TicoRawInfo info1, info4;
        CHECK(nijinwvdc::decode_ticoraw_bayer(cs0.data(), cs0.size(), 1, b1, info1, err));
        CHECK(nijinwvdc::decode_ticoraw_bayer(cs0.data(), cs0.size(), 4, b4, info4, err));
        CHECK(b1 == b4);

        if (m0.has_crop) {
            std::vector<uint16_t> cropped;
            uint8_t out_cfa[4] = {0};
            CHECK(nijinwvdc::apply_bayer_crop(
                b4.data(), info4.width, info4.height, m0.crop, m0.cfa_pattern, cropped, out_cfa));
            CHECK(cropped.size() == static_cast<size_t>(m0.crop[2]) * m0.crop[3]);

            nijinwvdc::MediaReader mr_nef;
            CHECK(mr_nef.open_file(env_nef, err));
            nijinwvdc::DecoderSession ds_nef(4);
            nijinwvdc::DecodeOptions dopts_nef_crop;
            dopts_nef_crop.apply_active_crop = true;
            std::vector<uint16_t> nef_direct;
            nijinwvdc::DecodedFrameInfo nef_ci;
            CHECK(ds_nef.decode_frame(mr_nef, 0, dopts_nef_crop, nef_direct, nef_ci, err));
            CHECK(nef_direct == cropped);
            CHECK(std::memcmp(nef_ci.cfa_pattern, out_cfa, 4) == 0);
        }
        {
            nijinwvdc::DecoderSession ds_t(4);
            nijinwvdc::DecodeOptions troi;
            troi.has_roi = true;
            troi.roi[0] = 64u; troi.roi[1] = 66u; troi.roi[2] = 512u; troi.roi[3] = 516u;
            std::vector<uint16_t> r_dir, r_ref;
            nijinwvdc::DecodedFrameInfo di_t;
            CHECK(ds_t.decode_codestream(std::span<const uint8_t>(cs0), troi, r_dir, di_t, err));
            uint8_t rcfa[4] = {};
            const uint8_t src_cfa[4] = {0, 1, 1, 2};
            const uint16_t roi16_a[4] = {64u, 66u, 512u, 516u};
            CHECK(nijinwvdc::apply_bayer_crop(
                b4.data(), info4.width, info4.height, roi16_a, src_cfa, r_ref, rcfa));
            CHECK(r_dir == r_ref);
            CHECK(std::memcmp(di_t.cfa_pattern, rcfa, 4) == 0);

            troi.roi[0] = 65u; troi.roi[1] = 67u; troi.roi[2] = 511u; troi.roi[3] = 513u;
            CHECK(ds_t.decode_codestream(std::span<const uint8_t>(cs0), troi, r_dir, di_t, err));
            const uint16_t roi16_b[4] = {65u, 67u, 511u, 513u};
            CHECK(nijinwvdc::apply_bayer_crop(
                b4.data(), info4.width, info4.height, roi16_b, src_cfa, r_ref, rcfa));
            CHECK(r_dir == r_ref);
            CHECK(std::memcmp(di_t.cfa_pattern, rcfa, 4) == 0);
        }
    }

    const char* env_arw = std::getenv("NIJINWVDC_TEST_ARW");
    if (env_arw && *env_arw && file_exists(env_arw)) {
        nijinwvdc::NevClipInfo clip;
        CHECK(nijinwvdc::parse_nev_clip(env_arw, clip, err));
        CHECK(clip.major_brand == "ARW");
        CHECK(clip.video_samples.size() == 1);

        nijinwvdc::NevFrameMeta m0, m_oob;
        std::vector<uint8_t> strip0;
        CHECK(nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, m0, &strip0, err));
        CHECK(!nijinwvdc::read_nev_frame_meta_and_codestream(clip, 1, m_oob, nullptr, err));

        std::vector<uint16_t> bad_bayer;
        nijinwvdc::Arw6Info bad_info;
        std::vector<uint8_t> trunc_strip(strip0.begin(), strip0.begin() + 256);
        CHECK(!nijinwvdc::decode_arw6_bayer(trunc_strip.data(), trunc_strip.size(), 1, bad_bayer, bad_info, err));

        std::vector<uint8_t> fuzz_mid = strip0;
        fuzz_mid[fuzz_mid.size() / 2] ^= 0xFFu;
        (void)nijinwvdc::decode_arw6_bayer(fuzz_mid.data(), fuzz_mid.size(), 2, bad_bayer, bad_info, err);

        std::vector<uint16_t> b1, b8;
        nijinwvdc::Arw6Info info1, info8;
        CHECK(nijinwvdc::decode_arw6_bayer(strip0.data(), strip0.size(), 1, b1, info1, err));
        CHECK(nijinwvdc::decode_arw6_bayer(strip0.data(), strip0.size(), 8, b8, info8, err));
        CHECK(b1.size() == static_cast<size_t>(info1.width) * info1.height);
        CHECK(b1 == b8);

        if (m0.has_crop) {
            std::vector<uint16_t> cropped;
            uint8_t out_cfa[4] = {0};
            CHECK(nijinwvdc::apply_bayer_crop(
                b8.data(), info8.width, info8.height, m0.crop, m0.cfa_pattern, cropped, out_cfa));
            CHECK(cropped.size() == static_cast<size_t>(m0.crop[2]) * m0.crop[3]);
        }

        std::string tmp_dng = temp_file_path("nijin_corpus_arw.dng");
        nijinwvdc::DngWriteOptions dopts;
        nijinwvdc::build_dng_options_from_clip_and_frame(clip, m0, info8, m0.has_crop, 0, dopts);
        if (m0.has_crop) {
            std::vector<uint16_t> cropped;
            CHECK(nijinwvdc::apply_bayer_crop(
                b8.data(), info8.width, info8.height, m0.crop, m0.cfa_pattern, cropped, dopts.cfa_pattern));
            dopts.width = m0.crop[2];
            dopts.height = m0.crop[3];
            dopts.has_crop = false;
            CHECK(nijinwvdc::write_cfa_dng(tmp_dng, cropped.data(), dopts, err));
        } else {
            CHECK(nijinwvdc::write_cfa_dng(tmp_dng, b8.data(), dopts, err));
        }
        std::remove(tmp_dng.c_str());

        nijinwvdc::MediaReader mr;
        CHECK(mr.open_file(env_arw, err));
        nijinwvdc::DecoderSession ds;
        nijinwvdc::DecodeOptions dopts_s;
        std::vector<uint16_t> bs;
        nijinwvdc::DecodedFrameInfo ds_info;
        CHECK(ds.decode_frame(mr, 0, dopts_s, bs, ds_info, err));
        CHECK(bs == b1);
        if (m0.has_crop) {
            std::vector<uint16_t> cropped, direct_crop;
            uint8_t out_cfa[4] = {0};
            CHECK(nijinwvdc::apply_bayer_crop(
                b8.data(), info8.width, info8.height, m0.crop, m0.cfa_pattern, cropped, out_cfa));
            nijinwvdc::DecodeOptions dopts_c;
            dopts_c.num_threads = 4;
            dopts_c.apply_active_crop = true;
            nijinwvdc::DecodedFrameInfo crop_info;
            CHECK(ds.decode_frame(mr, 0, dopts_c, direct_crop, crop_info, err));
            CHECK(direct_crop == cropped);
            CHECK(std::memcmp(crop_info.cfa_pattern, out_cfa, 4) == 0);
        }
    }
}

void test_cpp20_spans_and_direct_roi() {
    std::string err;
    auto cs_tile = build_synth_arw6_16x16(-700, 20, -20);
    auto arw_tiff = build_synth_arw_4tile_tiff();

    // 1. std::span overloads on MediaReader + DecoderSession + helpers
    nijinwvdc::MediaReader r_span;
    CHECK(r_span.open_memory(std::span<const uint8_t>(arw_tiff), err));
    CHECK(r_span.frame_count() == 1u);

    nijinwvdc::DecoderSession sess(2);
    nijinwvdc::DecodeOptions dopts;
    std::vector<uint16_t> full_bayer, span_bayer;
    nijinwvdc::DecodedFrameInfo fi_full, fi_span;
    CHECK(sess.decode_frame(r_span, 0, dopts, full_bayer, fi_full, err));
    CHECK(sess.decode_codestream(std::span<const uint8_t>(cs_tile), dopts, span_bayer, fi_span, err));
    CHECK(fi_span.output_width == 16u && fi_span.output_height == 16u);

    std::vector<uint16_t> tile_into(16u * 16u, 0u);
    CHECK(sess.decode_codestream_into(
        std::span<const uint8_t>(cs_tile), dopts, std::span<uint16_t>(tile_into), 0u, fi_span, err));
    CHECK(tile_into == span_bayer);

    // Undersized destination span must fail cleanly
    std::vector<uint16_t> too_small(16u * 15u, 0u);
    CHECK(!sess.decode_codestream_into(
        std::span<const uint8_t>(cs_tile), dopts, std::span<uint16_t>(too_small), 0u, fi_span, err));
    std::vector<uint16_t> frame_small(32u * 31u, 0u);
    CHECK(!sess.decode_frame_into(
        r_span, 0u, dopts, std::span<uint16_t>(frame_small), 0u, fi_full, err));

    // Exact-fit decode_frame_into span + helper std::span overloads
    std::vector<uint16_t> frame_into(32u * 32u, 0u);
    CHECK(sess.decode_frame_into(
        r_span, 0u, dopts, std::span<uint16_t>(frame_into), 0u, fi_span, err));
    CHECK(frame_into == full_bayer);

    std::vector<uint16_t> p00(16u * 16u), p01(16u * 16u), p10(16u * 16u), p11(16u * 16u);
    CHECK(nijinwvdc::split_bayer_cfa_planes(
        std::span<const uint16_t>(full_bayer), 32u, 32u, p00, p01, p10, p11));
    CHECK(!nijinwvdc::split_bayer_cfa_planes(
        std::span<const uint16_t>(too_small), 32u, 32u, p00, p01, p10, p11));

    const float blk[4] = {100.0f, 100.0f, 100.0f, 100.0f};
    std::vector<float> fn_out(32u * 32u);
    CHECK(nijinwvdc::normalize_bayer_to_float(
        std::span<const uint16_t>(full_bayer), 32u, 32u, blk, 16383.0f, true, std::span<float>(fn_out)));

    nijinwvdc::DngWriteOptions dopt;
    dopt.width = 32u;
    dopt.height = 32u;
    std::vector<uint8_t> dng_mem, dng_ref;
    CHECK(nijinwvdc::encode_cfa_dng_memory(std::span<const uint16_t>(full_bayer), dopt, dng_mem, err));
    CHECK(nijinwvdc::encode_cfa_dng_memory(full_bayer.data(), dopt, dng_ref, err));
    CHECK(dng_mem == dng_ref);

    // 2. Direct Stage 3 ROI / crop parity (4-tile quadrant-spanning + single-tile + odd stride)
    const uint32_t rois[][4] = {
        {1u, 1u, 17u, 15u},
        {16u, 16u, 16u, 16u},
        {5u, 7u, 20u, 19u},
        {0u, 14u, 32u, 6u},
    };
    for (const auto& r : rois) {
        nijinwvdc::DecodeOptions ropts;
        ropts.num_threads = 2;
        ropts.has_roi = true;
        std::memcpy(ropts.roi, r, sizeof(ropts.roi));

        std::vector<uint16_t> dir_out, ref_out;
        nijinwvdc::DecodedFrameInfo rinfo;
        CHECK(sess.decode_frame(r_span, 0u, ropts, dir_out, rinfo, err));
        uint8_t ref_cfa[4] = {};
        const uint16_t r16[4] = {
            static_cast<uint16_t>(r[0]), static_cast<uint16_t>(r[1]),
            static_cast<uint16_t>(r[2]), static_cast<uint16_t>(r[3])
        };
        CHECK(nijinwvdc::apply_bayer_crop(
            full_bayer.data(), 32u, 32u, r16, fi_full.meta.cfa_pattern, ref_out, ref_cfa));
        CHECK(dir_out == ref_out);
        CHECK(std::memcmp(rinfo.cfa_pattern, ref_cfa, 4) == 0);

        // Unaligned odd byte stride (fallback copy_bayer_roi_strided path) must match identically
        const size_t odd_stride = static_cast<size_t>(r[2]) * 2u + 1u;
        std::vector<uint8_t> odd_buf(static_cast<size_t>(r[3]) * odd_stride + 16u, 0xAAu);
        CHECK(sess.decode_frame_into(
            r_span, 0u, ropts, reinterpret_cast<uint16_t*>(odd_buf.data()), odd_stride, r[3], rinfo, err));
        for (uint32_t row = 0; row < r[3]; ++row) {
            CHECK(std::memcmp(
                odd_buf.data() + static_cast<size_t>(row) * odd_stride,
                reinterpret_cast<const uint8_t*>(ref_out.data() + static_cast<size_t>(row) * r[2]),
                static_cast<size_t>(r[2]) * 2u) == 0);
        }
    }

    // Single-tile A000 direct odd ROI parity
    {
        nijinwvdc::DecodeOptions tropts;
        tropts.has_roi = true;
        tropts.roi[0] = 1u; tropts.roi[1] = 3u; tropts.roi[2] = 10u; tropts.roi[3] = 9u;
        std::vector<uint16_t> dir_t, ref_t;
        nijinwvdc::DecodedFrameInfo tinfo;
        CHECK(sess.decode_codestream(std::span<const uint8_t>(cs_tile), tropts, dir_t, tinfo, err));
        const uint8_t cfa0[4] = {0, 1, 1, 2};
        uint8_t tcfa[4] = {};
        const uint16_t tc16[4] = {1u, 3u, 10u, 9u};
        CHECK(nijinwvdc::apply_bayer_crop(span_bayer.data(), 16u, 16u, tc16, cfa0, ref_t, tcfa));
        CHECK(dir_t == ref_t);
        CHECK(std::memcmp(tinfo.cfa_pattern, tcfa, 4) == 0);
    }
}

void test_focus_meta_parsing_and_dng_tags() {
    std::string err;

    // 1. Sony 0x94xx inverse LUT matches x^3 mod 249
    const auto inv_lut = nijinwvdc::detail::make_sony_94xx_inv_lut();
    for (uint32_t x = 0; x < 249u; ++x) {
        uint8_t c = static_cast<uint8_t>((x * x * x) % 249u);
        CHECK(inv_lut[c] == static_cast<uint8_t>(x));
    }

    // 2. End-to-end synthetic ARW with ExifIFD + Sony MakerNote 0x9402
    {
        auto build_sony_synth_arw = [&](uint8_t pos2, const char* fname) -> std::string {
            auto strip_base = build_synth_arw_4tile_tiff();
            // strip_base has IFD0 @ 8 with 3 entries, SubIFD @ 64, strip_off @ 512.
            // Relocate metadata blocks after the base image buffer so nothing collides.
            std::vector<uint8_t> arw = strip_base;
            if (arw.size() < 4096u) arw.resize(4096u, 0);
            const size_t strip_sz = arw.size();
            const size_t ifd0_off = strip_sz;
            const size_t exif_ifd_off = ifd0_off + 64u;
            const size_t mn_ifd_off = exif_ifd_off + 64u;
            const size_t tag9402_off = mn_ifd_off + 32u;
            const size_t fl_rat_off = tag9402_off + 64u;
            arw.resize(fl_rat_off + 32u, 0);

            // Point TIFF header IFD0 to new IFD0 @ ifd0_off
            put_le32(arw, 4, static_cast<uint32_t>(ifd0_off));
            // IFD0: 271 (Make), 272 (Model), 330 (SubIFDs -> 64), 34665 (ExifIFD)
            put_le16(arw, ifd0_off, 4u);
            auto put_ent = [&](size_t base, size_t idx, uint16_t tag, uint16_t dt, uint32_t cnt, uint32_t val) {
                size_t o = base + 2u + idx * 12u;
                put_le16(arw, o + 0, tag); put_le16(arw, o + 2, dt);
                put_le32(arw, o + 4, cnt); put_le32(arw, o + 8, val);
            };
            put_ent(ifd0_off, 0, 271, 2, 5, 48);
            put_ent(ifd0_off, 1, 272, 2, 9, 54);
            put_ent(ifd0_off, 2, 330, 4, 1, 64);
            put_ent(ifd0_off, 3, 34665, 4, 1, static_cast<uint32_t>(exif_ifd_off));

            // ExifIFD: 37386 (FocalLength=50.0), 37500 (MakerNote -> mn_ifd_off)
            put_le32(arw, fl_rat_off, 500u);
            put_le32(arw, fl_rat_off + 4, 10u);
            put_le16(arw, exif_ifd_off, 2u);
            put_ent(exif_ifd_off, 0, 37386, 5, 1, static_cast<uint32_t>(fl_rat_off));
            put_ent(exif_ifd_off, 1, 37500, 7, 0x60u, static_cast<uint32_t>(mn_ifd_off));

            // MakerNote IFD @ mn_ifd_off: 1 entry (0x9402)
            put_le16(arw, mn_ifd_off, 1u);
            put_ent(mn_ifd_off, 0, 0x9402, 7, 0x30u, static_cast<uint32_t>(tag9402_off));

            uint8_t dec9402[0x30] = {};
            dec9402[0] = 0x07u; // valid Tag9402 header
            // f_nom = 50.0 mm -> 500 = 0x01F4 LE
            dec9402[0x24] = 0xF4u; dec9402[0x25] = 0x01u;
            // f_opt = 52.0 mm -> 520 = 0x0208 LE
            dec9402[0x28] = 0x08u; dec9402[0x29] = 0x02u;
            dec9402[0x2D] = pos2;
            for (size_t i = 0; i < 0x30u; ++i) {
                uint32_t x = dec9402[i];
                arw[tag9402_off + i] = static_cast<uint8_t>((x >= 249u) ? x : ((x * x * x) % 249u));
            }
            std::string path = temp_file_path(fname);
            CHECK(write_bytes(path, arw));
            return path;
        };

        std::string p_fin = build_sony_synth_arw(110u, "nijin_sony_focus_fin.arw");
        nijinwvdc::NevClipInfo clip;
        nijinwvdc::NevFrameMeta meta;
        CHECK(nijinwvdc::parse_nev_clip(p_fin, clip, err));
        CHECK(nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, meta, nullptr, err));
        const double expected_d = (std::pow(2.0, 110.0 / 16.0 - 5.0) + 1.0) * (52.0 / 1000.0);
        CHECK(!meta.focus_is_infinity);
        CHECK(meta.focus_position == 110u);
        CHECK(std::fabs(meta.focal_length_mm - 50.0f) < 1e-3f);
        CHECK(std::fabs(meta.focal_length_opt_mm - 52.0f) < 1e-3f);
        CHECK(std::fabs(meta.focus_distance_m - expected_d) < 1e-3);
        CHECK(meta.focus_distance_low_m > 0.0f && meta.focus_distance_low_m < meta.focus_distance_m);
        CHECK(meta.focus_distance_high_m > meta.focus_distance_m);
        std::remove(p_fin.c_str());

        std::string p_inf = build_sony_synth_arw(255u, "nijin_sony_focus_inf.arw");
        nijinwvdc::NevClipInfo clip_inf;
        nijinwvdc::NevFrameMeta meta_inf;
        CHECK(nijinwvdc::parse_nev_clip(p_inf, clip_inf, err));
        CHECK(nijinwvdc::read_nev_frame_meta_and_codestream(clip_inf, 0, meta_inf, nullptr, err));
        CHECK(meta_inf.focus_is_infinity);
        CHECK(meta_inf.focus_distance_m >= 999999.0f);
        std::remove(p_inf.c_str());
    }

    // 3. Nikon LensData 0801 encryption/decryption (Z-mount finite + infinity)
    {
        std::vector<uint8_t> zeros(0x64u, 0u);
        zeros[0] = '0'; zeros[1] = '8'; zeros[2] = '0'; zeros[3] = '1';
        std::vector<uint8_t> ks;
        CHECK(nijinwvdc::detail::decrypt_nikon_lens_data(
            zeros.data(), zeros.size(), "8022671", 43092u, true, "NIKON Z 8", ks));
        CHECK(ks.size() == zeros.size());

        auto make_encrypted_0801 = [&](uint16_t u16_fd, uint8_t range_w, uint8_t drive_end,
                                       uint8_t steps_from_inf, int32_t lpa) {
            std::vector<uint8_t> pt(0x64u, 0u);
            pt[0] = '0'; pt[1] = '8'; pt[2] = '0'; pt[3] = '1';
            pt[0x35] = 1u;
            pt[0x3C] = 50u; pt[0x3D] = 0u; // focal = 50mm
            pt[0x4C] = range_w;
            pt[0x4E] = static_cast<uint8_t>(u16_fd & 0xFFu);
            pt[0x4F] = static_cast<uint8_t>((u16_fd >> 8) & 0xFFu);
            pt[0x56] = drive_end;
            pt[0x58] = steps_from_inf;
            uint32_t u_lpa = static_cast<uint32_t>(lpa);
            pt[0x5A] = static_cast<uint8_t>(u_lpa & 0xFFu);
            pt[0x5B] = static_cast<uint8_t>((u_lpa >> 8) & 0xFFu);
            pt[0x5C] = static_cast<uint8_t>((u_lpa >> 16) & 0xFFu);
            pt[0x5D] = static_cast<uint8_t>((u_lpa >> 24) & 0xFFu);
            for (size_t i = 4u; i < pt.size(); ++i) pt[i] ^= ks[i];
            return pt;
        };

        const uint16_t fd_2p5m = static_cast<uint16_t>(std::llround((12.0 * std::log2(2.5) + 80.0) * 256.0));
        auto enc_fin = make_encrypted_0801(fd_2p5m, 5u, 0u, 10u, 1234);
        nijinwvdc::NevFrameMeta nm_fin;
        nijinwvdc::detail::decode_nikon_lens_data(
            enc_fin.data(), enc_fin.size(), "8022671", 43092u, true, "NIKON Z 8", nm_fin);
        CHECK(!nm_fin.focus_is_infinity);
        CHECK(std::fabs(nm_fin.focal_length_mm - 50.0f) < 1e-3f);
        CHECK(std::fabs(nm_fin.focus_distance_m - 2.5f) < 0.02f);
        CHECK(nm_fin.has_lens_position_abs && nm_fin.lens_position_abs == 1234);
        CHECK(nm_fin.focus_position == fd_2p5m);

        auto enc_inf = make_encrypted_0801(0u, 0u, 2u, 0u, 0);
        nijinwvdc::NevFrameMeta nm_inf;
        nijinwvdc::detail::decode_nikon_lens_data(
            enc_inf.data(), enc_inf.size(), "8022671", 43092u, true, "NIKON Z 8", nm_inf);
        CHECK(nm_inf.focus_is_infinity);
        CHECK(nm_inf.focus_distance_m >= 999999.0f);
    }

    // 4. DNG SubjectDistance (37382) and FocalLength (37386) finite & infinity emission + IFD tag sorting
    {
        std::vector<uint16_t> px(16u * 16u, 1024u);
        auto check_dng_focus = [&](const char* fname, float dist_m, bool is_inf, uint32_t exp_num, uint32_t exp_den) {
            std::string p = temp_file_path(fname);
            nijinwvdc::DngWriteOptions opts;
            opts.width = 16; opts.height = 16; opts.bit_depth = 14; opts.white = 16383;
            opts.focal_length_mm = 50.0f;
            opts.focus_distance_m = dist_m;
            opts.focus_is_infinity = is_inf;
            CHECK(nijinwvdc::write_cfa_dng(p, px.data(), opts, err));
            auto raw = read_bytes(p);
            uint32_t ifd_off = rd_le32(&raw[4]);
            uint16_t cnt = rd_le16(&raw[ifd_off]);
            uint16_t prev_tag = 0;
            bool saw_37382 = false, saw_37386 = false;
            for (uint16_t i = 0; i < cnt; ++i) {
                const uint8_t* ent = &raw[ifd_off + 2u + static_cast<size_t>(i) * 12u];
                uint16_t tag = rd_le16(ent + 0);
                uint16_t dt = rd_le16(ent + 2);
                uint32_t tcnt = rd_le32(ent + 4);
                uint32_t vo = rd_le32(ent + 8);
                CHECK(i == 0 || tag > prev_tag);
                prev_tag = tag;
                if (tag == 37382u) {
                    saw_37382 = true;
                    CHECK(dt == 5u && tcnt == 1u);
                    CHECK(rd_le32(&raw[vo]) == exp_num && rd_le32(&raw[vo + 4u]) == exp_den);
                } else if (tag == 37386u) {
                    saw_37386 = true;
                    CHECK(dt == 5u && tcnt == 1u);
                    CHECK(rd_le32(&raw[vo]) == 500u && rd_le32(&raw[vo + 4u]) == 10u);
                }
            }
            CHECK(saw_37382 && saw_37386);
            std::remove(p.c_str());
        };
        check_dng_focus("nijin_dng_focus_fin.dng", 3.25f, false, 3250u, 1000u);
        check_dng_focus("nijin_dng_focus_inf.dng", 1000000.0f, true, 0xFFFFFFFFu, 1u);
    }
}

} // namespace

int main() {
    test_negative_containers_and_codestreams();
    test_cfa_crop_and_colormatrix();
    test_arw6_synthetic_1tile_and_4tile();
    test_dng_roundtrip_synthetic();
    test_session_reader_postproc_and_threading();
    test_optional_real_corpus();
    test_cpp20_spans_and_direct_roi();
    test_focus_meta_parsing_and_dng_tags();
    std::printf("test_nijinWVDC: ALL PASSED\n");
    return 0;
}
