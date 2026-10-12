#include "MixKernels.h"
#include "SimdConfig.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(MERIDIAN_SIMD_AVX)
 #include <immintrin.h>
#elif defined(MERIDIAN_SIMD_SSE2)
 #include <emmintrin.h>
#elif defined(MERIDIAN_SIMD_NEON)
 #include <arm_neon.h>
#endif

namespace meridian::dsp
{
namespace
{
// ---------------------------------------------------------------------------
// Thin per-backend vector layer: the kernels below are written once against
// these functions. Unaligned loads/stores throughout; on every CPU this code
// targets they cost the same as aligned ones when the data is aligned.
// ---------------------------------------------------------------------------
#if defined(MERIDIAN_SIMD_AVX)
using Vec = __m256d;
constexpr int lanes = 4;
inline Vec load (const double* p) noexcept           { return _mm256_loadu_pd (p); }
inline Vec loadFloat (const float* p) noexcept       { return _mm256_cvtps_pd (_mm_loadu_ps (p)); }
inline void store (double* p, Vec v) noexcept        { _mm256_storeu_pd (p, v); }
inline void storeFloat (float* p, Vec v) noexcept    { _mm_storeu_ps (p, _mm256_cvtpd_ps (v)); }
inline Vec splat (double x) noexcept                 { return _mm256_set1_pd (x); }
inline Vec add (Vec a, Vec b) noexcept               { return _mm256_add_pd (a, b); }
inline Vec sub (Vec a, Vec b) noexcept               { return _mm256_sub_pd (a, b); }
inline Vec mul (Vec a, Vec b) noexcept               { return _mm256_mul_pd (a, b); }
#elif defined(MERIDIAN_SIMD_SSE2)
using Vec = __m128d;
constexpr int lanes = 2;
inline Vec load (const double* p) noexcept           { return _mm_loadu_pd (p); }
inline Vec loadFloat (const float* p) noexcept
{
    return _mm_cvtps_pd (_mm_castsi128_ps (_mm_loadl_epi64 (reinterpret_cast<const __m128i*> (p))));
}
inline void store (double* p, Vec v) noexcept        { _mm_storeu_pd (p, v); }
inline void storeFloat (float* p, Vec v) noexcept
{
    _mm_storel_epi64 (reinterpret_cast<__m128i*> (p), _mm_castps_si128 (_mm_cvtpd_ps (v)));
}
inline Vec splat (double x) noexcept                 { return _mm_set1_pd (x); }
inline Vec add (Vec a, Vec b) noexcept               { return _mm_add_pd (a, b); }
inline Vec sub (Vec a, Vec b) noexcept               { return _mm_sub_pd (a, b); }
inline Vec mul (Vec a, Vec b) noexcept               { return _mm_mul_pd (a, b); }
#elif defined(MERIDIAN_SIMD_NEON)
using Vec = float64x2_t;
constexpr int lanes = 2;
inline Vec load (const double* p) noexcept           { return vld1q_f64 (p); }
inline Vec loadFloat (const float* p) noexcept       { return vcvt_f64_f32 (vld1_f32 (p)); }
inline void store (double* p, Vec v) noexcept        { vst1q_f64 (p, v); }
inline void storeFloat (float* p, Vec v) noexcept    { vst1_f32 (p, vcvt_f32_f64 (v)); }
inline Vec splat (double x) noexcept                 { return vdupq_n_f64 (x); }
inline Vec add (Vec a, Vec b) noexcept               { return vaddq_f64 (a, b); }
inline Vec sub (Vec a, Vec b) noexcept               { return vsubq_f64 (a, b); }
inline Vec mul (Vec a, Vec b) noexcept               { return vmulq_f64 (a, b); }
#endif

// Knuth TwoSum: t = s + x exactly decomposed into t + e. Six flops, no branches.
inline void twoSum (double& s, double& c, double x) noexcept
{
    const double t = s + x;
    const double z = t - s;
    c += (s - (t - z)) + (x - z);
    s = t;
}

#if defined(MERIDIAN_SIMD_ENABLED)
inline void twoSum (double* sumPtr, double* compPtr, Vec x) noexcept
{
    const Vec s = load (sumPtr);
    const Vec t = add (s, x);
    const Vec z = sub (t, s);
    const Vec e = add (sub (s, sub (t, z)), sub (x, z));
    store (sumPtr, t);
    store (compPtr, add (load (compPtr), e));
}
#endif
} // namespace

void accumulateScaled (const float* src, double gain, CompensatedChannel bus, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    const Vec g = splat (gain);
    for (; i + lanes <= numSamples; i += lanes)
        twoSum (bus.sum + i, bus.comp + i, mul (loadFloat (src + i), g));
#endif
    for (; i < numSamples; ++i)
        twoSum (bus.sum[i], bus.comp[i], static_cast<double> (src[i]) * gain);
}

void accumulateScaled (const float* src, const double* gains, CompensatedChannel bus, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    for (; i + lanes <= numSamples; i += lanes)
        twoSum (bus.sum + i, bus.comp + i, mul (loadFloat (src + i), load (gains + i)));
#endif
    for (; i < numSamples; ++i)
        twoSum (bus.sum[i], bus.comp[i], static_cast<double> (src[i]) * gains[i]);
}

void resolve (CompensatedChannel bus, double* dst, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    for (; i + lanes <= numSamples; i += lanes)
        store (dst + i, add (load (bus.sum + i), load (bus.comp + i)));
#endif
    for (; i < numSamples; ++i)
        dst[i] = bus.sum[i] + bus.comp[i];
}

void clear (CompensatedChannel bus, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    std::memset (bus.sum, 0, sizeof (double) * static_cast<size_t> (numSamples));
    std::memset (bus.comp, 0, sizeof (double) * static_cast<size_t> (numSamples));
}

void convertToFloat (const double* src, float* dst, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    for (; i + lanes <= numSamples; i += lanes)
        storeFloat (dst + i, load (src + i));
#endif
    for (; i < numSamples; ++i)
        dst[i] = static_cast<float> (src[i]);
}

void fillLinearRamp (double* dst, double first, double increment, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    alignas (32) double offsets[lanes];
    for (int lane = 0; lane < lanes; ++lane)
        offsets[lane] = static_cast<double> (lane);

    const Vec inc = splat (increment);
    const Vec base = splat (first);
    const Vec laneOffsets = load (offsets);
    for (; i + lanes <= numSamples; i += lanes)
        store (dst + i, add (base, mul (add (splat (static_cast<double> (i)), laneOffsets), inc)));
#endif
    for (; i < numSamples; ++i)
        dst[i] = first + static_cast<double> (i) * increment;
}

void multiplyInPlace (double* dst, const double* factors, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    for (; i + lanes <= numSamples; i += lanes)
        store (dst + i, mul (load (dst + i), load (factors + i)));
#endif
    for (; i < numSamples; ++i)
        dst[i] *= factors[i];
}

void multiplyInPlace (double* dst, double factor, int numSamples) noexcept
{
    int i = 0;
#if defined(MERIDIAN_SIMD_ENABLED)
    const Vec f = splat (factor);
    for (; i + lanes <= numSamples; i += lanes)
        store (dst + i, mul (load (dst + i), f));
#endif
    for (; i < numSamples; ++i)
        dst[i] *= factor;
}

// The peak scans use four independent accumulators so the compiler can keep
// several max operations in flight; they auto-vectorise on all three targets.
float peakMagnitude (const float* src, int numSamples) noexcept
{
    float m0 = 0.0f, m1 = 0.0f, m2 = 0.0f, m3 = 0.0f;
    int i = 0;
    for (; i + 4 <= numSamples; i += 4)
    {
        m0 = std::max (m0, std::abs (src[i]));
        m1 = std::max (m1, std::abs (src[i + 1]));
        m2 = std::max (m2, std::abs (src[i + 2]));
        m3 = std::max (m3, std::abs (src[i + 3]));
    }
    for (; i < numSamples; ++i)
        m0 = std::max (m0, std::abs (src[i]));
    return std::max (std::max (m0, m1), std::max (m2, m3));
}

double peakMagnitude (const double* src, int numSamples) noexcept
{
    double m0 = 0.0, m1 = 0.0;
    int i = 0;
    for (; i + 2 <= numSamples; i += 2)
    {
        m0 = std::max (m0, std::abs (src[i]));
        m1 = std::max (m1, std::abs (src[i + 1]));
    }
    for (; i < numSamples; ++i)
        m0 = std::max (m0, std::abs (src[i]));
    return std::max (m0, m1);
}

double peakMagnitudeScaled (const float* src, const double* gains, int numSamples) noexcept
{
    double m0 = 0.0, m1 = 0.0;
    int i = 0;
    for (; i + 2 <= numSamples; i += 2)
    {
        m0 = std::max (m0, std::abs (static_cast<double> (src[i]) * gains[i]));
        m1 = std::max (m1, std::abs (static_cast<double> (src[i + 1]) * gains[i + 1]));
    }
    for (; i < numSamples; ++i)
        m0 = std::max (m0, std::abs (static_cast<double> (src[i]) * gains[i]));
    return std::max (m0, m1);
}

void decibelsToGain (const double* decibels, double* gains, int numSamples, double minusInfinityDb) noexcept
{
    constexpr double ln10Over20 = 0.11512925464970228420; // ln(10) / 20
    for (int i = 0; i < numSamples; ++i)
        gains[i] = decibels[i] > minusInfinityDb ? std::exp (decibels[i] * ln10Over20) : 0.0;
}

void gainToDecibels (const double* gains, double* decibels, int numSamples, double minusInfinityDb) noexcept
{
    const double floorGain = std::pow (10.0, minusInfinityDb / 20.0);
    for (int i = 0; i < numSamples; ++i)
        decibels[i] = gains[i] > floorGain ? 20.0 * std::log10 (gains[i]) : minusInfinityDb;
}

const char* simdBackendName() noexcept
{
#if defined(MERIDIAN_SIMD_AVX)
    return "AVX";
#elif defined(MERIDIAN_SIMD_SSE2)
    return "SSE2";
#elif defined(MERIDIAN_SIMD_NEON)
    return "NEON";
#else
    return "scalar";
#endif
}

} // namespace meridian::dsp
