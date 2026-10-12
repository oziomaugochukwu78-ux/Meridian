// TPDF dither: the error statistics, and a signal below one LSB surviving.

#include <dsp/Dither.h>
#include <dsp/SignalAnalysis.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double pi = 3.14159265358979323846;
}

TEST_CASE ("Dither: output lies exactly on the 16-bit grid", "[dither]")
{
    Dither d;
    bool onGrid = true;
    for (int i = 0; i < 10000; ++i)
    {
        const float y = d.quantise ((float) std::sin (i * 0.01) * 0.7f, 16);
        const double k = (double) y * 32768.0;
        onGrid = onGrid && std::abs (k - std::round (k)) < 1.0e-9;
    }
    CHECK (onGrid);
}

TEST_CASE ("Dither: the error has zero mean, power LSB^2/4, and no correlation with the signal", "[dither]")
{
    Dither d (12345);
    const double lsb = 1.0 / 32768.0;
    double sum = 0.0, sumSquares = 0.0, cross = 0.0, signalPower = 0.0;
    const int n = 480000;
    for (int i = 0; i < n; ++i)
    {
        const double x = 0.3 * std::sin (2.0 * pi * 997.0 * i / 48000.0) + 0.1 * std::sin (2.0 * pi * 51.0 * i / 48000.0);
        const double e = (double) d.quantise ((float) x, 16) - (double) (float) x;
        sum += e;
        sumSquares += e * e;
        cross += e * x;
        signalPower += x * x;
    }
    const double mean = sum / n, power = sumSquares / n;
    CHECK (std::abs (mean) < 0.01 * lsb);
    CHECK (power / (lsb * lsb / 4.0) == Approx (1.0).margin (0.02));
    CHECK (std::abs (cross / std::sqrt (sumSquares * signalPower)) < 0.005);
}

TEST_CASE ("Dither: a 1 kHz tone at -100 dBFS (below one 16-bit LSB) survives; plain rounding erases it", "[dither]")
{
    const double amplitude = std::pow (10.0, -100.0 / 20.0); // 0.33 LSB
    const int n = 48000 * 4;
    std::vector<float> dithered ((size_t) n), rounded ((size_t) n);
    Dither d (777);
    for (int i = 0; i < n; ++i)
    {
        const auto x = (float) (amplitude * std::sin (2.0 * pi * 1000.0 * i / 48000.0));
        dithered[(size_t) i] = d.quantise (x, 16);
        rounded[(size_t) i] = Dither::round (x, 16);
    }
    CHECK (20.0 * std::log10 (toneAmplitude (dithered.data(), n, 1000.0, 48000.0)) == Approx (-100.0).margin (1.0));
    CHECK (std::all_of (rounded.begin(), rounded.end(), [] (float v) { return v == 0.0f; }));
}

TEST_CASE ("Dither: the same seed gives the same output (reproducible bounces)", "[dither]")
{
    Dither a (42), b (42);
    bool same = true;
    for (int i = 0; i < 1000; ++i)
        same = same && a.quantise (0.1f, 24) == b.quantise (0.1f, 24);
    CHECK (same);
}
