/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NIJINWVDC_EXPORT_H
#define NIJINWVDC_EXPORT_H

#define NIJINWVDC_VERSION_MAJOR 0
#define NIJINWVDC_VERSION_MINOR 3
#define NIJINWVDC_VERSION_PATCH 0
#define NIJINWVDC_VERSION_STRING "0.3.0"

#if defined(NIJINWVDC_STATIC)
#define NIJINWVDC_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef NIJINWVDC_BUILDING_DLL
#define NIJINWVDC_API __declspec(dllexport)
#else
#define NIJINWVDC_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define NIJINWVDC_API __attribute__((visibility("default")))
#else
#define NIJINWVDC_API
#endif

/* Bitmask returned by simd_capabilities() / nijin_simd_capabilities() */
#define NIJIN_SIMD_NONE      0u
#define NIJIN_SIMD_ARM_NEON  (1u << 0)
#define NIJIN_SIMD_X86_SSE2  (1u << 1)
#define NIJIN_SIMD_X86_SSE41 (1u << 2)
#define NIJIN_SIMD_X86_AVX2  (1u << 3)

#endif /* NIJINWVDC_EXPORT_H */
