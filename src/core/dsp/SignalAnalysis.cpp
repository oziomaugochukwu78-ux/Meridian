#include "SignalAnalysis.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{

LevelStats measureLevels (const float* samples, int numSamples) noexcept
{
    LevelStats stats;
    if (numSamples <= 0)
        return stats;

    double sumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double x = samples[i];
        stats.peak = std::max (stats.peak, std::abs (x));
        sumSquares += x * x;
    }
    stats.rms = std::sqrt (sumSquares / numSamples);
    return stats;
}

double gainToDb (double gain) noexcept
{
    return gain > 1.0e-10 ? 20.0 * std::log10 (gain) : -200.0;
}

double toneAmplitude (const float* samples, int numSamples, double frequency, double sampleRate) noexcept
{
    if (numSamples <= 0 || sampleRate <= 0.0)
        return 0.0;

    constexpr double twoPi = 6.28318530717958647692;
    const double omega = twoPi * frequency / sampleRate;
    const double coeff = 2.0 * std::cos (omega);
    double s1 = 0.0, s2 = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double s0 = samples[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt (std::max (0.0, power)) / numSamples;
}

double estimateFrequency (const float* samples, int numSamples, double sampleRate) noexcept
{
    double first = -1.0, last = -1.0;
    int crossings = 0;

    for (int i = 1; i < numSamples; ++i)
    {
        const double a = samples[i - 1];
        const double b = samples[i];
        if (a < 0.0 && b >= 0.0)
        {
            const double t = (i - 1) + (-a) / (b - a); // linear interpolation of the crossing
            if (crossings == 0)
                first = t;
            last = t;
            ++crossings;
        }
    }

    if (crossings < 2 || last <= first)
        return 0.0;
    return (crossings - 1) * sampleRate / (last - first);
}

} // namespace meridian::dsp
