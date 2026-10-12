#pragma once

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{

/** Level-meter geometry shared by the Edit and Mix windows.

    The scale is piecewise linear in dB, like a hardware console bridge: the top
    20 dB, where mixing decisions happen, get 60 % of the meter's height. */
struct MeterScale
{
    static constexpr double floorDb = -60.0;

    /** Maps a level in dB to 0 (bottom) .. 1 (top, 0 dBFS). */
    static double positionForDb (double db) noexcept
    {
        if (db <= floorDb)
            return 0.0;
        if (db >= 0.0)
            return 1.0;
        if (db >= -20.0)
            return 0.4 + 0.6 * (db + 20.0) / 20.0;   // -20 .. 0 dB   -> 0.4 .. 1.0
        return 0.4 * (db - floorDb) / (-20.0 - floorDb); // -60 .. -20 dB -> 0.0 .. 0.4
    }

    static double dbForGain (double gain) noexcept
    {
        return gain > 1.0e-6 ? 20.0 * std::log10 (gain) : floorDb - 1.0;
    }

    /** Colour zone of a meter segment, matching classic console metering:
        green below -12 dBFS, yellow to -3, orange to 0, red is clip. */
    enum class Zone { green, yellow, orange, red };

    static Zone zoneForDb (double db) noexcept
    {
        if (db >= 0.0)  return Zone::red;
        if (db >= -3.0) return Zone::orange;
        if (db >= -12.0) return Zone::yellow;
        return Zone::green;
    }
};

} // namespace meridian::dsp
