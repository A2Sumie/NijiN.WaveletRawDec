// SPDX-License-Identifier: Apache-2.0
#include "../include/nev2bayer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
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
    std::string base = (td && *td) ? td : "/tmp";
    if (!base.empty() && base.back() == '/') base.pop_back();
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
    nev2bayer::NevClipInfo clip;
    CHECK(!nev2bayer::parse_nev_clip("/tmp/does_not_exist_nijin_123456.nev", clip, err));

    nev2bayer::TicoRawInfo jxs;
    std::vector<uint16_t> out;
    uint8_t tiny[8] = {0xff, 0x10, 0xff, 0x50, 0, 0, 0, 0};
    CHECK(!nev2bayer::probe_ticoraw_header(nullptr, 0, jxs, err));
    CHECK(!nev2bayer::probe_ticoraw_header(tiny, sizeof(tiny), jxs, err));
    CHECK(!nev2bayer::decode_ticoraw_bayer(tiny, sizeof(tiny), 1, out, jxs, err));

    std::vector<uint8_t> junk(256, 0xAA);
    junk[0] = 0xFF; junk[1] = 0x10;
    junk[2] = 0xFF; junk[3] = 0x50;
    CHECK(!nev2bayer::probe_ticoraw_header(junk.data(), junk.size(), jxs, err));
    CHECK(!nev2bayer::decode_ticoraw_bayer(junk.data(), junk.size(), 2, out, jxs, err));

    // Malformed ISOBMFF file (ftyp only, no moov)
    std::string bad_nev = temp_file_path("nijin_bad_ftyp.nev");
    std::vector<uint8_t> ftyp_only = {
        0, 0, 0, 16, 'f', 't', 'y', 'p', 'n', 'i', 'k', 'o', 0, 0, 0, 1
    };
    CHECK(write_bytes(bad_nev, ftyp_only));
    CHECK(!nev2bayer::parse_nev_clip(bad_nev, clip, err));
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
    CHECK(!nev2bayer::parse_nev_clip(legacy_nef, clip, err));
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
    CHECK(!nev2bayer::parse_nev_clip(legacy_arw, clip, err));
    CHECK(err.find("Unsupported Sony ARW") != std::string::npos);
    std::remove(legacy_arw.c_str());

    // Negative ARW6 codestream and strip probes
    nev2bayer::Arw6Info ainfo;
    std::vector<uint16_t> aout;
    uint8_t bad_a000[16] = {'B', 'A', 'D', '0', 0};
    CHECK(!nev2bayer::probe_arw6_codestream(nullptr, 0, ainfo, err));
    CHECK(!nev2bayer::probe_arw6_codestream(bad_a000, sizeof(bad_a000), ainfo, err));
    CHECK(!nev2bayer::decode_arw6_tile(bad_a000, sizeof(bad_a000), 1, 0, 0, 16, 16, aout, ainfo, err));

    for (uint32_t bad_tc : {0u, 2u, 5u}) {
        std::vector<uint8_t> bad_strip(0x80, 0);
        put_le32(bad_strip, 0, bad_tc);
        CHECK(!nev2bayer::probe_arw6_strip(bad_strip.data(), bad_strip.size(), ainfo, err));
        CHECK(!nev2bayer::decode_arw6_bayer(bad_strip.data(), bad_strip.size(), 1, aout, ainfo, err));
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
    CHECK(nev2bayer::apply_bayer_crop(src, 4, 4, c00, rggb, dst, dcfa));
    CHECK(dcfa[0] == 0 && dcfa[1] == 1 && dcfa[2] == 1 && dcfa[3] == 2);

    // Odd X, even Y -> GRBG [1, 0, 2, 1]
    uint16_t c10[4] = {1, 0, 2, 2};
    CHECK(nev2bayer::apply_bayer_crop(src, 4, 4, c10, rggb, dst, dcfa));
    CHECK(dst.size() == 4 && dst[0] == 2 && dst[1] == 3 && dst[2] == 6 && dst[3] == 7);
    CHECK(dcfa[0] == 1 && dcfa[1] == 0 && dcfa[2] == 2 && dcfa[3] == 1);

    // Even X, odd Y -> GBRG [1, 2, 0, 1]
    uint16_t c01[4] = {0, 1, 2, 2};
    CHECK(nev2bayer::apply_bayer_crop(src, 4, 4, c01, rggb, dst, dcfa));
    CHECK(dst[0] == 5 && dst[3] == 10);
    CHECK(dcfa[0] == 1 && dcfa[1] == 2 && dcfa[2] == 0 && dcfa[3] == 1);

    // Odd X, odd Y -> BGGR [2, 1, 1, 0]
    uint16_t c11[4] = {1, 1, 2, 2};
    CHECK(nev2bayer::apply_bayer_crop(src, 4, 4, c11, rggb, dst, dcfa));
    CHECK(dcfa[0] == 2 && dcfa[1] == 1 && dcfa[2] == 1 && dcfa[3] == 0);

    // Out-of-bounds crop rejected
    uint16_t bad_crop[4] = {3, 0, 2, 2};
    CHECK(!nev2bayer::apply_bayer_crop(src, 4, 4, bad_crop, rggb, dst, dcfa));

    double cm[9] = {0};
    const char* known_models[] = {
        "NIKON Z 8", "NIKON Z 9", "NIKON Z6_3", "NIKON ZR", "NIKON Z f", "NIKON Z50_2", "NIKON Z5_2"
    };
    for (const char* m : known_models) {
        CHECK(nev2bayer::lookup_nikon_d65_colormatrix(m, cm));
        CHECK(cm[0] > 0.8 && cm[4] > 1.0 && cm[8] > 0.5);
    }
    CHECK(!nev2bayer::lookup_nikon_d65_colormatrix("UNKNOWN CAMERA", cm));
    CHECK(cm[0] > 1.0 && cm[4] > 1.0);

    const char* sony_models[] = {
        "ILCE-7M5", "ILCE-7RM5", "ILCE-7M4", "ILCE-7CR", "ILCE-1M2", "ILCE-9M3", "ILME-FX3"
    };
    for (const char* m : sony_models) {
        CHECK(nev2bayer::lookup_camera_d65_colormatrix("SONY", m, cm));
        CHECK(cm[0] > 0.6 && cm[4] > 1.0 && cm[8] > 0.4);
    }
    CHECK(!nev2bayer::lookup_camera_d65_colormatrix("SONY", "UNKNOWN MODEL", cm));
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

std::vector<uint8_t> build_synth_arw6_res_stream(int n_bands, int chunk_count, int32_t dc_val) {
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
            chunks[ci] = bw.finish_padded(8);
        } else {
            chunks[ci].assign(8, 0);
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

std::vector<uint8_t> build_synth_arw6_16x16(int32_t gl_dc, int32_t r_dc, int32_t b_dc) {
    std::vector<uint8_t> streams[5][3];
    const int32_t dcs[3] = {gl_dc, r_dc, b_dc};
    uint32_t grp_bytes[4] = {0, 0, 0, 0};

    for (int r = 0; r < 4; ++r) {
        int nb = (r == 0) ? 1 : 3;
        int nchunks = (r == 0) ? 1 : 2;
        for (int c = 0; c < 3; ++c) {
            streams[r][c] = build_synth_arw6_res_stream(nb, nchunks, (r == 0) ? dcs[c] : 0);
            grp_bytes[r] += static_cast<uint32_t>(streams[r][c].size());
        }
    }
    streams[4][0] = build_synth_arw6_res_stream(1, 2, 0);

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

    put_be16(8, 16);   // w
    put_be16(10, 8);   // h / 2
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

void test_arw6_synthetic_1tile_and_4tile() {
    std::string err;

    // 1) 16x16 linear-region synthetic tile: G_L=-700 (u=1348), R=+20 (u=1388), B=-20 (u=1308)
    auto cs_lin = build_synth_arw6_16x16(-700, 20, -20);
    nev2bayer::Arw6Info info1, info4;
    CHECK(nev2bayer::probe_arw6_codestream(cs_lin.data(), cs_lin.size(), info1, err));
    CHECK(info1.width == 16 && info1.height == 16 && info1.coded_depth == 14);

    std::vector<uint8_t> strip1(0x68, 0);
    put_le32(strip1, 0, 1); // tile_cnt = 1
    put_le32(strip1, 8, 0x68);
    put_le32(strip1, 16, 0); put_le32(strip1, 20, 0);
    put_le32(strip1, 24, 16); put_le32(strip1, 28, 16);
    strip1.insert(strip1.end(), cs_lin.begin(), cs_lin.end());

    std::vector<uint16_t> b1, b4;
    CHECK(nev2bayer::decode_arw6_bayer(strip1.data(), strip1.size(), 1, b1, info1, err));
    CHECK(nev2bayer::decode_arw6_bayer(strip1.data(), strip1.size(), 4, b4, info4, err));
    CHECK(b1.size() == 16u * 16u && b1 == b4);
    for (uint32_t r = 0; r < 8; ++r) {
        for (uint32_t c = 0; c < 8; ++c) {
            CHECK(b1[(r * 2) * 16 + c * 2] == 694);
            CHECK(b1[(r * 2) * 16 + c * 2 + 1] == 674);
            CHECK(b1[(r * 2 + 1) * 16 + c * 2] == 674);
            CHECK(b1[(r * 2 + 1) * 16 + c * 2 + 1] == 654);
        }
    }

    // 2) Nonlinear upper-LUT tile: G_L=+400 (u=2448), R=+100, B=-100
    auto cs_hi = build_synth_arw6_16x16(400, 100, -100);
    std::vector<uint16_t> hi1, hi4;
    CHECK(nev2bayer::decode_arw6_tile(cs_hi.data(), cs_hi.size(), 1, 0, 0, 16, 16, hi1, info1, err));
    CHECK(nev2bayer::decode_arw6_tile(cs_hi.data(), cs_hi.size(), 4, 0, 0, 16, 16, hi4, info4, err));
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

    nev2bayer::NevClipInfo clip;
    CHECK(nev2bayer::parse_nev_clip(arw_path, clip, err));
    CHECK(clip.major_brand == "ARW");
    CHECK(clip.tracks.size() == 1 && clip.tracks[0].format == "ARW6");

    nev2bayer::NevFrameMeta meta;
    std::vector<uint8_t> loaded_strip;
    CHECK(nev2bayer::read_nev_frame_meta_and_codestream(clip, 0, meta, &loaded_strip, err));
    CHECK(meta.has_crop && meta.crop[2] == 28 && meta.crop[3] == 28);

    std::vector<uint16_t> fb1, fb4;
    nev2bayer::Arw6Info finfo1, finfo4;
    CHECK(nev2bayer::decode_arw6_bayer(loaded_strip.data(), loaded_strip.size(), 1, fb1, finfo1, err));
    CHECK(nev2bayer::decode_arw6_bayer(loaded_strip.data(), loaded_strip.size(), 4, fb4, finfo4, err));
    CHECK(finfo4.tiles.size() == 4 && finfo4.width == 32 && finfo4.height == 32);
    CHECK(fb1 == fb4);
    CHECK(fb1[0 * 32 + 1] == 674);
    CHECK(fb1[0 * 32 + 17] == 649);
    CHECK(fb1[16 * 32 + 1] == 699);
    CHECK(fb1[16 * 32 + 17] == 714);

    nev2bayer::DngWriteOptions dopts;
    nev2bayer::build_dng_options_from_clip_and_frame(clip, meta, finfo4, true, 1, dopts);
    std::vector<uint16_t> cropped;
    CHECK(nev2bayer::apply_bayer_crop(fb4.data(), finfo4.width, finfo4.height, meta.crop, meta.cfa_pattern, cropped, dopts.cfa_pattern));
    dopts.width = meta.crop[2];
    dopts.height = meta.crop[3];
    dopts.has_crop = false;
    CHECK(nev2bayer::write_cfa_dng(dng_path, cropped.data(), dopts, err));
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
        nev2bayer::DngWriteOptions opts;
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
        CHECK(nev2bayer::write_cfa_dng(p, pixels.data(), opts, err));

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

void test_optional_real_corpus() {
    std::string err;
    const char* env_nev = std::getenv("NEV2BAYER_TEST_NEV");
    if (env_nev && *env_nev && file_exists(env_nev)) {
        nev2bayer::NevClipInfo clip;
        CHECK(nev2bayer::parse_nev_clip(env_nev, clip, err));
        CHECK(clip.major_brand == "niko");
        CHECK(!clip.video_samples.empty());

        nev2bayer::NevFrameMeta m0, m_oob;
        std::vector<uint8_t> cs0;
        CHECK(nev2bayer::read_nev_frame_meta_and_codestream(clip, 0, m0, &cs0, err));
        CHECK(!nev2bayer::read_nev_frame_meta_and_codestream(
            clip, static_cast<uint32_t>(clip.video_samples.size()), m_oob, nullptr, err));
        CHECK(m0.width > 0 && m0.height > 0 && cs0.size() > 256);

        std::vector<uint16_t> bad_bayer;
        nev2bayer::TicoRawInfo bad_info;
        std::vector<uint8_t> trunc_cs(cs0.begin(), cs0.begin() + 256);
        CHECK(!nev2bayer::decode_ticoraw_bayer(trunc_cs.data(), trunc_cs.size(), 1, bad_bayer, bad_info, err));

        // Corrupt the EOC marker bytes -> clean structural failure
        std::vector<uint8_t> corrupt_eoc = cs0;
        corrupt_eoc[corrupt_eoc.size() - 1] ^= 0xFFu;
        CHECK(!nev2bayer::decode_ticoraw_bayer(corrupt_eoc.data(), corrupt_eoc.size(), 2, bad_bayer, bad_info, err));

        // Corrupt a data payload byte inside the middle precinct: decoder must stay memory-safe
        std::vector<uint8_t> fuzz_mid = cs0;
        fuzz_mid[fuzz_mid.size() / 2] ^= 0xFFu;
        (void)nev2bayer::decode_ticoraw_bayer(fuzz_mid.data(), fuzz_mid.size(), 2, bad_bayer, bad_info, err);

        std::vector<uint16_t> b1, b4;
        nev2bayer::TicoRawInfo info1, info4;
        CHECK(nev2bayer::decode_ticoraw_bayer(cs0.data(), cs0.size(), 1, b1, info1, err));
        CHECK(nev2bayer::decode_ticoraw_bayer(cs0.data(), cs0.size(), 4, b4, info4, err));
        CHECK(b1.size() == static_cast<size_t>(info1.width) * info1.height);
        CHECK(b1 == b4);

        std::string tmp_dng = temp_file_path("nijin_corpus_nev.dng");
        nev2bayer::DngWriteOptions dopts;
        nev2bayer::build_dng_options_from_clip_and_frame(clip, m0, info4, false, 0, dopts);
        CHECK(nev2bayer::write_cfa_dng(tmp_dng, b4.data(), dopts, err));
        std::remove(tmp_dng.c_str());
    }

    const char* env_nef = std::getenv("NEV2BAYER_TEST_NEF");
    if (env_nef && *env_nef && file_exists(env_nef)) {
        nev2bayer::NevClipInfo clip;
        CHECK(nev2bayer::parse_nev_clip(env_nef, clip, err));
        CHECK(clip.major_brand == "NEF");
        CHECK(clip.video_samples.size() == 1);

        nev2bayer::NevFrameMeta m0, m_oob;
        std::vector<uint8_t> cs0;
        CHECK(nev2bayer::read_nev_frame_meta_and_codestream(clip, 0, m0, &cs0, err));
        CHECK(!nev2bayer::read_nev_frame_meta_and_codestream(clip, 1, m_oob, nullptr, err));

        std::vector<uint16_t> b1, b4;
        nev2bayer::TicoRawInfo info1, info4;
        CHECK(nev2bayer::decode_ticoraw_bayer(cs0.data(), cs0.size(), 1, b1, info1, err));
        CHECK(nev2bayer::decode_ticoraw_bayer(cs0.data(), cs0.size(), 4, b4, info4, err));
        CHECK(b1 == b4);

        if (m0.has_crop) {
            std::vector<uint16_t> cropped;
            uint8_t out_cfa[4] = {0};
            CHECK(nev2bayer::apply_bayer_crop(
                b4.data(), info4.width, info4.height, m0.crop, m0.cfa_pattern, cropped, out_cfa));
            CHECK(cropped.size() == static_cast<size_t>(m0.crop[2]) * m0.crop[3]);
        }
    }

    const char* env_arw = std::getenv("NEV2BAYER_TEST_ARW");
    if (env_arw && *env_arw && file_exists(env_arw)) {
        nev2bayer::NevClipInfo clip;
        CHECK(nev2bayer::parse_nev_clip(env_arw, clip, err));
        CHECK(clip.major_brand == "ARW");
        CHECK(clip.video_samples.size() == 1);

        nev2bayer::NevFrameMeta m0, m_oob;
        std::vector<uint8_t> strip0;
        CHECK(nev2bayer::read_nev_frame_meta_and_codestream(clip, 0, m0, &strip0, err));
        CHECK(!nev2bayer::read_nev_frame_meta_and_codestream(clip, 1, m_oob, nullptr, err));

        std::vector<uint16_t> bad_bayer;
        nev2bayer::Arw6Info bad_info;
        std::vector<uint8_t> trunc_strip(strip0.begin(), strip0.begin() + 256);
        CHECK(!nev2bayer::decode_arw6_bayer(trunc_strip.data(), trunc_strip.size(), 1, bad_bayer, bad_info, err));

        std::vector<uint8_t> fuzz_mid = strip0;
        fuzz_mid[fuzz_mid.size() / 2] ^= 0xFFu;
        (void)nev2bayer::decode_arw6_bayer(fuzz_mid.data(), fuzz_mid.size(), 2, bad_bayer, bad_info, err);

        std::vector<uint16_t> b1, b8;
        nev2bayer::Arw6Info info1, info8;
        CHECK(nev2bayer::decode_arw6_bayer(strip0.data(), strip0.size(), 1, b1, info1, err));
        CHECK(nev2bayer::decode_arw6_bayer(strip0.data(), strip0.size(), 8, b8, info8, err));
        CHECK(b1.size() == static_cast<size_t>(info1.width) * info1.height);
        CHECK(b1 == b8);

        if (m0.has_crop) {
            std::vector<uint16_t> cropped;
            uint8_t out_cfa[4] = {0};
            CHECK(nev2bayer::apply_bayer_crop(
                b8.data(), info8.width, info8.height, m0.crop, m0.cfa_pattern, cropped, out_cfa));
            CHECK(cropped.size() == static_cast<size_t>(m0.crop[2]) * m0.crop[3]);
        }

        std::string tmp_dng = temp_file_path("nijin_corpus_arw.dng");
        nev2bayer::DngWriteOptions dopts;
        nev2bayer::build_dng_options_from_clip_and_frame(clip, m0, info8, m0.has_crop, 0, dopts);
        if (m0.has_crop) {
            std::vector<uint16_t> cropped;
            CHECK(nev2bayer::apply_bayer_crop(
                b8.data(), info8.width, info8.height, m0.crop, m0.cfa_pattern, cropped, dopts.cfa_pattern));
            dopts.width = m0.crop[2];
            dopts.height = m0.crop[3];
            dopts.has_crop = false;
            CHECK(nev2bayer::write_cfa_dng(tmp_dng, cropped.data(), dopts, err));
        } else {
            CHECK(nev2bayer::write_cfa_dng(tmp_dng, b8.data(), dopts, err));
        }
        std::remove(tmp_dng.c_str());
    }
}

} // namespace

int main() {
    test_negative_containers_and_codestreams();
    test_cfa_crop_and_colormatrix();
    test_arw6_synthetic_1tile_and_4tile();
    test_dng_roundtrip_synthetic();
    test_optional_real_corpus();
    std::printf("test_nev2bayer: ALL PASSED\n");
    return 0;
}
