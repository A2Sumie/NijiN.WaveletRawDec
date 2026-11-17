// SPDX-License-Identifier: Apache-2.0
// Clean-room C++17 TicoRAW / JPEG XS R0 Bayer decoder for nijinWVDC.
// Implements ISO/IEC 21122-1 / R0 Bayer extensions (CAP, PIH, CDT, WGT, SLH,
// bounded GCLI prediction, uniform dequantization, 5/3 IDWT, Star-Tetrix RCT,
// and piecewise Fliproot NLT inverse LUT) with zero external dependencies.

#include "../include/nijinWVDC.h"
#include "codec_internal.h"
#include "quant_model_internal.h"
#include "simd_platform.h"
#include "worker_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace nijinwvdc {
namespace {

using detail::rd_be64_fast;
using detail::clz64_nz;

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
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
    return x >> shift;
#else
    if (shift == 0u) {
        return x;
    }
    if (x < 0) {
        return ~((~x) >> shift);
    }
    return x >> shift;
#endif
}

inline int count_leading_ones_64(uint64_t w) {
    const uint64_t inv = ~w;
    return (inv == 0ULL) ? 64 : clz64_nz(inv);
}

inline int popcount4(uint32_t mask) {
    static const uint8_t kPop4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    return kPop4[mask & 15u];
}

// Lookup mapping [nzmask (0..15)][s_nib (0..15)] -> 4-bit negative lane mask.
struct SignExpandTable {
    uint8_t mask[16][16];
};

const SignExpandTable& get_sign_expand_table() {
    static const SignExpandTable tbl = []() {
        SignExpandTable t{};
        for (int nz = 0; nz < 16; ++nz) {
            for (int sn = 0; sn < 16; ++sn) {
                int sbit = 3;
                uint8_t out = 0;
                for (int lane = 0; lane < 4; ++lane) {
                    if ((nz >> lane) & 1) {
                        if ((sn >> sbit) & 1) {
                            out = static_cast<uint8_t>(out | (1u << lane));
                        }
                        --sbit;
                    }
                }
                t.mask[nz][sn] = out;
            }
        }
        return t;
    }();
    return tbl;
}

// Bounds-safe 64-bit MSB-first window peek starting at bit `bit_pos` inside `buf[0..byte_len)`.
// Bits beyond `byte_len` read as 0.
inline uint64_t peek64z(const uint8_t* buf, size_t byte_len, size_t bit_pos) {
    const size_t byte_idx = bit_pos >> 3;
    const unsigned shift = static_cast<unsigned>(bit_pos & 7u);
    if (shift == 0u && byte_idx + 8u <= byte_len) {
        return rd_be64_fast(buf + byte_idx);
    }
    if (byte_idx + 9u <= byte_len) {
        const uint64_t hi = rd_be64_fast(buf + byte_idx);
        const uint64_t lo = buf[byte_idx + 8u];
        return (hi << shift) | (lo >> (8u - shift));
    }
    uint64_t hi = 0ULL;
    uint64_t lo = 0ULL;
    for (size_t i = 0u; i < 8u; ++i) {
        hi <<= 8;
        if (byte_idx + i < byte_len) {
            hi |= buf[byte_idx + i];
        }
    }
    if (byte_idx + 8u < byte_len) {
        lo = buf[byte_idx + 8u];
    }
    return shift ? ((hi << shift) | (lo >> (8u - shift))) : hi;
}

inline uint32_t read_bits_msb(const uint8_t* buf, size_t byte_len, size_t& bit_pos, int n_bits) {
    if (n_bits <= 0) {
        return 0;
    }
    const uint32_t val = static_cast<uint32_t>(peek64z(buf, byte_len, bit_pos) >> (64 - n_bits));
    bit_pos += static_cast<size_t>(n_bits);
    return val;
}

// Branchless 64-bit SWAR bit-matrix transpose unpacking 4 MSB-interleaved lanes of `n_bits` in [1, 15].
inline void deinterleave_4lanes_msb(uint64_t dw, int n_bits, uint32_t r[4]) {
    uint64_t x = dw >> (64 - 4 * n_bits);
    uint64_t t;
    t = ((x >> 3) ^ x) & 0x0A0A0A0A0A0A0A0AULL;
    x ^= t ^ (t << 3);
    t = ((x >> 12) ^ x) & 0x0000F0F00000F0F0ULL;
    x ^= t ^ (t << 12);
    t = ((x >> 6) ^ x) & 0x00CC00CC00CC00CCULL;
    x ^= t ^ (t << 6);
    t = ((x >> 24) ^ x) & 0x00000000FF00FF00ULL;
    x ^= t ^ (t << 24);
    r[0] = static_cast<uint32_t>(x >> 48);
    r[1] = static_cast<uint32_t>((x >> 32) & 0xFFFFu);
    r[2] = static_cast<uint32_t>((x >> 16) & 0xFFFFu);
    r[3] = static_cast<uint32_t>(x & 0xFFFFu);
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
    int comp_v0_seg[3][6] = {};
    int comp_v1_seg[3][2] = {};
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
        const int c_map[3] = {0, 1, 3};
        for (int ci = 0; ci < 3; ++ci) {
            for (int si = 0; si < kNumSegments; ++si) {
                const SegmentDesc& sd = segs[si];
                if (sd.comp == c_map[ci]) {
                    if (sd.v_lvl == 0) {
                        comp_v0_seg[ci][sd.h_lvl] = si;
                    } else {
                        comp_v1_seg[ci][sd.h_lvl] = si;
                    }
                }
            }
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

#if NIJIN_X86_64
NIJIN_NOINLINE NIJIN_TARGET_AVX2 void idwt53_1d_avx2(int32_t* out, const int32_t* even, const int32_t* odd, int len) {
    const int limit_k = (len - 2) >> 1;
    int k = 0;
    int32_t s0n = even[0] - asr_s32(odd[0] + 1, 1);
    const __m256i two_v = _mm256_set1_epi32(2);
    __m256i s0_prev_tail = _mm256_set1_epi32(s0n);
    for (; k + 8 <= limit_k; k += 8) {
        const __m256i ev_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(even + k + 1));
        const __m256i od0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(odd + k));
        const __m256i od1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(odd + k + 1));
        const __m256i s0_hi = _mm256_sub_epi32(
            ev_v, _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(od0, od1), two_v), 2));
        const __m256i mid = _mm256_permute2x128_si256(s0_prev_tail, s0_hi, 0x21);
        const __m256i s0_cur = _mm256_alignr_epi8(s0_hi, mid, 12);
        const __m256i odd_out = _mm256_add_epi32(
            _mm256_srai_epi32(_mm256_add_epi32(s0_cur, s0_hi), 1), od0);
        const __m256i lo = _mm256_unpacklo_epi32(s0_cur, odd_out);
        const __m256i hi = _mm256_unpackhi_epi32(s0_cur, odd_out);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 2 * k), _mm256_permute2x128_si256(lo, hi, 0x20));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 2 * k + 8), _mm256_permute2x128_si256(lo, hi, 0x31));
        s0_prev_tail = s0_hi;
    }
    s0n = _mm256_extract_epi32(s0_prev_tail, 7);
    int32_t d1n = odd[k];
    int i = 2 * k;
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
#endif

// ISO/IEC 21122-1 5/3 integer LeGall 1D inverse DWT (non-overlapping out/even buffers).
inline void idwt53_1d(int32_t* out, const int32_t* even, const int32_t* odd, int len) {
    if (len <= 0) {
        return;
    }
    if (len == 1) {
        out[0] = even[0];
        return;
    }
    const int limit_k = (len - 2) >> 1;
#if NIJIN_X86_64
    if (limit_k >= 8 && detail::cpu_has_avx2()) {
        idwt53_1d_avx2(out, even, odd, len);
        return;
    }
#endif
    int k = 0;
    int32_t s0n = even[0] - asr_s32(odd[0] + 1, 1);
#if NIJIN_ARM_NEON
    if (limit_k >= 4) {
        const int32x4_t two_v = vdupq_n_s32(2);
        int32x4_t s0_prev_tail = vdupq_n_s32(s0n);
        for (; k + 4 <= limit_k; k += 4) {
            const int32x4_t ev_v = vld1q_s32(even + k + 1);
            const int32x4_t od0 = vld1q_s32(odd + k);
            const int32x4_t od1 = vld1q_s32(odd + k + 1);
            const int32x4_t s0_hi = vsubq_s32(ev_v, vshrq_n_s32(vaddq_s32(vaddq_s32(od0, od1), two_v), 2));
            const int32x4_t s0_cur = vextq_s32(s0_prev_tail, s0_hi, 3);
            const int32x4_t odd_out = vaddq_s32(vshrq_n_s32(vaddq_s32(s0_cur, s0_hi), 1), od0);
            int32x4x2_t pair;
            pair.val[0] = s0_cur;
            pair.val[1] = odd_out;
            vst2q_s32(out + 2 * k, pair);
            s0_prev_tail = s0_hi;
        }
        s0n = vgetq_lane_s32(s0_prev_tail, 3);
    }
#elif NIJIN_X86_64
    if (limit_k >= 4) {
        const __m128i two_v = _mm_set1_epi32(2);
        __m128i s0_prev_tail = _mm_set1_epi32(s0n);
        for (; k + 4 <= limit_k; k += 4) {
            const __m128i ev_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(even + k + 1));
            const __m128i od0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(odd + k));
            const __m128i od1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(odd + k + 1));
            const __m128i s0_hi = _mm_sub_epi32(
                ev_v, _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(od0, od1), two_v), 2));
            const __m128i s0_cur = _mm_or_si128(
                _mm_slli_si128(s0_hi, 4), _mm_srli_si128(s0_prev_tail, 12));
            const __m128i odd_out = _mm_add_epi32(
                _mm_srai_epi32(_mm_add_epi32(s0_cur, s0_hi), 1), od0);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * k), _mm_unpacklo_epi32(s0_cur, odd_out));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * k + 4), _mm_unpackhi_epi32(s0_cur, odd_out));
            s0_prev_tail = s0_hi;
        }
        s0n = _mm_cvtsi128_si32(_mm_srli_si128(s0_prev_tail, 12));
    }
#endif
    int32_t d1n = odd[k];
    int i = 2 * k;
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

struct NltLutCacheKey {
    bool gamma_enabled = false;
    int gamma_type = -1;
    int in_bitdepth = 0;
    int out_bitdepth = 0;
    int bw = 0;
    bool bayer_tweaks = false;
    int offset_16 = 0;
    int offset_sqrt = 0;
    int r_flip = 0;

    bool operator==(const NltLutCacheKey& o) const {
        return gamma_enabled == o.gamma_enabled &&
               gamma_type == o.gamma_type &&
               in_bitdepth == o.in_bitdepth &&
               out_bitdepth == o.out_bitdepth &&
               bw == o.bw &&
               bayer_tweaks == o.bayer_tweaks &&
               offset_16 == o.offset_16 &&
               offset_sqrt == o.offset_sqrt &&
               r_flip == o.r_flip;
    }
};

struct BakedNltCache {
    NltLutCacheKey key;
    bool valid = false;
    int lut_max_idx = 0;
    std::vector<uint16_t> lut;

    void prepare(const ParsedR0Header& hdr) {
        if (!hdr.gamma_enabled) {
            return;
        }
        NltLutCacheKey cur_key;
        cur_key.gamma_enabled = hdr.gamma_enabled;
        cur_key.gamma_type = hdr.gamma_type;
        cur_key.in_bitdepth = hdr.in_bitdepth;
        cur_key.out_bitdepth = hdr.out_bitdepth;
        cur_key.bw = hdr.bw;
        cur_key.bayer_tweaks = hdr.bayer_tweaks;
        cur_key.offset_16 = hdr.offset_16;
        cur_key.offset_sqrt = hdr.offset_sqrt;
        cur_key.r_flip = hdr.r_flip;
        if (valid && (key == cur_key)) {
            return;
        }

        const int input_scale = hdr.bayer_tweaks ? (static_cast<int>(hdr.bw) - 2) : hdr.out_bitdepth;
        const int scale = input_scale - hdr.in_bitdepth;
        const int scale_off = (scale > 0) ? (1 << (scale - 1)) : 0;
        const int32_t max_value = (1 << hdr.in_bitdepth) - 1;

        const int lut_max_val = (1 << hdr.out_bitdepth) - 1;
        const int lut_size = hdr.offset_sqrt + (1 << hdr.out_bitdepth) + 1;
        lut.resize(static_cast<size_t>(lut_size));
        const unsigned u_scale = (scale > 0) ? static_cast<unsigned>(scale) : 0u;

        if (hdr.gamma_type == 1) {
            for (int n = 0; n < lut_size; ++n) {
                int32_t raw_v = 0;
                if (n <= hdr.offset_sqrt) {
                    const uint64_t d = static_cast<uint64_t>(hdr.offset_sqrt - n);
                    const uint64_t sq = (d * d + 0x8000u) >> 16;
                    raw_v = hdr.offset_16 - static_cast<int32_t>(sq);
                } else {
                    const uint64_t nc = static_cast<uint64_t>(std::min(n, lut_max_val));
                    const uint64_t scaled = (static_cast<uint64_t>(hdr.r_flip) *
                                             (nc - static_cast<uint64_t>(hdr.offset_sqrt)) +
                                             0x8000u) >> 16;
                    const uint64_t val = ((scaled * scaled + 0x8000u) >> 16) +
                                         static_cast<uint64_t>(hdr.offset_16);
                    raw_v = static_cast<int32_t>(
                        std::min<uint64_t>(val, static_cast<uint64_t>(lut_max_val)));
                }
                const int32_t out = (scale > 0) ? asr_s32(raw_v + scale_off, u_scale) : raw_v;
                lut[static_cast<size_t>(n)] = static_cast<uint16_t>(clamp_i32(out, 0, max_value));
            }
        } else {
            const int thresh = 4 * hdr.offset_16;
            for (int n = 0; n < lut_size; ++n) {
                int32_t v = n;
                if (hdr.gamma_type == 2) {
                    v = (n <= thresh) ? asr_s32(n, 2) : hdr.offset_16 + (n - thresh);
                }
                const int32_t raw_v = clamp_i32(v, 0, lut_max_val);
                const int32_t out = (scale > 0) ? asr_s32(raw_v + scale_off, u_scale) : raw_v;
                lut[static_cast<size_t>(n)] = static_cast<uint16_t>(clamp_i32(out, 0, max_value));
            }
        }
        lut_max_idx = lut_size - 1;
        key = cur_key;
        valid = true;
    }
};

struct SliceWorkBuf {
    std::vector<uint8_t> gcli_buf;
    std::vector<int32_t> seg_bufs[kNumSegments];
    std::vector<int32_t> idwt_a;
    std::vector<int32_t> idwt_b;
    std::vector<int32_t> v_ev_next;
    std::vector<int32_t> y0_a;
    std::vector<int32_t> y0_b;
    std::vector<int32_t> g0_slots[3];
    std::vector<int32_t> g1_slots[3];

    void ensure_stage1(const CodecGeometry& geom) {
        if (gcli_buf.size() < static_cast<size_t>(geom.total_ofs_grp)) {
            gcli_buf.resize(static_cast<size_t>(geom.total_ofs_grp), 0);
        }
        for (int sid = 0; sid < kNumSegments; ++sid) {
            const size_t need = static_cast<size_t>(geom.segs[sid].len_grp) * 4u;
            if (seg_bufs[sid].size() < need) {
                seg_bufs[sid].resize(need, 0);
            }
        }
        if (idwt_a.size() < static_cast<size_t>(geom.w_pix)) {
            idwt_a.resize(static_cast<size_t>(geom.w_pix), 0);
        }
        if (idwt_b.size() < static_cast<size_t>(geom.w_pix)) {
            idwt_b.resize(static_cast<size_t>(geom.w_pix), 0);
        }
    }

    void ensure_v_even_cap(int w_pix) {
        if (v_ev_next.size() < static_cast<size_t>(w_pix)) {
            v_ev_next.resize(static_cast<size_t>(w_pix), 0);
        }
    }

    void ensure_ring_cap(int w_pix) {
        const size_t need = static_cast<size_t>(w_pix);
        if (y0_a.size() < need) y0_a.resize(need);
        if (y0_b.size() < need) y0_b.resize(need);
        for (int i = 0; i < 3; ++i) {
            if (g0_slots[i].size() < need) g0_slots[i].resize(need);
            if (g1_slots[i].size() < need) g1_slots[i].resize(need);
        }
    }
};

struct TicoArenaImpl {
    std::vector<int32_t> planes;
    std::vector<PrecinctSpan> prec_spans;
    std::vector<SliceWorkBuf> workers;
    BakedNltCache nlt_cache;

    void ensure_workers(int n) {
        if (static_cast<int>(workers.size()) < n) {
            workers.resize(static_cast<size_t>(n));
        }
    }
};

inline std::mutex& global_tico_arena_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline TicoArenaImpl& global_tico_arena_instance() {
    static TicoArenaImpl arena;
    return arena;
}

struct TicoArenaLease {
    std::unique_lock<std::mutex> global_lk;
    std::unique_ptr<TicoArenaImpl> local_arena;
    TicoArenaImpl* ptr = nullptr;

    explicit TicoArenaLease(TicoArenaImpl* external) {
        if (external != nullptr) {
            ptr = external;
            return;
        }
        std::unique_lock<std::mutex> lk(global_tico_arena_mutex(), std::try_to_lock);
        if (lk.owns_lock()) {
            global_lk = std::move(lk);
            ptr = &global_tico_arena_instance();
        } else {
            local_arena.reset(new TicoArenaImpl());
            ptr = local_arena.get();
        }
    }
};

bool decode_single_slice_entropy_and_hdwt(
    const uint8_t* data,
    const ParsedR0Header& hdr,
    const CodecGeometry& geom,
    const std::vector<PrecinctSpan>& prec_spans,
    int slice_idx,
    SliceWorkBuf& ww,
    int32_t* v_lh_planes[3],
    int32_t* plane_yh) {
    const SignExpandTable& sign_tbl = get_sign_expand_table();
    const int w_pix = geom.w_pix;
    const int n_prec = geom.n_prec;
    const int precs_per_slice = geom.precs_per_slice;
    const int fq = hdr.fq;
    const int bw = hdr.bw;

    std::memset(ww.gcli_buf.data(), 0, static_cast<size_t>(geom.total_ofs_grp));
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
        const uint64_t mode_w = rd_be64_fast(phdr + 4);
        for (int j = 0; j < kNumSubbands; ++j) {
            const int sb = geom.sp_sb_order[j];
            const uint32_t b2 = static_cast<uint32_t>(mode_w >> (54 - 2 * j)) & 3u;
            sb_bsig[sb] = (b2 >> 1) != 0u;
            sb_bvpred[sb] = (b2 & 1u) != 0u;
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

            const uint8_t* const sig_sub = cur + 7u;
            const uint8_t* const gcli_sub = sig_sub + sig_B;
            const uint8_t* const data_sub = gcli_sub + gcli_B;
            const uint8_t* const sign_sub = data_sub + data_B;
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

                uint8_t* const g_out = ww.gcli_buf.data() + sd.ofs_grp;
                const uint8_t* const g_ref = ww.gcli_buf.data() + sd.pv_ofs_grp;
                int32_t* const coeff_dst = ww.seg_bufs[sd.seg_id].data();

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
                            if (!use_vpred) {
                                std::memset(g_out + cg0, static_cast<uint8_t>(k), static_cast<size_t>(cnt));
                            } else {
                                for (int i = 0; i < cnt; ++i) {
                                    const int ref = static_cast<int>(g_ref[cg0 + i]);
                                    g_out[cg0 + i] = static_cast<uint8_t>(std::max(ref, k));
                                }
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

                // 2. Branchless SWAR 4-lane magnitude unpack, uniform dequantization, and signs
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
                    const uint64_t dw = peek64z(data_sub, data_B, d_bp);
                    d_bp += static_cast<size_t>(n_bits) * 4u;

                    uint32_t r[4];
                    deinterleave_4lanes_msb(dw, n_bits, r);

                    const uint32_t mul = kUniformDqMul[n_bits - 1];
                    const uint32_t m0 = ((r[0] * mul) >> q_shift) << fq;
                    const uint32_t m1 = ((r[1] * mul) >> q_shift) << fq;
                    const uint32_t m2 = ((r[2] * mul) >> q_shift) << fq;
                    const uint32_t m3 = ((r[3] * mul) >> q_shift) << fq;
                    const uint32_t nzmask =
                        (m0 != 0u ? 1u : 0u) |
                        (m1 != 0u ? 2u : 0u) |
                        (m2 != 0u ? 4u : 0u) |
                        (m3 != 0u ? 8u : 0u);
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

                    const uint8_t neg_mask = sign_tbl.mask[nzmask][s_nib & 15u];
                    const int32_t s0 = static_cast<int32_t>(m0);
                    const int32_t s1 = static_cast<int32_t>(m1);
                    const int32_t s2 = static_cast<int32_t>(m2);
                    const int32_t s3 = static_cast<int32_t>(m3);
                    d4[0] = (neg_mask & 1u) ? -s0 : s0;
                    d4[1] = (neg_mask & 2u) ? -s1 : s1;
                    d4[2] = (neg_mask & 4u) ? -s2 : s2;
                    d4[3] = (neg_mask & 8u) ? -s3 : s3;
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
        for (int ci = 0; ci < 3; ++ci) {
            const int* const v0_ids = geom.comp_v0_seg[ci];
            const int* const v1_ids = geom.comp_v1_seg[ci];

            std::memcpy(ww.idwt_a.data(), ww.seg_bufs[v0_ids[0]].data(),
                        static_cast<size_t>(geom.v0_subband_len[0]) * sizeof(int32_t));
            int len_prev = geom.v0_subband_len[0];
            int32_t* ping = ww.idwt_a.data();
            int32_t* pong = ww.idwt_b.data();
            for (int hl = 1; hl <= 5; ++hl) {
                const int len_next = len_prev + geom.v0_subband_len[hl];
                int32_t* dst = (hl == 5)
                                   ? (v_lh_planes[ci] + static_cast<size_t>(p_idx) * static_cast<size_t>(w_pix))
                                   : pong;
                idwt53_1d(dst, ping, ww.seg_bufs[v0_ids[hl]].data(), len_next);
                len_prev = len_next;
                std::swap(ping, pong);
            }

            idwt53_1d(v_lh_planes[ci] + static_cast<size_t>(n_prec + p_idx) * static_cast<size_t>(w_pix),
                      ww.seg_bufs[v1_ids[0]].data(), ww.seg_bufs[v1_ids[1]].data(), w_pix);
        }

        // comp == 2 (Yh): zero DWT, direct copy of seg 12 (row 2*p) and seg 23 (row 2*p + 1)
        std::memcpy(plane_yh + static_cast<size_t>(2 * p_idx) * static_cast<size_t>(w_pix),
                    ww.seg_bufs[12].data(), static_cast<size_t>(w_pix) * sizeof(int32_t));
        std::memcpy(plane_yh + static_cast<size_t>(2 * p_idx + 1) * static_cast<size_t>(w_pix),
                    ww.seg_bufs[23].data(), static_cast<size_t>(w_pix) * sizeof(int32_t));
    }
    return true;
}

#if NIJIN_X86_64
inline __m128i sse2_clamp_epi32(__m128i v, __m128i lo, __m128i hi) {
    const __m128i lt_lo = _mm_cmpgt_epi32(lo, v);
    const __m128i v_lo = _mm_or_si128(_mm_and_si128(lt_lo, lo), _mm_andnot_si128(lt_lo, v));
    const __m128i gt_hi = _mm_cmpgt_epi32(v_lo, hi);
    return _mm_or_si128(_mm_and_si128(gt_hi, hi), _mm_andnot_si128(gt_hi, v_lo));
}

NIJIN_TARGET_AVX2 inline void vert_idwt_even_k0_avx2(const int32_t* L, const int32_t* Hc, int32_t* dst, int w_pix) {
    int col = 0;
    const __m256i one_v = _mm256_set1_epi32(1);
    for (; col + 8 <= w_pix; col += 8) {
        const __m256i lv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(L + col));
        const __m256i hv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Hc + col));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + col),
                            _mm256_sub_epi32(lv, _mm256_srai_epi32(_mm256_add_epi32(hv, one_v), 1)));
    }
    for (; col < w_pix; ++col) {
        dst[col] = L[col] - asr_s32(Hc[col] + 1, 1);
    }
}

NIJIN_TARGET_AVX2 inline void vert_idwt_even_avx2(const int32_t* L, const int32_t* Hp, const int32_t* Hc, int32_t* dst, int w_pix) {
    int col = 0;
    const __m256i two_v = _mm256_set1_epi32(2);
    for (; col + 8 <= w_pix; col += 8) {
        const __m256i lv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(L + col));
        const __m256i hp = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Hp + col));
        const __m256i hc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Hc + col));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + col),
                            _mm256_sub_epi32(lv, _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(hp, hc), two_v), 2)));
    }
    for (; col < w_pix; ++col) {
        dst[col] = L[col] - asr_s32(Hp[col] + Hc[col] + 2, 2);
    }
}

NIJIN_TARGET_AVX2 inline void vert_idwt_odd_avx2(const int32_t* Hc, const int32_t* ev0, const int32_t* ev1, int32_t* dst, int w_pix) {
    int col = 0;
    for (; col + 8 <= w_pix; col += 8) {
        const __m256i hc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Hc + col));
        const __m256i e0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ev0 + col));
        const __m256i e1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ev1 + col));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + col),
                            _mm256_add_epi32(hc, _mm256_srai_epi32(_mm256_add_epi32(e0, e1), 1)));
    }
    for (; col < w_pix; ++col) {
        dst[col] = Hc[col] + asr_s32(ev0[col] + ev1[col], 1);
    }
}

NIJIN_TARGET_AVX2 inline void vert_idwt_odd_last_avx2(const int32_t* Hc, const int32_t* ev0, int32_t* dst, int w_pix) {
    int col = 0;
    for (; col + 8 <= w_pix; col += 8) {
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(dst + col),
            _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(Hc + col)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ev0 + col))));
    }
    for (; col < w_pix; ++col) {
        dst[col] = Hc[col] + ev0[col];
    }
}

NIJIN_NOINLINE NIJIN_TARGET_AVX2 void run_vidwt_task_avx2(
    const int32_t* lo_base,
    const int32_t* hi_base,
    int32_t* dst_base,
    int32_t* ev_next_buf,
    int k0,
    int k1,
    int n_prec,
    int w_pix) {
    int32_t* ev_cur = dst_base + static_cast<size_t>(2 * k0) * static_cast<size_t>(w_pix);
    if (k0 == 0) {
        vert_idwt_even_k0_avx2(lo_base, hi_base, ev_cur, w_pix);
    } else {
        const int32_t* const L0 = lo_base + static_cast<size_t>(k0) * static_cast<size_t>(w_pix);
        const int32_t* const Hp0 = hi_base + static_cast<size_t>(k0 - 1) * static_cast<size_t>(w_pix);
        const int32_t* const Hc0 = hi_base + static_cast<size_t>(k0) * static_cast<size_t>(w_pix);
        vert_idwt_even_avx2(L0, Hp0, Hc0, ev_cur, w_pix);
    }
    for (int k = k0; k < k1; ++k) {
        const int32_t* const Hc = hi_base + static_cast<size_t>(k) * static_cast<size_t>(w_pix);
        int32_t* const dst_odd = dst_base + static_cast<size_t>(2 * k + 1) * static_cast<size_t>(w_pix);
        if (k + 1 < n_prec) {
            int32_t* const ev_nxt = (k + 1 < k1)
                                        ? (dst_base + static_cast<size_t>(2 * (k + 1)) * static_cast<size_t>(w_pix))
                                        : ev_next_buf;
            const int32_t* const Ln = lo_base + static_cast<size_t>(k + 1) * static_cast<size_t>(w_pix);
            const int32_t* const Hn = hi_base + static_cast<size_t>(k + 1) * static_cast<size_t>(w_pix);
            vert_idwt_even_avx2(Ln, Hc, Hn, ev_nxt, w_pix);
            vert_idwt_odd_avx2(Hc, ev_cur, ev_nxt, dst_odd, w_pix);
            ev_cur = ev_nxt;
        } else {
            vert_idwt_odd_last_avx2(Hc, ev_cur, dst_odd, w_pix);
        }
    }
}
#endif

inline void vert_idwt_even_k0(const int32_t* L, const int32_t* Hc, int32_t* dst, int w_pix) {
    int col = 0;
#if NIJIN_ARM_NEON
    const int32x4_t one_v = vdupq_n_s32(1);
    for (; col + 4 <= w_pix; col += 4) {
        const int32x4_t lv = vld1q_s32(L + col);
        const int32x4_t hv = vld1q_s32(Hc + col);
        vst1q_s32(dst + col, vsubq_s32(lv, vshrq_n_s32(vaddq_s32(hv, one_v), 1)));
    }
#elif NIJIN_X86_64
    const __m128i one_v = _mm_set1_epi32(1);
    for (; col + 4 <= w_pix; col += 4) {
        const __m128i lv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(L + col));
        const __m128i hv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Hc + col));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + col),
                         _mm_sub_epi32(lv, _mm_srai_epi32(_mm_add_epi32(hv, one_v), 1)));
    }
#endif
    for (; col < w_pix; ++col) {
        dst[col] = L[col] - asr_s32(Hc[col] + 1, 1);
    }
}

inline void vert_idwt_even(const int32_t* L, const int32_t* Hp, const int32_t* Hc, int32_t* dst, int w_pix) {
    int col = 0;
#if NIJIN_ARM_NEON
    const int32x4_t two_v = vdupq_n_s32(2);
    for (; col + 4 <= w_pix; col += 4) {
        const int32x4_t lv = vld1q_s32(L + col);
        const int32x4_t hp = vld1q_s32(Hp + col);
        const int32x4_t hc = vld1q_s32(Hc + col);
        vst1q_s32(dst + col, vsubq_s32(lv, vshrq_n_s32(vaddq_s32(vaddq_s32(hp, hc), two_v), 2)));
    }
#elif NIJIN_X86_64
    const __m128i two_v = _mm_set1_epi32(2);
    for (; col + 4 <= w_pix; col += 4) {
        const __m128i lv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(L + col));
        const __m128i hp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Hp + col));
        const __m128i hc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Hc + col));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + col),
                         _mm_sub_epi32(lv, _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(hp, hc), two_v), 2)));
    }
#endif
    for (; col < w_pix; ++col) {
        dst[col] = L[col] - asr_s32(Hp[col] + Hc[col] + 2, 2);
    }
}

inline void vert_idwt_odd(const int32_t* Hc, const int32_t* ev0, const int32_t* ev1, int32_t* dst, int w_pix) {
    int col = 0;
#if NIJIN_ARM_NEON
    for (; col + 4 <= w_pix; col += 4) {
        const int32x4_t hc = vld1q_s32(Hc + col);
        const int32x4_t e0 = vld1q_s32(ev0 + col);
        const int32x4_t e1 = vld1q_s32(ev1 + col);
        vst1q_s32(dst + col, vaddq_s32(hc, vshrq_n_s32(vaddq_s32(e0, e1), 1)));
    }
#elif NIJIN_X86_64
    for (; col + 4 <= w_pix; col += 4) {
        const __m128i hc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Hc + col));
        const __m128i e0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ev0 + col));
        const __m128i e1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ev1 + col));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + col),
                         _mm_add_epi32(hc, _mm_srai_epi32(_mm_add_epi32(e0, e1), 1)));
    }
#endif
    for (; col < w_pix; ++col) {
        dst[col] = Hc[col] + asr_s32(ev0[col] + ev1[col], 1);
    }
}

inline void vert_idwt_odd_last(const int32_t* Hc, const int32_t* ev0, int32_t* dst, int w_pix) {
    int col = 0;
#if NIJIN_ARM_NEON
    for (; col + 4 <= w_pix; col += 4) {
        vst1q_s32(dst + col, vaddq_s32(vld1q_s32(Hc + col), vld1q_s32(ev0 + col)));
    }
#elif NIJIN_X86_64
    for (; col + 4 <= w_pix; col += 4) {
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(dst + col),
            _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(Hc + col)),
                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(ev0 + col))));
    }
#endif
    for (; col < w_pix; ++col) {
        dst[col] = Hc[col] + ev0[col];
    }
}

inline void compute_y0_row(
    const int32_t* yl,
    const int32_t* yh_up,
    const int32_t* yh,
    int32_t* y0_out,
    int w_pix) {
    int c = 0;
#if NIJIN_ARM_NEON
    for (; c + 4 <= w_pix - 1; c += 4) {
        const int32x4_t s = vaddq_s32(
            vaddq_s32(vld1q_s32(yh_up + c), vld1q_s32(yh_up + c + 1)),
            vaddq_s32(vld1q_s32(yh + c), vld1q_s32(yh + c + 1)));
        vst1q_s32(y0_out + c, vsubq_s32(vld1q_s32(yl + c), vshrq_n_s32(s, 3)));
    }
#elif NIJIN_X86_64
    for (; c + 4 <= w_pix - 1; c += 4) {
        const __m128i s = _mm_add_epi32(
            _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(yh_up + c)),
                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(yh_up + c + 1))),
            _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(yh + c)),
                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(yh + c + 1))));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(y0_out + c),
                         _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(yl + c)),
                                       _mm_srai_epi32(s, 3)));
    }
#endif
    for (; c < w_pix; ++c) {
        const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
        const int32_t yh_sum = yh_up[c] + yh_up[c_rt] + yh[c] + yh[c_rt];
        y0_out[c] = yl[c] - asr_s32(yh_sum, 3);
    }
}

inline void compute_g0_g1_row(
    const int32_t* y0,
    const int32_t* y0_dn,
    const int32_t* yh,
    const int32_t* cr,
    const int32_t* cr_dn,
    const int32_t* cb_up,
    const int32_t* cb,
    int32_t* g0_out,
    int32_t* g1_out,
    int w_pix) {
    if (w_pix <= 0) {
        return;
    }
    // c = 0 boundary
    {
        const int c_rt = (1 < w_pix) ? 1 : 0;
        g0_out[0] = y0[0] - asr_s32(cb[0] + cb_up[0] + cr[0] + cr[c_rt], 3);
        g1_out[0] = asr_s32(y0_dn[0] + y0[0] + y0_dn[0] + y0[0], 2) +
                    yh[0] - asr_s32(cb[0] + cb[0] + cr[0] + cr_dn[0], 3);
    }
    int c = 1;
#if NIJIN_ARM_NEON
    for (; c + 4 <= w_pix - 1; c += 4) {
        const int32x4_t cb_c = vld1q_s32(cb + c);
        const int32x4_t cb_u = vld1q_s32(cb_up + c);
        const int32x4_t cr_c = vld1q_s32(cr + c);
        const int32x4_t cr_r = vld1q_s32(cr + c + 1);
        const int32x4_t y0_c = vld1q_s32(y0 + c);

        const int32x4_t s_g0 = vaddq_s32(vaddq_s32(cb_c, cb_u), vaddq_s32(cr_c, cr_r));
        vst1q_s32(g0_out + c, vsubq_s32(y0_c, vshrq_n_s32(s_g0, 3)));

        const int32x4_t s_y = vaddq_s32(
            vaddq_s32(vld1q_s32(y0_dn + c), y0_c),
            vaddq_s32(vld1q_s32(y0_dn + c - 1), vld1q_s32(y0 + c - 1)));
        const int32x4_t s_c = vaddq_s32(
            vaddq_s32(vld1q_s32(cb + c - 1), cb_c),
            vaddq_s32(cr_c, vld1q_s32(cr_dn + c)));
        const int32x4_t g1_v = vsubq_s32(
            vaddq_s32(vshrq_n_s32(s_y, 2), vld1q_s32(yh + c)),
            vshrq_n_s32(s_c, 3));
        vst1q_s32(g1_out + c, g1_v);
    }
#elif NIJIN_X86_64
    for (; c + 4 <= w_pix - 1; c += 4) {
        const __m128i cb_c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cb + c));
        const __m128i cb_u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cb_up + c));
        const __m128i cr_c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr + c));
        const __m128i cr_r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr + c + 1));
        const __m128i y0_c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y0 + c));

        const __m128i s_g0 = _mm_add_epi32(_mm_add_epi32(cb_c, cb_u), _mm_add_epi32(cr_c, cr_r));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(g0_out + c), _mm_sub_epi32(y0_c, _mm_srai_epi32(s_g0, 3)));

        const __m128i s_y = _mm_add_epi32(
            _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(y0_dn + c)), y0_c),
            _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(y0_dn + c - 1)),
                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(y0 + c - 1))));
        const __m128i s_c = _mm_add_epi32(
            _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(cb + c - 1)), cb_c),
            _mm_add_epi32(cr_c, _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr_dn + c))));
        const __m128i g1_v = _mm_sub_epi32(
            _mm_add_epi32(_mm_srai_epi32(s_y, 2), _mm_loadu_si128(reinterpret_cast<const __m128i*>(yh + c))),
            _mm_srai_epi32(s_c, 3));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(g1_out + c), g1_v);
    }
#endif
    for (; c < w_pix; ++c) {
        const int c_lt = c - 1;
        const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
        g0_out[c] = y0[c] - asr_s32(cb[c] + cb_up[c] + cr[c] + cr[c_rt], 3);
        g1_out[c] = asr_s32(y0_dn[c] + y0[c] + y0_dn[c_lt] + y0[c_lt], 2) +
                    yh[c] - asr_s32(cb[c_lt] + cb[c] + cr[c] + cr_dn[c], 3);
    }
}

inline uint16_t map_sample_scalar(
    int32_t x,
    int post_offset,
    int scale,
    int scale_off,
    int32_t max_value,
    const uint16_t* baked_lut,
    int lut_max_idx) {
    if (baked_lut) {
        return baked_lut[clamp_i32(x + post_offset, 0, lut_max_idx)];
    }
    const int32_t out = (scale > 0) ? asr_s32(x + post_offset + scale_off, static_cast<unsigned>(scale))
                                    : (x + post_offset);
    return static_cast<uint16_t>(clamp_i32(out, 0, max_value));
}

inline void emit_bayer_row_pair(
    const int32_t* g1_up,
    const int32_t* g0_cur,
    const int32_t* g1_cur,
    const int32_t* g0_dn,
    const int32_t* cr_cur,
    const int32_t* cb_cur,
    uint16_t* out_even,
    uint16_t* out_odd,
    int w_pix,
    int post_offset,
    int scale,
    int scale_off,
    int32_t max_value,
    const uint16_t* baked_lut,
    int lut_max_idx) {
    if (w_pix <= 0) {
        return;
    }
    // c = 0 boundary
    {
        const int c_rt = (1 < w_pix) ? 1 : 0;
        const int32_t g0 = g0_cur[0];
        const int32_t g1 = g1_cur[0];
        const int32_t red = asr_s32(g1 + g0 + g0_cur[0] + g1_up[0], 2) + cr_cur[0];
        const int32_t blue = asr_s32(g1 + g0 + g1_cur[c_rt] + g0_dn[0], 2) + cb_cur[0];
        out_even[0] = map_sample_scalar(red, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_even[1] = map_sample_scalar(g0, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[0]  = map_sample_scalar(g1, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[1]  = map_sample_scalar(blue, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
    }

    int c = 1;
#if NIJIN_ARM_NEON
    const int32x4_t zero_v = vdupq_n_s32(0);
    const int32x4_t off_v = vdupq_n_s32(post_offset);
    if (baked_lut) {
        const int32x4_t max_idx_v = vdupq_n_s32(lut_max_idx);
        for (; c + 8 <= w_pix - 1; c += 8) {
            uint16x8_t r_u16 = vdupq_n_u16(0);
            uint16x8_t g0_u16 = vdupq_n_u16(0);
            uint16x8_t g1_u16 = vdupq_n_u16(0);
            uint16x8_t b_u16 = vdupq_n_u16(0);

            {
                const int32x4_t g0 = vld1q_s32(g0_cur + c);
                const int32x4_t g1 = vld1q_s32(g1_cur + c);
                const int32x4_t gsum = vaddq_s32(g1, g0);
                const int32x4_t red = vaddq_s32(
                    vshrq_n_s32(vaddq_s32(gsum, vaddq_s32(vld1q_s32(g0_cur + c - 1), vld1q_s32(g1_up + c))), 2),
                    vld1q_s32(cr_cur + c));
                const int32x4_t blue = vaddq_s32(
                    vshrq_n_s32(vaddq_s32(gsum, vaddq_s32(vld1q_s32(g1_cur + c + 1), vld1q_s32(g0_dn + c))), 2),
                    vld1q_s32(cb_cur + c));

                const int32x4_t ir = vminq_s32(vmaxq_s32(vaddq_s32(red, off_v), zero_v), max_idx_v);
                const int32x4_t ig0 = vminq_s32(vmaxq_s32(vaddq_s32(g0, off_v), zero_v), max_idx_v);
                const int32x4_t ig1 = vminq_s32(vmaxq_s32(vaddq_s32(g1, off_v), zero_v), max_idx_v);
                const int32x4_t ib = vminq_s32(vmaxq_s32(vaddq_s32(blue, off_v), zero_v), max_idx_v);

#define NRAW_LUT_LO(LANE)                                                  \
                r_u16  = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ir, LANE)], r_u16, LANE); \
                g0_u16 = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ig0, LANE)], g0_u16, LANE); \
                g1_u16 = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ig1, LANE)], g1_u16, LANE); \
                b_u16  = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ib, LANE)], b_u16, LANE);
                NRAW_LUT_LO(0)
                NRAW_LUT_LO(1)
                NRAW_LUT_LO(2)
                NRAW_LUT_LO(3)
#undef NRAW_LUT_LO
            }
            {
                const int32x4_t g0 = vld1q_s32(g0_cur + c + 4);
                const int32x4_t g1 = vld1q_s32(g1_cur + c + 4);
                const int32x4_t gsum = vaddq_s32(g1, g0);
                const int32x4_t red = vaddq_s32(
                    vshrq_n_s32(vaddq_s32(gsum, vaddq_s32(vld1q_s32(g0_cur + c + 3), vld1q_s32(g1_up + c + 4))), 2),
                    vld1q_s32(cr_cur + c + 4));
                const int32x4_t blue = vaddq_s32(
                    vshrq_n_s32(vaddq_s32(gsum, vaddq_s32(vld1q_s32(g1_cur + c + 5), vld1q_s32(g0_dn + c + 4))), 2),
                    vld1q_s32(cb_cur + c + 4));

                const int32x4_t ir = vminq_s32(vmaxq_s32(vaddq_s32(red, off_v), zero_v), max_idx_v);
                const int32x4_t ig0 = vminq_s32(vmaxq_s32(vaddq_s32(g0, off_v), zero_v), max_idx_v);
                const int32x4_t ig1 = vminq_s32(vmaxq_s32(vaddq_s32(g1, off_v), zero_v), max_idx_v);
                const int32x4_t ib = vminq_s32(vmaxq_s32(vaddq_s32(blue, off_v), zero_v), max_idx_v);

#define NRAW_LUT_HI(LANE)                                                  \
                r_u16  = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ir, LANE)], r_u16, (LANE) + 4); \
                g0_u16 = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ig0, LANE)], g0_u16, (LANE) + 4); \
                g1_u16 = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ig1, LANE)], g1_u16, (LANE) + 4); \
                b_u16  = vsetq_lane_u16(baked_lut[vgetq_lane_s32(ib, LANE)], b_u16, (LANE) + 4);
                NRAW_LUT_HI(0)
                NRAW_LUT_HI(1)
                NRAW_LUT_HI(2)
                NRAW_LUT_HI(3)
#undef NRAW_LUT_HI
            }

            uint16x8x2_t pair_ev;
            pair_ev.val[0] = r_u16;
            pair_ev.val[1] = g0_u16;
            vst2q_u16(out_even + 2 * c, pair_ev);

            uint16x8x2_t pair_od;
            pair_od.val[0] = g1_u16;
            pair_od.val[1] = b_u16;
            vst2q_u16(out_odd + 2 * c, pair_od);
        }
    } else {
        const int32x4_t max_val_v = vdupq_n_s32(max_value);
        const int32x4_t total_off_v = vdupq_n_s32(post_offset + scale_off);
        const int32x4_t neg_shift_v = vdupq_n_s32(-scale);
        for (; c + 4 <= w_pix - 1; c += 4) {
            const int32x4_t g0 = vld1q_s32(g0_cur + c);
            const int32x4_t g1 = vld1q_s32(g1_cur + c);
            const int32x4_t gsum = vaddq_s32(g1, g0);
            const int32x4_t red = vaddq_s32(
                vshrq_n_s32(vaddq_s32(gsum, vaddq_s32(vld1q_s32(g0_cur + c - 1), vld1q_s32(g1_up + c))), 2),
                vld1q_s32(cr_cur + c));
            const int32x4_t blue = vaddq_s32(
                vshrq_n_s32(vaddq_s32(gsum, vaddq_s32(vld1q_s32(g1_cur + c + 1), vld1q_s32(g0_dn + c))), 2),
                vld1q_s32(cb_cur + c));

            int32x4_t or_v = vaddq_s32(red, total_off_v);
            int32x4_t og0_v = vaddq_s32(g0, total_off_v);
            int32x4_t og1_v = vaddq_s32(g1, total_off_v);
            int32x4_t ob_v = vaddq_s32(blue, total_off_v);
            if (scale > 0) {
                or_v = vshlq_s32(or_v, neg_shift_v);
                og0_v = vshlq_s32(og0_v, neg_shift_v);
                og1_v = vshlq_s32(og1_v, neg_shift_v);
                ob_v = vshlq_s32(ob_v, neg_shift_v);
            }
            or_v = vminq_s32(vmaxq_s32(or_v, zero_v), max_val_v);
            og0_v = vminq_s32(vmaxq_s32(og0_v, zero_v), max_val_v);
            og1_v = vminq_s32(vmaxq_s32(og1_v, zero_v), max_val_v);
            ob_v = vminq_s32(vmaxq_s32(ob_v, zero_v), max_val_v);

            uint16x4x2_t pair_ev;
            pair_ev.val[0] = vqmovun_s32(or_v);
            pair_ev.val[1] = vqmovun_s32(og0_v);
            vst2_u16(out_even + 2 * c, pair_ev);

            uint16x4x2_t pair_od;
            pair_od.val[0] = vqmovun_s32(og1_v);
            pair_od.val[1] = vqmovun_s32(ob_v);
            vst2_u16(out_odd + 2 * c, pair_od);
        }
    }
#elif NIJIN_X86_64
    const __m128i zero_v = _mm_setzero_si128();
    const __m128i off_v = _mm_set1_epi32(post_offset);
    if (baked_lut) {
        const __m128i max_idx_v = _mm_set1_epi32(lut_max_idx);
        for (; c + 4 <= w_pix - 1; c += 4) {
            const __m128i g0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g0_cur + c));
            const __m128i g1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g1_cur + c));
            const __m128i gsum = _mm_add_epi32(g1, g0);
            const __m128i red = _mm_add_epi32(
                _mm_srai_epi32(_mm_add_epi32(gsum,
                    _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g0_cur + c - 1)),
                                  _mm_loadu_si128(reinterpret_cast<const __m128i*>(g1_up + c)))), 2),
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr_cur + c)));
            const __m128i blue = _mm_add_epi32(
                _mm_srai_epi32(_mm_add_epi32(gsum,
                    _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g1_cur + c + 1)),
                                  _mm_loadu_si128(reinterpret_cast<const __m128i*>(g0_dn + c)))), 2),
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(cb_cur + c)));

            const __m128i ir = sse2_clamp_epi32(_mm_add_epi32(red, off_v), zero_v, max_idx_v);
            const __m128i ig0 = sse2_clamp_epi32(_mm_add_epi32(g0, off_v), zero_v, max_idx_v);
            const __m128i ig1 = sse2_clamp_epi32(_mm_add_epi32(g1, off_v), zero_v, max_idx_v);
            const __m128i ib = sse2_clamp_epi32(_mm_add_epi32(blue, off_v), zero_v, max_idx_v);

            int32_t xr[4], xg0[4], xg1[4], xb[4];
            _mm_storeu_si128(reinterpret_cast<__m128i*>(xr), ir);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(xg0), ig0);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(xg1), ig1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(xb), ib);

            for (int lane = 0; lane < 4; ++lane) {
                out_even[2 * (c + lane)]     = baked_lut[xr[lane]];
                out_even[2 * (c + lane) + 1] = baked_lut[xg0[lane]];
                out_odd[2 * (c + lane)]      = baked_lut[xg1[lane]];
                out_odd[2 * (c + lane) + 1]  = baked_lut[xb[lane]];
            }
        }
    } else {
        const __m128i max_val_v = _mm_set1_epi32(max_value);
        const __m128i total_off_v = _mm_set1_epi32(post_offset + scale_off);
        const __m128i shift_cnt_v = _mm_cvtsi32_si128(scale > 0 ? scale : 0);
        for (; c + 4 <= w_pix - 1; c += 4) {
            const __m128i g0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g0_cur + c));
            const __m128i g1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g1_cur + c));
            const __m128i gsum = _mm_add_epi32(g1, g0);
            const __m128i red = _mm_add_epi32(
                _mm_srai_epi32(_mm_add_epi32(gsum,
                    _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g0_cur + c - 1)),
                                  _mm_loadu_si128(reinterpret_cast<const __m128i*>(g1_up + c)))), 2),
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr_cur + c)));
            const __m128i blue = _mm_add_epi32(
                _mm_srai_epi32(_mm_add_epi32(gsum,
                    _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g1_cur + c + 1)),
                                  _mm_loadu_si128(reinterpret_cast<const __m128i*>(g0_dn + c)))), 2),
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(cb_cur + c)));

            __m128i or_v = _mm_add_epi32(red, total_off_v);
            __m128i og0_v = _mm_add_epi32(g0, total_off_v);
            __m128i og1_v = _mm_add_epi32(g1, total_off_v);
            __m128i ob_v = _mm_add_epi32(blue, total_off_v);
            if (scale > 0) {
                or_v = _mm_sra_epi32(or_v, shift_cnt_v);
                og0_v = _mm_sra_epi32(og0_v, shift_cnt_v);
                og1_v = _mm_sra_epi32(og1_v, shift_cnt_v);
                ob_v = _mm_sra_epi32(ob_v, shift_cnt_v);
            }
            or_v = sse2_clamp_epi32(or_v, zero_v, max_val_v);
            og0_v = sse2_clamp_epi32(og0_v, zero_v, max_val_v);
            og1_v = sse2_clamp_epi32(og1_v, zero_v, max_val_v);
            ob_v = sse2_clamp_epi32(ob_v, zero_v, max_val_v);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out_even + 2 * c),
                             _mm_or_si128(or_v, _mm_slli_epi32(og0_v, 16)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out_odd + 2 * c),
                             _mm_or_si128(og1_v, _mm_slli_epi32(ob_v, 16)));
        }
    }
#endif
    for (; c < w_pix; ++c) {
        const int c_lt = c - 1;
        const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
        const int32_t g0 = g0_cur[c];
        const int32_t g1 = g1_cur[c];
        const int32_t red = asr_s32(g1 + g0 + g0_cur[c_lt] + g1_up[c], 2) + cr_cur[c];
        const int32_t blue = asr_s32(g1 + g0 + g1_cur[c_rt] + g0_dn[c], 2) + cb_cur[c];
        out_even[2 * c]     = map_sample_scalar(red, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_even[2 * c + 1] = map_sample_scalar(g0, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[2 * c]      = map_sample_scalar(g1, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[2 * c + 1]  = map_sample_scalar(blue, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
    }
}

#if NIJIN_X86_64
NIJIN_TARGET_AVX2 inline void compute_y0_row_avx2(
    const int32_t* yl,
    const int32_t* yh_up,
    const int32_t* yh,
    int32_t* y0_out,
    int w_pix) {
    int c = 0;
    for (; c + 8 <= w_pix - 1; c += 8) {
        const __m256i s = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(yh_up + c)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(yh_up + c + 1))),
            _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(yh + c)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(yh + c + 1))));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(y0_out + c),
                            _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(yl + c)),
                                             _mm256_srai_epi32(s, 3)));
    }
    for (; c < w_pix; ++c) {
        const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
        const int32_t yh_sum = yh_up[c] + yh_up[c_rt] + yh[c] + yh[c_rt];
        y0_out[c] = yl[c] - asr_s32(yh_sum, 3);
    }
}

NIJIN_TARGET_AVX2 inline void compute_g0_g1_row_avx2(
    const int32_t* y0,
    const int32_t* y0_dn,
    const int32_t* yh,
    const int32_t* cr,
    const int32_t* cr_dn,
    const int32_t* cb_up,
    const int32_t* cb,
    int32_t* g0_out,
    int32_t* g1_out,
    int w_pix) {
    if (w_pix <= 0) {
        return;
    }
    {
        const int c_rt = (1 < w_pix) ? 1 : 0;
        g0_out[0] = y0[0] - asr_s32(cb[0] + cb_up[0] + cr[0] + cr[c_rt], 3);
        g1_out[0] = asr_s32(y0_dn[0] + y0[0] + y0_dn[0] + y0[0], 2) +
                    yh[0] - asr_s32(cb[0] + cb[0] + cr[0] + cr_dn[0], 3);
    }
    int c = 1;
    for (; c + 8 <= w_pix - 1; c += 8) {
        const __m256i cb_c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cb + c));
        const __m256i cb_u = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cb_up + c));
        const __m256i cr_c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cr + c));
        const __m256i cr_r = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cr + c + 1));
        const __m256i y0_c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y0 + c));

        const __m256i s_g0 = _mm256_add_epi32(_mm256_add_epi32(cb_c, cb_u), _mm256_add_epi32(cr_c, cr_r));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(g0_out + c),
                            _mm256_sub_epi32(y0_c, _mm256_srai_epi32(s_g0, 3)));

        const __m256i s_y = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(y0_dn + c)), y0_c),
            _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(y0_dn + c - 1)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y0 + c - 1))));
        const __m256i s_c = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(cb + c - 1)), cb_c),
            _mm256_add_epi32(cr_c, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cr_dn + c))));
        const __m256i g1_v = _mm256_sub_epi32(
            _mm256_add_epi32(_mm256_srai_epi32(s_y, 2),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(yh + c))),
            _mm256_srai_epi32(s_c, 3));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(g1_out + c), g1_v);
    }
    for (; c < w_pix; ++c) {
        const int c_lt = c - 1;
        const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
        g0_out[c] = y0[c] - asr_s32(cb[c] + cb_up[c] + cr[c] + cr[c_rt], 3);
        g1_out[c] = asr_s32(y0_dn[c] + y0[c] + y0_dn[c_lt] + y0[c_lt], 2) +
                    yh[c] - asr_s32(cb[c_lt] + cb[c] + cr[c] + cr_dn[c], 3);
    }
}

NIJIN_TARGET_AVX2 inline void emit_bayer_row_pair_avx2(
    const int32_t* g1_up,
    const int32_t* g0_cur,
    const int32_t* g1_cur,
    const int32_t* g0_dn,
    const int32_t* cr_cur,
    const int32_t* cb_cur,
    uint16_t* out_even,
    uint16_t* out_odd,
    int w_pix,
    int post_offset,
    int scale,
    int scale_off,
    int32_t max_value,
    const uint16_t* baked_lut,
    int lut_max_idx) {
    if (w_pix <= 0) {
        return;
    }
    {
        const int c_rt = (1 < w_pix) ? 1 : 0;
        const int32_t g0 = g0_cur[0];
        const int32_t g1 = g1_cur[0];
        const int32_t red = asr_s32(g1 + g0 + g0_cur[0] + g1_up[0], 2) + cr_cur[0];
        const int32_t blue = asr_s32(g1 + g0 + g1_cur[c_rt] + g0_dn[0], 2) + cb_cur[0];
        out_even[0] = map_sample_scalar(red, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_even[1] = map_sample_scalar(g0, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[0]  = map_sample_scalar(g1, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[1]  = map_sample_scalar(blue, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
    }
    int c = 1;
    const __m256i zero_v = _mm256_setzero_si256();
    const __m256i off_v = _mm256_set1_epi32(post_offset);
    if (baked_lut) {
        const __m256i max_idx_v = _mm256_set1_epi32(lut_max_idx);
        for (; c + 8 <= w_pix - 1; c += 8) {
            const __m256i g0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g0_cur + c));
            const __m256i g1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g1_cur + c));
            const __m256i gsum = _mm256_add_epi32(g1, g0);
            const __m256i red = _mm256_add_epi32(
                _mm256_srai_epi32(_mm256_add_epi32(gsum,
                    _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(g0_cur + c - 1)),
                                     _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g1_up + c)))), 2),
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cr_cur + c)));
            const __m256i blue = _mm256_add_epi32(
                _mm256_srai_epi32(_mm256_add_epi32(gsum,
                    _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(g1_cur + c + 1)),
                                     _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g0_dn + c)))), 2),
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cb_cur + c)));

            const __m256i ir = _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(red, off_v), zero_v), max_idx_v);
            const __m256i ig0 = _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(g0, off_v), zero_v), max_idx_v);
            const __m256i ig1 = _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(g1, off_v), zero_v), max_idx_v);
            const __m256i ib = _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(blue, off_v), zero_v), max_idx_v);

            int32_t xr[8], xg0[8], xg1[8], xb[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(xr), ir);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(xg0), ig0);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(xg1), ig1);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(xb), ib);

            uint16_t lr[8], lg0[8], lg1[8], lb[8];
            for (int lane = 0; lane < 8; ++lane) {
                lr[lane]  = baked_lut[xr[lane]];
                lg0[lane] = baked_lut[xg0[lane]];
                lg1[lane] = baked_lut[xg1[lane]];
                lb[lane]  = baked_lut[xb[lane]];
            }
            const __m128i vr = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lr));
            const __m128i vg0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lg0));
            const __m128i vg1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lg1));
            const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lb));

            _mm_storeu_si128(reinterpret_cast<__m128i*>(out_even + 2 * c), _mm_unpacklo_epi16(vr, vg0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out_even + 2 * c + 8), _mm_unpackhi_epi16(vr, vg0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out_odd + 2 * c), _mm_unpacklo_epi16(vg1, vb));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out_odd + 2 * c + 8), _mm_unpackhi_epi16(vg1, vb));
        }
    } else {
        const __m256i max_val_v = _mm256_set1_epi32(max_value);
        const __m256i total_off_v = _mm256_set1_epi32(post_offset + scale_off);
        const __m128i shift_cnt_v = _mm_cvtsi32_si128(scale > 0 ? scale : 0);
        for (; c + 8 <= w_pix - 1; c += 8) {
            const __m256i g0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g0_cur + c));
            const __m256i g1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g1_cur + c));
            const __m256i gsum = _mm256_add_epi32(g1, g0);
            const __m256i red = _mm256_add_epi32(
                _mm256_srai_epi32(_mm256_add_epi32(gsum,
                    _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(g0_cur + c - 1)),
                                     _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g1_up + c)))), 2),
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cr_cur + c)));
            const __m256i blue = _mm256_add_epi32(
                _mm256_srai_epi32(_mm256_add_epi32(gsum,
                    _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(g1_cur + c + 1)),
                                     _mm256_loadu_si256(reinterpret_cast<const __m256i*>(g0_dn + c)))), 2),
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cb_cur + c)));

            __m256i or_v = _mm256_add_epi32(red, total_off_v);
            __m256i og0_v = _mm256_add_epi32(g0, total_off_v);
            __m256i og1_v = _mm256_add_epi32(g1, total_off_v);
            __m256i ob_v = _mm256_add_epi32(blue, total_off_v);
            if (scale > 0) {
                or_v = _mm256_sra_epi32(or_v, shift_cnt_v);
                og0_v = _mm256_sra_epi32(og0_v, shift_cnt_v);
                og1_v = _mm256_sra_epi32(og1_v, shift_cnt_v);
                ob_v = _mm256_sra_epi32(ob_v, shift_cnt_v);
            }
            or_v = _mm256_min_epi32(_mm256_max_epi32(or_v, zero_v), max_val_v);
            og0_v = _mm256_min_epi32(_mm256_max_epi32(og0_v, zero_v), max_val_v);
            og1_v = _mm256_min_epi32(_mm256_max_epi32(og1_v, zero_v), max_val_v);
            ob_v = _mm256_min_epi32(_mm256_max_epi32(ob_v, zero_v), max_val_v);

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_even + 2 * c),
                                _mm256_or_si256(or_v, _mm256_slli_epi32(og0_v, 16)));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_odd + 2 * c),
                                _mm256_or_si256(og1_v, _mm256_slli_epi32(ob_v, 16)));
        }
    }
    for (; c < w_pix; ++c) {
        const int c_lt = c - 1;
        const int c_rt = (c + 1 < w_pix) ? (c + 1) : (w_pix - 1);
        const int32_t g0 = g0_cur[c];
        const int32_t g1 = g1_cur[c];
        const int32_t red = asr_s32(g1 + g0 + g0_cur[c_lt] + g1_up[c], 2) + cr_cur[c];
        const int32_t blue = asr_s32(g1 + g0 + g1_cur[c_rt] + g0_dn[c], 2) + cb_cur[c];
        out_even[2 * c]     = map_sample_scalar(red, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_even[2 * c + 1] = map_sample_scalar(g0, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[2 * c]      = map_sample_scalar(g1, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
        out_odd[2 * c + 1]  = map_sample_scalar(blue, post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
    }
}

NIJIN_NOINLINE NIJIN_TARGET_AVX2 void run_rct_nlt_strip_avx2(
    int r0,
    int r1,
    SliceWorkBuf& ww,
    const int32_t* plane_yl,
    const int32_t* plane_yh,
    const int32_t* plane_cr,
    const int32_t* plane_cb,
    uint16_t* dst,
    size_t dst_stride_u16,
    int w_pix,
    int h_pix,
    int post_offset,
    int scale,
    int scale_off,
    int32_t max_value,
    const uint16_t* baked_lut,
    int lut_max_idx) {
    auto compute_y0_at = [&](int r, int32_t* out_y0) {
        const int r_up = (r > 0) ? (r - 1) : 0;
        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
        const size_t row_up = static_cast<size_t>(r_up) * static_cast<size_t>(w_pix);
        compute_y0_row_avx2(plane_yl + row, plane_yh + row_up, plane_yh + row, out_y0, w_pix);
    };

    auto compute_g01_at = [&](int r, const int32_t* y0_r, const int32_t* y0_dn_r, int32_t* g0_r, int32_t* g1_r) {
        const int r_up = (r > 0) ? (r - 1) : 0;
        const int r_dn = (r + 1 < h_pix) ? (r + 1) : (h_pix - 1);
        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
        const size_t row_up = static_cast<size_t>(r_up) * static_cast<size_t>(w_pix);
        const size_t row_dn = static_cast<size_t>(r_dn) * static_cast<size_t>(w_pix);
        compute_g0_g1_row_avx2(
            y0_r, y0_dn_r, plane_yh + row,
            plane_cr + row, plane_cr + row_dn,
            plane_cb + row_up, plane_cb + row,
            g0_r, g1_r, w_pix);
    };

    int32_t* y0_cur = ww.y0_a.data();
    int32_t* y0_nxt = ww.y0_b.data();
    int32_t* g0_up = ww.g0_slots[0].data();
    int32_t* g1_up = ww.g1_slots[0].data();
    int32_t* g0_cur = ww.g0_slots[1].data();
    int32_t* g1_cur = ww.g1_slots[1].data();
    int32_t* g0_dn = ww.g0_slots[2].data();
    int32_t* g1_dn = ww.g1_slots[2].data();
    const int32_t* g1_up_ptr = nullptr;

    if (r0 == 0) {
        compute_y0_at(0, y0_cur);
        if (1 < h_pix) {
            compute_y0_at(1, y0_nxt);
            compute_g01_at(0, y0_cur, y0_nxt, g0_cur, g1_cur);
            std::swap(y0_cur, y0_nxt);
        } else {
            compute_g01_at(0, y0_cur, y0_cur, g0_cur, g1_cur);
        }
        g1_up_ptr = g1_cur;
    } else {
        compute_y0_at(r0 - 1, y0_cur);
        compute_y0_at(r0, y0_nxt);
        compute_g01_at(r0 - 1, y0_cur, y0_nxt, g0_up, g1_up);
        std::swap(y0_cur, y0_nxt);
        if (r0 + 1 < h_pix) {
            compute_y0_at(r0 + 1, y0_nxt);
            compute_g01_at(r0, y0_cur, y0_nxt, g0_cur, g1_cur);
            std::swap(y0_cur, y0_nxt);
        } else {
            compute_g01_at(r0, y0_cur, y0_cur, g0_cur, g1_cur);
        }
        g1_up_ptr = g1_up;
    }

    for (int r = r0; r < r1; ++r) {
        const int32_t* g0_dn_ptr = nullptr;
        if (r + 1 < h_pix) {
            if (r + 2 < h_pix) {
                compute_y0_at(r + 2, y0_nxt);
                compute_g01_at(r + 1, y0_cur, y0_nxt, g0_dn, g1_dn);
            } else {
                compute_g01_at(r + 1, y0_cur, y0_cur, g0_dn, g1_dn);
            }
            g0_dn_ptr = g0_dn;
        } else {
            g0_dn_ptr = g0_cur;
        }

        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
        uint16_t* const out_even = dst + static_cast<size_t>(2 * r) * dst_stride_u16;
        uint16_t* const out_odd = out_even + dst_stride_u16;
        emit_bayer_row_pair_avx2(
            g1_up_ptr, g0_cur, g1_cur, g0_dn_ptr,
            plane_cr + row, plane_cb + row,
            out_even, out_odd, w_pix,
            post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);

        if (r + 1 < r1) {
            int32_t* const tmp0 = g0_up;
            int32_t* const tmp1 = g1_up;
            g0_up = g0_cur;
            g1_up = g1_cur;
            g0_cur = g0_dn;
            g1_cur = g1_dn;
            g0_dn = tmp0;
            g1_dn = tmp1;
            g1_up_ptr = g1_up;
            if (r + 2 < h_pix) {
                std::swap(y0_cur, y0_nxt);
            }
        }
    }
}
#endif

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

namespace detail {

TicoArena* create_tico_arena() {
    return reinterpret_cast<TicoArena*>(new TicoArenaImpl());
}

void destroy_tico_arena(TicoArena* arena) {
    delete reinterpret_cast<TicoArenaImpl*>(arena);
}

bool decode_ticoraw_bayer_strided(
    const uint8_t* data,
    size_t len,
    int num_threads,
    uint16_t* dst,
    size_t dst_stride_u16,
    size_t dst_height_capacity,
    TicoRawInfo& info_out,
    std::string& err,
    WorkerPool* external_pool,
    TicoArena* external_arena,
    WaveletQuantProfile* quant_out) {
    ParsedR0Header hdr;
    if (!parse_r0_header(data, len, hdr, err)) {
        return false;
    }
    CodecGeometry geom;
    geom.init(hdr);

    if (!dst || dst_stride_u16 < static_cast<size_t>(geom.full_w) ||
        dst_height_capacity < static_cast<size_t>(geom.full_h)) {
        err = "Destination Bayer buffer too small for TicoRAW frame";
        return false;
    }

    TicoArenaLease arena_lease(reinterpret_cast<TicoArenaImpl*>(external_arena));
    TicoArenaImpl& arena = *arena_lease.ptr;

    // Pre-scan all slices & precincts sequentially (validates full stream before worker dispatch)
    std::vector<PrecinctSpan>& prec_spans = arena.prec_spans;
    if (prec_spans.size() < static_cast<size_t>(geom.n_prec)) {
        prec_spans.resize(static_cast<size_t>(geom.n_prec));
    }
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

    const int threads = std::max(1, detail::resolve_worker_count(num_threads));
    arena.ensure_workers(threads);
    detail::PoolLease pool(threads, external_pool);

    const int w_pix = geom.w_pix;
    const int h_pix = geom.h_pix;
    const int n_prec = geom.n_prec;
    const size_t plane_elems = static_cast<size_t>(w_pix) * static_cast<size_t>(h_pix);
    if (arena.planes.size() < 7u * plane_elems) {
        arena.planes.resize(7u * plane_elems);
    }

    int32_t* const plane_base = arena.planes.data();
    int32_t* v_lh_planes[3] = {plane_base, plane_base + plane_elems, plane_base + 2u * plane_elems};
    int32_t* const plane_yh = plane_base + 3u * plane_elems;
    int32_t* const plane_yl = plane_base + 4u * plane_elems;
    int32_t* const plane_cr = plane_base + 5u * plane_elems;
    int32_t* const plane_cb = plane_base + 6u * plane_elems;
    int32_t* out_spatial[3] = {plane_yl, plane_cr, plane_cb};

    // Stage 1: Dynamic slice-stealing entropy decode + 1D horizontal IDWT
    std::atomic<int> err_flag{0};
    pool.parallel_chunks(static_cast<size_t>(geom.n_slices), 1u, threads,
                         [&](size_t s0, size_t s1, int worker_idx) {
        SliceWorkBuf& ww = arena.workers[static_cast<size_t>(worker_idx)];
        ww.ensure_stage1(geom);
        for (size_t s = s0; s < s1; ++s) {
            if (err_flag.load(std::memory_order_relaxed) != 0) {
                return;
            }
            if (!decode_single_slice_entropy_and_hdwt(
                    data, hdr, geom, prec_spans, static_cast<int>(s), ww, v_lh_planes, plane_yh)) {
                err_flag.store(1, std::memory_order_relaxed);
                return;
            }
        }
    });
    if (err_flag.load(std::memory_order_relaxed) != 0) {
        err = "Corrupt TicoRAW packet entropy data";
        return false;
    }

#if NIJIN_X86_64
    const bool use_avx2 = detail::cpu_has_avx2();
#endif

    // Stage 1.5: Single-pass fused vertical 1-level 5/3 IDWT over all (component, strip) tasks
    struct VIdwtTask {
        int ci = 0;
        int k0 = 0;
        int k1 = 0;
    };
    const int n_strips = (threads <= 1)
                             ? 1
                             : std::min<int>(std::max<int>(1, n_prec / 32), std::max<int>(1, (threads * 4) / 3));
    std::vector<VIdwtTask> vidwt_tasks;
    vidwt_tasks.reserve(static_cast<size_t>(3 * n_strips));
    for (int ci = 0; ci < 3; ++ci) {
        for (int s = 0; s < n_strips; ++s) {
            VIdwtTask vt;
            vt.ci = ci;
            vt.k0 = (s * n_prec) / n_strips;
            vt.k1 = ((s + 1) * n_prec) / n_strips;
            if (vt.k0 < vt.k1) {
                vidwt_tasks.push_back(vt);
            }
        }
    }

    pool.parallel_chunks(vidwt_tasks.size(), 1u, threads, [&](size_t t0, size_t t1, int worker_idx) {
        SliceWorkBuf& ww = arena.workers[static_cast<size_t>(worker_idx)];
        ww.ensure_v_even_cap(w_pix);
        int32_t* const ev_next_buf = ww.v_ev_next.data();
        for (size_t ti = t0; ti < t1; ++ti) {
            const int ci = vidwt_tasks[ti].ci;
            const int k0 = vidwt_tasks[ti].k0;
            const int k1 = vidwt_tasks[ti].k1;
            const int32_t* const lo_base = v_lh_planes[ci];
            const int32_t* const hi_base = v_lh_planes[ci] + static_cast<size_t>(n_prec) * static_cast<size_t>(w_pix);
            int32_t* const dst_base = out_spatial[ci];

#if NIJIN_X86_64
            if (use_avx2) {
                run_vidwt_task_avx2(lo_base, hi_base, dst_base, ev_next_buf, k0, k1, n_prec, w_pix);
                continue;
            }
#endif

            int32_t* ev_cur = dst_base + static_cast<size_t>(2 * k0) * static_cast<size_t>(w_pix);
            if (k0 == 0) {
                vert_idwt_even_k0(lo_base, hi_base, ev_cur, w_pix);
            } else {
                const int32_t* const L0 = lo_base + static_cast<size_t>(k0) * static_cast<size_t>(w_pix);
                const int32_t* const Hp0 = hi_base + static_cast<size_t>(k0 - 1) * static_cast<size_t>(w_pix);
                const int32_t* const Hc0 = hi_base + static_cast<size_t>(k0) * static_cast<size_t>(w_pix);
                vert_idwt_even(L0, Hp0, Hc0, ev_cur, w_pix);
            }

            for (int k = k0; k < k1; ++k) {
                const int32_t* const Hc = hi_base + static_cast<size_t>(k) * static_cast<size_t>(w_pix);
                int32_t* const dst_odd = dst_base + static_cast<size_t>(2 * k + 1) * static_cast<size_t>(w_pix);
                if (k + 1 < n_prec) {
                    int32_t* const ev_nxt = (k + 1 < k1)
                                                ? (dst_base + static_cast<size_t>(2 * (k + 1)) * static_cast<size_t>(w_pix))
                                                : ev_next_buf;
                    const int32_t* const Ln = lo_base + static_cast<size_t>(k + 1) * static_cast<size_t>(w_pix);
                    const int32_t* const Hn = hi_base + static_cast<size_t>(k + 1) * static_cast<size_t>(w_pix);
                    vert_idwt_even(Ln, Hc, Hn, ev_nxt, w_pix);
                    vert_idwt_odd(Hc, ev_cur, ev_nxt, dst_odd, w_pix);
                    ev_cur = ev_nxt;
                } else {
                    vert_idwt_odd_last(Hc, ev_cur, dst_odd, w_pix);
                }
            }
        }
    });

    // Stage 2 + Stage 3 Fused: L1/L2-ring Inverse Star-Tetrix RCT + cached baked NLT LUT -> uint16 RGGB Bayer
    const int full_h = geom.full_h;
    const int active_quad_h = full_h / 2;

    const int input_scale = hdr.bayer_tweaks ? (static_cast<int>(hdr.bw) - 2) : hdr.out_bitdepth;
    const int scale = input_scale - hdr.in_bitdepth;
    const int scale_off = (scale > 0) ? (1 << (scale - 1)) : 0;
    const int post_offset = 1 << (scale + hdr.in_bitdepth - 1);
    const int32_t max_value = (1 << hdr.in_bitdepth) - 1;

    arena.nlt_cache.prepare(hdr);
    const uint16_t* const baked_lut = hdr.gamma_enabled ? arena.nlt_cache.lut.data() : nullptr;
    const int lut_max_idx = hdr.gamma_enabled ? arena.nlt_cache.lut_max_idx : 0;

    auto compute_y0_at = [&](int r, int32_t* out_y0) {
        const int r_up = (r > 0) ? (r - 1) : 0;
        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
        const size_t row_up = static_cast<size_t>(r_up) * static_cast<size_t>(w_pix);
        compute_y0_row(plane_yl + row, plane_yh + row_up, plane_yh + row, out_y0, w_pix);
    };

    auto compute_g01_at = [&](int r, const int32_t* y0_r, const int32_t* y0_dn_r, int32_t* g0_r, int32_t* g1_r) {
        const int r_up = (r > 0) ? (r - 1) : 0;
        const int r_dn = (r + 1 < h_pix) ? (r + 1) : (h_pix - 1);
        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
        const size_t row_up = static_cast<size_t>(r_up) * static_cast<size_t>(w_pix);
        const size_t row_dn = static_cast<size_t>(r_dn) * static_cast<size_t>(w_pix);
        compute_g0_g1_row(
            y0_r, y0_dn_r, plane_yh + row,
            plane_cr + row, plane_cr + row_dn,
            plane_cb + row_up, plane_cb + row,
            g0_r, g1_r, w_pix);
    };

    pool.parallel_strips(static_cast<size_t>(active_quad_h), 32u, threads,
                         [&](size_t r0_s, size_t r1_s, int worker_idx) {
        const int r0 = static_cast<int>(r0_s);
        const int r1 = static_cast<int>(r1_s);
        SliceWorkBuf& ww = arena.workers[static_cast<size_t>(worker_idx)];
        ww.ensure_ring_cap(w_pix);

#if NIJIN_X86_64
        if (use_avx2) {
            run_rct_nlt_strip_avx2(
                r0, r1, ww, plane_yl, plane_yh, plane_cr, plane_cb,
                dst, dst_stride_u16, w_pix, h_pix,
                post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);
            return;
        }
#endif

        int32_t* y0_cur = ww.y0_a.data();
        int32_t* y0_nxt = ww.y0_b.data();
        int32_t* g0_up = ww.g0_slots[0].data();
        int32_t* g1_up = ww.g1_slots[0].data();
        int32_t* g0_cur = ww.g0_slots[1].data();
        int32_t* g1_cur = ww.g1_slots[1].data();
        int32_t* g0_dn = ww.g0_slots[2].data();
        int32_t* g1_dn = ww.g1_slots[2].data();
        const int32_t* g1_up_ptr = nullptr;

        if (r0 == 0) {
            compute_y0_at(0, y0_cur);
            if (1 < h_pix) {
                compute_y0_at(1, y0_nxt);
                compute_g01_at(0, y0_cur, y0_nxt, g0_cur, g1_cur);
                std::swap(y0_cur, y0_nxt);
            } else {
                compute_g01_at(0, y0_cur, y0_cur, g0_cur, g1_cur);
            }
            g1_up_ptr = g1_cur;
        } else {
            compute_y0_at(r0 - 1, y0_cur);
            compute_y0_at(r0, y0_nxt);
            compute_g01_at(r0 - 1, y0_cur, y0_nxt, g0_up, g1_up);
            std::swap(y0_cur, y0_nxt);
            if (r0 + 1 < h_pix) {
                compute_y0_at(r0 + 1, y0_nxt);
                compute_g01_at(r0, y0_cur, y0_nxt, g0_cur, g1_cur);
                std::swap(y0_cur, y0_nxt);
            } else {
                compute_g01_at(r0, y0_cur, y0_cur, g0_cur, g1_cur);
            }
            g1_up_ptr = g1_up;
        }

        for (int r = r0; r < r1; ++r) {
            const int32_t* g0_dn_ptr = nullptr;
            if (r + 1 < h_pix) {
                if (r + 2 < h_pix) {
                    compute_y0_at(r + 2, y0_nxt);
                    compute_g01_at(r + 1, y0_cur, y0_nxt, g0_dn, g1_dn);
                } else {
                    compute_g01_at(r + 1, y0_cur, y0_cur, g0_dn, g1_dn);
                }
                g0_dn_ptr = g0_dn;
            } else {
                g0_dn_ptr = g0_cur;
            }

            const size_t row = static_cast<size_t>(r) * static_cast<size_t>(w_pix);
            uint16_t* const out_even = dst + static_cast<size_t>(2 * r) * dst_stride_u16;
            uint16_t* const out_odd = out_even + dst_stride_u16;
            emit_bayer_row_pair(
                g1_up_ptr, g0_cur, g1_cur, g0_dn_ptr,
                plane_cr + row, plane_cb + row,
                out_even, out_odd, w_pix,
                post_offset, scale, scale_off, max_value, baked_lut, lut_max_idx);

            if (r + 1 < r1) {
                int32_t* const tmp0 = g0_up;
                int32_t* const tmp1 = g1_up;
                g0_up = g0_cur;
                g1_up = g1_cur;
                g0_cur = g0_dn;
                g1_cur = g1_dn;
                g0_dn = tmp0;
                g1_dn = tmp1;
                g1_up_ptr = g1_up;
                if (r + 2 < h_pix) {
                    std::swap(y0_cur, y0_nxt);
                }
            }
        }
    });

    fill_info_from_parsed(hdr, geom, info_out);

    if (quant_out != nullptr) {
        WaveletQuantProfile qp_prof;
        qp_prof.available = true;
        qp_prof.codec = CodecFamily::TicoRaw;
        qp_prof.nlt_enabled = hdr.gamma_enabled;
        qp_prof.raw_bit_depth = static_cast<uint16_t>(hdr.in_bitdepth);
        qp_prof.code_bit_depth = static_cast<uint16_t>(input_scale);
        qp_prof.nlt_raw_white_adu = static_cast<uint32_t>(max_value);
        qp_prof.nlt_code_max = (1u << static_cast<unsigned>(input_scale)) - 1u;
        qp_prof.row_step = 4u;

        const uint32_t num_row_blocks = (static_cast<uint32_t>(geom.full_h) + 3u) / 4u;
        qp_prof.q_luma_code_row.assign(num_row_blocks, 0.0f);
        qp_prof.q_chroma_code_row.assign(num_row_blocks, 0.0f);

        const detail::TicoSubbandWeightTable& wtbl = detail::get_ticoraw_subband_weights();
        const double u_full = static_cast<double>(1u << static_cast<unsigned>(input_scale));
        const double inv_u_full = 1.0 / std::max(1.0, u_full);

        std::vector<double> raw_luma_var(num_row_blocks, 0.0);
        std::vector<double> raw_chroma_var(num_row_blocks, 0.0);
        double luma_scale[6] = {};
        double chroma_scale[6] = {};
        double luma_w16[16] = {};
        double chroma_w16[16] = {};

        const int p_eval_end = std::min(geom.n_prec, static_cast<int>(num_row_blocks));
        for (int p_idx = 0; p_idx < p_eval_end; ++p_idx) {
            const uint8_t* phdr = data + prec_spans[static_cast<size_t>(p_idx)].offset;
            const uint8_t qp = phdr[3];
            const uint8_t rp = phdr[4];
            double pv_luma = 0.0;
            double pv_chroma = 0.0;
            for (int j = 0; j < kNumSubbands; ++j) {
                const int sb = geom.sp_sb_order[j];
                const int v = static_cast<int>(qp) - static_cast<int>(geom.sb_gains[sb]);
                int gtli = 0;
                if (v > 0) {
                    const int refine = (static_cast<int>(geom.sb_prios[sb]) < static_cast<int>(rp)) ? 1 : 0;
                    gtli = std::min(v - refine, 15);
                }
                const int eff_shift = gtli + static_cast<int>(hdr.fq);
                if (eff_shift <= 0) {
                    continue;
                }
                const double step_u = std::ldexp(1.0, eff_shift);
                const double var_u = (step_u * step_u) / 12.0;
                const detail::SubbandSynthWeight& sw = wtbl.by_sb[static_cast<size_t>(sb)];
                const double dl = var_u * sw.luma_gain;
                const double dc = var_u * sw.chroma_gain;
                pv_luma += dl;
                pv_chroma += dc;
                luma_scale[sw.scale_bin] += dl;
                chroma_scale[sw.scale_bin] += dc;
                for (int k = 0; k < 16; ++k) {
                    luma_w16[k] += var_u * sw.walsh16_luma[k];
                    chroma_w16[k] += var_u * sw.walsh16_chroma[k];
                }
            }
            raw_luma_var[static_cast<size_t>(p_idx)] = pv_luma;
            raw_chroma_var[static_cast<size_t>(p_idx)] = pv_chroma;
        }
        // Vertical 5/3 IDWT cross-precinct coupling weights [0.125, 0.75, 0.125].
        for (uint32_t i = 0; i < num_row_blocks; ++i) {
            const uint32_t ip = (i > 0u) ? (i - 1u) : 0u;
            const uint32_t in = (i + 1u < num_row_blocks) ? (i + 1u) : (num_row_blocks - 1u);
            const double vl = 0.125 * raw_luma_var[ip] + 0.75 * raw_luma_var[i] + 0.125 * raw_luma_var[in];
            const double vc = 0.125 * raw_chroma_var[ip] + 0.75 * raw_chroma_var[i] + 0.125 * raw_chroma_var[in];
            qp_prof.q_luma_code_row[i] = static_cast<float>(std::sqrt(std::max(0.0, vl)) * inv_u_full);
            qp_prof.q_chroma_code_row[i] = static_cast<float>(std::sqrt(std::max(0.0, vc)) * inv_u_full);
        }

        if (hdr.gamma_enabled && baked_lut != nullptr) {
            detail::compute_dadu_du_raw_from_lut(
                baked_lut, lut_max_idx + 1, 24,
                static_cast<uint32_t>(max_value), qp_prof.nlt_dadu_du_raw);
        } else {
            const float lin_slope = std::ldexp(1.0f, -scale);
            std::fill(qp_prof.nlt_dadu_du_raw,
                      qp_prof.nlt_dadu_du_raw + WaveletQuantProfile::kNltSlopeLutSize,
                      lin_slope);
        }

        detail::finalize_quant_profile_stats(qp_prof);
        detail::normalize_spectra(luma_scale, chroma_scale, luma_w16, chroma_w16, qp_prof);
        recalibrate_quant_profile_range(qp_prof, 0.0f, static_cast<float>(max_value));
        *quant_out = std::move(qp_prof);
    }

    return true;
}

} // namespace detail

bool decode_ticoraw_bayer(
    const uint8_t* data,
    size_t len,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    TicoRawInfo& info_out,
    std::string& err) {
    TicoRawInfo probed;
    if (!probe_ticoraw_header(data, len, probed, err)) {
        return false;
    }
    const size_t need = static_cast<size_t>(probed.width) * static_cast<size_t>(probed.height);
    if (bayer_out.size() != need) {
        bayer_out.resize(need);
    }
    return detail::decode_ticoraw_bayer_strided(
        data, len, num_threads, bayer_out.data(), probed.width, probed.height,
        info_out, err, nullptr, nullptr);
}

} // namespace nijinwvdc
