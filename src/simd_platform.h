// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_SIMD_PLATFORM_H
#define NIJINWVDC_SIMD_PLATFORM_H

#include "../include/nijinWVDC_export.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#define NIJIN_ARM_NEON 1
#if defined(_MSC_VER) && defined(_M_ARM64)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#else
#define NIJIN_ARM_NEON 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define NIJIN_X86_64 1
#include <immintrin.h>
#if !defined(_MSC_VER)
#include <cpuid.h>
#endif
#else
#define NIJIN_X86_64 0
#endif

#if defined(_MSC_VER)
#include <cstdlib>
#include <intrin.h>
#endif

#if (defined(__GNUC__) || defined(__clang__)) && NIJIN_X86_64
#define NIJIN_TARGET_AVX2 __attribute__((target("avx2")))
#define NIJIN_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define NIJIN_TARGET_AVX2
#define NIJIN_NOINLINE __declspec(noinline)
#else
#define NIJIN_TARGET_AVX2
#define NIJIN_NOINLINE
#endif

namespace nijinwvdc {
namespace detail {

constexpr uint16_t bswap16(uint16_t v) noexcept {
    if (!std::is_constant_evaluated()) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap16(v);
#elif defined(_MSC_VER)
        return _byteswap_ushort(v);
#endif
    }
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

constexpr uint32_t bswap32(uint32_t v) noexcept {
    if (!std::is_constant_evaluated()) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap32(v);
#elif defined(_MSC_VER)
        return _byteswap_ulong(v);
#endif
    }
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

constexpr uint64_t bswap64(uint64_t v) noexcept {
    if (!std::is_constant_evaluated()) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap64(v);
#elif defined(_MSC_VER)
        return _byteswap_uint64(v);
#endif
    }
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) << 8) |
           ((v & 0x000000FF00000000ULL) >> 8) |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}

inline uint64_t rd_be64_fast(const uint8_t* p) noexcept {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    if constexpr (std::endian::native == std::endian::little) {
        return bswap64(v);
    } else {
        return v;
    }
}

constexpr int32_t clz64_nz(uint64_t v) noexcept {
    return static_cast<int32_t>(std::countl_zero(v));
}

// Lookup mapping [nzmask (0..15)][s_nib (0..15)] -> 4-bit negative lane mask.
struct SignExpandTable {
    uint8_t mask[16][16] = {};
};

consteval SignExpandTable make_sign_expand_table() noexcept {
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
}

inline constexpr SignExpandTable kSignExpandTable = make_sign_expand_table();

inline uint32_t detect_simd_capabilities() {
    uint32_t caps = NIJIN_SIMD_NONE;
#if NIJIN_ARM_NEON
    caps |= NIJIN_SIMD_ARM_NEON;
#endif
#if NIJIN_X86_64
    caps |= NIJIN_SIMD_X86_SSE2;
#if defined(_MSC_VER)
    int info1[4] = {0, 0, 0, 0};
    __cpuid(info1, 1);
    const unsigned int ecx1 = static_cast<unsigned int>(info1[2]);
    if ((ecx1 & (1u << 19)) != 0u) {
        caps |= NIJIN_SIMD_X86_SSE41;
    }
    const bool osxsave = (ecx1 & (1u << 27)) != 0u;
    const bool avx = (ecx1 & (1u << 28)) != 0u;
    if (osxsave && avx && ((_xgetbv(0) & 6ULL) == 6ULL)) {
        int max_leaf[4] = {0, 0, 0, 0};
        __cpuid(max_leaf, 0);
        if (max_leaf[0] >= 7) {
            int info7[4] = {0, 0, 0, 0};
            __cpuidex(info7, 7, 0);
            if ((static_cast<unsigned int>(info7[1]) & (1u << 5)) != 0u) {
                caps |= NIJIN_SIMD_X86_AVX2;
            }
        }
    }
#else
    unsigned int eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    if (__get_cpuid(1u, &eax, &ebx, &ecx, &edx)) {
        if ((ecx & (1u << 19)) != 0u) {
            caps |= NIJIN_SIMD_X86_SSE41;
        }
        const bool osxsave = (ecx & (1u << 27)) != 0u;
        const bool avx = (ecx & (1u << 28)) != 0u;
        if (osxsave && avx) {
            uint32_t xcr0_lo = 0u;
            uint32_t xcr0_hi = 0u;
            asm volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0u));
            (void)xcr0_hi;
            if ((xcr0_lo & 6u) == 6u &&
                __get_cpuid_count(7u, 0u, &eax, &ebx, &ecx, &edx)) {
                if ((ebx & (1u << 5)) != 0u) {
                    caps |= NIJIN_SIMD_X86_AVX2;
                }
            }
        }
    }
#endif
#endif
    return caps;
}

inline uint32_t cached_simd_capabilities() {
    static const uint32_t caps = detect_simd_capabilities();
    return caps;
}

inline bool cpu_has_avx2() {
    return (cached_simd_capabilities() & NIJIN_SIMD_X86_AVX2) != 0u;
}

inline bool cpu_has_sse41() {
    return (cached_simd_capabilities() & NIJIN_SIMD_X86_SSE41) != 0u;
}

} // namespace detail
} // namespace nijinwvdc

#endif // NIJINWVDC_SIMD_PLATFORM_H
