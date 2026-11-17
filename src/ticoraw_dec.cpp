// SPDX-License-Identifier: Apache-2.0
// Clean-room C++17 TicoRAW / JPEG XS R0 Bayer decoder for nev2bayer.
// Implements ISO/IEC 21122-1 / R0 Bayer extensions (CAP, PIH, CDT, WGT, SLH,
// bounded GCLI prediction, uniform dequantization, 5/3 IDWT, Star-Tetrix RCT,
// and piecewise Fliproot NLT inverse LUT) with zero external dependencies.

#include "../include/nev2bayer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace nev2bayer {
namespace {

constexpr int kMaxComponents = 4;
constexpr int kMaxSubbandSlots = 72;
constexpr int kNumSegments = 26;
constexpr int kNumSubbands = 25;
constexpr int kNumPackets = 8;
constexpr size_t kMinStreamBytes = 100;

constexpr uint32_t kUniformDqMul[15] = {
    0x15555u, 0x12492u, 0x11111u, 0x10842u, 0x10410u,
    0x10204u, 0x10101u, 0x10080u, 0x10040u, 0x10020u,
    0x10010u, 0x10008u, 0x10004u, 0x10002u, 0x10001u,
};

inline uint16_t rd_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t rd_be24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

inline uint32_t rd_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline int32_t clamp_i32(int32_t x, int32_t lo, int32_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline int32_t asr_s32(int32_t x, unsigned shift) {
    if (shift == 0) {
        return x;
    }
    if (x < 0) {
        return ~((~x) >> shift);
    }
    return x >> shift;
}

inline int count_leading_ones_64(uint64_t w) {
    uint64_t inv = ~w;
    if (inv == 0) {
        return 64;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(inv);
#else
    int n = 0;
    while ((inv >> 63) == 0) {
        inv <<= 1;
        ++n;
    }
    return n;
#endif
}

inline int popcount4(uint32_t mask) {
    static const uint8_t kPop4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    return kPop4[mask & 15u];
}

// Bounds-safe 64-bit MSB-first window peek starting at bit `bit_pos` inside `buf[0..byte_len)`.
// Bits beyond `byte_len` read as 0.
inline uint64_t peek64z(const uint8_t* buf, size_t byte_len, size_t bit_pos) {
    const size_t byte_idx = bit_pos >> 3;
    const unsigned shift = static_cast<unsigned>(bit_pos & 7u);
    uint64_t hi = 0;
    uint64_t lo = 0;
    if (byte_idx + 9u <= byte_len) {
        const uint8_t* p = buf + byte_idx;
        hi = (static_cast<uint64_t>(p[0]) << 56) |
             (static_cast<uint64_t>(p[1]) << 48) |
             (static_cast<uint64_t>(p[2]) << 40) |
             (static_cast<uint64_t>(p[3]) << 32) |
             (static_cast<uint64_t>(p[4]) << 24) |
             (static_cast<uint64_t>(p[5]) << 16) |
             (static_cast<uint64_t>(p[6]) << 8) |
             static_cast<uint64_t>(p[7]);
        lo = p[8];
    } else {
        for (size_t i = 0; i < 8u; ++i) {
            hi <<= 8;
            if (byte_idx + i < byte_len) {
                hi |= buf[byte_idx + i];
            }
        }
        if (byte_idx + 8u < byte_len) {
            lo = buf[byte_idx + 8u];
        }
    }
    return shift ? ((hi << shift) | (lo >> (8u - shift))) : hi;
}

inline uint32_t read_bits_msb(const uint8_t* buf, size_t byte_len, size_t& bit_pos, int n_bits) {
    if (n_bits <= 0) {
        return 0;
    }
    uint32_t val = static_cast<uint32_t>(peek64z(buf, byte_len, bit_pos) >> (64 - n_bits));
    bit_pos += static_cast<size_t>(n_bits);
    return val;
}

struct Lane2Table {
    uint8_t lut[256];
};

const Lane2Table& get_lane2_lut() {
    static const Lane2Table table = []() {
        Lane2Table t{};
        for (int v = 0; v < 256; ++v) {
            unsigned pk = 0;
            for (int lane = 0; lane < 4; ++lane) {
                unsigned pair = (static_cast<unsigned>((v >> (7 - lane)) & 1) << 1) |
                                static_cast<unsigned>((v >> (3 - lane)) & 1);
                pk |= pair << (6 - 2 * lane);
            }
            t.lut[v] = static_cast<uint8_t>(pk);
        }
        return t;
    }();
    return table;
}

struct ParsedR0Header {
    uint32_t lcod = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t slh = 16;
    uint8_t nc = 4;
    uint8_t ng = 4;
    uint8_t ss = 8;
    uint8_t bw = 18;
    uint8_t fq = 4;
    uint8_t br = 4;
    uint8_t ppoc = 1;
    uint8_t ndecomp_h = 5;
    uint8_t ndecomp_v = 1;
    uint8_t quantizer_type = 1;
    uint8_t sign_strategy = 1;
    uint8_t run_mode = 0;

    bool long_pkt_hdr = false;
    bool no_raw_mode_gcli = false;
    bool b_gains = false;
    bool interframe = false;
    bool gamma_enabled = false;
    int gamma_type = -1;
    int in_bitdepth = 14;
    int out_bitdepth = 16;
    int offset_16 = 0;
    int offset_sqrt = 0;
    int r_val = 0;
    int r_flip = 0;
    bool bayer_tweaks = false;

    uint8_t cdt_depth[kMaxComponents] = {};
    uint8_t cdt_hor[kMaxComponents] = {};
    uint8_t cdt_ver[kMaxComponents] = {};

    uint8_t wgt_gain[kNumSubbands] = {};
    uint8_t wgt_prio[kNumSubbands] = {};

    size_t first_slh_off = 0;
};

struct SegmentDesc {
    int seg_id = 0;
    int pkt_idx = 0;
    int sb_id = 0;
    int pv_seg = 0;
    int comp = 0;
    int v_lvl = 0;
    int h_lvl = 0;
    int len_coef = 0;
    int len_grp = 0;
    int sig_groups = 0;
    int ofs_grp = 0;
    int pv_ofs_grp = 0;
};

struct CodecGeometry {
    int full_w = 0;
    int full_h = 0;
    int h_pad_pix = 0;
    int w_pix = 0;
    int h_pix = 0;
    int precs_per_slice = 0;
    int slh_pix = 0;
    int n_prec = 0;
    int n_slices = 0;
    int total_ofs_grp = 0;

    int v0_subband_len[6] = {};
    int v1_subband_len[2] = {};
    SegmentDesc segs[kNumSegments] = {};
    int pkt_seg_ids[kNumPackets][8] = {};
    int pkt_seg_count[kNumPackets] = {};
    int sp_sb_order[kNumSubbands] = {};
    uint8_t sb_gains[kMaxSubbandSlots] = {};
    uint8_t sb_prios[kMaxSubbandSlots] = {};

    void init(const ParsedR0Header& hdr) {
        full_w = hdr.width;
        full_h = hdr.height;
        h_pad_pix = (4 - (full_h % 4)) % 4;
        w_pix = full_w / 2;
        h_pix = (full_h + h_pad_pix) / 2;
        precs_per_slice = hdr.slh;
        slh_pix = precs_per_slice * 2;
        n_prec = h_pix / 2;
        n_slices = (n_prec + precs_per_slice - 1) / precs_per_slice;

        int w_cur = w_pix;
        for (int h = 5; h >= 1; --h) {
            v0_subband_len[h] = w_cur >> 1;
            w_cur = (w_cur + 1) >> 1;
        }
        v0_subband_len[0] = w_cur;
        v1_subband_len[1] = w_pix >> 1;
        v1_subband_len[0] = (w_pix + 1) >> 1;

        for (int p = 0; p < kNumPackets; ++p) {
            pkt_seg_count[p] = 0;
        }

        int nb_seg = 0;
        int nb_pkt = 0;
        int num_sb = 0;
        int ofs_grp = 0;

        for (int vl = 0; vl < 2; ++vl) {
            for (int c = 0; c < 4; ++c) {
                const int h_max = (c == 2) ? 1 : ((vl == 0) ? 6 : 2);
                for (int hl = 0; hl < h_max; ++hl) {
                    const int sb = (c == 2) ? (hl + 18 * c) : (hl + 6 * (vl + 3 * c));
                    const int wcoef = (c == 2) ? w_pix : ((vl == 0) ? v0_subband_len[hl] : v1_subband_len[hl]);
                    const int wgrp = (wcoef + 3) >> 2;
                    const int sgs = (wgrp + 7) >> 3;
                    int pv = nb_seg;
                    if (c == 2) {
                        pv = (vl == 1) ? (nb_seg - 11) : (nb_seg + 11);
                    }
                    SegmentDesc& sd = segs[nb_seg];
                    sd.seg_id = nb_seg;
                    sd.pkt_idx = nb_pkt;
                    sd.sb_id = sb;
                    sd.pv_seg = pv;
                    sd.comp = c;
                    sd.v_lvl = vl;
                    sd.h_lvl = hl;
                    sd.len_coef = wcoef;
                    sd.len_grp = wgrp;
                    sd.sig_groups = sgs;
                    sd.ofs_grp = ofs_grp;
                    ofs_grp += 8 * sgs;

                    pkt_seg_ids[nb_pkt][pkt_seg_count[nb_pkt]++] = nb_seg;
                    if (c != 2 || vl == 0) {
                        sp_sb_order[num_sb++] = sb;
                    }
                    ++nb_seg;
                }
                ++nb_pkt;
            }
        }
        total_ofs_grp = ofs_grp;

        for (int i = 0; i < kNumSegments; ++i) {
            segs[i].pv_ofs_grp = segs[segs[i].pv_seg].ofs_grp;
        }

        std::memset(sb_gains, 0, sizeof(sb_gains));
        std::memset(sb_prios, 0, sizeof(sb_prios));
        for (int j = 0; j < kNumSubbands; ++j) {
            const int sb = sp_sb_order[j];
            sb_gains[sb] = hdr.wgt_gain[j];
            sb_prios[sb] = hdr.wgt_prio[j];
        }
    }
};

struct PrecinctSpan {
    size_t offset = 0;
    size_t total_bytes = 0;
};

bool parse_r0_header(const uint8_t* data, size_t len, ParsedR0Header& out, std::string& err) {
    if (!data || len < kMinStreamBytes) {
        err = "TicoRAW stream too short";
        return false;
    }
    if (rd_be16(data) != 0xFF10u) {
        err = "Missing TicoRAW SOC (0xFF10)";
        return false;
    }
    size_t pos = 2;

    // CAP (0xFF50)
    if (pos + 4 > len || rd_be16(data + pos) != 0xFF50u) {
        err = "Missing TicoRAW CAP marker (0xFF50)";
        return false;
    }
    const uint16_t lcap = rd_be16(data + pos + 2);
    if (lcap < 34u || pos + 2u + lcap > len) {
        err = "Truncated or malformed CAP segment";
        return false;
    }
    const uint8_t* cap = data + pos + 4;
    const size_t cap_body_len = static_cast<size_t>(lcap) - 2u;
    if (std::memcmp(cap, "CONTACT_INTOPIX_", 16) != 0) {
        err = "Missing CONTACT_INTOPIX_ CAP signature";
        return false;
    }
    for (size_t i = 18; i < cap_body_len; ++i) {
        if (cap[i] != 0) {
            err = "Non-zero reserved bytes in CAP segment";
            return false;
        }
    }
    out.interframe = (cap[16] & 0x10u) != 0;
    if (out.interframe) {
        err = "Interframe TicoRAW streams are unsupported";
        return false;
    }
    pos += 2u + lcap;

    // PIH (0xFF12)
    if (pos + 4 > len || rd_be16(data + pos) != 0xFF12u) {
        err = "Missing TicoRAW PIH marker (0xFF12)";
        return false;
    }
    const uint16_t lpih = rd_be16(data + pos + 2);
    if (lpih != 39u || pos + 2u + lpih > len) {
        err = "Unsupported or truncated PIH segment length";
        return false;
    }
    const uint8_t* p = data + pos + 4;
    out.lcod = rd_be32(p + 0);
    if (rd_be16(p + 4) != 0u || rd_be16(p + 6) != 0u) {
        err = "Unsupported Ppih/Plev profile in PIH";
        return false;
    }
    out.width = rd_be16(p + 8);
    out.height = rd_be16(p + 10);
    if (rd_be16(p + 12) != 0u) {
        err = "Unsupported tiled precinct width (Cw != 0)";
        return false;
    }
    out.slh = rd_be16(p + 14);
    out.nc = p[16];
    out.ng = p[17];
    out.ss = p[18];
    out.bw = p[19];
    out.fq = static_cast<uint8_t>(p[20] >> 4);
    out.br = static_cast<uint8_t>(p[20] & 0x0Fu);
    const uint8_t slm = static_cast<uint8_t>(p[21] >> 7);
    out.ppoc = static_cast<uint8_t>((p[21] >> 4) & 0x07u);
    const uint8_t colour_transform = static_cast<uint8_t>(p[21] & 0x0Fu);
    out.ndecomp_h = static_cast<uint8_t>(p[22] >> 4);
    out.ndecomp_v = static_cast<uint8_t>(p[22] & 0x0Fu);
    out.quantizer_type = static_cast<uint8_t>(p[23] >> 4);
    out.sign_strategy = static_cast<uint8_t>((p[23] >> 2) & 0x03u);
    out.run_mode = static_cast<uint8_t>(p[23] & 0x03u);

    if (out.width < 4 || (out.width & 1u) != 0 || out.height < 4 || (out.height & 1u) != 0) {
        err = "Invalid TicoRAW image dimensions in PIH";
        return false;
    }
    if (out.slh == 0 || out.nc != 4 || out.ng != 4 || out.ss != 8 || out.bw == 0 || out.bw > 20 ||
        out.fq > 8 || out.br != 4 || slm != 0 || out.ppoc != 1 || colour_transform != 0 ||
        out.ndecomp_h != 5 || out.ndecomp_v != 1 || out.quantizer_type != 1 ||
        out.sign_strategy != 1 || out.run_mode != 0) {
        err = "Unsupported TicoRAW PIH coding configuration";
        return false;
    }

    // Parse 13-byte MSB-first PIH extension bitfield (p[24..36])
    const uint8_t* ext = p + 24;
    const size_t ext_bytes = 13;
    size_t bp = 0;

    const uint32_t long_hdr_bits = read_bits_msb(ext, ext_bytes, bp, 2);
    out.long_pkt_hdr = (long_hdr_bits & 1u) != 0;
    out.no_raw_mode_gcli = read_bits_msb(ext, ext_bytes, bp, 2) != 0;
    out.b_gains = read_bits_msb(ext, ext_bytes, bp, 1) != 0;
    const uint32_t g_en = read_bits_msb(ext, ext_bytes, bp, 4);
    const uint32_t g_raw = read_bits_msb(ext, ext_bytes, bp, 4);
    out.in_bitdepth = static_cast<int>(read_bits_msb(ext, ext_bytes, bp, 8));
    out.out_bitdepth = static_cast<int>(read_bits_msb(ext, ext_bytes, bp, 8));

    if (!out.long_pkt_hdr || out.b_gains || g_en > 1u || g_raw > 2u) {
        err = "Unsupported TicoRAW PIH extension flags";
        return false;
    }
    if (out.in_bitdepth < 8 || out.in_bitdepth > 16 ||
        out.out_bitdepth < out.in_bitdepth || out.out_bitdepth > 16) {
        err = "Unsupported TicoRAW PIH bit depth";
        return false;
    }
    out.gamma_enabled = (g_en != 0u);
    if (out.gamma_enabled) {
        if (g_raw != 1u && g_raw != 2u) {
            err = "Unsupported TicoRAW PIH gamma curve selection";
            return false;
        }
        const int bd_shift = out.out_bitdepth - out.in_bitdepth;
        const int raw_off13 = static_cast<int>(read_bits_msb(ext, ext_bytes, bp, 13));
        out.offset_16 = (raw_off13 >> bd_shift) << bd_shift;
        const uint32_t offset_mode = read_bits_msb(ext, ext_bytes, bp, 2);
        if (g_raw == 1u) {
            out.gamma_type = (offset_mode == 1u) ? 1 : 2;
        } else {
            out.gamma_type = (offset_mode == 3u) ? 3 : 2;
        }
        out.r_val = static_cast<int>(read_bits_msb(ext, ext_bytes, bp, 18));
        out.r_flip = static_cast<int>(read_bits_msb(ext, ext_bytes, bp, 18));
        out.offset_sqrt = static_cast<int>(read_bits_msb(ext, ext_bytes, bp, 16));
        const uint32_t tweaks_disable = read_bits_msb(ext, ext_bytes, bp, 1);
        out.bayer_tweaks = (tweaks_disable == 0u);
        (void)read_bits_msb(ext, ext_bytes, bp, 4); // reserved pad4
    } else {
        out.gamma_type = -1;
        out.bayer_tweaks = true;
    }

    const uint32_t qux_dwt53 = read_bits_msb(ext, ext_bytes, bp, 1);
    const uint32_t qux_ycbcr = read_bits_msb(ext, ext_bytes, bp, 1);
    const uint32_t diff_vh = read_bits_msb(ext, ext_bytes, bp, 1);
    if (qux_dwt53 != 1u || qux_ycbcr != 1u || diff_vh != 1u) {
        err = "Unsupported non-Bayer capability flags in PIH extension";
        return false;
    }
    pos += 2u + lpih;

    // CDT (0xFF13)
    if (pos + 4 > len || rd_be16(data + pos) != 0xFF13u) {
        err = "Missing TicoRAW CDT marker (0xFF13)";
        return false;
    }
    const uint16_t lcdt = rd_be16(data + pos + 2);
    if (lcdt != 14u || pos + 2u + lcdt > len) {
        err = "Unsupported or truncated CDT segment";
        return false;
    }
    const uint8_t* cdt = data + pos + 4;
    for (int c = 0; c < 4; ++c) {
        out.cdt_depth[c] = cdt[3 * c];
        const uint8_t sx = static_cast<uint8_t>(cdt[3 * c + 1] >> 4);
        const uint8_t sy = static_cast<uint8_t>(cdt[3 * c + 1] & 0x0Fu);
        out.cdt_hor[c] = static_cast<uint8_t>(cdt[3 * c + 2] >> 4);
        out.cdt_ver[c] = static_cast<uint8_t>(cdt[3 * c + 2] & 0x0Fu);
        if (out.cdt_depth[c] != out.in_bitdepth || sx != 1 || sy != 1) {
            err = "Unsupported CDT component geometry or depth";
            return false;
        }
        if (c == 2) {
            if (out.cdt_hor[c] != 0 || out.cdt_ver[c] != 0) {
                err = "Unsupported CDT decomposition levels for Yh";
                return false;
            }
        } else if (out.cdt_hor[c] != 5 || out.cdt_ver[c] != 1) {
            err = "Unsupported CDT decomposition levels";
            return false;
        }
    }
    pos += 2u + lcdt;

    // WGT (0xFF14)
    if (pos + 4 > len || rd_be16(data + pos) != 0xFF14u) {
        err = "Missing TicoRAW WGT marker (0xFF14)";
        return false;
    }
    const uint16_t lwgt = rd_be16(data + pos + 2);
    if (lwgt != 2u + 2u * static_cast<uint16_t>(kNumSubbands) || pos + 2u + lwgt > len) {
        err = "Unsupported or truncated WGT segment";
        return false;
    }
    const uint8_t* wgt = data + pos + 4;
    for (int j = 0; j < kNumSubbands; ++j) {
        out.wgt_gain[j] = wgt[2 * j];
        out.wgt_prio[j] = wgt[2 * j + 1];
    }
    pos += 2u + lwgt;

    // Optional COM (0xFF15) markers
    while (pos + 4 <= len && rd_be16(data + pos) == 0xFF15u) {
        const uint16_t lcom = rd_be16(data + pos + 2);
        if (lcom < 2u || pos + 2u + lcom > len) {
            err = "Truncated COM segment";
            return false;
        }
        pos += 2u + lcom;
    }

    if (pos + 6 > len || rd_be16(data + pos) != 0xFF20u) {
        err = "Missing initial SLH marker (0xFF20)";
        return false;
    }
    out.first_slh_off = pos;
    return true;
}

void fill_info_from_parsed(const ParsedR0Header& hdr, const CodecGeometry& geom, TicoRawInfo& out) {
    out = TicoRawInfo{};
    out.lcod = hdr.lcod;
    out.width = hdr.width;
    out.height = hdr.height;
    out.h_pad_pix = static_cast<uint16_t>(geom.h_pad_pix);
    out.slh = hdr.slh;
    out.slh_pix = static_cast<uint16_t>(geom.slh_pix);
    out.nc = hdr.nc;
    out.ng = hdr.ng;
    out.ss = hdr.ss;
    out.bw = hdr.bw;
    out.fq = hdr.fq;
    out.br = hdr.br;
    out.ppoc = hdr.ppoc;
    out.ndecomp_h = hdr.ndecomp_h;
    out.ndecomp_v = hdr.ndecomp_v;
    out.quantizer_type = hdr.quantizer_type;
    out.sign_strategy = hdr.sign_strategy;
    out.run_mode = hdr.run_mode;
    out.is_long_pkt_hdr = hdr.long_pkt_hdr;
    out.interframe = hdr.interframe;
    out.gamma_enabled = hdr.gamma_enabled;
    out.gamma_type = hdr.gamma_type;
    out.in_bitdepth = hdr.in_bitdepth;
    out.out_bitdepth = hdr.out_bitdepth;
    out.offset_16 = hdr.offset_16;
    out.offset_sqrt = hdr.offset_sqrt;
    out.r_val = hdr.r_val;
    out.r_flip = hdr.r_flip;
    out.cdt_depth = hdr.cdt_depth[0];
    out.yh_hor_ndecomp = hdr.cdt_hor[2];
    out.yh_ver_ndecomp = hdr.cdt_ver[2];
    out.num_sb = kNumSubbands;
    out.num_slices = geom.n_slices;
    out.num_precincts = geom.n_prec;
    out.line_stride = 4 * geom.w_pix;
}

// ISO/IEC 21122-1 5/3 integer LeGall 1D inverse DWT (non-overlapping out/even buffers).
inline void idwt53_1d(int32_t* out, const int32_t* even, const int32_t* odd, int len) {
    if (len <= 0) {
        return;
    }
    if (len == 1) {
        out[0] = even[0];
        return;
    }
    int32_t d1n = odd[0];
    int32_t s0n = even[0] - asr_s32(d1n + 1, 1);
    int i = 0;
    for (; i < len - 3; i += 2) {
        const int32_t d1c = d1n;
        const int32_t s0c = s0n;
        d1n = odd[(i >> 1) + 1];
        s0n = even[(i >> 1) + 1] - asr_s32(d1n + d1c + 2, 2);
        out[i] = s0c;
        out[i + 1] = asr_s32(s0n + s0c, 1) + d1c;
    }
    out[i] = s0n;
    if ((len & 1) != 0) {
        const int32_t last_even = even[(len - 1) >> 1] - asr_s32(d1n + 1, 1);
        out[len - 1] = last_even;
        out[len - 2] = asr_s32(last_even + s0n, 1) + d1n;
    } else {
        out[len - 1] = s0n + d1n;
    }
}

void build_inverse_nlt_lut(const ParsedR0Header& hdr, std::vector<int32_t>& lut) {
    const int lut_max_val = (1 << hdr.out_bitdepth) - 1;
    const int lut_size = hdr.offset_sqrt + (1 << hdr.out_bitdepth) + 1;
    lut.resize(static_cast<size_t>(lut_size));
    if (hdr.gamma_type == 1) {
        for (int n = 0; n < lut_size; ++n) {
            if (n <= hdr.offset_sqrt) {
                const uint64_t d = static_cast<uint64_t>(hdr.offset_sqrt - n);
                const uint64_t sq = (d * d + 0x8000u) >> 16;
                lut[static_cast<size_t>(n)] = hdr.offset_16 - static_cast<int32_t>(sq);
            } else {
                const uint64_t nc = static_cast<uint64_t>(std::min(n, lut_max_val));
                const uint64_t scaled = (static_cast<uint64_t>(hdr.r_flip) *
                                         (nc - static_cast<uint64_t>(hdr.offset_sqrt)) +
                                         0x8000u) >> 16;
                const uint64_t val = ((scaled * scaled + 0x8000u) >> 16) +
                                     static_cast<uint64_t>(hdr.offset_16);
                lut[static_cast<size_t>(n)] = static_cast<int32_t>(
                    std::min<uint64_t>(val, static_cast<uint64_t>(lut_max_val)));
            }
        }
    } else {
        // Linear pass-through scaled fallback for non-fliproot profiles
        const int thresh = 4 * hdr.offset_16;
        for (int n = 0; n < lut_size; ++n) {
            int32_t v = n;
            if (hdr.gamma_type == 2) {
                v = (n <= thresh) ? asr_s32(n, 2) : hdr.offset_16 + (n - thresh);
            }
            lut[static_cast<size_t>(n)] = clamp_i32(v, 0, lut_max_val);
        }
    }
}

template <typename Fn>
void parallel_for_range(int count, int num_threads, Fn&& fn) {
    if (count <= 0) {
        return;
    }
    const int threads = std::max(1, std::min(num_threads, count));
    if (threads == 1) {
        fn(0, count);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads - 1));
    for (int t = 1; t < threads; ++t) {
        const int lo = (count * t) / threads;
        const int hi = (count * (t + 1)) / threads;
        workers.emplace_back([&, lo, hi]() { fn(lo, hi); });
    }
    fn(0, count / threads);
    for (auto& th : workers) {
        th.join();
    }
}

bool decode_slice_entropy_and_hdwt(
    const uint8_t* data,
    const ParsedR0Header& hdr,
    const CodecGeometry& geom,
    const std::vector<PrecinctSpan>& prec_spans,
    int s0,
    int s1,
    int32_t* v_lh_planes[3],
    int32_t* plane_yh) {
    const Lane2Table& lane2 = get_lane2_lut();
    const int w_pix = geom.w_pix;
    const int n_prec = geom.n_prec;
    const int precs_per_slice = geom.precs_per_slice;
    const int fq = hdr.fq;
    const int bw = hdr.bw;

    std::vector<uint8_t> gcli_buf(static_cast<size_t>(geom.total_ofs_grp), 0);
    std::vector<int32_t> seg_bufs[kNumSegments];
    for (int sid = 0; sid < kNumSegments; ++sid) {
        seg_bufs[sid].resize(static_cast<size_t>(geom.segs[sid].len_grp) * 4u, 0);
    }
    std::vector<int32_t> idwt_a(static_cast<size_t>(w_pix), 0);
    std::vector<int32_t> idwt_b(static_cast<size_t>(w_pix), 0);

    for (int slice_idx = s0; slice_idx < s1; ++slice_idx) {
        std::fill(gcli_buf.begin(), gcli_buf.end(), 0);
        const int p_begin = slice_idx * precs_per_slice;
        const int p_end_idx = std::min(n_prec, p_begin + precs_per_slice);

        for (int p_idx = p_begin; p_idx < p_end_idx; ++p_idx) {
            const PrecinctSpan& ps = prec_spans[static_cast<size_t>(p_idx)];
            const uint8_t* phdr = data + ps.offset;
            const uint8_t* p_end = phdr + ps.total_bytes;
            const uint8_t qp = phdr[3];
            const uint8_t rp = phdr[4];

            bool sb_bsig[kMaxSubbandSlots] = {};
            bool sb_bvpred[kMaxSubbandSlots] = {};
            uint8_t sb_gtli[kMaxSubbandSlots] = {};
            size_t mode_bp = 40;
            for (int j = 0; j < kNumSubbands; ++j) {
                const int sb = geom.sp_sb_order[j];
                const uint32_t b2 = read_bits_msb(phdr, 12, mode_bp, 2);
                sb_bsig[sb] = (b2 >> 1) != 0;
                sb_bvpred[sb] = (b2 & 1u) != 0;
                const int v = static_cast<int>(qp) - static_cast<int>(geom.sb_gains[sb]);
                if (v > 0) {
                    const int refine = (static_cast<int>(geom.sb_prios[sb]) < static_cast<int>(rp)) ? 1 : 0;
                    sb_gtli[sb] = static_cast<uint8_t>(std::min(v - refine, 15));
                } else {
                    sb_gtli[sb] = 0;
                }
            }

            const bool first_of_slice = (p_idx == p_begin);
            const uint8_t* cur = phdr + 12;

            for (int pkt_idx = 0; pkt_idx < kNumPackets; ++pkt_idx) {
                if (cur + 7 > p_end) {
                    return false;
                }
                size_t pkt_bp = 0;
                const bool pkt_is_raw = read_bits_msb(cur, 7, pkt_bp, 1) != 0;
                const size_t data_B = read_bits_msb(cur, 7, pkt_bp, 20);
                const size_t gcli_B = read_bits_msb(cur, 7, pkt_bp, 20);
                const size_t sign_B = read_bits_msb(cur, 7, pkt_bp, 15);
                if (pkt_is_raw && hdr.no_raw_mode_gcli) {
                    return false;
                }

                size_t sig_bits = 0;
                if (!pkt_is_raw) {
                    for (int si = 0; si < geom.pkt_seg_count[pkt_idx]; ++si) {
                        const SegmentDesc& sd = geom.segs[geom.pkt_seg_ids[pkt_idx][si]];
                        if (sb_bsig[sd.sb_id]) {
                            sig_bits += static_cast<size_t>(sd.sig_groups);
                        }
                    }
                }
                const size_t sig_B = (sig_bits + 7u) >> 3;
                const size_t payload_total = 7u + sig_B + gcli_B + data_B + sign_B;
                if (payload_total > static_cast<size_t>(p_end - cur)) {
                    return false;
                }

                const uint8_t* sig_sub = cur + 7u;
                const uint8_t* gcli_sub = sig_sub + sig_B;
                const uint8_t* data_sub = gcli_sub + gcli_B;
                const uint8_t* sign_sub = data_sub + data_B;
                const size_t gcli_bits_max = gcli_B * 8u;
                const size_t data_bits_max = data_B * 8u;
                const size_t sign_bits_max = sign_B * 8u;

                size_t s_bp = 0;
                size_t g_bp = 0;
                size_t d_bp = 0;
                size_t sgn_bp = 0;

                for (int si = 0; si < geom.pkt_seg_count[pkt_idx]; ++si) {
                    const SegmentDesc& sd = geom.segs[geom.pkt_seg_ids[pkt_idx][si]];
                    const int sb = sd.sb_id;
                    const int k = sb_gtli[sb];
                    const bool use_sig = (!pkt_is_raw) && sb_bsig[sb];
                    const bool use_vpred = (!pkt_is_raw) && sb_bvpred[sb] &&
                                           !(first_of_slice && sd.pv_ofs_grp >= sd.ofs_grp);

                    uint8_t* const g_out = gcli_buf.data() + sd.ofs_grp;
                    const uint8_t* const g_ref = gcli_buf.data() + sd.pv_ofs_grp;
                    int32_t* const coeff_dst = seg_bufs[sd.seg_id].data();

                    // 1. Decode GCLI values into gcli_buf
                    if (pkt_is_raw) {
                        for (int cg = 0; cg < sd.len_grp; ++cg) {
                            if (g_bp + 4u > gcli_bits_max) {
                                return false;
                            }
                            g_out[cg] = static_cast<uint8_t>(
                                read_bits_msb(gcli_sub, gcli_B, g_bp, 4));
                        }
                    } else {
                        for (int sg = 0; sg < sd.sig_groups; ++sg) {
                            const int cg0 = sg * 8;
                            const int cnt = std::min(8, sd.len_grp - cg0);
                            bool all_z = false;
                            if (use_sig) {
                                all_z = ((sig_sub[s_bp >> 3] >> (7u - (s_bp & 7u))) & 1u) != 0;
                                ++s_bp;
                            }
                            if (all_z) {
                                for (int i = 0; i < cnt; ++i) {
                                    const int ref = use_vpred ? static_cast<int>(g_ref[cg0 + i]) : 0;
                                    g_out[cg0 + i] = static_cast<uint8_t>(std::max(ref, k));
                                }
                            } else {
                                for (int i = 0; i < cnt; ++i) {
                                    int u = 0;
                                    while (true) {
                                        if (g_bp >= gcli_bits_max) {
                                            return false;
                                        }
                                        const uint64_t w = peek64z(gcli_sub, gcli_B, g_bp);
                                        const int ones = count_leading_ones_64(w);
                                        const size_t avail = gcli_bits_max - g_bp;
                                        if (static_cast<size_t>(ones) >= avail) {
                                            return false;
                                        }
                                        u += ones;
                                        if (u > 2 * bw) {
                                            return false;
                                        }
                                        if (ones < 64) {
                                            g_bp += static_cast<size_t>(ones) + 1u;
                                            break;
                                        }
                                        g_bp += 64u;
                                    }
                                    const int ref = use_vpred ? static_cast<int>(g_ref[cg0 + i]) : 0;
                                    const int t = std::max(ref, k);
                                    const int dl = t - k;
                                    int g = 0;
                                    if (u == 0) {
                                        g = t;
                                    } else if (u <= 2 * dl) {
                                        g = (u & 1) ? (t - ((u + 1) >> 1)) : (t + (u >> 1));
                                    } else {
                                        g = k + u;
                                    }
                                    if (g > bw) {
                                        return false;
                                    }
                                    g_out[cg0 + i] = static_cast<uint8_t>(g);
                                }
                            }
                        }
                    }

                    // 2. Unpack interleaved magnitudes, dequantize uniformly, apply signs
                    const unsigned q_shift = static_cast<unsigned>(16 - k);
                    for (int cg = 0; cg < sd.len_grp; ++cg) {
                        const int n_bits = static_cast<int>(g_out[cg]) - k;
                        int32_t* const d4 = coeff_dst + 4 * cg;
                        if (n_bits <= 0) {
                            d4[0] = d4[1] = d4[2] = d4[3] = 0;
                            continue;
                        }
                        if (n_bits > 15 || d_bp + static_cast<size_t>(n_bits) * 4u > data_bits_max) {
                            return false;
                        }
                        uint32_t r[4] = {0, 0, 0, 0};
                        const uint64_t dw = peek64z(data_sub, data_B, d_bp);
                        const int full_bytes = n_bits >> 1;
                        for (int b = 0; b < full_bytes; ++b) {
                            const unsigned byte = static_cast<unsigned>(dw >> (56 - 8 * b)) & 0xFFu;
                            const unsigned pk = lane2.lut[byte];
                            r[0] = (r[0] << 2) | (pk >> 6);
                            r[1] = (r[1] << 2) | ((pk >> 4) & 3u);
                            r[2] = (r[2] << 2) | ((pk >> 2) & 3u);
                            r[3] = (r[3] << 2) | (pk & 3u);
                        }
                        if ((n_bits & 1) != 0) {
                            const unsigned byte = static_cast<unsigned>(dw >> (56 - 8 * full_bytes)) & 0xFFu;
                            r[0] = (r[0] << 1) | ((byte >> 7) & 1u);
                            r[1] = (r[1] << 1) | ((byte >> 6) & 1u);
                            r[2] = (r[2] << 1) | ((byte >> 5) & 1u);
                            r[3] = (r[3] << 1) | ((byte >> 4) & 1u);
                        }
                        d_bp += static_cast<size_t>(n_bits) * 4u;

                        const uint32_t mul = kUniformDqMul[n_bits - 1];
                        uint32_t mag[4];
                        uint32_t nzmask = 0;
                        for (int lane = 0; lane < 4; ++lane) {
                            mag[lane] = ((r[lane] * mul) >> q_shift) << fq;
                            if (mag[lane] != 0u) {
                                nzmask |= (1u << lane);
                            }
                        }
                        if (nzmask == 0u) {
                            d4[0] = d4[1] = d4[2] = d4[3] = 0;
                            continue;
                        }
                        const int nz_cnt = popcount4(nzmask);
                        if (sgn_bp + static_cast<size_t>(nz_cnt) > sign_bits_max) {
                            return false;
                        }
                        const uint32_t s_nib = static_cast<uint32_t>(
                            peek64z(sign_sub, sign_B, sgn_bp) >> 60);
                        sgn_bp += static_cast<size_t>(nz_cnt);
                        int s_bit_idx = 3;
                        for (int lane = 0; lane < 4; ++lane) {
                            if (mag[lane] == 0u) {
                                d4[lane] = 0;
                            } else {
                                const bool neg = ((s_nib >> s_bit_idx) & 1u) != 0;
                                --s_bit_idx;
                                const int32_t mv = static_cast<int32_t>(mag[lane]);
                                d4[lane] = neg ? -mv : mv;
                            }
                        }
                    }
                }

                if (((g_bp + 7u) >> 3) != gcli_B ||
                    ((d_bp + 7u) >> 3) != data_B ||
                    ((sgn_bp + 7u) >> 3) != sign_B) {
                    return false;
                }
                cur += payload_total;
            }

            // 3. Reconstruct 1D horizontal IDWT rows directly into v_lh_planes[0..2], and copy Yh
            const int c_map[3] = {0, 1, 3};
            for (int ci = 0; ci < 3; ++ci) {
                const int comp = c_map[ci];
                int32_t* v0_src[6] = {};
                int32_t* v1_src[2] = {};
                for (int si = 0; si < kNumSegments; ++si) {
                    const SegmentDesc& sd = geom.segs[si];
                    if (sd.comp == comp) {
                        if (sd.v_lvl == 0) {
                            v0_src[sd.h_lvl] = seg_bufs[si].data();
                        } else {
                            v1_src[sd.h_lvl] = seg_bufs[si].data();
                        }
                    }
                }

                // v = 0: 5-level horizontal cascade h_lvl = 1 .. 5
                std::memcpy(idwt_a.data(), v0_src[0],
                            static_cast<size_t>(geom.v0_subband_len[0]) * sizeof(int32_t));
                int len_prev = geom.v0_subband_len[0];
                int32_t* ping = idwt_a.data();
                int32_t* pong = idwt_b.data();
                for (int hl = 1; hl <= 5; ++hl) {
                    const int len_next = len_prev + geom.v0_subband_len[hl];
                    int32_t* dst = (hl == 5)
                                       ? (v_lh_planes[ci] + static_cast<size_t>(p_idx) * static_cast<size_t>(w_pix))
                                       : pong;
                    idwt53_1d(dst, ping, v0_src[hl], len_next);
                    len_prev = len_next;
                    std::swap(ping, pong);
                }

                // v = 1: 1-level horizontal IDWT
                idwt53_1d(v_lh_planes[ci] + static_cast<size_t>(n_prec + p_idx) * static_cast<size_t>(w_pix),
                          v1_src[0], v1_src[1], w_pix);
            }

            // comp == 2 (Yh): zero DWT, direct copy of seg 12 (row 2*p) and seg 23 (row 2*p + 1)
            std::memcpy(plane_yh + static_cast<size_t>(2 * p_idx) * static_cast<size_t>(w_pix),
                        seg_bufs[12].data(), static_cast<size_t>(w_pix) * sizeof(int32_t));
            std::memcpy(plane_yh + static_cast<size_t>(2 * p_idx + 1) * static_cast<size_t>(w_pix),
                        seg_bufs[23].data(), static_cast<size_t>(w_pix) * sizeof(int32_t));
        }
    }
    return true;
}

} // namespace

bool probe_ticoraw_header(const uint8_t* data, size_t len, TicoRawInfo& info, std::string& err) {
    ParsedR0Header hdr;
    if (!parse_r0_header(data, len, hdr, err)) {
        return false;
    }
    CodecGeometry geom;
    geom.init(hdr);
    fill_info_from_parsed(hdr, geom, info);
    return true;
}

bool decode_ticoraw_bayer(
    const uint8_t* data,
    size_t len,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    TicoRawInfo& info_out,
    std::string& err) {
    ParsedR0Header hdr;
    if (!parse_r0_header(data, len, hdr, err)) {
        return false;
    }
    CodecGeometry geom;
    geom.init(hdr);

    // Pre-scan all slices & precincts sequentially (validates full stream before worker dispatch)
    std::vector<PrecinctSpan> prec_spans(static_cast<size_t>(geom.n_prec));
    size_t pos = hdr.first_slh_off;
    for (int s = 0; s < geom.n_slices; ++s) {
        if (pos + 6u > len ||
            rd_be16(data + pos) != 0xFF20u ||
            rd_be16(data + pos + 2) != 4u ||
            rd_be16(data + pos + 4) != static_cast<uint16_t>(s)) {
            err = "Malformed or truncated SLH slice header";
            return false;
        }
        pos += 6u;
        const int p0 = s * geom.precs_per_slice;
        const int p1 = std::min(geom.n_prec, p0 + geom.precs_per_slice);
        for (int p = p0; p < p1; ++p) {
            if (pos + 12u > len) {
                err = "Truncated precinct header";
                return false;
            }
            const size_t body_len = rd_be24(data + pos);
            const size_t total = 12u + body_len;
            if (body_len == 0 || pos + total > len) {
                err = "Truncated precinct body";
                return false;
            }
            prec_spans[static_cast<size_t>(p)] = PrecinctSpan{pos, total};
            pos += total;
        }
    }
    if (pos + 2u > len || rd_be16(data + pos) != 0xFF11u) {
        err = "Missing TicoRAW EOC marker (0xFF11)";
        return false;
    }

    const int threads = (num_threads > 0)
                            ? num_threads
                            : static_cast<int>(std::max(1u, std::min(8u, std::thread::hardware_concurrency())));

    const int w_pix = geom.w_pix;
    const int h_pix = geom.h_pix;
    const int n_prec = geom.n_prec;
    const size_t plane_elems = static_cast<size_t>(w_pix) * static_cast<size_t>(h_pix);

    // Seven half-resolution int32 planes:
    //   [0..2]: v_low + v_high for (Yl, Cr, Cb); [0..1] reused in Stage 2 for (G0, G1)
    //   [3]:    Yh (direct copy)
    //   [4..6]: spatial (Yl, Cr, Cb) after vertical IDWT
    std::unique_ptr<int32_t[]> arena(new int32_t[7u * plane_elems]);
    int32_t* v_lh_planes[3] = {arena.get(), arena.get() + plane_elems, arena.get() + 2u * plane_elems};
    int32_t* plane_yh = arena.get() + 3u * plane_elems;
    int32_t* plane_yl = arena.get() + 4u * plane_elems;
    int32_t* plane_cr = arena.get() + 5u * plane_elems;
    int32_t* plane_cb = arena.get() + 6u * plane_elems;
    int32_t* out_spatial[3] = {plane_yl, plane_cr, plane_cb};

    // Stage 1: Slice-parallel entropy decode + 1D horizontal IDWT (zero overlap re-decode)
    std::atomic<int> err_flag{0};
    parallel_for_range(geom.n_slices, threads, [&](int s0, int s1) {
        if (!decode_slice_entropy_and_hdwt(data, hdr, geom, prec_spans, s0, s1, v_lh_planes, plane_yh)) {
            err_flag.store(1, std::memory_order_relaxed);
        }
    });
    if (err_flag.load() != 0) {
        err = "Corrupt TicoRAW packet entropy data";
        return false;
    }

    // Stage 1.5: Parallel vertical 1-level 5/3 IDWT (even pass then odd pass)
    parallel_for_range(n_prec, threads, [&](int k0, int k1) {
        for (int ci = 0; ci < 3; ++ci) {
            const int32_t* const lo_base = v_lh_planes[ci];
            const int32_t* const hi_base = v_lh_planes[ci] + static_cast<size_t>(n_prec) * static_cast<size_t>(w_pix);
            int32_t* const dst_base = out_spatial[ci];
            for (int k = k0; k < k1; ++k) {
                const int32_t* const L = lo_base + static_cast<size_t>(k) * static_cast<size_t>(w_pix);
                const int32_t* const Hc = hi_base + static_cast<size_t>(k) * static_cast<size_t>(w_pix);
                int32_t* const dst_even = dst_base + static_cast<size_t>(2 * k) * static_cast<size_t>(w_pix);
                if (k == 0) {
                    for (int col = 0; col < w_pix; ++col) {
                        dst_even[col] = L[col] - asr_s32(Hc[col] + 1, 1);
                    }
                } else {
                    const int32_t* const Hp = hi_base + static_cast<size_t>(k - 1) * static_cast<size_t>(w_pix);
                    for (int col = 0; col < w_pix; ++col) {
                        dst_even[col] = L[col] - asr_s32(Hp[col] + Hc[col] + 2, 2);
                    }
                }
            }
        }
    });
    parallel_for_range(n_prec, threads, [&](int k0, int k1) {
        for (int ci = 0; ci < 3; ++ci) {
            const int32_t* const hi_base = v_lh_planes[ci] + static_cast<size_t>(n_prec) * static_cast<size_t>(w_pix);
            int32_t* const dst_base = out_spatial[ci];
            for (int k = k0; k < k1; ++k) {
                const int32_t* const Hc = hi_base + static_cast<size_t>(k) * static_cast<size_t>(w_pix);
                const int32_t* const ev0 = dst_base + static_cast<size_t>(2 * k) * static_cast<size_t>(w_pix);
                int32_t* const dst_odd = dst_base + static_cast<size_t>(2 * k + 1) * static_cast<size_t>(w_pix);
                if (k < n_prec - 1) {
                    const int32_t* const ev1 = dst_base + static_cast<size_t>(2 * k + 2) * static_cast<size_t>(w_pix);
                    for (int col = 0; col < w_pix; ++col) {
                        dst_odd[col] = Hc[col] + asr_s32(ev0[col] + ev1[col], 1);
                    }
                } else {
                    for (int col = 0; col < w_pix; ++col) {
                        dst_odd[col] = Hc[col] + ev0[col];
                    }
                }
            }
        }
    });

    // Reuse v_lh_planes[0..1] for reconstructed (G0, G1)
    int32_t* const plane_g0 = v_lh_planes[0];
    int32_t* const plane_g1 = v_lh_planes[1];

    // Stage 2: Inverse Star-Tetrix step 1 -> recover (G0, G1) from (Yl, Yh, Cr, Cb)
    parallel_for_range(h_pix, threads, [&](int r0, int r1) {
        for (int r = r0; r < r1; ++r) {
            const int r_up = (r > 0) ? (r - 1) : 0;
            const int r_dn = (r + 1 < h_pix) ? (r + 1) : (h_pix - 1);
            const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
            const size_t row_up = static_cast<size_t>(r_up) * static_cast<size_t>(w_pix);
            const size_t row_dn = static_cast<size_t>(r_dn) * static_cast<size_t>(w_pix);

            int32_t y0_lt = 0;
            int32_t y0_dn_lt = 0;
            for (int c = 0; c < w_pix; ++c) {
                const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
                const int32_t yh_sum = plane_yh[row_up + c] + plane_yh[row_up + c_rt] +
                                       plane_yh[row + c] + plane_yh[row + c_rt];
                const int32_t y0 = plane_yl[row + c] - asr_s32(yh_sum, 3);
                const int32_t y0_dn = (r == h_pix - 1)
                    ? y0
                    : (plane_yl[row_dn + c] -
                       asr_s32(plane_yh[row + c] + plane_yh[row + c_rt] +
                               plane_yh[row_dn + c] + plane_yh[row_dn + c_rt], 3));
                if (c == 0) {
                    y0_lt = y0;
                    y0_dn_lt = y0_dn;
                }
                const int c_lt = (c > 0) ? (c - 1) : 0;
                plane_g0[row + c] = y0 - asr_s32(plane_cb[row + c] + plane_cb[row_up + c] +
                                                 plane_cr[row + c] + plane_cr[row + c_rt], 3);
                plane_g1[row + c] = asr_s32(y0_dn + y0 + y0_dn_lt + y0_lt, 2) +
                                    plane_yh[row + c] -
                                    asr_s32(plane_cb[row + c_lt] + plane_cb[row + c] +
                                            plane_cr[row + c] + plane_cr[row_dn + c], 3);
                y0_lt = y0;
                y0_dn_lt = y0_dn;
            }
        }
    });

    // Stage 3: Inverse Star-Tetrix step 2 (R, B) + NLT LUT lookup -> uint16 RGGB Bayer
    const int full_w = geom.full_w;
    const int full_h = geom.full_h;
    const int active_quad_h = full_h / 2;
    bayer_out.resize(static_cast<size_t>(full_w) * static_cast<size_t>(full_h));

    const int input_scale = hdr.bayer_tweaks ? (static_cast<int>(hdr.bw) - 2) : hdr.out_bitdepth;
    const int scale = input_scale - hdr.in_bitdepth;
    const int scale_off = (scale > 0) ? (1 << (scale - 1)) : 0;
    const int post_offset = 1 << (scale + hdr.in_bitdepth - 1);
    const int32_t max_value = (1 << hdr.in_bitdepth) - 1;

    std::vector<int32_t> nlt_lut;
    if (hdr.gamma_enabled) {
        build_inverse_nlt_lut(hdr, nlt_lut);
    }
    const int lut_max_idx = static_cast<int>(nlt_lut.size()) - 1;
    const int32_t* const lut_ptr = hdr.gamma_enabled ? nlt_lut.data() : nullptr;

    auto map_sample = [&](int32_t x) -> uint16_t {
        if (lut_ptr) {
            const int idx = clamp_i32(x + post_offset, 0, lut_max_idx);
            const int32_t out = (scale > 0) ? asr_s32(lut_ptr[idx] + scale_off, scale) : lut_ptr[idx];
            return static_cast<uint16_t>(clamp_i32(out, 0, max_value));
        }
        const int32_t out = (scale > 0) ? asr_s32(x + post_offset + scale_off, scale) : (x + post_offset);
        return static_cast<uint16_t>(clamp_i32(out, 0, max_value));
    };

    parallel_for_range(active_quad_h, threads, [&](int r0, int r1) {
        for (int r = r0; r < r1; ++r) {
            const int r_up = (r > 0) ? (r - 1) : 0;
            const int r_dn = (r + 1 < h_pix) ? (r + 1) : (h_pix - 1);
            const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
            const size_t row_up = static_cast<size_t>(r_up) * static_cast<size_t>(w_pix);
            const size_t row_dn = static_cast<size_t>(r_dn) * static_cast<size_t>(w_pix);
            uint16_t* const out_even = bayer_out.data() + static_cast<size_t>(2 * r) * static_cast<size_t>(full_w);
            uint16_t* const out_odd = out_even + full_w;

            for (int c = 0; c < w_pix; ++c) {
                const int c_lt = (c > 0) ? (c - 1) : 0;
                const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
                const int32_t g0 = plane_g0[row + c];
                const int32_t g1 = plane_g1[row + c];
                const int32_t red = asr_s32(g1 + g0 + plane_g0[row + c_lt] + plane_g1[row_up + c], 2) +
                                    plane_cr[row + c];
                const int32_t blue = asr_s32(g1 + g0 + plane_g1[row + c_rt] + plane_g0[row_dn + c], 2) +
                                     plane_cb[row + c];

                out_even[2 * c] = map_sample(red);
                out_even[2 * c + 1] = map_sample(g0);
                out_odd[2 * c] = map_sample(g1);
                out_odd[2 * c + 1] = map_sample(blue);
            }
        }
    });

    fill_info_from_parsed(hdr, geom, info_out);
    return true;
}

} // namespace nev2bayer
