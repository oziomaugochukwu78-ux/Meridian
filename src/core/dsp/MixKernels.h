#pragma once

// Inner loops of the mixer. Everything here is noexcept, allocation-free and
// safe to call on the audio thread.
//
// THE SUMMING BUS
// ---------------
// Each bus channel is a pair of double arrays (sum, comp). The true value of
// sample i is sum[i] + comp[i]. Every addition into the bus goes through
// Knuth's TwoSum error-free transformation: the rounding error of each add is
// computed exactly and accumulated in comp. The pair carries about 106 bits of
// significand, which is the precision of a 128-bit ("double-double")
// accumulator, while every operation stays a plain hardware double op that
// vectorises with SSE2/AVX/NEON.
//
// What this buys, honestly stated: floating-point buses never clip
// internally, even at 32 bits. What extra precision removes is rounding
// error that grows with the number of summed tracks and with large
// level differences between them. With TwoSum the result of summing N tracks
// is independent of their order and accurate to the last bit of a double.

namespace meridian::dsp
{

struct CompensatedChannel
{
    double* sum = nullptr;
    double* comp = nullptr;
};

/** bus += src * gain, with compensated addition. */
void accumulateScaled (const float* src, double gain, CompensatedChannel bus, int numSamples) noexcept;

/** bus += src[i] * gains[i], with compensated addition. */
void accumulateScaled (const float* src, const double* gains, CompensatedChannel bus, int numSamples) noexcept;

/** dst[i] = sum[i] + comp[i]: collapses a compensated channel to a plain double signal. */
void resolve (CompensatedChannel bus, double* dst, int numSamples) noexcept;

/** Clears both arrays of a compensated channel. */
void clear (CompensatedChannel bus, int numSamples) noexcept;

void convertToFloat (const double* src, float* dst, int numSamples) noexcept;

/** dst[i] = first + i * increment. Computed per index, so it never drifts. */
void fillLinearRamp (double* dst, double first, double increment, int numSamples) noexcept;

void multiplyInPlace (double* dst, const double* factors, int numSamples) noexcept;
void multiplyInPlace (double* dst, double factor, int numSamples) noexcept;

/** Largest |src[i]|. */
float peakMagnitude (const float* src, int numSamples) noexcept;
double peakMagnitude (const double* src, int numSamples) noexcept;

/** Largest |src[i] * gains[i]|, for post-fader metering. */
double peakMagnitudeScaled (const float* src, const double* gains, int numSamples) noexcept;

/** Batch dB <-> linear conversions for control-rate data (meters, automation
    breakpoints). The audio path itself never converts per sample: gains are
    ramped in the linear domain, so these run a handful of times per block. */
void decibelsToGain (const double* decibels, double* gains, int numSamples, double minusInfinityDb) noexcept;
void gainToDecibels (const double* gains, double* decibels, int numSamples, double minusInfinityDb) noexcept;

/** Name of the SIMD backend compiled in ("AVX", "SSE2", "NEON" or "scalar"). */
const char* simdBackendName() noexcept;

} // namespace meridian::dsp
