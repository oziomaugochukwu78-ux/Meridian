// Measured behaviour of the de-esser.

#include <dsp/Biquad.h>
#include <dsp/DeEsser.h>
#include <dsp/SignalAnalysis.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0, pi = 3.14159265358979323846;

std::vector<float> tone (double hz, double dbfs, int n)
{
    std::vector<float> x ((size_t) n);
    const double a = std::pow (10.0, dbfs / 20.0);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = (float) (a * std::sin (2.0 * pi * hz * i / rate));
    return x;
}

DeEsser make (double threshold, double frequency, double range)
{
    DeEsser d;
    d.prepare (rate);
    DeEsser::Params p;
    p.thresholdDb = threshold;
    p.frequencyHz = frequency;
    p.rangeDb = range;
    d.setParams (p);
    return d;
}

/** The detector's high-pass response at hz (complex): a 2nd-order Butterworth at `frequency`. */
std::complex<double> highPassAt (double frequency, double hz)
{
    const auto c = Biquad::design (Biquad::Type::highPass, frequency, 0.7071067811865476, 0.0, rate);
    const auto z1 = std::polar (1.0, -2.0 * pi * hz / rate), z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}
} // namespace

TEST_CASE ("De-esser: a loud tone below the frequency passes bit for bit", "[deesser]")
{
    auto d = make (-30.0, 6000.0, 12.0);
    auto x = tone (500.0, -3.0, 48000);
    const auto original = x;
    d.process (x.data(), nullptr, (int) x.size());
    CHECK (x == original);
    CHECK (d.getGainReductionDb() <= 0.0);
}

TEST_CASE ("De-esser: a tone in the band is reduced by (level over threshold) x 3/4", "[deesser]")
{
    // 8 kHz at -18 dBFS; the detector band passes |H(8k)| of it.
    auto d = make (-30.0, 6000.0, 24.0);
    auto x = tone (8000.0, -18.0, 48000);
    d.process (x.data(), nullptr, (int) x.size());
    const auto h = highPassAt (6000.0, 8000.0);
    const double bandLevel = -18.0 + 20.0 * std::log10 (std::abs (h));
    const double expectedReduction = (bandLevel + 30.0) * 0.75;
    CHECK (d.getGainReductionDb() == Approx (expectedReduction).margin (0.1));

    // The tone goes through a high shelf at 6 kHz set to -reduction dB.
    const auto shelf = Biquad::design (Biquad::Type::highShelf, 6000.0, 1.0, -expectedReduction, rate);
    const double expectedOut = -18.0 + 20.0 * std::log10 (Biquad::magnitudeAt (shelf, 8000.0, rate));
    const double measured = 20.0 * std::log10 (toneAmplitude (x.data() + 24000, 24000, 8000.0, rate));
    CHECK (measured == Approx (expectedOut).margin (0.15));
    CHECK (measured < -18.0 - 3.0); // audibly turned down
}

TEST_CASE ("De-esser: the reduction never exceeds the range", "[deesser]")
{
    auto d = make (-60.0, 4000.0, 6.0);
    auto x = tone (9000.0, -3.0, 24000);
    d.process (x.data(), nullptr, (int) x.size());
    CHECK (d.getGainReductionDb() == Approx (6.0).margin (1.0e-6));
}

TEST_CASE ("De-esser: in a mixture only the high part is turned down", "[deesser]")
{
    auto d = make (-40.0, 6000.0, 12.0);
    auto low = tone (300.0, -12.0, 48000), high = tone (9000.0, -12.0, 48000);
    std::vector<float> x (low.size());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = low[i] + high[i];
    d.process (x.data(), nullptr, (int) x.size());
    const double lowOut = 20.0 * std::log10 (toneAmplitude (x.data() + 24000, 24000, 300.0, rate));
    const double highOut = 20.0 * std::log10 (toneAmplitude (x.data() + 24000, 24000, 9000.0, rate));
    CHECK (lowOut == Approx (-12.0).margin (0.2));  // essentially untouched (the shelf is flat down there)
    CHECK (highOut < -12.0 - 6.0);                  // the sibilant part is down
}
