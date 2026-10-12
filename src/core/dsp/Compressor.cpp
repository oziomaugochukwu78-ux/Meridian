#include "Compressor.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }

/** One-pole coefficient for a time constant: after `ms` a step has covered 1 - 1/e. */
double coefficient (double ms, double sampleRate) noexcept
{
    return std::exp (-1000.0 / (ms * sampleRate));
}
} // namespace

void Compressor::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    update();
    reset();
}

void Compressor::reset() noexcept
{
    held = 0.0;
    smoothed = 0.0;
}

void Compressor::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.thresholdDb = std::clamp (finiteOr (p.thresholdDb, defaults.thresholdDb), -60.0, 0.0);
    params.ratio = std::clamp (finiteOr (p.ratio, defaults.ratio), 1.0, 20.0);
    params.attackMs = std::clamp (finiteOr (p.attackMs, defaults.attackMs), 0.05, 200.0);
    params.releaseMs = std::clamp (finiteOr (p.releaseMs, defaults.releaseMs), 5.0, 3000.0);
    params.kneeDb = std::clamp (finiteOr (p.kneeDb, defaults.kneeDb), 0.0, 24.0);
    params.makeupDb = std::clamp (finiteOr (p.makeupDb, defaults.makeupDb), 0.0, 24.0);
    params.mix = std::clamp (finiteOr (p.mix, defaults.mix), 0.0, 1.0);
    update();
}

void Compressor::update() noexcept
{
    attackCoef = coefficient (params.attackMs, sampleRate);
    releaseCoef = coefficient (params.releaseMs, sampleRate);
    makeupGain = std::pow (10.0, params.makeupDb / 20.0);
}

double Compressor::staticCurve (double inputDb) const noexcept
{
    // Written as gain reduction times (1 - 1/ratio), so 1:1 gives exactly zero.
    const double slope = 1.0 - 1.0 / params.ratio;
    const double over = inputDb - params.thresholdDb;
    const double knee = params.kneeDb;
    double reduction = 0.0;
    if (2.0 * over > knee)
        reduction = slope * over;
    else if (knee > 0.0 && 2.0 * over > -knee)
        reduction = slope * (over + knee / 2.0) * (over + knee / 2.0) / (2.0 * knee);
    return inputDb - reduction;
}

void Compressor::process (float* left, float* right, int numSamples) noexcept
{
    const double wet = params.mix, dry = 1.0 - params.mix;
    constexpr double floorDb = -200.0;

    for (int n = 0; n < numSamples; ++n)
    {
        const double l = left[n];
        const double r = right != nullptr ? (double) right[n] : l;
        const double peak = std::max (std::abs (l), std::abs (r));
        const double levelDb = peak > 1.0e-10 ? 20.0 * std::log10 (peak) : floorDb;
        const double target = levelDb - staticCurve (levelDb); // wanted gain reduction, dB

        // Release: hold the peak reduction and let it fall back exponentially.
        held = std::max (target, releaseCoef * held + (1.0 - releaseCoef) * target);
        // Attack: approach the held reduction exponentially.
        smoothed = attackCoef * smoothed + (1.0 - attackCoef) * held;
        if (held < 1.0e-9 && smoothed < 1.0e-9) // inaudible; stop before the decay reaches denormals
            held = smoothed = 0.0;

        const double gain = smoothed > 0.0 ? makeupGain * std::pow (10.0, -smoothed / 20.0) : makeupGain;
        const double g = dry + wet * gain;
        left[n] = (float) (l * g);
        if (right != nullptr)
            right[n] = (float) (r * g);
    }
}

} // namespace meridian::dsp
