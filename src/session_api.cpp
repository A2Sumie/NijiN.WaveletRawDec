// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC.h"
#include "codec_internal.h"
#include "simd_platform.h"
#include "tiff_internal.h"
#include "worker_pool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nijinwvdc {
namespace {

inline void put_le16_u8(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void put_le32_u8(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

const NevClipInfo& empty_clip_info() {
    static const NevClipInfo kEmpty;
    return kEmpty;
}

inline uint32_t adjust_drop_frame_smpte(uint32_t fn, uint32_t nf) {
    if (nf != 30u && nf != 60u) {
        return fn;
    }
    const uint32_t drop = (nf == 60u) ? 4u : 2u;
    const uint32_t f10m = nf * 600u - 9u * drop;
    const uint32_t f1m = nf * 60u - drop;
    const uint32_t d10 = fn / f10m;
    const uint32_t rem = fn % f10m;
    const uint32_t m_extra = (rem >= nf * 60u) ? ((rem - drop) / f1m) : 0u;
    return fn + drop * (9u * d10 + m_extra);
}

#if NIJIN_X86_64
NIJIN_NOINLINE NIJIN_TARGET_AVX2 void split_rowpair_avx2(
    const uint16_t* r0,
    const uint16_t* r1,
    uint32_t half_w,
    uint16_t* d00,
    uint16_t* d01,
    uint16_t* d10,
    uint16_t* d11) {
    uint32_t q = 0;
    for (; q + 16u <= half_w; q += 16u) {
        const __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + 2u * q));
        const __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + 2u * q + 16u));
        const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + 2u * q));
        const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + 2u * q + 16u));

        const __m256i ev_a = _mm256_permute4x64_epi64(
            _mm256_packs_epi32(
                _mm256_srai_epi32(_mm256_slli_epi32(a0, 16), 16),
                _mm256_srai_epi32(_mm256_slli_epi32(a1, 16), 16)),
            0xD8);
        const __m256i od_a = _mm256_permute4x64_epi64(
            _mm256_packs_epi32(_mm256_srai_epi32(a0, 16), _mm256_srai_epi32(a1, 16)),
            0xD8);

        const __m256i ev_b = _mm256_permute4x64_epi64(
            _mm256_packs_epi32(
                _mm256_srai_epi32(_mm256_slli_epi32(b0, 16), 16),
                _mm256_srai_epi32(_mm256_slli_epi32(b1, 16), 16)),
            0xD8);
        const __m256i od_b = _mm256_permute4x64_epi64(
            _mm256_packs_epi32(_mm256_srai_epi32(b0, 16), _mm256_srai_epi32(b1, 16)),
            0xD8);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d00 + q), ev_a);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d01 + q), od_a);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d10 + q), ev_b);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d11 + q), od_b);
    }
    for (; q < half_w; ++q) {
        d00[q] = r0[2u * q];
        d01[q] = r0[2u * q + 1u];
        d10[q] = r1[2u * q];
        d11[q] = r1[2u * q + 1u];
    }
}

void split_rowpair_sse2(
    const uint16_t* r0,
    const uint16_t* r1,
    uint32_t half_w,
    uint16_t* d00,
    uint16_t* d01,
    uint16_t* d10,
    uint16_t* d11) {
    uint32_t q = 0;
    for (; q + 8u <= half_w; q += 8u) {
        const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + 2u * q));
        const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + 2u * q + 8u));
        const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + 2u * q));
        const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + 2u * q + 8u));

        const __m128i ev_a = _mm_packs_epi32(
            _mm_srai_epi32(_mm_slli_epi32(a0, 16), 16),
            _mm_srai_epi32(_mm_slli_epi32(a1, 16), 16));
        const __m128i od_a = _mm_packs_epi32(_mm_srai_epi32(a0, 16), _mm_srai_epi32(a1, 16));

        const __m128i ev_b = _mm_packs_epi32(
            _mm_srai_epi32(_mm_slli_epi32(b0, 16), 16),
            _mm_srai_epi32(_mm_slli_epi32(b1, 16), 16));
        const __m128i od_b = _mm_packs_epi32(_mm_srai_epi32(b0, 16), _mm_srai_epi32(b1, 16));

        _mm_storeu_si128(reinterpret_cast<__m128i*>(d00 + q), ev_a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d01 + q), od_a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d10 + q), ev_b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d11 + q), od_b);
    }
    for (; q < half_w; ++q) {
        d00[q] = r0[2u * q];
        d01[q] = r0[2u * q + 1u];
        d10[q] = r1[2u * q];
        d11[q] = r1[2u * q + 1u];
    }
}

NIJIN_NOINLINE NIJIN_TARGET_AVX2 void normalize_row_avx2(
    const uint16_t* srow,
    float* drow,
    uint32_t width,
    float b0,
    float inv0,
    float b1,
    float inv1,
    bool clamp_01) {
    const __m256 vb = _mm256_set_ps(b1, b0, b1, b0, b1, b0, b1, b0);
    const __m256 vinv = _mm256_set_ps(inv1, inv0, inv1, inv0, inv1, inv0, inv1, inv0);
    const __m256 vzero = _mm256_setzero_ps();
    const __m256 vone = _mm256_set1_ps(1.0f);
    uint32_t x = 0;
    for (; x + 8u <= width; x += 8u) {
        const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srow + x));
        __m256 f = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(raw));
        f = _mm256_mul_ps(_mm256_sub_ps(f, vb), vinv);
        if (clamp_01) {
            f = _mm256_min_ps(_mm256_max_ps(f, vzero), vone);
        }
        _mm256_storeu_ps(drow + x, f);
    }
    for (; x < width; ++x) {
        const bool odd = (x & 1u) != 0u;
        float v = (static_cast<float>(srow[x]) - (odd ? b1 : b0)) * (odd ? inv1 : inv0);
        if (clamp_01) {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
        }
        drow[x] = v;
    }
}

void normalize_row_sse2(
    const uint16_t* srow,
    float* drow,
    uint32_t width,
    float b0,
    float inv0,
    float b1,
    float inv1,
    bool clamp_01) {
    const __m128 vb = _mm_set_ps(b1, b0, b1, b0);
    const __m128 vinv = _mm_set_ps(inv1, inv0, inv1, inv0);
    const __m128 vzero = _mm_setzero_ps();
    const __m128 vone = _mm_set1_ps(1.0f);
    const __m128i zero16 = _mm_setzero_si128();
    uint32_t x = 0;
    for (; x + 8u <= width; x += 8u) {
        const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srow + x));
        __m128 f0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(raw, zero16));
        __m128 f1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(raw, zero16));
        f0 = _mm_mul_ps(_mm_sub_ps(f0, vb), vinv);
        f1 = _mm_mul_ps(_mm_sub_ps(f1, vb), vinv);
        if (clamp_01) {
            f0 = _mm_min_ps(_mm_max_ps(f0, vzero), vone);
            f1 = _mm_min_ps(_mm_max_ps(f1, vzero), vone);
        }
        _mm_storeu_ps(drow + x, f0);
        _mm_storeu_ps(drow + x + 4u, f1);
    }
    for (; x < width; ++x) {
        const bool odd = (x & 1u) != 0u;
        float v = (static_cast<float>(srow[x]) - (odd ? b1 : b0)) * (odd ? inv1 : inv0);
        if (clamp_01) {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
        }
        drow[x] = v;
    }
}
#endif // NIJIN_X86_64

bool copy_bayer_roi_strided(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t src_w,
    uint32_t src_h,
    uint32_t x0,
    uint32_t y0,
    uint32_t cw,
    uint32_t ch,
    const uint8_t src_cfa[4],
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    uint8_t dst_cfa[4]) {
    if (!src || !dst || !src_cfa || !dst_cfa || cw == 0 || ch == 0 ||
        x0 + cw > src_w || y0 + ch > src_h) {
        return false;
    }
    const size_t min_src_stride = static_cast<size_t>(src_w) * sizeof(uint16_t);
    const size_t min_dst_stride = static_cast<size_t>(cw) * sizeof(uint16_t);
    const size_t s_stride = (src_row_stride_bytes == 0) ? min_src_stride : src_row_stride_bytes;
    const size_t d_stride = (dst_row_stride_bytes == 0) ? min_dst_stride : dst_row_stride_bytes;
    if (s_stride < min_src_stride || d_stride < min_dst_stride) {
        return false;
    }
    const uint8_t* sbase = reinterpret_cast<const uint8_t*>(src);
    uint8_t* dbase = reinterpret_cast<uint8_t*>(dst);
    const size_t x_off_bytes = static_cast<size_t>(x0) * sizeof(uint16_t);
    const size_t row_bytes = static_cast<size_t>(cw) * sizeof(uint16_t);
    for (uint32_t r = 0; r < ch; ++r) {
        std::memcpy(
            dbase + static_cast<size_t>(r) * d_stride,
            sbase + static_cast<size_t>(y0 + r) * s_stride + x_off_bytes,
            row_bytes);
    }
    const uint32_t dy = y0 & 1u;
    const uint32_t dx = x0 & 1u;
    for (uint32_t ry = 0; ry < 2; ++ry) {
        for (uint32_t rx = 0; rx < 2; ++rx) {
            dst_cfa[ry * 2 + rx] = src_cfa[((ry ^ dy) * 2) + (rx ^ dx)];
        }
    }
    return true;
}

bool probe_codestream_dims(
    const uint8_t* data,
    size_t len,
    CodecFamily& codec_out,
    uint32_t& coded_w,
    uint32_t& coded_h,
    uint16_t& bit_depth,
    bool& is_single_a000_tile,
    std::string& err) {
    codec_out = CodecFamily::Unknown;
    coded_w = 0;
    coded_h = 0;
    bit_depth = 14;
    is_single_a000_tile = false;
    if (!data || len < 8) {
        err = "Empty or truncated raw wavelet codestream";
        return false;
    }
    if (data[0] == 0xFF && data[1] == 0x10) {
        TicoRawInfo jxs;
        if (!probe_ticoraw_header(data, len, jxs, err)) {
            return false;
        }
        codec_out = CodecFamily::TicoRaw;
        coded_w = jxs.width;
        coded_h = jxs.height;
        bit_depth = (jxs.out_bitdepth > 0) ? static_cast<uint16_t>(jxs.out_bitdepth) : 14;
        return true;
    }
    if (len >= 16 && std::memcmp(data, "A000", 4) == 0) {
        Arw6Info arw;
        if (!probe_arw6_codestream(data, len, arw, err)) {
            return false;
        }
        codec_out = CodecFamily::Arw6;
        coded_w = arw.width;
        coded_h = arw.height;
        bit_depth = arw.bit_depth;
        is_single_a000_tile = true;
        return true;
    }
    if (len >= 0x68) {
        Arw6Info arw;
        if (!probe_arw6_strip(data, len, arw, err)) {
            return false;
        }
        codec_out = CodecFamily::Arw6;
        coded_w = arw.width;
        coded_h = arw.height;
        bit_depth = arw.bit_depth;
        is_single_a000_tile = false;
        return true;
    }
    err = "Unrecognized wavelet codestream header";
    return false;
}

} // namespace

const char* version_string() {
    return NIJINWVDC_VERSION_STRING;
}

uint32_t simd_capabilities() {
    return detail::detect_simd_capabilities();
}

bool split_bayer_cfa_planes(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    uint16_t* c00,
    uint16_t* c01,
    uint16_t* c10,
    uint16_t* c11,
    size_t plane_row_stride_bytes) {
    if (!src || !c00 || !c01 || !c10 || !c11 ||
        width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
        return false;
    }
    const uint32_t half_w = width / 2u;
    const uint32_t half_h = height / 2u;
    const size_t min_src_stride = static_cast<size_t>(width) * sizeof(uint16_t);
    const size_t min_plane_stride = static_cast<size_t>(half_w) * sizeof(uint16_t);
    const size_t s_stride = (src_row_stride_bytes == 0) ? min_src_stride : src_row_stride_bytes;
    const size_t p_stride = (plane_row_stride_bytes == 0) ? min_plane_stride : plane_row_stride_bytes;
    if (s_stride < min_src_stride || p_stride < min_plane_stride) {
        return false;
    }

    const uint8_t* sbase = reinterpret_cast<const uint8_t*>(src);
    uint8_t* p00 = reinterpret_cast<uint8_t*>(c00);
    uint8_t* p01 = reinterpret_cast<uint8_t*>(c01);
    uint8_t* p10 = reinterpret_cast<uint8_t*>(c10);
    uint8_t* p11 = reinterpret_cast<uint8_t*>(c11);

#if NIJIN_X86_64
    const bool use_avx2 = detail::cpu_has_avx2();
#endif

    for (uint32_t py = 0; py < half_h; ++py) {
        const uint16_t* r0 = reinterpret_cast<const uint16_t*>(sbase + static_cast<size_t>(2u * py) * s_stride);
        const uint16_t* r1 = reinterpret_cast<const uint16_t*>(sbase + static_cast<size_t>(2u * py + 1u) * s_stride);
        uint16_t* d00 = reinterpret_cast<uint16_t*>(p00 + static_cast<size_t>(py) * p_stride);
        uint16_t* d01 = reinterpret_cast<uint16_t*>(p01 + static_cast<size_t>(py) * p_stride);
        uint16_t* d10 = reinterpret_cast<uint16_t*>(p10 + static_cast<size_t>(py) * p_stride);
        uint16_t* d11 = reinterpret_cast<uint16_t*>(p11 + static_cast<size_t>(py) * p_stride);

#if NIJIN_ARM_NEON
        uint32_t q = 0;
        for (; q + 8u <= half_w; q += 8u) {
            const uint16x8x2_t v0 = vld2q_u16(r0 + 2u * q);
            const uint16x8x2_t v1 = vld2q_u16(r1 + 2u * q);
            vst1q_u16(d00 + q, v0.val[0]);
            vst1q_u16(d01 + q, v0.val[1]);
            vst1q_u16(d10 + q, v1.val[0]);
            vst1q_u16(d11 + q, v1.val[1]);
        }
        for (; q < half_w; ++q) {
            d00[q] = r0[2u * q];
            d01[q] = r0[2u * q + 1u];
            d10[q] = r1[2u * q];
            d11[q] = r1[2u * q + 1u];
        }
#elif NIJIN_X86_64
        if (use_avx2) {
            split_rowpair_avx2(r0, r1, half_w, d00, d01, d10, d11);
        } else {
            split_rowpair_sse2(r0, r1, half_w, d00, d01, d10, d11);
        }
#else
        for (uint32_t q = 0; q < half_w; ++q) {
            d00[q] = r0[2u * q];
            d01[q] = r0[2u * q + 1u];
            d10[q] = r1[2u * q];
            d11[q] = r1[2u * q + 1u];
        }
#endif
    }
    return true;
}

bool normalize_bayer_to_float(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    const float black[4],
    float white_level,
    bool clamp_01,
    float* dst,
    size_t dst_row_stride_bytes) {
    if (!src || !black || !dst || width == 0 || height == 0) {
        return false;
    }
    float inv[4] = {};
    for (int p = 0; p < 4; ++p) {
        const float denom = white_level - black[p];
        if (denom <= 0.0f) {
            return false;
        }
        inv[p] = 1.0f / denom;
    }
    const size_t min_src_stride = static_cast<size_t>(width) * sizeof(uint16_t);
    const size_t min_dst_stride = static_cast<size_t>(width) * sizeof(float);
    const size_t s_stride = (src_row_stride_bytes == 0) ? min_src_stride : src_row_stride_bytes;
    const size_t d_stride = (dst_row_stride_bytes == 0) ? min_dst_stride : dst_row_stride_bytes;
    if (s_stride < min_src_stride || d_stride < min_dst_stride) {
        return false;
    }

    const uint8_t* sbase = reinterpret_cast<const uint8_t*>(src);
    uint8_t* dbase = reinterpret_cast<uint8_t*>(dst);

#if NIJIN_X86_64
    const bool use_avx2 = detail::cpu_has_avx2();
#endif

    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t* srow = reinterpret_cast<const uint16_t*>(sbase + static_cast<size_t>(y) * s_stride);
        float* drow = reinterpret_cast<float*>(dbase + static_cast<size_t>(y) * d_stride);
        const uint32_t rp = (y & 1u) * 2u;
        const float b0 = black[rp];
        const float inv0 = inv[rp];
        const float b1 = black[rp + 1u];
        const float inv1 = inv[rp + 1u];

#if NIJIN_ARM_NEON
        const float32x4_t vb = {b0, b1, b0, b1};
        const float32x4_t vinv = {inv0, inv1, inv0, inv1};
        const float32x4_t vzero = vdupq_n_f32(0.0f);
        const float32x4_t vone = vdupq_n_f32(1.0f);
        uint32_t x = 0;
        for (; x + 8u <= width; x += 8u) {
            const uint16x8_t u = vld1q_u16(srow + x);
            float32x4_t f0 = vmulq_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(u))), vb), vinv);
            float32x4_t f1 = vmulq_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(u))), vb), vinv);
            if (clamp_01) {
                f0 = vminq_f32(vmaxq_f32(f0, vzero), vone);
                f1 = vminq_f32(vmaxq_f32(f1, vzero), vone);
            }
            vst1q_f32(drow + x, f0);
            vst1q_f32(drow + x + 4u, f1);
        }
        for (; x < width; ++x) {
            const bool odd = (x & 1u) != 0u;
            float v = (static_cast<float>(srow[x]) - (odd ? b1 : b0)) * (odd ? inv1 : inv0);
            if (clamp_01) {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
            }
            drow[x] = v;
        }
#elif NIJIN_X86_64
        if (use_avx2) {
            normalize_row_avx2(srow, drow, width, b0, inv0, b1, inv1, clamp_01);
        } else {
            normalize_row_sse2(srow, drow, width, b0, inv0, b1, inv1, clamp_01);
        }
#else
        for (uint32_t x = 0; x < width; ++x) {
            const bool odd = (x & 1u) != 0u;
            float v = (static_cast<float>(srow[x]) - (odd ? b1 : b0)) * (odd ? inv1 : inv0);
            if (clamp_01) {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
            }
            drow[x] = v;
        }
#endif
    }
    return true;
}

struct MediaReader::Impl {
    detail::RandomAccessReader reader;
    NevClipInfo clip;
    std::vector<uint64_t> audio_chunk_frame_prefix;

    void finalize_open() {
        audio_chunk_frame_prefix.clear();
        audio_chunk_frame_prefix.reserve(clip.audio_chunks.size() + 1u);
        uint64_t acc = 0;
        audio_chunk_frame_prefix.push_back(0);
        for (const auto& c : clip.audio_chunks) {
            acc += c.sample_count;
            audio_chunk_frame_prefix.push_back(acc);
        }
    }

    void reset() {
        reader.close();
        clip = NevClipInfo();
        audio_chunk_frame_prefix.clear();
    }
};

MediaReader::MediaReader() : impl_(new Impl()) {}
MediaReader::~MediaReader() = default;
MediaReader::MediaReader(MediaReader&&) noexcept = default;
MediaReader& MediaReader::operator=(MediaReader&&) noexcept = default;

bool MediaReader::open_file(const std::string& path, std::string& err) {
    if (!impl_) impl_.reset(new Impl());
    impl_->reset();
    if (!impl_->reader.open_file(path, err)) {
        return false;
    }
    if (!detail::parse_nev_clip_reader(impl_->reader, path, impl_->clip, err)) {
        impl_->reset();
        return false;
    }
    impl_->finalize_open();
    return true;
}

bool MediaReader::open_memory(const uint8_t* data, size_t size, std::string& err) {
    if (!impl_) impl_.reset(new Impl());
    impl_->reset();
    if (!impl_->reader.open_memory(data, size, err)) {
        return false;
    }
    if (!detail::parse_nev_clip_reader(impl_->reader, "<memory>", impl_->clip, err)) {
        impl_->reset();
        return false;
    }
    impl_->finalize_open();
    return true;
}

bool MediaReader::open_callbacks(const IoCallbacks& callbacks, std::string& err) {
    if (!impl_) impl_.reset(new Impl());
    impl_->reset();
    if (!impl_->reader.open_callbacks(callbacks, err)) {
        return false;
    }
    if (!detail::parse_nev_clip_reader(impl_->reader, "<callbacks>", impl_->clip, err)) {
        impl_->reset();
        return false;
    }
    impl_->finalize_open();
    return true;
}

void MediaReader::close() {
    if (impl_) {
        impl_->reset();
    }
}

bool MediaReader::is_open() const {
    return impl_ && impl_->reader.is_open() && !impl_->clip.video_samples.empty();
}

ContainerFormat MediaReader::container_format() const {
    return impl_ ? impl_->clip.container_format : ContainerFormat::Unknown;
}

CodecFamily MediaReader::codec_family() const {
    return impl_ ? impl_->clip.codec_family : CodecFamily::Unknown;
}

const NevClipInfo& MediaReader::clip_info() const {
    return impl_ ? impl_->clip : empty_clip_info();
}

uint32_t MediaReader::frame_count() const {
    return impl_ ? static_cast<uint32_t>(impl_->clip.video_samples.size()) : 0u;
}

bool MediaReader::read_frame_packet(
    uint32_t frame_index,
    NevFrameMeta& meta,
    std::vector<uint8_t>* codestream_out,
    std::string& err) const {
    if (!is_open()) {
        err = "MediaReader is not open";
        return false;
    }
    return detail::read_nev_frame_meta_and_codestream_reader(
        impl_->reader, impl_->clip, frame_index, meta, codestream_out, err);
}

bool MediaReader::get_frame_timing(
    uint32_t frame_index,
    VideoFrameTiming& timing_out,
    std::string& err) const {
    timing_out = VideoFrameTiming();
    if (!is_open()) {
        err = "MediaReader is not open";
        return false;
    }
    const auto& c = impl_->clip;
    if (frame_index >= c.video_samples.size()) {
        err = "Frame index out of range";
        return false;
    }
    timing_out.frame_index = frame_index;
    uint32_t ts = 0;
    if (c.video_track_index >= 0 &&
        static_cast<size_t>(c.video_track_index) < c.tracks.size()) {
        ts = c.tracks[static_cast<size_t>(c.video_track_index)].timescale;
    }
    timing_out.timescale = ts;

    uint64_t pts = 0;
    uint32_t dur = 0;
    if (!c.video_stts.empty()) {
        uint32_t rem = frame_index;
        for (size_t i = 0; i < c.video_stts.size(); ++i) {
            const uint32_t sc = c.video_stts[i].sample_count;
            const uint32_t sd = c.video_stts[i].sample_delta;
            if (rem < sc) {
                pts += static_cast<uint64_t>(rem) * sd;
                dur = sd;
                break;
            }
            pts += static_cast<uint64_t>(sc) * sd;
            rem -= sc;
        }
    } else {
        pts = frame_index;
    }
    timing_out.pts_ticks = pts;
    timing_out.duration_ticks = dur;
    if (ts > 0) {
        timing_out.pts_seconds = static_cast<double>(pts) / static_cast<double>(ts);
        timing_out.duration_seconds = static_cast<double>(dur) / static_cast<double>(ts);
    } else if (c.fps > 0.0) {
        timing_out.pts_seconds = static_cast<double>(frame_index) / c.fps;
        timing_out.duration_seconds = 1.0 / c.fps;
    }

    if (c.timecode_track_index >= 0 &&
        static_cast<size_t>(c.timecode_track_index) < c.tracks.size() &&
        !c.timecode_samples.empty()) {
        const auto& tm = c.tracks[static_cast<size_t>(c.timecode_track_index)];
        const uint32_t nf = (tm.tmcd_num_frames > 0u) ? tm.tmcd_num_frames : 30u;
        const bool df = (tm.tmcd_flags & 1u) != 0u;
        const uint32_t fn = (frame_index < c.timecode_samples.size())
                                ? c.timecode_samples[frame_index]
                                : (c.timecode_samples[0] + frame_index);
        const uint32_t fn_adj = df ? adjust_drop_frame_smpte(fn, nf) : fn;
        const uint32_t ff = fn_adj % nf;
        const uint32_t total_sec = fn_adj / nf;
        const uint32_t ss = total_sec % 60u;
        const uint32_t mm = (total_sec / 60u) % 60u;
        const uint32_t hh = (total_sec / 3600u) % 24u;

        timing_out.has_timecode = true;
        timing_out.drop_frame = df;
        timing_out.tc_hours = static_cast<uint8_t>(hh);
        timing_out.tc_minutes = static_cast<uint8_t>(mm);
        timing_out.tc_seconds = static_cast<uint8_t>(ss);
        timing_out.tc_frames = static_cast<uint8_t>(ff);
        std::snprintf(
            timing_out.timecode_str,
            sizeof(timing_out.timecode_str),
            "%02u:%02u:%02u%c%02u",
            static_cast<unsigned>(hh % 100u),
            static_cast<unsigned>(mm % 100u),
            static_cast<unsigned>(ss % 100u),
            df ? ';' : ':',
            static_cast<unsigned>(ff % 100u));
    }
    return true;
}

AudioFormatInfo MediaReader::audio_info() const {
    AudioFormatInfo out;
    if (!impl_ || !is_open()) {
        return out;
    }
    const auto& c = impl_->clip;
    if (c.audio_track_index < 0 ||
        static_cast<size_t>(c.audio_track_index) >= c.tracks.size() ||
        c.total_audio_frames == 0) {
        return out;
    }
    const auto& tr = c.tracks[static_cast<size_t>(c.audio_track_index)];
    out.available = true;
    out.format = tr.format;
    out.timescale = tr.timescale;
    out.sample_rate = tr.audio_sample_rate;
    out.channels = tr.audio_channels;
    out.bits_per_channel = tr.audio_bits_per_channel;
    out.bytes_per_packet = tr.audio_bytes_per_packet;
    out.lpcm_flags = tr.audio_lpcm_flags;
    out.is_float = (tr.audio_lpcm_flags & 1u) != 0u;
    out.is_big_endian = (tr.audio_lpcm_flags & 2u) != 0u;
    out.is_signed = (tr.audio_lpcm_flags & 4u) != 0u || out.is_float;
    out.total_frames = c.total_audio_frames;
    return out;
}

bool MediaReader::read_audio_pcm_into(
    uint64_t start_frame,
    uint64_t req_frames,
    void* dst,
    size_t dst_bytes,
    uint64_t* frames_read_out,
    std::string& err) const {
    if (frames_read_out) *frames_read_out = 0;
    if (!is_open()) {
        err = "MediaReader is not open";
        return false;
    }
    const AudioFormatInfo ainfo = audio_info();
    if (!ainfo.available || ainfo.bytes_per_packet == 0) {
        err = "Clip does not contain an audio track";
        return false;
    }
    if (start_frame >= ainfo.total_frames || req_frames == 0) {
        return true;
    }
    const uint64_t to_read = std::min<uint64_t>(req_frames, ainfo.total_frames - start_frame);
    const uint64_t needed_bytes = to_read * static_cast<uint64_t>(ainfo.bytes_per_packet);
    if (!dst || dst_bytes < needed_bytes) {
        err = "Audio destination buffer too small";
        return false;
    }

    const auto& chunks = impl_->clip.audio_chunks;
    const auto& pref = impl_->audio_chunk_frame_prefix;
    auto it = std::upper_bound(pref.begin(), pref.end(), start_frame);
    size_t ci = static_cast<size_t>(std::distance(pref.begin(), it));
    if (ci == 0) {
        err = "Internal audio chunk index error";
        return false;
    }
    --ci;

    uint8_t* out_p = static_cast<uint8_t*>(dst);
    uint64_t rem = to_read;
    uint64_t cur_frame = start_frame;
    while (rem > 0 && ci < chunks.size()) {
        const uint64_t chunk_start = pref[ci];
        const uint64_t in_chunk_off_frames = cur_frame - chunk_start;
        const uint64_t chunk_avail = static_cast<uint64_t>(chunks[ci].sample_count) - in_chunk_off_frames;
        const uint64_t take = std::min<uint64_t>(rem, chunk_avail);
        const uint64_t byte_off =
            chunks[ci].offset + in_chunk_off_frames * static_cast<uint64_t>(ainfo.bytes_per_packet);
        const size_t nbytes = static_cast<size_t>(take * static_cast<uint64_t>(ainfo.bytes_per_packet));
        if (!impl_->reader.read(byte_off, out_p, nbytes)) {
            err = "Failed reading audio chunk bytes";
            return false;
        }
        out_p += nbytes;
        cur_frame += take;
        rem -= take;
        ++ci;
    }
    if (frames_read_out) *frames_read_out = to_read;
    return true;
}

bool MediaReader::read_audio_pcm(
    uint64_t start_frame,
    uint64_t req_frames,
    std::vector<uint8_t>& pcm_out,
    std::string& err) const {
    pcm_out.clear();
    if (!is_open()) {
        err = "MediaReader is not open";
        return false;
    }
    const AudioFormatInfo ainfo = audio_info();
    if (!ainfo.available || ainfo.bytes_per_packet == 0) {
        err = "Clip does not contain an audio track";
        return false;
    }
    if (start_frame >= ainfo.total_frames || req_frames == 0) {
        return true;
    }
    const uint64_t to_read = std::min<uint64_t>(req_frames, ainfo.total_frames - start_frame);
    const uint64_t needed_bytes64 = to_read * static_cast<uint64_t>(ainfo.bytes_per_packet);
    if (needed_bytes64 > static_cast<uint64_t>(SIZE_MAX)) {
        err = "Requested audio range exceeds addressable memory";
        return false;
    }
    pcm_out.resize(static_cast<size_t>(needed_bytes64));
    uint64_t got_frames = 0;
    return read_audio_pcm_into(start_frame, to_read, pcm_out.data(), pcm_out.size(), &got_frames, err);
}

bool MediaReader::export_audio_wav(const std::string& wav_path, std::string& err) const {
    if (!is_open()) {
        err = "MediaReader is not open";
        return false;
    }
    const AudioFormatInfo ainfo = audio_info();
    if (!ainfo.available) {
        err = "Clip does not contain an exportable audio track";
        return false;
    }
    if (ainfo.channels == 0 ||
        (ainfo.bits_per_channel != 8u && ainfo.bits_per_channel != 16u &&
         ainfo.bits_per_channel != 24u && ainfo.bits_per_channel != 32u) ||
        ainfo.sample_rate <= 0.0) {
        err = "Unsupported audio PCM parameters for WAV export";
        return false;
    }
    const uint32_t sample_bytes = ainfo.bits_per_channel / 8u;
    const uint32_t frame_bytes = ainfo.channels * sample_bytes;
    if (frame_bytes != ainfo.bytes_per_packet) {
        err = "Non-packed LPCM stride cannot be written directly as standard RIFF WAV";
        return false;
    }
    const uint64_t data_bytes64 = ainfo.total_frames * static_cast<uint64_t>(frame_bytes);
    if (data_bytes64 > 0xFFFFFF00ULL - 44ULL) {
        err = "Audio stream exceeds 4 GiB RIFF WAV limit";
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(data_bytes64);
    const uint32_t sr = static_cast<uint32_t>(std::llround(ainfo.sample_rate));

    FILE* fp = detail::open_c_file_write(wav_path);
    if (!fp) {
        err = "Failed opening WAV output for writing: " + wav_path;
        return false;
    }

    uint8_t hdr[44] = {};
    std::memcpy(hdr + 0, "RIFF", 4);
    put_le32_u8(hdr + 4, 36u + data_bytes);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put_le32_u8(hdr + 16, 16u);
    put_le16_u8(hdr + 20, ainfo.is_float ? 3u : 1u);
    put_le16_u8(hdr + 22, static_cast<uint16_t>(ainfo.channels));
    put_le32_u8(hdr + 24, sr);
    put_le32_u8(hdr + 28, sr * frame_bytes);
    put_le16_u8(hdr + 32, static_cast<uint16_t>(frame_bytes));
    put_le16_u8(hdr + 34, static_cast<uint16_t>(ainfo.bits_per_channel));
    std::memcpy(hdr + 36, "data", 4);
    put_le32_u8(hdr + 40, data_bytes);

    bool ok = (std::fwrite(hdr, 1, sizeof(hdr), fp) == sizeof(hdr));
    const uint64_t kChunkFrames = 65536ULL;
    std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(kChunkFrames, ainfo.total_frames)) * frame_bytes);
    uint64_t pos = 0;
    while (ok && pos < ainfo.total_frames) {
        const uint64_t n = std::min<uint64_t>(kChunkFrames, ainfo.total_frames - pos);
        uint64_t got = 0;
        if (!read_audio_pcm_into(pos, n, buf.data(), buf.size(), &got, err) || got != n) {
            ok = false;
            break;
        }
        const size_t nbytes = static_cast<size_t>(n) * frame_bytes;
        if (ainfo.is_big_endian && sample_bytes > 1u) {
            for (size_t i = 0; i < nbytes; i += sample_bytes) {
                if (sample_bytes == 2u) {
                    std::swap(buf[i], buf[i + 1]);
                } else if (sample_bytes == 3u) {
                    std::swap(buf[i], buf[i + 2]);
                } else if (sample_bytes == 4u) {
                    std::swap(buf[i], buf[i + 3]);
                    std::swap(buf[i + 1], buf[i + 2]);
                }
            }
        }
        if (std::fwrite(buf.data(), 1, nbytes, fp) != nbytes) {
            err = "Write failure while writing WAV audio";
            ok = false;
        }
        pos += n;
    }
    if (std::fclose(fp) != 0) ok = false;
    if (!ok && err.empty()) {
        err = "Failed writing WAV output: " + wav_path;
    }
    return ok;
}

bool MediaReader::extract_preview_jpeg(std::vector<uint8_t>& jpeg_out, std::string& err) const {
    if (!is_open()) {
        err = "MediaReader is not open";
        return false;
    }
    const auto& c = impl_->clip;
    uint64_t off = 0;
    uint32_t sz = 0;
    if (c.ncm1_size >= sz && c.ncm1_size > 0) {
        off = c.ncm1_offset;
        sz = c.ncm1_size;
    }
    if (c.ncvw_size > sz) {
        off = c.ncvw_offset;
        sz = c.ncvw_size;
    }
    if (sz == 0 && c.ncth_size > 0) {
        off = c.ncth_offset;
        sz = c.ncth_size;
    }
    if (sz == 0) {
        err = "Clip does not contain an embedded JPEG preview";
        return false;
    }
    return detail::extract_blob_reader(impl_->reader, off, sz, jpeg_out, err);
}

struct DecoderSession::Impl {
    int default_threads = 0;
    detail::WorkerPool pool;
    detail::TicoArena* tico_arena = nullptr;
    detail::Arw6Arena* arw6_arena = nullptr;
    std::vector<uint8_t> packet_buf;
    std::vector<uint16_t> scratch_pixels;

    explicit Impl(int threads)
        : default_threads(threads),
          tico_arena(detail::create_tico_arena()),
          arw6_arena(detail::create_arw6_arena()) {}

    ~Impl() {
        detail::destroy_tico_arena(tico_arena);
        detail::destroy_arw6_arena(arw6_arena);
    }

    bool decode_codestream_core(
        const uint8_t* data,
        size_t len,
        const DecodeOptions& opts,
        const uint8_t* meta_cfa_or_null,
        const NevFrameMeta* opt_meta,
        uint16_t* dst,
        size_t dst_row_stride_bytes,
        size_t dst_height_capacity,
        DecodedFrameInfo& info_out,
        std::string& err) {
        info_out = DecodedFrameInfo();
        CodecFamily codec = CodecFamily::Unknown;
        uint32_t coded_w = 0, coded_h = 0;
        uint16_t bd = 14;
        bool single_a000 = false;
        if (!probe_codestream_dims(data, len, codec, coded_w, coded_h, bd, single_a000, err)) {
            return false;
        }

        uint8_t active_cfa[4] = {0, 1, 1, 2};
        if (meta_cfa_or_null) {
            std::memcpy(active_cfa, meta_cfa_or_null, 4);
        }

        bool use_crop = false;
        uint32_t crop_x = 0, crop_y = 0, crop_w = coded_w, crop_h = coded_h;
        if (opts.has_roi) {
            crop_x = opts.roi[0];
            crop_y = opts.roi[1];
            crop_w = opts.roi[2];
            crop_h = opts.roi[3];
            if (crop_w == 0 || crop_h == 0 || crop_x + crop_w > coded_w || crop_y + crop_h > coded_h) {
                err = "Requested ROI exceeds coded sensor dimensions";
                return false;
            }
            use_crop = true;
        } else if (opts.apply_active_crop && opt_meta && opt_meta->has_crop &&
                   opt_meta->crop[2] > 0 && opt_meta->crop[3] > 0) {
            crop_x = opt_meta->crop[0];
            crop_y = opt_meta->crop[1];
            crop_w = opt_meta->crop[2];
            crop_h = opt_meta->crop[3];
            if (crop_x + crop_w <= coded_w && crop_y + crop_h <= coded_h) {
                use_crop = true;
            }
        }

        const size_t min_dst_stride = static_cast<size_t>(crop_w) * sizeof(uint16_t);
        const size_t actual_stride = (dst_row_stride_bytes == 0) ? min_dst_stride : dst_row_stride_bytes;
        if (!dst || actual_stride < min_dst_stride || dst_height_capacity < crop_h) {
            err = "Destination Bayer buffer too small for output dimensions";
            return false;
        }

        const int req_threads = (opts.num_threads > 0) ? opts.num_threads : default_threads;
        const int eff_threads = detail::resolve_worker_count(req_threads);
        detail::WorkerPool* ext_pool = (eff_threads > 1) ? &pool : nullptr;

        const bool direct_to_dst = !use_crop && ((actual_stride & 1u) == 0);
        uint16_t* dec_dst = dst;
        size_t dec_stride_u16 = actual_stride / 2u;
        size_t dec_h_cap = dst_height_capacity;
        if (!direct_to_dst) {
            scratch_pixels.resize(static_cast<size_t>(coded_w) * static_cast<size_t>(coded_h));
            dec_dst = scratch_pixels.data();
            dec_stride_u16 = coded_w;
            dec_h_cap = coded_h;
        }

        if (codec == CodecFamily::TicoRaw) {
            TicoRawInfo jxs;
            if (!detail::decode_ticoraw_bayer_strided(
                    data, len, eff_threads, dec_dst, dec_stride_u16, dec_h_cap,
                    jxs, err, ext_pool, tico_arena)) {
                return false;
            }
        } else if (single_a000) {
            Arw6Info arw;
            if (!detail::decode_arw6_tile_strided(
                    data, len, eff_threads, 0, 0, coded_w, coded_h,
                    dec_dst, dec_stride_u16, dec_h_cap,
                    arw, err, ext_pool, arw6_arena)) {
                return false;
            }
        } else {
            Arw6Info arw;
            if (!detail::decode_arw6_bayer_strided(
                    data, len, eff_threads, dec_dst, dec_stride_u16, dec_h_cap,
                    arw, err, ext_pool, arw6_arena)) {
                return false;
            }
        }

        uint8_t out_cfa[4] = {0, 1, 1, 2};
        if (!direct_to_dst) {
            if (!copy_bayer_roi_strided(
                    dec_dst,
                    static_cast<size_t>(coded_w) * sizeof(uint16_t),
                    coded_w,
                    coded_h,
                    crop_x,
                    crop_y,
                    crop_w,
                    crop_h,
                    active_cfa,
                    dst,
                    actual_stride,
                    out_cfa)) {
                err = "Failed extracting ROI into caller buffer";
                return false;
            }
        } else {
            std::memcpy(out_cfa, active_cfa, 4);
        }

        info_out.codec = codec;
        info_out.coded_width = coded_w;
        info_out.coded_height = coded_h;
        info_out.output_width = crop_w;
        info_out.output_height = crop_h;
        info_out.offset_x = crop_x;
        info_out.offset_y = crop_y;
        info_out.bit_depth = bd;
        std::memcpy(info_out.cfa_pattern, out_cfa, 4);
        return true;
    }
};

DecoderSession::DecoderSession(int num_threads) : impl_(new Impl(num_threads)) {}
DecoderSession::~DecoderSession() = default;
DecoderSession::DecoderSession(DecoderSession&&) noexcept = default;
DecoderSession& DecoderSession::operator=(DecoderSession&&) noexcept = default;

void DecoderSession::set_num_threads(int num_threads) {
    if (impl_) {
        impl_->default_threads = num_threads;
    }
}

int DecoderSession::num_threads() const {
    return impl_ ? impl_->default_threads : 0;
}

bool DecoderSession::decode_codestream_into(
    const uint8_t* data,
    size_t len,
    const DecodeOptions& opts,
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    size_t dst_height_capacity,
    DecodedFrameInfo& info_out,
    std::string& err) {
    if (!impl_) impl_.reset(new Impl(0));
    return impl_->decode_codestream_core(
        data, len, opts, nullptr, nullptr, dst, dst_row_stride_bytes, dst_height_capacity, info_out, err);
}

bool DecoderSession::decode_codestream(
    const uint8_t* data,
    size_t len,
    const DecodeOptions& opts,
    std::vector<uint16_t>& bayer_out,
    DecodedFrameInfo& info_out,
    std::string& err) {
    CodecFamily codec = CodecFamily::Unknown;
    uint32_t coded_w = 0, coded_h = 0;
    uint16_t bd = 14;
    bool single_a000 = false;
    if (!probe_codestream_dims(data, len, codec, coded_w, coded_h, bd, single_a000, err)) {
        return false;
    }
    const uint32_t out_w = opts.has_roi ? opts.roi[2] : coded_w;
    const uint32_t out_h = opts.has_roi ? opts.roi[3] : coded_h;
    if (out_w == 0 || out_h == 0) {
        err = "Invalid output dimensions";
        return false;
    }
    bayer_out.resize(static_cast<size_t>(out_w) * static_cast<size_t>(out_h));
    return decode_codestream_into(
        data, len, opts, bayer_out.data(),
        static_cast<size_t>(out_w) * sizeof(uint16_t), out_h, info_out, err);
}

bool DecoderSession::decode_frame_into(
    const MediaReader& reader,
    uint32_t frame_index,
    const DecodeOptions& opts,
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    size_t dst_height_capacity,
    DecodedFrameInfo& info_out,
    std::string& err) {
    if (!impl_) impl_.reset(new Impl(0));
    NevFrameMeta meta;
    if (!reader.read_frame_packet(frame_index, meta, &impl_->packet_buf, err)) {
        return false;
    }
    if (!impl_->decode_codestream_core(
            impl_->packet_buf.data(),
            impl_->packet_buf.size(),
            opts,
            meta.cfa_pattern,
            &meta,
            dst,
            dst_row_stride_bytes,
            dst_height_capacity,
            info_out,
            err)) {
        return false;
    }
    info_out.has_meta = true;
    info_out.meta = meta;
    std::string t_err;
    if (reader.get_frame_timing(frame_index, info_out.timing, t_err)) {
        info_out.has_timing = true;
    }
    return true;
}

bool DecoderSession::decode_frame(
    const MediaReader& reader,
    uint32_t frame_index,
    const DecodeOptions& opts,
    std::vector<uint16_t>& bayer_out,
    DecodedFrameInfo& info_out,
    std::string& err) {
    if (!impl_) impl_.reset(new Impl(0));
    NevFrameMeta meta;
    if (!reader.read_frame_packet(frame_index, meta, &impl_->packet_buf, err)) {
        return false;
    }
    CodecFamily codec = CodecFamily::Unknown;
    uint32_t coded_w = 0, coded_h = 0;
    uint16_t bd = 14;
    bool single_a000 = false;
    if (!probe_codestream_dims(
            impl_->packet_buf.data(), impl_->packet_buf.size(),
            codec, coded_w, coded_h, bd, single_a000, err)) {
        return false;
    }
    uint32_t out_w = coded_w;
    uint32_t out_h = coded_h;
    if (opts.has_roi) {
        out_w = opts.roi[2];
        out_h = opts.roi[3];
    } else if (opts.apply_active_crop && meta.has_crop &&
               meta.crop[2] > 0 && meta.crop[3] > 0 &&
               static_cast<uint32_t>(meta.crop[0]) + meta.crop[2] <= coded_w &&
               static_cast<uint32_t>(meta.crop[1]) + meta.crop[3] <= coded_h) {
        out_w = meta.crop[2];
        out_h = meta.crop[3];
    }
    if (out_w == 0 || out_h == 0) {
        err = "Invalid output dimensions";
        return false;
    }
    bayer_out.resize(static_cast<size_t>(out_w) * static_cast<size_t>(out_h));
    if (!impl_->decode_codestream_core(
            impl_->packet_buf.data(),
            impl_->packet_buf.size(),
            opts,
            meta.cfa_pattern,
            &meta,
            bayer_out.data(),
            static_cast<size_t>(out_w) * sizeof(uint16_t),
            out_h,
            info_out,
            err)) {
        return false;
    }
    info_out.has_meta = true;
    info_out.meta = meta;
    std::string t_err;
    if (reader.get_frame_timing(frame_index, info_out.timing, t_err)) {
        info_out.has_timing = true;
    }
    return true;
}

} // namespace nijinwvdc
