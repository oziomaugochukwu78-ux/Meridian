#pragma once

#include <complex>

namespace meridian::dsp
{

/** A second-order IIR section (the "biquad"), designed with the standard
    audio-EQ formulas (R. Bristow-Johnson's cookbook) and run in transposed
    direct form II in double precision.

    designResponse() gives the exact analytic magnitude, which the tests use to
    check that measured responses match the design. */
class Biquad
{
public:
    enum class Type { lowPass, highPass, peak, lowShelf, highShelf };

    struct Coefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // normalised (a0 = 1)
    };

    /** frequency in Hz; q for pass filters and peaks (0.707 = Butterworth),
        for shelves the shelf slope (1 = steepest without overshoot);
        gainDb for peaks and shelves. */
    static Coefficients design (Type, double frequency, double q, double gainDb, double sampleRate) noexcept;

    /** |H(e^jw)| of the given coefficients at `frequency`. */
    static double magnitudeAt (const Coefficients&, double frequency, double sampleRate) noexcept;

    void setCoefficients (const Coefficients& c) noexcept { coeffs = c; }
    void reset() noexcept { z1 = z2 = 0.0; }

    double process (double x) noexcept
    {
        const double y = coeffs.b0 * x + z1;
        z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
        z2 = coeffs.b2 * x - coeffs.a2 * y;
        return y;
    }

private:
    Coefficients coeffs;
    double z1 = 0.0, z2 = 0.0;
};

} // namespace meridian::dsp
