// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_SIMD_PLATFORM_H
#define NIJINWVDC_SIMD_PLATFORM_H

#include "../include/nijinWVDC_export.h"

#include <cstdint>
#include <cstring>

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

inline uint64_t rd_be64_fast(const uint8_t* p) {
#if defined(__GNUC__) || defined(__clang__)
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return __builtin_bswap64(v);
#elif defined(_MSC_VER)
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return _byteswap_uint64(v);
#else
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
#endif
}

inline int32_t clz64_nz(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(v);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long idx = 0;
    _BitScanReverse64(&idx, v);
    return static_cast<int32_t>(63u - idx);
#else
    int32_t n = 0;
    while ((v >> 63) == 0ULL) {
        v <<= 1;
        ++n;
    }
    return n;
#endif
}

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
