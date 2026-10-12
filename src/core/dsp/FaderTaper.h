#pragma once

namespace meridian::dsp
{

/** Console-style fader law: maps fader travel (0 = bottom, 1 = top) to dB.

    A plain linear-in-dB fader wastes most of its travel on levels nobody mixes
    at. Like a hardware console, this taper gives the region around unity
    (-12 .. +6 dB) roughly half of the travel, puts 0 dB at 75 % and tops out
    at +12 dB. Between breakpoints the mapping is linear in dB, so it is exactly
    invertible. */
struct FaderTaper
{
    static constexpr double maxDb = 12.0;
    static constexpr double minDb = -144.0;  ///< Anything at or below this is "-inf" (silence).
    static constexpr double unityPosition = 0.75;

    /** Returns minDb for position 0 (fader fully down = off). */
    static double positionToDb (double position) noexcept;

    /** Inverse of positionToDb; -inf or anything <= minDb maps to 0. */
    static double dbToPosition (double db) noexcept;
};

} // namespace meridian::dsp
