#pragma once

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{

struct PanGains
{
    double left = 1.0;
    double right = 1.0;
};

namespace detail
{
    constexpr double quarterPi = 0.78539816339744830962;
    constexpr double sqrt2 = 1.41421356237309504880;

    inline double panAngle (double pan) noexcept
    {
        return (std::clamp (pan, -1.0, 1.0) + 1.0) * quarterPi; // 0 .. pi/2
    }
} // namespace detail

/** Constant-power (sine/cosine) pan for a mono source, -1 = hard left, +1 = hard right.
    left^2 + right^2 == 1 at every position, so perceived loudness stays constant
    as a source moves across the field; the centre sits at -3.01 dB per side. */
inline PanGains monoPan (double pan) noexcept
{
    const double theta = detail::panAngle (pan);
    return { std::cos (theta), std::sin (theta) };
}

/** Balance control for a stereo source built on the same sine/cosine curve,
    scaled so the centre is unity on both sides (a stereo track at centre must
    pass through untouched). Moving right attenuates only the left channel along
    the constant-power curve, and vice versa. */
inline PanGains stereoBalance (double pan) noexcept
{
    // The untouched side is exactly 1.0 rather than sqrt2 * cos(pi/4), which
    // rounds to 0.9999999999999999 and would colour a centred stereo track.
    const double theta = detail::panAngle (pan);
    return { pan > 0.0 ? std::min (1.0, detail::sqrt2 * std::cos (theta)) : 1.0,
             pan < 0.0 ? std::min (1.0, detail::sqrt2 * std::sin (theta)) : 1.0 };
}

} // namespace meridian::dsp
