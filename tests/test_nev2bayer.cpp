// SPDX-License-Identifier: Apache-2.0
#include "../include/nev2bayer.h"

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
}

} // namespace

int main() {
    test_negative_containers_and_codestreams();
    test_cfa_crop_and_colormatrix();
    test_dng_roundtrip_synthetic();
    test_optional_real_corpus();
    std::printf("test_nev2bayer: ALL PASSED\n");
    return 0;
}
