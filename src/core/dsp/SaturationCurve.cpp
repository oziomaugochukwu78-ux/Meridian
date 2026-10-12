#include "SaturationCurve.h"
#include "SimdConfig.h"

#include <algorithm>
#include <cmath>

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
    drive = std::pow (10.0, std::clamp (driveDb, 0.0, 24.0) / 20.0);
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

void SaturationCurve::processBlock (double* data, int numSamples) const noexcept
{
    for (int i = 0; i < numSamples; ++i)
        data[i] = process (data[i]);
}

} // namespace meridian::dsp
