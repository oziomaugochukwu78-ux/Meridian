#include "SaturationCurve.h"
#include "SignalAnalysis.h"
#include "SimdConfig.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace meridian::dsp
{
namespace
{
// Bias is capped well inside the cubic's knee (|u| < 1) so its derivative,
// 1 - b^2, never approaches zero.
constexpr double maxBias = 0.35;
} // namespace

void SaturationCurve::setDriveDecibels (double driveDb) noexcept
{
    drive = std::pow (10.0, std::clamp (driveDb, minDriveDb, maxDriveDb) / 20.0);
    recalculate();
}

void SaturationCurve::setAsymmetry (double amount) noexcept
{
    bias = std::clamp (amount, 0.0, 1.0) * maxBias;
    recalculate();
}

void SaturationCurve::setType (Type newType) noexcept
{
    type = newType;
    recalculate();
}

double SaturationCurve::shape (double u) const noexcept
{
    if (type == Type::tanh)
        return std::tanh (u);

    // Cubic soft clip: smooth up to |u| = 1, flat at +-2/3 beyond.
    if (u >= 1.0)  return 2.0 / 3.0;
    if (u <= -1.0) return -2.0 / 3.0;
    return u - (u * u * u) / 3.0;
}

void SaturationCurve::recalculate() noexcept
{
    biasOffset = shape (bias);

    double slopeAtBias = 1.0;
    if (type == Type::tanh)
    {
        const double t = std::tanh (bias);
        slopeAtBias = 1.0 - t * t;
    }
    else
    {
        slopeAtBias = 1.0 - bias * bias;
    }

    normaliser = 1.0 / (drive * slopeAtBias);
}

double SaturationCurve::process (double x) const noexcept
{
    return (shape (drive * x + bias) - biasOffset) * normaliser;
}

SaturationCurve::Character SaturationCurve::measure (Type type, double driveDb, double asymmetry)
{
    SaturationCurve curve;
    curve.setType (type);
    curve.setDriveDecibels (driveDb);
    curve.setAsymmetry (asymmetry);

    // One second: whole cycles of the fundamental and of every harmonic.
    constexpr int rate = 48000;
    const double amplitude = std::pow (10.0, -6.0 / 20.0);
    std::vector<float> out (rate);
    for (int i = 0; i < rate; ++i)
        out[(size_t) i] = (float) curve.process (amplitude * std::sin (2.0 * 3.14159265358979323846 * 1000.0 * i / rate));

    double harmonics[11] = {};
    for (int k = 1; k <= 10; ++k)
        harmonics[k] = toneAmplitude (out.data(), rate, 1000.0 * k, (double) rate);
    double distortion = 0.0;
    for (int k = 2; k <= 10; ++k)
        distortion += harmonics[k] * harmonics[k];

    Character c;
    c.thdPercent = harmonics[1] > 0.0 ? 100.0 * std::sqrt (distortion) / harmonics[1] : 0.0;
    c.h2OverH3Db = 20.0 * std::log10 ((harmonics[2] + 1.0e-12) / (harmonics[3] + 1.0e-12));
    return c;
}

void SaturationCurve::processBlock (double* data, int numSamples) const noexcept
{
    for (int i = 0; i < numSamples; ++i)
        data[i] = process (data[i]);
}

} // namespace meridian::dsp
