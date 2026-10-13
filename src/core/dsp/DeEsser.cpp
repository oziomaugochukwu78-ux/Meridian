#include "DeEsser.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }
constexpr double ratioSlope = 0.75; // 4:1
} // namespace

void DeEsser::prepare (double rate) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    rmsCoef = std::exp (-1000.0 / (1.0 * sampleRate));
    attackCoef = std::exp (-1000.0 / (0.5 * sampleRate));
    releaseCoef = std::exp (-1000.0 / (60.0 * sampleRate));
    update();
    reset();
}

void DeEsser::reset() noexcept
{
    for (auto* f : { &detectL, &detectR, &shelfL, &shelfR })
        f->reset();
    meanSquare = heldDb = reductionDb = 0.0;
    setShelf (0.0);
    untilUpdate = 0;
}

void DeEsser::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.thresholdDb = std::clamp (finiteOr (p.thresholdDb, defaults.thresholdDb), -60.0, 0.0);
    params.frequencyHz = std::clamp (finiteOr (p.frequencyHz, defaults.frequencyHz), 2000.0, 12000.0);
    params.rangeDb = std::clamp (finiteOr (p.rangeDb, defaults.rangeDb), 0.0, 24.0);
    update();
}

void DeEsser::update() noexcept
{
    const auto c = Biquad::design (Biquad::Type::highPass, std::min (params.frequencyHz, 0.45 * sampleRate), 0.7071067811865476, 0.0,
                                   sampleRate);
    detectL.setCoefficients (c);
    detectR.setCoefficients (c);
    setShelf (shelfDb);
}

void DeEsser::setShelf (double gainDb) noexcept
{
    shelfDb = gainDb;
    const auto c = Biquad::design (Biquad::Type::highShelf, std::min (params.frequencyHz, 0.45 * sampleRate), 1.0, gainDb, sampleRate);
    shelfL.setCoefficients (c);
    shelfR.setCoefficients (c);
}

void DeEsser::process (float* left, float* right, int numSamples) noexcept
{
    for (int n = 0; n < numSamples; ++n)
    {
        const double l = left[n];
        const double r = right != nullptr ? (double) right[n] : l;

        // Detector: the band's power over ~1 ms; x2 makes a steady sine read its peak.
        const double bandL = detectL.process (l);
        const double bandR = right != nullptr ? detectR.process (r) : bandL;
        meanSquare = rmsCoef * meanSquare + (1.0 - rmsCoef) * std::max (bandL * bandL, bandR * bandR);
        const double levelDb = meanSquare > 1.0e-20 ? 10.0 * std::log10 (2.0 * meanSquare) : -200.0;
        const double target = std::clamp ((levelDb - params.thresholdDb) * ratioSlope, 0.0, params.rangeDb);

        heldDb = std::max (target, releaseCoef * heldDb + (1.0 - releaseCoef) * target);
        reductionDb = attackCoef * reductionDb + (1.0 - attackCoef) * heldDb;
        if (heldDb < 1.0e-6 && reductionDb < 1.0e-6)
            heldDb = reductionDb = 0.0; // inaudible: the shelf goes back to exactly 0 dB

        if (--untilUpdate <= 0)
        {
            untilUpdate = coefficientInterval;
            if (std::abs (-reductionDb - shelfDb) > 0.005 || (reductionDb <= 0.0 && shelfDb < 0.0))
                setShelf (reductionDb > 0.0 ? -reductionDb : 0.0);
        }

        left[n] = (float) shelfL.process (l);
        if (right != nullptr)
            right[n] = (float) shelfR.process (r);
    }
}

} // namespace meridian::dsp
