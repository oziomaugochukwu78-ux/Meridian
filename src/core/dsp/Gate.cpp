#include "Gate.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }
double gainOf (double db) noexcept { return std::pow (10.0, db / 20.0); }
} // namespace

void Gate::prepare (double rate) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    update();
    reset();
}

void Gate::reset() noexcept
{
    open = false;
    holdLeft = 0;
    gain = floorGain;
}

void Gate::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.thresholdDb = std::clamp (finiteOr (p.thresholdDb, defaults.thresholdDb), -80.0, 0.0);
    params.rangeDb = std::clamp (finiteOr (p.rangeDb, defaults.rangeDb), -80.0, 0.0);
    params.attackMs = std::clamp (finiteOr (p.attackMs, defaults.attackMs), 0.05, 50.0);
    params.holdMs = std::clamp (finiteOr (p.holdMs, defaults.holdMs), 0.0, 2000.0);
    params.releaseMs = std::clamp (finiteOr (p.releaseMs, defaults.releaseMs), 5.0, 3000.0);
    params.hysteresisDb = std::clamp (finiteOr (p.hysteresisDb, defaults.hysteresisDb), 0.0, 20.0);
    update();
}

void Gate::update() noexcept
{
    openLevel = gainOf (params.thresholdDb);
    closeLevel = gainOf (params.thresholdDb - params.hysteresisDb);
    floorGain = gainOf (params.rangeDb);
    attackCoef = std::exp (-1000.0 / (params.attackMs * sampleRate));
    releaseCoef = std::exp (-1000.0 / (params.releaseMs * sampleRate));
    holdSamples = (int) std::lround (params.holdMs * sampleRate / 1000.0);
}

void Gate::process (float* left, float* right, int numSamples) noexcept
{
    for (int n = 0; n < numSamples; ++n)
    {
        const double l = left[n];
        const double r = right != nullptr ? (double) right[n] : l;
        const double peak = std::max (std::abs (l), std::abs (r));

        if (peak >= openLevel)
        {
            open = true;
            holdLeft = holdSamples;
        }
        else if (open)
        {
            if (peak >= closeLevel)
                holdLeft = holdSamples; // within the hysteresis: still open
            else if (holdLeft > 0)
                --holdLeft;
            else
                open = false;
        }

        const double target = open ? 1.0 : floorGain;
        gain = target + (target > gain ? attackCoef : releaseCoef) * (gain - target);
        if (std::abs (gain - target) <= target * 1.0e-9)
            gain = target; // settled: exactly unity (bit for bit) or exactly the range

        left[n] = (float) (l * gain);
        if (right != nullptr)
            right[n] = (float) (r * gain);
    }
}

} // namespace meridian::dsp
