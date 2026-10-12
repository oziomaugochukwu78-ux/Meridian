#pragma once

#include <cmath>

namespace meridian::dsp
{

/** First-order DC-blocking high-pass: y[n] = x[n] - x[n-1] + r * y[n-1].

    Asymmetric saturation (the source of the console's even harmonics) shifts
    the waveform's average away from zero. This removes that offset without
    touching audible bass: with the default 5 Hz corner the response at 20 Hz
    is -0.26 dB. */
class DcBlocker
{
public:
    void prepare (double sampleRate, double cornerHz = 5.0) noexcept
    {
        constexpr double twoPi = 6.28318530717958647692;
        r = std::exp (-twoPi * cornerHz / sampleRate);
        reset();
    }

    void reset() noexcept
    {
        x1 = 0.0;
        y1 = 0.0;
    }

    void process (double* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double x = data[i];
            const double y = x - x1 + r * y1;
            x1 = x;
            y1 = y;
            data[i] = y;
        }
    }

private:
    double r = 0.9993;
    double x1 = 0.0;
    double y1 = 0.0;
};

} // namespace meridian::dsp
