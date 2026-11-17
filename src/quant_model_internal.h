// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_QUANT_MODEL_INTERNAL_H
#define NIJINWVDC_QUANT_MODEL_INTERNAL_H

#include "../include/nijinWVDC.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nijinwvdc {
namespace detail {

struct SubbandSynthWeight {
    double luma_gain = 0.0;
    double chroma_gain = 0.0;
    uint32_t scale_bin = 0; // 0..5 -> [DC/LL, coarse..fine, Nyquist/G_H]
    double walsh16_luma[16] = {};
    double walsh16_chroma[16] = {};
};

inline void walsh4_1d_basis(int w, double out[4]) {
    // 1D 4-point sequency-ordered orthonormal Walsh-Hadamard basis.
    static constexpr double kBasis[4][4] = {
        { 0.5,  0.5,  0.5,  0.5}, // 0 zero crossings
        { 0.5,  0.5, -0.5, -0.5}, // 1 zero crossing
        { 0.5, -0.5, -0.5,  0.5}, // 2 zero crossings
        { 0.5, -0.5,  0.5, -0.5}, // 3 zero crossings (Nyquist)
    };
    for (int i = 0; i < 4; ++i) {
        out[i] = kBasis[w][i];
    }
}

struct Walsh4x4Autocorr {
    // A[k][ty + 3][tx + 3] for ty, tx in [-3, +3].
    double a[16][7][7] = {};

    Walsh4x4Autocorr() {
        for (int wy = 0; wy < 4; ++wy) {
            double by[4];
            walsh4_1d_basis(wy, by);
            for (int wx = 0; wx < 4; ++wx) {
                double bx[4];
                walsh4_1d_basis(wx, bx);
                const int k = wy * 4 + wx;
                for (int y1 = 0; y1 < 4; ++y1) {
                    for (int x1 = 0; x1 < 4; ++x1) {
                        const double p1 = by[y1] * bx[x1];
                        for (int y2 = 0; y2 < 4; ++y2) {
                            for (int x2 = 0; x2 < 4; ++x2) {
                                a[k][(y2 - y1) + 3][(x2 - x1) + 3] += p1 * by[y2] * bx[x2];
                            }
                        }
                    }
                }
            }
        }
    }
};

inline const Walsh4x4Autocorr& get_walsh4x4_autocorr() {
    static const Walsh4x4Autocorr kAuto;
    return kAuto;
}

// 1-level 1D 5/3 IDWT from split low (len_l) and high (len_h) arrays into out (len = len_l + len_h).
inline void idwt53_1d_level(const double* lo, int len_l, const double* hi, int len_h, double* out) {
    const int n = len_l + len_h;
    if (n <= 0) {
        return;
    }
    if (n == 1) {
        out[0] = (len_l > 0) ? lo[0] : 0.0;
        return;
    }
    for (int i = 0; i < len_l; ++i) {
        const double hp = (i > 0) ? hi[i - 1] : ((len_h > 0) ? hi[0] : 0.0);
        const double hc = (i < len_h) ? hi[i] : ((i > 0) ? hi[i - 1] : 0.0);
        out[2 * i] = lo[i] - 0.25 * (hp + hc);
    }
    for (int i = 0; i < len_h; ++i) {
        const double e0 = out[2 * i];
        const double e1 = (2 * i + 2 < n) ? out[2 * i + 2] : e0;
        out[2 * i + 1] = hi[i] + 0.5 * (e0 + e1);
    }
}

// Build 1D 5/3 impulse response of length G centered at index G/2.
// level_from_finest = 0 for LL at depth `total_levels`; 1..total_levels for highpass (1 = finest, total_levels = coarsest).
inline void build_1d_53_impulse(
    int total_levels,
    int level_from_finest,
    bool is_ll,
    int grid_n,
    std::vector<double>& out_1d,
    int& stride_out) {
    if (total_levels == 0) {
        out_1d.assign(grid_n, 0.0);
        out_1d[grid_n / 2] = 1.0;
        stride_out = 1;
        return;
    }
    std::vector<int> ll_lens(total_levels + 1);
    std::vector<int> hi_lens(total_levels + 1);
    ll_lens[0] = grid_n;
    for (int l = 1; l <= total_levels; ++l) {
        hi_lens[l] = ll_lens[l - 1] >> 1;
        ll_lens[l] = (ll_lens[l - 1] + 1) >> 1;
    }

    std::vector<double> ll_cur(ll_lens[total_levels], 0.0);
    if (is_ll) {
        ll_cur[ll_lens[total_levels] / 2] = 1.0;
    }

    std::vector<double> hi_buf;
    std::vector<double> next_buf;
    for (int l = total_levels; l >= 1; --l) {
        hi_buf.assign(hi_lens[l], 0.0);
        if (!is_ll && l == level_from_finest) {
            hi_buf[hi_lens[l] / 2] = 1.0;
        }
        next_buf.assign(ll_lens[l] + hi_lens[l], 0.0);
        idwt53_1d_level(ll_cur.data(), ll_lens[l], hi_buf.data(), hi_lens[l], next_buf.data());
        ll_cur.swap(next_buf);
    }
    out_1d = std::move(ll_cur);
    const int ds = is_ll ? total_levels : level_from_finest;
    stride_out = 1 << ds;
}

inline void accumulate_grid_walsh(
    const double* g,
    int n,
    double area_stride,
    double& var_out,
    double walsh_out[16]) {
    constexpr int kMargin = 4;
    double r[7][7] = {};
    for (int y = kMargin; y < n - kMargin; ++y) {
        for (int x = kMargin; x < n - kMargin; ++x) {
            const double c = g[static_cast<size_t>(y) * n + x];
            if (c == 0.0) {
                continue;
            }
            for (int ty = -3; ty <= 3; ++ty) {
                const double* row = g + static_cast<size_t>(y + ty) * n;
                for (int tx = -3; tx <= 3; ++tx) {
                    r[ty + 3][tx + 3] += c * row[x + tx];
                }
            }
        }
    }
    const double inv_s = 1.0 / area_stride;
    for (int ty = 0; ty < 7; ++ty) {
        for (int tx = 0; tx < 7; ++tx) {
            r[ty][tx] *= inv_s;
        }
    }
    var_out = r[3][3];
    const Walsh4x4Autocorr& auto_w = get_walsh4x4_autocorr();
    for (int k = 0; k < 16; ++k) {
        double p = 0.0;
        for (int ty = 0; ty < 7; ++ty) {
            for (int tx = 0; tx < 7; ++tx) {
                p += auto_w.a[k][ty][tx] * r[ty][tx];
            }
        }
        walsh_out[k] += std::max(0.0, p);
    }
}

inline void finalize_ticoraw_subband_weight(
    int comp_id,
    int v_lvl,
    int h_lvl,
    SubbandSynthWeight& out) {
    constexpr int G = 128;
    const size_t G2 = static_cast<size_t>(G) * G;

    std::vector<double> h1_x;
    std::vector<double> h1_y;
    int sx = 1;
    int sy = 1;

    if (comp_id == 2) {
        h1_x.assign(G, 0.0);
        h1_x[G / 2] = 1.0;
        h1_y.assign(G, 0.0);
        h1_y[G / 2] = 1.0;
        sx = 1;
        sy = 1;
        out.scale_bin = 5u;
    } else {
        const bool is_ll_x = (h_lvl == 0);
        const int hl_from_finest = is_ll_x ? 0 : (6 - h_lvl);
        build_1d_53_impulse(5, hl_from_finest, is_ll_x, G, h1_x, sx);
        build_1d_53_impulse(1, 1, (v_lvl == 0), G, h1_y, sy);
        if (v_lvl == 0 && h_lvl == 0) {
            out.scale_bin = 0u;
        } else if (v_lvl == 0 && h_lvl == 1) {
            out.scale_bin = 1u;
        } else if (v_lvl == 0 && h_lvl == 2) {
            out.scale_bin = 2u;
        } else if (v_lvl == 0 && h_lvl == 3) {
            out.scale_bin = 3u;
        } else {
            out.scale_bin = 4u;
        }
    }

    std::vector<double> yl(G2, 0.0);
    std::vector<double> yh(G2, 0.0);
    std::vector<double> cr(G2, 0.0);
    std::vector<double> cb(G2, 0.0);

    double* target = (comp_id == 0) ? yl.data()
                   : (comp_id == 1) ? cr.data()
                   : (comp_id == 2) ? yh.data()
                                    : cb.data();
    for (int y = 0; y < G; ++y) {
        const double vy = h1_y[y];
        if (vy == 0.0) continue;
        for (int x = 0; x < G; ++x) {
            target[static_cast<size_t>(y) * G + x] = vy * h1_x[x];
        }
    }

    std::vector<double> y0(G2, 0.0);
    std::vector<double> g0(G2, 0.0);
    std::vector<double> g1(G2, 0.0);
    std::vector<double> dr(G2, 0.0);
    std::vector<double> db(G2, 0.0);

    for (int r = 1; r < G - 1; ++r) {
        for (int c = 1; c < G - 1; ++c) {
            const size_t idx = static_cast<size_t>(r) * G + c;
            const double yh_s = yh[idx - G] + yh[idx - G + 1] + yh[idx] + yh[idx + 1];
            y0[idx] = yl[idx] - 0.125 * yh_s;
        }
    }
    for (int r = 1; r < G - 1; ++r) {
        for (int c = 1; c < G - 1; ++c) {
            const size_t idx = static_cast<size_t>(r) * G + c;
            g0[idx] = y0[idx] - 0.125 * (cb[idx] + cb[idx - G] + cr[idx] + cr[idx + 1]);
            g1[idx] = 0.25 * (y0[idx + G] + y0[idx] + y0[idx + G - 1] + y0[idx - 1]) +
                      yh[idx] - 0.125 * (cb[idx - 1] + cb[idx] + cr[idx] + cr[idx + G]);
        }
    }
    for (int r = 2; r < G - 2; ++r) {
        for (int c = 2; c < G - 2; ++c) {
            const size_t idx = static_cast<size_t>(r) * G + c;
            const double red = 0.25 * (g1[idx] + g0[idx] + g0[idx - 1] + g1[idx - G]) + cr[idx];
            const double blu = 0.25 * (g1[idx] + g0[idx] + g1[idx + 1] + g0[idx + G]) + cb[idx];
            const double gavg = 0.5 * (g0[idx] + g1[idx]);
            dr[idx] = red - gavg;
            db[idx] = blu - gavg;
        }
    }

    const double area_s = static_cast<double>(sy) * static_cast<double>(sx);
    double v_g0 = 0.0, v_g1 = 0.0, v_dr = 0.0, v_db = 0.0;
    double w_g0[16] = {}, w_g1[16] = {}, w_dr[16] = {}, w_db[16] = {};
    accumulate_grid_walsh(g0.data(), G, area_s, v_g0, w_g0);
    accumulate_grid_walsh(g1.data(), G, area_s, v_g1, w_g1);
    accumulate_grid_walsh(dr.data(), G, area_s, v_dr, w_dr);
    accumulate_grid_walsh(db.data(), G, area_s, v_db, w_db);

    out.luma_gain = 0.5 * (v_g0 + v_g1);
    out.chroma_gain = 0.5 * (v_dr + v_db);
    for (int k = 0; k < 16; ++k) {
        out.walsh16_luma[k] = 0.5 * (w_g0[k] + w_g1[k]);
        out.walsh16_chroma[k] = 0.5 * (w_dr[k] + w_db[k]);
    }
}

struct TicoSubbandWeightTable {
    // Indexed by sb_id in [0, 71].
    std::array<SubbandSynthWeight, 72> by_sb{};

    TicoSubbandWeightTable() {
        for (int vl = 0; vl < 2; ++vl) {
            for (int c = 0; c < 4; ++c) {
                const int h_max = (c == 2) ? 1 : ((vl == 0) ? 6 : 2);
                for (int hl = 0; hl < h_max; ++hl) {
                    const int sb = (c == 2) ? (hl + 18 * c) : (hl + 6 * (vl + 3 * c));
                    finalize_ticoraw_subband_weight(c, vl, hl, by_sb[sb]);
                }
            }
        }
    }
};

inline const TicoSubbandWeightTable& get_ticoraw_subband_weights() {
    static const TicoSubbandWeightTable tbl;
    return tbl;
}

struct Arw6SubbandWeightTable {
    // [c in 0..3][r in 0..3][b in 0..3]:
    // c=0: G_L, c=1: R, c=2: B (all decomp_levels = 3); c=3: G_H (r=0,b=0).
    SubbandSynthWeight w[4][4][4]{};

    Arw6SubbandWeightTable() {
        constexpr int G = 64;
        const size_t G2 = static_cast<size_t>(G) * G;
        for (int c = 0; c < 4; ++c) {
            const int max_r = (c == 3) ? 0 : 3;
            for (int r = 0; r <= max_r; ++r) {
                const int b0 = (r == 0) ? 0 : 1;
                const int b1 = (r == 0) ? 0 : 3;
                for (int b = b0; b <= b1; ++b) {
                    std::vector<double> hx, hy;
                    int sx = 1, sy = 1;
                    if (c == 3) {
                        hx.assign(G, 0.0);
                        hx[G / 2] = 1.0;
                        hy.assign(G, 0.0);
                        hy[G / 2] = 1.0;
                        w[c][r][b].scale_bin = 5u;
                    } else if (r == 0) {
                        build_1d_53_impulse(3, 0, true, G, hx, sx);
                        build_1d_53_impulse(3, 0, true, G, hy, sy);
                        w[c][r][b].scale_bin = 0u;
                    } else {
                        const bool hi_x = (b == 1 || b == 3);
                        const bool hi_y = (b == 2 || b == 3);
                        const int lv = 4 - r; // r=1 -> coarsest level 3; r=3 -> finest level 1
                        build_1d_53_impulse(3, lv, !hi_x, G, hx, sx);
                        build_1d_53_impulse(3, lv, !hi_y, G, hy, sy);
                        // For HL+LH+HH: r=1->bin 2, r=2->bin 3, r=3->bin 4
                        w[c][r][b].scale_bin = static_cast<uint32_t>(r + 1);
                    }

                    std::vector<double> gl(G2, 0.0), rp(G2, 0.0), bp(G2, 0.0), gh(G2, 0.0);
                    double* target = (c == 0) ? gl.data()
                                   : (c == 1) ? rp.data()
                                   : (c == 2) ? bp.data()
                                              : gh.data();
                    for (int y = 0; y < G; ++y) {
                        const double vy = hy[y];
                        if (vy == 0.0) continue;
                        for (int x = 0; x < G; ++x) {
                            target[static_cast<size_t>(y) * G + x] = vy * hx[x];
                        }
                    }

                    std::vector<double> g0(G2, 0.0), g1(G2, 0.0), dr(G2, 0.0), db(G2, 0.0);
                    for (int row = 1; row < G - 1; ++row) {
                        for (int col = 1; col < G - 1; ++col) {
                            const size_t idx = static_cast<size_t>(row) * G + col;
                            g0[idx] = gl[idx] - 0.125 * (gh[idx - G] + gh[idx - G + 1] + gh[idx] + gh[idx + 1]);
                        }
                    }
                    for (int row = 2; row < G - 2; ++row) {
                        for (int col = 2; col < G - 2; ++col) {
                            const size_t idx = static_cast<size_t>(row) * G + col;
                            g1[idx] = gh[idx] + 0.25 * (g0[idx - 1] + g0[idx] + g0[idx + G - 1] + g0[idx + G]);
                            dr[idx] = 2.0 * rp[idx];
                            db[idx] = 2.0 * bp[idx];
                        }
                    }

                    const double area_s = static_cast<double>(sy) * static_cast<double>(sx);
                    double v_g0 = 0.0, v_g1 = 0.0, v_dr = 0.0, v_db = 0.0;
                    double w_g0[16] = {}, w_g1[16] = {}, w_dr[16] = {}, w_db[16] = {};
                    accumulate_grid_walsh(g0.data(), G, area_s, v_g0, w_g0);
                    accumulate_grid_walsh(g1.data(), G, area_s, v_g1, w_g1);
                    accumulate_grid_walsh(dr.data(), G, area_s, v_dr, w_dr);
                    accumulate_grid_walsh(db.data(), G, area_s, v_db, w_db);

                    SubbandSynthWeight& dst = w[c][r][b];
                    dst.luma_gain = 0.5 * (v_g0 + v_g1);
                    dst.chroma_gain = 0.5 * (v_dr + v_db);
                    for (int k = 0; k < 16; ++k) {
                        dst.walsh16_luma[k] = 0.5 * (w_g0[k] + w_g1[k]);
                        dst.walsh16_chroma[k] = 0.5 * (w_dr[k] + w_db[k]);
                    }
                }
            }
        }
    }
};

inline const Arw6SubbandWeightTable& get_arw6_subband_weights() {
    static const Arw6SubbandWeightTable tbl;
    return tbl;
}

// Compute smoothed d(ADU)/du curve indexed by raw ADU ratio r in [0, 1] (= raw / raw_white_adu)
// from a monotonically non-decreasing NLT LUT `lut[0..lut_len-1]`.
inline void compute_dadu_du_raw_from_lut(
    const uint16_t* lut,
    int lut_len,
    int half_win,
    uint32_t raw_white_adu,
    float out_dadu_du_raw[WaveletQuantProfile::kNltSlopeLutSize]) {
    if (!lut || lut_len < 4 || raw_white_adu == 0u) {
        std::fill(out_dadu_du_raw, out_dadu_du_raw + WaveletQuantProfile::kNltSlopeLutSize, 1.0f);
        return;
    }
    const int hw = std::max(1, std::min(half_win, lut_len / 4));
    for (uint32_t j = 0; j < WaveletQuantProfile::kNltSlopeLutSize; ++j) {
        const double target_adu = (static_cast<double>(j) / 255.0) * static_cast<double>(raw_white_adu);
        const uint16_t tgt_u16 = static_cast<uint16_t>(std::min<double>(65535.0, std::max(0.0, target_adu)));
        const uint16_t* it = std::lower_bound(lut, lut + lut_len, tgt_u16);
        int idx = static_cast<int>(it - lut);
        if (idx >= lut_len) {
            idx = lut_len - 1;
        }
        // Step back from top saturation plateau so slope reflects the curve rather than clamped flat cap.
        while (idx > 2 * hw && lut[idx] == lut[idx - 1]) {
            --idx;
        }
        int u0 = std::max(0, idx - hw);
        int u1 = std::min(lut_len - 1, idx + hw);
        while (u1 > u0 + 2 && lut[u1] == lut[u1 - 1]) {
            --u1;
        }
        if (u1 - u0 < hw && u1 > 2 * hw) {
            u0 = std::max(0, u1 - 2 * hw);
        }
        const int du = std::max(1, u1 - u0);
        const double slope = static_cast<double>(lut[u1] - lut[u0]) / static_cast<double>(du);
        out_dadu_du_raw[j] = static_cast<float>(std::max(1e-4, slope));
    }
}

inline void finalize_quant_profile_stats(WaveletQuantProfile& prof) {
    const uint32_t n = static_cast<uint32_t>(prof.q_luma_code_row.size());
    prof.num_rows = n;
    if (n == 0u) {
        prof.mean_luma_code_std = 0.0f;
        prof.mean_chroma_code_std = 0.0f;
        prof.max_luma_code_std = 0.0f;
        prof.max_chroma_code_std = 0.0f;
        return;
    }
    double sum_l = 0.0, sum_c = 0.0;
    float max_l = 0.0f, max_c = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float l = prof.q_luma_code_row[i];
        const float c = prof.q_chroma_code_row[i];
        sum_l += l;
        sum_c += c;
        if (l > max_l) max_l = l;
        if (c > max_c) max_c = c;
    }
    prof.mean_luma_code_std = static_cast<float>(sum_l / static_cast<double>(n));
    prof.mean_chroma_code_std = static_cast<float>(sum_c / static_cast<double>(n));
    prof.max_luma_code_std = max_l;
    prof.max_chroma_code_std = max_c;
}

inline void normalize_spectra(
    const double luma_scale[6],
    const double chroma_scale[6],
    const double luma_w16[16],
    const double chroma_w16[16],
    WaveletQuantProfile& prof) {
    double ls_sum = 0.0, cs_sum = 0.0;
    double lw_sum = 0.0, cw_sum = 0.0;
    for (int i = 0; i < 6; ++i) {
        ls_sum += std::max(0.0, luma_scale[i]);
        cs_sum += std::max(0.0, chroma_scale[i]);
    }
    for (int k = 0; k < 16; ++k) {
        lw_sum += std::max(0.0, luma_w16[k]);
        cw_sum += std::max(0.0, chroma_w16[k]);
    }
    for (int i = 0; i < 6; ++i) {
        prof.luma_scale_psd[i] = (ls_sum > 0.0) ? static_cast<float>(luma_scale[i] / ls_sum) : (i == 5 ? 1.0f : 0.0f);
        prof.chroma_scale_psd[i] = (cs_sum > 0.0) ? static_cast<float>(chroma_scale[i] / cs_sum) : (i == 5 ? 1.0f : 0.0f);
    }
    for (int k = 0; k < 16; ++k) {
        prof.walsh16_luma_norm[k] = (lw_sum > 0.0) ? static_cast<float>((16.0 * luma_w16[k]) / lw_sum) : 1.0f;
        prof.walsh16_chroma_norm[k] = (cw_sum > 0.0) ? static_cast<float>((16.0 * chroma_w16[k]) / cw_sum) : 1.0f;
    }
}

} // namespace detail
} // namespace nijinwvdc

#endif // NIJINWVDC_QUANT_MODEL_INTERNAL_H
