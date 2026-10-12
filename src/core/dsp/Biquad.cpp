#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{

Biquad::Coefficients Biquad::design (Type type, double frequency, double q, double gainDb, double sampleRate) noexcept
{
    constexpr double pi = 3.14159265358979323846;
    frequency = std::clamp (frequency, 10.0, 0.49 * sampleRate);
    q = std::max (0.05, q);

    const double w0 = 2.0 * pi * frequency / sampleRate;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double A = std::pow (10.0, gainDb / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type)
    {
        case Type::lowPass:
        {
            const double alpha = sw / (2.0 * q);
            b0 = (1.0 - cw) / 2.0; b1 = 1.0 - cw; b2 = b0;
            a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            break;
        }
        case Type::highPass:
        {
            const double alpha = sw / (2.0 * q);
            b0 = (1.0 + cw) / 2.0; b1 = -(1.0 + cw); b2 = b0;
            a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
            break;
        }
        case Type::peak:
        {
            const double alpha = sw / (2.0 * q);
            b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
            break;
        }
        case Type::lowShelf:
        case Type::highShelf:
        {
            // q is the shelf slope S here.
            const double alpha = sw / 2.0 * std::sqrt ((A + 1.0 / A) * (1.0 / q - 1.0) + 2.0);
            const double twoSqrtAAlpha = 2.0 * std::sqrt (A) * alpha;
            if (type == Type::lowShelf)
            {
                b0 = A * ((A + 1) - (A - 1) * cw + twoSqrtAAlpha);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - twoSqrtAAlpha);
                a0 = (A + 1) + (A - 1) * cw + twoSqrtAAlpha;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - twoSqrtAAlpha;
            }
            else
            {
                b0 = A * ((A + 1) + (A - 1) * cw + twoSqrtAAlpha);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - twoSqrtAAlpha);
                a0 = (A + 1) - (A - 1) * cw + twoSqrtAAlpha;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - twoSqrtAAlpha;
            }
            break;
        }
    }

    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

double Biquad::magnitudeAt (const Coefficients& c, double frequency, double sampleRate) noexcept
{
    constexpr double pi = 3.14159265358979323846;
    const std::complex<double> z1 = std::polar (1.0, -2.0 * pi * frequency / sampleRate);
    const std::complex<double> z2 = z1 * z1;
    return std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}

} // namespace meridian::dsp
