#pragma once

// Selects the SIMD backend for the hand-vectorised mixing kernels.
// Include this ONLY from .cpp files of meridian_core: the fast-math guard below
// must fire for the translation units that implement compensated arithmetic.

#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
 #error "Meridian's compensated summing needs strict IEEE-754 semantics. Do not build meridian_core with -ffast-math or /fp:fast."
#endif

#if defined(__AVX__)
 #define MERIDIAN_SIMD_AVX 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #define MERIDIAN_SIMD_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
 #define MERIDIAN_SIMD_NEON 1
#endif

#if defined(MERIDIAN_SIMD_AVX) || defined(MERIDIAN_SIMD_SSE2) || defined(MERIDIAN_SIMD_NEON)
 #define MERIDIAN_SIMD_ENABLED 1
#endif
