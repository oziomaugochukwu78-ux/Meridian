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

//==============================================================================
// Noise shaping

namespace
{
/** Average power per DFT bin of `x` between fLo and fHi, over 1024-sample blocks
    (an averaged periodogram: enough to compare two noise spectra). */
double bandPower (const std::vector<double>& x, double fLo, double fHi, double sampleRate)
{
    constexpr int block = 1024;
    const int lo = (int) std::ceil (fLo / sampleRate * block), hi = (int) std::floor (fHi / sampleRate * block);
    double total = 0.0;
    int count = 0;
    for (size_t start = 0; start + block <= x.size(); start += block)
        for (int k = lo; k <= hi; ++k)
        {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < block; ++n)
            {
                const double w = 2.0 * pi * k * n / block;
                re += x[start + (size_t) n] * std::cos (w);
                im -= x[start + (size_t) n] * std::sin (w);
            }
            total += re * re + im * im;
            ++count;
        }
    return total / count;
}

/** The quantisation error (output minus input, in LSBs) of 16-bit dither on a tone. */
std::vector<double> errorOf (bool shaped, int n)
{
    Dither d (2468);
    std::vector<double> e ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const auto x = (float) (0.25 * std::sin (2.0 * pi * 997.0 * i / 48000.0));
        const float y = shaped ? d.quantiseShaped (x, 16) : d.quantise (x, 16);
        e[(size_t) i] = ((double) y - (double) x) * 32768.0;
    }
    return e;
}
} // namespace

TEST_CASE ("Noise-shaped dither: output lies exactly on the 16-bit grid", "[dither]")
{
    Dither d;
    bool onGrid = true;
    for (int i = 0; i < 10000; ++i)
    {
        const double k = (double) d.quantiseShaped ((float) std::sin (i * 0.01) * 0.7f, 16) * 32768.0;
        onGrid = onGrid && std::abs (k - std::round (k)) < 1.0e-9;
    }
    CHECK (onGrid);
}

TEST_CASE ("Noise-shaped dither: the total error power is the plain dither's times sum f_k^2 (4.61)", "[dither]")
{
    const auto e = errorOf (true, 480000);
    double sum = 0.0, sumSquares = 0.0;
    for (const double v : e)
    {
        sum += v;
        sumSquares += v * v;
    }
    CHECK (std::abs (sum / (double) e.size()) < 0.01);
    CHECK (Dither::shapedNoisePowerGain == Approx (4.610334).margin (1.0e-6));
    CHECK (sumSquares / (double) e.size() / 0.25 == Approx (Dither::shapedNoisePowerGain).epsilon (0.02));
}

TEST_CASE ("Noise-shaped dither: at least 12 dB less noise from 2 to 5 kHz, and more near Nyquist, than plain TPDF", "[dither]")
{
    const auto plain = errorOf (false, 32768), shaped = errorOf (true, 32768);
    const double midGain = 10.0 * std::log10 (bandPower (shaped, 2000.0, 5000.0, 48000.0) / bandPower (plain, 2000.0, 5000.0, 48000.0));
    const double topGain = 10.0 * std::log10 (bandPower (shaped, 18000.0, 23000.0, 48000.0) / bandPower (plain, 18000.0, 23000.0, 48000.0));
    CHECK (midGain < -12.0);
    CHECK (topGain > 8.0);
}

TEST_CASE ("Noise-shaped dither: a -100 dBFS tone still survives, and the same seed gives the same output", "[dither]")
{
    const double amplitude = std::pow (10.0, -100.0 / 20.0);
    const int n = 48000 * 4;
    std::vector<float> out ((size_t) n);
    Dither d (777), a (42), b (42);
    bool same = true;
    for (int i = 0; i < n; ++i)
    {
        const auto x = (float) (amplitude * std::sin (2.0 * pi * 1000.0 * i / 48000.0));
        out[(size_t) i] = d.quantiseShaped (x, 16);
        same = same && a.quantiseShaped (x, 16) == b.quantiseShaped (x, 16);
    }
    CHECK (20.0 * std::log10 (toneAmplitude (out.data(), n, 1000.0, 48000.0)) == Approx (-100.0).margin (1.0));
    CHECK (same);
}
