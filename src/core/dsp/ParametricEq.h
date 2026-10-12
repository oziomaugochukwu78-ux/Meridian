#pragma once

#include "Biquad.h"

#include <array>

namespace meridian::dsp
{

/** A seven-section parametric EQ, in processing order:

        high-pass (12 dB/oct Butterworth, on/off)
        low shelf
        three peaking bands
        high shelf
        low-pass (12 dB/oct Butterworth, on/off)
        output gain

    Sections that do nothing (pass filters off, 0 dB bands) are skipped, so a
    flat EQ is bit-transparent. Coefficients are recomputed only when settings
    change. Real-time safe after prepare(). */
class ParametricEq
{
public:
    struct Band
    {
        double frequency = 1000.0; ///< Hz
        double gainDb = 0.0;       ///< -24..+24
        double q = 1.0;            ///< peaks: bandwidth (0.1..18); shelves: slope (0.3..1)
    };

    struct Params
    {
        bool highPassOn = false;
        double highPassHz = 80.0;
        Band lowShelf { 100.0, 0.0, 1.0 };
        std::array<Band, 3> peaks { { { 250.0, 0.0, 1.0 }, { 1000.0, 0.0, 1.0 }, { 4000.0, 0.0, 1.0 } } };
        Band highShelf { 8000.0, 0.0, 1.0 };
        bool lowPassOn = false;
        double lowPassHz = 18000.0;
        double outputDb = 0.0;
    };

    static constexpr int numSections = 7;

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    /** In place; up to two channels. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** The designed magnitude response at `frequency` (linear gain). */
    double responseAt (double frequency) const noexcept;

private:
    void update() noexcept;

    Params params;
    double sampleRate = 48000.0;
    double outputGain = 1.0;
    std::array<Biquad::Coefficients, numSections> coeffs {};
    std::array<bool, numSections> active {};
    std::array<std::array<Biquad, numSections>, 2> filters {};
};

} // namespace meridian::dsp
