#include "ParametricEq.h"

#include "FloatCompare.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
double finiteOr (double value, double fallback) noexcept { return std::isfinite (value) ? value : fallback; }
} // namespace

void ParametricEq::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    update();
    reset();
}

void ParametricEq::reset() noexcept
{
    for (auto& channel : filters)
        for (auto& f : channel)
            f.reset();
}

void ParametricEq::setParams (const Params& p) noexcept
{
    const Params defaults;
    params = p;

    auto band = [] (Band b, const Band& fallback, double qLo, double qHi)
    {
        b.frequency = std::clamp (finiteOr (b.frequency, fallback.frequency), 20.0, 20000.0);
        b.gainDb = std::clamp (finiteOr (b.gainDb, 0.0), -24.0, 24.0);
        b.q = std::clamp (finiteOr (b.q, fallback.q), qLo, qHi);
        return b;
    };
    params.highPassHz = std::clamp (finiteOr (p.highPassHz, defaults.highPassHz), 20.0, 2000.0);
    params.lowShelf = band (p.lowShelf, defaults.lowShelf, 0.3, 1.0);
    for (size_t i = 0; i < params.peaks.size(); ++i)
        params.peaks[i] = band (p.peaks[i], defaults.peaks[i], 0.1, 18.0);
    params.highShelf = band (p.highShelf, defaults.highShelf, 0.3, 1.0);
    params.lowPassHz = std::clamp (finiteOr (p.lowPassHz, defaults.lowPassHz), 200.0, 20000.0);
    params.outputDb = std::clamp (finiteOr (p.outputDb, 0.0), -24.0, 24.0);
    update();
}

void ParametricEq::update() noexcept
{
    using T = Biquad::Type;
    constexpr double butterworthQ = 0.70710678118654752;

    coeffs[0] = Biquad::design (T::highPass, params.highPassHz, butterworthQ, 0.0, sampleRate);
    active[0] = params.highPassOn;
    coeffs[1] = Biquad::design (T::lowShelf, params.lowShelf.frequency, params.lowShelf.q, params.lowShelf.gainDb, sampleRate);
    active[1] = ! exactlyEqual (params.lowShelf.gainDb, 0.0);
    for (int i = 0; i < 3; ++i)
    {
        const auto& b = params.peaks[(size_t) i];
        coeffs[(size_t) (2 + i)] = Biquad::design (T::peak, b.frequency, b.q, b.gainDb, sampleRate);
        active[(size_t) (2 + i)] = ! exactlyEqual (b.gainDb, 0.0);
    }
    coeffs[5] = Biquad::design (T::highShelf, params.highShelf.frequency, params.highShelf.q, params.highShelf.gainDb, sampleRate);
    active[5] = ! exactlyEqual (params.highShelf.gainDb, 0.0);
    coeffs[6] = Biquad::design (T::lowPass, params.lowPassHz, butterworthQ, 0.0, sampleRate);
    active[6] = params.lowPassOn;

    for (auto& channel : filters)
        for (size_t s = 0; s < numSections; ++s)
            channel[s].setCoefficients (coeffs[s]);

    outputGain = std::pow (10.0, params.outputDb / 20.0);
}

void ParametricEq::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    const bool anyActive = std::any_of (active.begin(), active.end(), [] (bool a) { return a; });
    for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
    {
        float* x = channels[ch];
        auto& chain = filters[(size_t) ch];
        if (anyActive)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                double s = x[i];
                for (size_t k = 0; k < numSections; ++k)
                    if (active[k])
                        s = chain[k].process (s);
                x[i] = (float) s;
            }
        }
        if (! exactlyEqual (params.outputDb, 0.0))
            for (int i = 0; i < numSamples; ++i)
                x[i] = (float) (x[i] * outputGain);
    }
}

double ParametricEq::responseAt (double frequency) const noexcept
{
    double gain = outputGain;
    for (size_t k = 0; k < numSections; ++k)
        if (active[k])
            gain *= Biquad::magnitudeAt (coeffs[k], frequency, sampleRate);
    return gain;
}

} // namespace meridian::dsp
