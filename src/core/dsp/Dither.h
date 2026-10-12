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
};

} // namespace meridian::dsp
