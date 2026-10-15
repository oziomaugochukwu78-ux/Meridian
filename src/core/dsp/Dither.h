#pragma once

#include <cmath>
#include <cstdint>

namespace meridian::dsp
{

/** Word-length reduction with TPDF dither.

    Before rounding to `bits`, triangular noise of +-1 LSB (the sum of two
    independent uniform values) is added. The rounding error then has zero
    mean, a power of exactly LSB^2/4 whatever the signal, and no correlation
    with it, so it is a constant, benign hiss instead of distortion; and
    signals quieter than one LSB still come through (tested).

    The generator is a seeded xorshift32, so a bounce with dither is
    reproducible to the bit. */
class Dither
{
public:
    explicit Dither (uint32_t seed = 0x9e3779b9u) noexcept : state (seed != 0 ? seed : 1u) {}

    /** x quantised to a `bits`-bit grid (values k / 2^(bits-1)), with dither. */
    float quantise (float x, int bits) noexcept
    {
        const double scale = std::ldexp (1.0, bits - 1);
        const double v = (double) x * scale + uniform() + uniform();
        return (float) (clampToGrid (std::floor (v + 0.5), scale) / scale);
    }

    /** quantise() with noise shaping. The total error of each sample (dither plus
        rounding, white with power LSB^2/4) is fed back through a 3-tap filter, so
        the noise reaching the output is that error filtered by
            F(z) = 1 - 1.623 z^-1 + 0.982 z^-2 - 0.109 z^-3
        (Lipshitz, Vanderkooy and Wannamaker, "Minimally audible noise shaping",
        JAES 1991): about 14 to 18 dB less noise from 2 to 5 kHz, where hearing is
        most sensitive, and more near Nyquist. The total noise power is the plain
        dither's times shapedNoisePowerGain (+6.6 dB): the price of moving it. */
    float quantiseShaped (float x, int bits) noexcept
    {
        const double scale = std::ldexp (1.0, bits - 1);
        const double u = (double) x * scale - (shape[0] * error[0] + shape[1] * error[1] + shape[2] * error[2]);
        const double q = clampToGrid (std::floor (u + uniform() + uniform() + 0.5), scale);
        // Normally |q - u| <= 1.5 steps; at the grid's ends the clamp could make it
        // huge and the loop unstable, so what is fed back is limited.
        const double e = q - u < -2.0 ? -2.0 : (q - u > 2.0 ? 2.0 : q - u);
        error[2] = error[1];
        error[1] = error[0];
        error[0] = e;
        return (float) (q / scale);
    }

    /** sum f_k^2 for F(z) above: the shaped noise power over the plain dither's. */
    static constexpr double shapedNoisePowerGain = 1.0 + 1.623 * 1.623 + 0.982 * 0.982 + 0.109 * 0.109;

    /** The same grid, plain rounding, no dither (for comparison). */
    static float round (float x, int bits) noexcept
    {
        const double scale = std::ldexp (1.0, bits - 1);
        return (float) (clampToGrid (std::floor ((double) x * scale + 0.5), scale) / scale);
    }

private:
    static double clampToGrid (double k, double scale) noexcept
    {
        return k < -scale ? -scale : (k > scale - 1.0 ? scale - 1.0 : k);
    }

    /** Uniform in [-0.5, 0.5). */
    double uniform() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (double) state / 4294967296.0 - 0.5;
    }

    uint32_t state;

    // Noise shaping: feedback coefficients h_k of F(z) = 1 - sum h_k z^-k, and the last errors.
    static constexpr double shape[3] = { 1.623, -0.982, 0.109 };
    double error[3] = { 0.0, 0.0, 0.0 };
};

} // namespace meridian::dsp
