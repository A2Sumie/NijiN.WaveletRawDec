// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_QUANT_MODEL_INTERNAL_H
#define NIJINWVDC_QUANT_MODEL_INTERNAL_H

#include "../include/nijinWVDC.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nijinwvdc {
namespace detail {

struct SubbandSynthWeight {
    double luma_gain = 0.0;
    double chroma_gain = 0.0;
    uint32_t scale_bin = 0; // 0..5 -> [DC/LL, coarse..fine, Nyquist/G_H]
    double walsh16_luma[16] = {};
    double walsh16_chroma[16] = {};
};

constexpr void walsh4_1d_basis(int w, double out[4]) noexcept {
    constexpr double kBasis[4][4] = {
        { 0.5,  0.5,  0.5,  0.5},
        { 0.5,  0.5, -0.5, -0.5},
        { 0.5, -0.5, -0.5,  0.5},
        { 0.5, -0.5,  0.5, -0.5},
    };
    for (int i = 0; i < 4; ++i) {
        out[i] = kBasis[w][i];
    }
}

struct Walsh4x4Autocorr {
    double a[16][7][7] = {};
};

consteval Walsh4x4Autocorr make_walsh4x4_autocorr() noexcept {
    Walsh4x4Autocorr out{};
    for (int wy = 0; wy < 4; ++wy) {
        double by[4]{};
        walsh4_1d_basis(wy, by);
        for (int wx = 0; wx < 4; ++wx) {
            double bx[4]{};
            walsh4_1d_basis(wx, bx);
            const int k = wy * 4 + wx;
            for (int y1 = 0; y1 < 4; ++y1) {
                for (int x1 = 0; x1 < 4; ++x1) {
                    const double p1 = by[y1] * bx[x1];
                    for (int y2 = 0; y2 < 4; ++y2) {
                        for (int x2 = 0; x2 < 4; ++x2) {
                            out.a[k][(y2 - y1) + 3][(x2 - x1) + 3] += p1 * by[y2] * bx[x2];
                        }
                    }
                }
            }
        }
    }
    return out;
}

inline constexpr Walsh4x4Autocorr kWalsh4x4Autocorr = make_walsh4x4_autocorr();

inline constexpr const Walsh4x4Autocorr& get_walsh4x4_autocorr() noexcept {
    return kWalsh4x4Autocorr;
}

struct Autocorr1D {
    double r[8] = {};
};

// Exact infinite-domain 1D LeGall 5/3 synthesis autocorrelation R[0..7] / stride.
// grid_n=256 (5-level) and 128 (3-level) eliminate boundary reflection on all subbands.
consteval Autocorr1D build_1d_53_autocorr(int total_levels, int level_from_finest, bool is_ll) noexcept {
    Autocorr1D out{};
    if (total_levels == 0) {
        out.r[0] = 1.0;
        return out;
    }
    const int grid_n = (total_levels >= 5) ? 256 : 128;
    int ll_lens[6] = {};
    int hi_lens[6] = {};
    ll_lens[0] = grid_n;
    for (int l = 1; l <= total_levels; ++l) {
        hi_lens[l] = ll_lens[l - 1] >> 1;
        ll_lens[l] = (ll_lens[l - 1] + 1) >> 1;
    }
    double cur[256] = {};
    if (is_ll) {
        cur[ll_lens[total_levels] / 2] = 1.0;
    }
    for (int l = total_levels; l >= 1; --l) {
        const int len_l = ll_lens[l];
        const int len_h = hi_lens[l];
        const int n = len_l + len_h;
        const int hi_imp = (!is_ll && l == level_from_finest) ? (len_h / 2) : -1;
        double nxt[256] = {};
        for (int i = 0; i < len_l; ++i) {
            const double hp = (i > 0) ? ((i - 1 == hi_imp) ? 1.0 : 0.0) : ((hi_imp == 0) ? 1.0 : 0.0);
            const double hc = (i < len_h) ? ((i == hi_imp) ? 1.0 : 0.0) : ((i > 0 && i - 1 == hi_imp) ? 1.0 : 0.0);
            nxt[2 * i] = cur[i] - 0.25 * (hp + hc);
        }
        for (int i = 0; i < len_h; ++i) {
            const double e0 = nxt[2 * i];
            const double e1 = (2 * i + 2 < n) ? nxt[2 * i + 2] : e0;
            nxt[2 * i + 1] = ((i == hi_imp) ? 1.0 : 0.0) + 0.5 * (e0 + e1);
        }
        for (int i = 0; i < n; ++i) {
            cur[i] = nxt[i];
        }
    }
    const int ds = is_ll ? total_levels : level_from_finest;
    const double inv_s = 1.0 / static_cast<double>(1 << ds);
    for (int d = 0; d < 8; ++d) {
        double s = 0.0;
        for (int i = 0; i + d < grid_n; ++i) {
            s += cur[i] * cur[i + d];
        }
        out.r[d] = s * inv_s;
    }
    return out;
}

struct SparseStencilAutocorr {
    int8_t dy[81] = {};
    int8_t dx[81] = {};
    double w[81] = {};
    int count = 0;
};

template <int M>
consteval SparseStencilAutocorr make_sparse_pair_autocorr(const double (&k0)[M][M], const double (&k1)[M][M]) noexcept {
    double rk[9][9] = {};
    for (int y = 0; y < M; ++y) {
        for (int x = 0; x < M; ++x) {
            const double v0 = k0[y][x];
            const double v1 = k1[y][x];
            if (v0 == 0.0 && v1 == 0.0) {
                continue;
            }
            for (int dy = -4; dy <= 4; ++dy) {
                const int y2 = y + dy;
                if (y2 < 0 || y2 >= M) {
                    continue;
                }
                for (int dx = -4; dx <= 4; ++dx) {
                    const int x2 = x + dx;
                    if (x2 < 0 || x2 >= M) {
                        continue;
                    }
                    rk[dy + 4][dx + 4] += 0.5 * (v0 * k0[y2][x2] + v1 * k1[y2][x2]);
                }
            }
        }
    }
    SparseStencilAutocorr out{};
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const double val = rk[dy + 4][dx + 4];
            if (val != 0.0) {
                out.dy[out.count] = static_cast<int8_t>(dy);
                out.dx[out.count] = static_cast<int8_t>(dx);
                out.w[out.count] = val;
                ++out.count;
            }
        }
    }
    return out;
}

consteval void eval_sparse_walsh(
    const SparseStencilAutocorr& rk_l,
    const SparseStencilAutocorr& rk_c,
    const Autocorr1D& ay,
    const Autocorr1D& ax,
    SubbandSynthWeight& out) noexcept {
    double rl[7][7] = {};
    double rc[7][7] = {};
    for (int ty = -3; ty <= 3; ++ty) {
        for (int tx = -3; tx <= 3; ++tx) {
            double sl = 0.0;
            for (int i = 0; i < rk_l.count; ++i) {
                const int dy = ty - static_cast<int>(rk_l.dy[i]);
                const int dx = tx - static_cast<int>(rk_l.dx[i]);
                const int ly = (dy < 0) ? -dy : dy;
                const int lx = (dx < 0) ? -dx : dx;
                sl += rk_l.w[i] * ay.r[ly] * ax.r[lx];
            }
            rl[ty + 3][tx + 3] = sl;

            double sc = 0.0;
            for (int i = 0; i < rk_c.count; ++i) {
                const int dy = ty - static_cast<int>(rk_c.dy[i]);
                const int dx = tx - static_cast<int>(rk_c.dx[i]);
                const int ly = (dy < 0) ? -dy : dy;
                const int lx = (dx < 0) ? -dx : dx;
                sc += rk_c.w[i] * ay.r[ly] * ax.r[lx];
            }
            rc[ty + 3][tx + 3] = sc;
        }
    }
    out.luma_gain = rl[3][3];
    out.chroma_gain = rc[3][3];
    for (int k = 0; k < 16; ++k) {
        double pl = 0.0;
        double pc = 0.0;
        for (int ty = 0; ty < 7; ++ty) {
            for (int tx = 0; tx < 7; ++tx) {
                const double wa = kWalsh4x4Autocorr.a[k][ty][tx];
                pl += wa * rl[ty][tx];
                pc += wa * rc[ty][tx];
            }
        }
        out.walsh16_luma[k] = (pl > 0.0) ? pl : 0.0;
        out.walsh16_chroma[k] = (pc > 0.0) ? pc : 0.0;
    }
}

struct TicoSubbandWeightTable {
    std::array<SubbandSynthWeight, 72> by_sb{};
};

consteval TicoSubbandWeightTable make_ticoraw_subband_weights() noexcept {
    constexpr int M = 13;
    SparseStencilAutocorr rk_l[4]{};
    SparseStencilAutocorr rk_c[4]{};
    for (int c = 0; c < 4; ++c) {
        double yl[M][M]{};
        double yh[M][M]{};
        double cr[M][M]{};
        double cb[M][M]{};
        if (c == 0) yl[M / 2][M / 2] = 1.0;
        else if (c == 1) cr[M / 2][M / 2] = 1.0;
        else if (c == 2) yh[M / 2][M / 2] = 1.0;
        else cb[M / 2][M / 2] = 1.0;

        double y0[M][M]{};
        double g0[M][M]{};
        double g1[M][M]{};
        double dr[M][M]{};
        double db[M][M]{};

        for (int r = 1; r < M - 1; ++r) {
            for (int col = 1; col < M - 1; ++col) {
                y0[r][col] = yl[r][col] - 0.125 * (yh[r - 1][col] + yh[r - 1][col + 1] + yh[r][col] + yh[r][col + 1]);
            }
        }
        for (int r = 1; r < M - 1; ++r) {
            for (int col = 1; col < M - 1; ++col) {
                g0[r][col] = y0[r][col] - 0.125 * (cb[r][col] + cb[r - 1][col] + cr[r][col] + cr[r][col + 1]);
                g1[r][col] = 0.25 * (y0[r + 1][col] + y0[r][col] + y0[r + 1][col - 1] + y0[r][col - 1]) +
                             yh[r][col] - 0.125 * (cb[r][col - 1] + cb[r][col] + cr[r][col] + cr[r + 1][col]);
            }
        }
        for (int r = 2; r < M - 2; ++r) {
            for (int col = 2; col < M - 2; ++col) {
                const double red = 0.25 * (g1[r][col] + g0[r][col] + g0[r][col - 1] + g1[r - 1][col]) + cr[r][col];
                const double blu = 0.25 * (g1[r][col] + g0[r][col] + g1[r][col + 1] + g0[r + 1][col]) + cb[r][col];
                const double gavg = 0.5 * (g0[r][col] + g1[r][col]);
                dr[r][col] = red - gavg;
                db[r][col] = blu - gavg;
            }
        }
        rk_l[c] = make_sparse_pair_autocorr(g0, g1);
        rk_c[c] = make_sparse_pair_autocorr(dr, db);
    }

    Autocorr1D ax_v[6]{};
    ax_v[0] = build_1d_53_autocorr(5, 0, true);
    for (int hl = 1; hl < 6; ++hl) {
        ax_v[hl] = build_1d_53_autocorr(5, 6 - hl, false);
    }
    const Autocorr1D ay_v[2] = {
        build_1d_53_autocorr(1, 1, true),
        build_1d_53_autocorr(1, 1, false),
    };
    const Autocorr1D a_yh = build_1d_53_autocorr(0, 0, true);

    TicoSubbandWeightTable tbl{};
    for (int vl = 0; vl < 2; ++vl) {
        for (int c = 0; c < 4; ++c) {
            const int h_max = (c == 2) ? 1 : ((vl == 0) ? 6 : 2);
            for (int hl = 0; hl < h_max; ++hl) {
                const int sb = (c == 2) ? (hl + 18 * c) : (hl + 6 * (vl + 3 * c));
                SubbandSynthWeight& out = tbl.by_sb[sb];
                if (c == 2) {
                    out.scale_bin = 5u;
                    eval_sparse_walsh(rk_l[c], rk_c[c], a_yh, a_yh, out);
                } else {
                    out.scale_bin = (vl == 0 && hl <= 3) ? static_cast<uint32_t>(hl) : 4u;
                    eval_sparse_walsh(rk_l[c], rk_c[c], ay_v[vl], ax_v[hl], out);
                }
            }
        }
    }
    return tbl;
}

inline constexpr TicoSubbandWeightTable kTicoSubbandWeightTable = make_ticoraw_subband_weights();

inline constexpr const TicoSubbandWeightTable& get_ticoraw_subband_weights() noexcept {
    return kTicoSubbandWeightTable;
}

struct Arw6SubbandWeightTable {
    SubbandSynthWeight w[4][4][4]{};
};

consteval Arw6SubbandWeightTable make_arw6_subband_weights() noexcept {
    constexpr int M = 13;
    SparseStencilAutocorr rk_l[4]{};
    SparseStencilAutocorr rk_c[4]{};
    for (int c = 0; c < 4; ++c) {
        double gl[M][M]{};
        double rp[M][M]{};
        double bp[M][M]{};
        double gh[M][M]{};
        if (c == 0) gl[M / 2][M / 2] = 1.0;
        else if (c == 1) rp[M / 2][M / 2] = 1.0;
        else if (c == 2) bp[M / 2][M / 2] = 1.0;
        else gh[M / 2][M / 2] = 1.0;

        double g0[M][M]{};
        double g1[M][M]{};
        double dr[M][M]{};
        double db[M][M]{};
        for (int row = 1; row < M - 1; ++row) {
            for (int col = 1; col < M - 1; ++col) {
                g0[row][col] = gl[row][col] - 0.125 * (gh[row - 1][col] + gh[row - 1][col + 1] + gh[row][col] + gh[row][col + 1]);
            }
        }
        for (int row = 2; row < M - 2; ++row) {
            for (int col = 2; col < M - 2; ++col) {
                g1[row][col] = gh[row][col] + 0.25 * (g0[row][col - 1] + g0[row][col] + g0[row + 1][col - 1] + g0[row + 1][col]);
                dr[row][col] = 2.0 * rp[row][col];
                db[row][col] = 2.0 * bp[row][col];
            }
        }
        rk_l[c] = make_sparse_pair_autocorr(g0, g1);
        rk_c[c] = make_sparse_pair_autocorr(dr, db);
    }

    const Autocorr1D ll3 = build_1d_53_autocorr(3, 0, true);
    Autocorr1D hi[4]{};
    for (int r = 1; r <= 3; ++r) {
        hi[r] = build_1d_53_autocorr(3, 4 - r, false);
    }
    const Autocorr1D a_gh = build_1d_53_autocorr(0, 0, true);

    Arw6SubbandWeightTable tbl{};
    for (int c = 0; c < 4; ++c) {
        if (c == 2) {
            for (int r = 0; r <= 3; ++r) {
                for (int b = 0; b < 4; ++b) {
                    tbl.w[2][r][b] = tbl.w[1][r][b];
                }
            }
            continue;
        }
        const int max_r = (c == 3) ? 0 : 3;
        for (int r = 0; r <= max_r; ++r) {
            const int b0 = (r == 0) ? 0 : 1;
            const int b1 = (r == 0) ? 0 : 3;
            for (int b = b0; b <= b1; ++b) {
                SubbandSynthWeight& dst = tbl.w[c][r][b];
                if (c == 3) {
                    dst.scale_bin = 5u;
                    eval_sparse_walsh(rk_l[c], rk_c[c], a_gh, a_gh, dst);
                } else if (r == 0) {
                    dst.scale_bin = 0u;
                    eval_sparse_walsh(rk_l[c], rk_c[c], ll3, ll3, dst);
                } else {
                    const bool hi_x = (b == 1 || b == 3);
                    const bool hi_y = (b == 2 || b == 3);
                    dst.scale_bin = static_cast<uint32_t>(r + 1);
                    eval_sparse_walsh(rk_l[c], rk_c[c], hi_y ? hi[r] : ll3, hi_x ? hi[r] : ll3, dst);
                }
            }
        }
    }
    return tbl;
}

inline constexpr Arw6SubbandWeightTable kArw6SubbandWeightTable = make_arw6_subband_weights();

inline constexpr const Arw6SubbandWeightTable& get_arw6_subband_weights() noexcept {
    return kArw6SubbandWeightTable;
}

constexpr int lower_bound_u16_idx(const uint16_t* lut, int n, uint16_t tgt) noexcept {
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        const int mid = lo + ((hi - lo) >> 1);
        if (lut[mid] < tgt) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Compute smoothed d(ADU)/du curve indexed by raw ADU ratio r in [0, 1] (= raw / raw_white_adu)
// from a monotonically non-decreasing NLT LUT `lut[0..lut_len-1]`.
constexpr void compute_dadu_du_raw_from_lut(
    const uint16_t* lut,
    int lut_len,
    int half_win,
    uint32_t raw_white_adu,
    float out_dadu_du_raw[WaveletQuantProfile::kNltSlopeLutSize]) noexcept {
    if (!lut || lut_len < 4 || raw_white_adu == 0u) {
        for (uint32_t j = 0; j < WaveletQuantProfile::kNltSlopeLutSize; ++j) {
            out_dadu_du_raw[j] = 1.0f;
        }
        return;
    }
    const int lim_hw = lut_len / 4;
    const int hw = (half_win < 1) ? 1 : ((half_win > lim_hw) ? lim_hw : half_win);
    for (uint32_t j = 0; j < WaveletQuantProfile::kNltSlopeLutSize; ++j) {
        const double target_adu = (static_cast<double>(j) / 255.0) * static_cast<double>(raw_white_adu);
        const double clamped_adu = (target_adu < 0.0) ? 0.0 : ((target_adu > 65535.0) ? 65535.0 : target_adu);
        const uint16_t tgt_u16 = static_cast<uint16_t>(clamped_adu);
        int idx = lower_bound_u16_idx(lut, lut_len, tgt_u16);
        if (idx >= lut_len) {
            idx = lut_len - 1;
        }
        while (idx > 2 * hw && lut[idx] == lut[idx - 1]) {
            --idx;
        }
        int u0 = (idx - hw > 0) ? (idx - hw) : 0;
        int u1 = (idx + hw < lut_len - 1) ? (idx + hw) : (lut_len - 1);
        while (u1 > u0 + 2 && lut[u1] == lut[u1 - 1]) {
            --u1;
        }
        if (u1 - u0 < hw && u1 > 2 * hw) {
            u0 = (u1 - 2 * hw > 0) ? (u1 - 2 * hw) : 0;
        }
        const int du = (u1 - u0 > 1) ? (u1 - u0) : 1;
        const double slope = static_cast<double>(lut[u1] - lut[u0]) / static_cast<double>(du);
        out_dadu_du_raw[j] = static_cast<float>((slope > 1e-4) ? slope : 1e-4);
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
