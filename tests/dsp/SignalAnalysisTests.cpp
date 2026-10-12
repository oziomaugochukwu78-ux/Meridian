#include <dsp/SignalAnalysis.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace meridian::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
std::vector<float> sine (double frequency, double amplitude, double sampleRate, int n)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = (float) (amplitude * std::sin (6.283185307179586 * frequency * i / sampleRate));
    return x;
}
} // namespace

TEST_CASE ("Level stats of a sine", "[analysis]")
{
    const auto x = sine (1000.0, 0.5, 48000.0, 4800);
    const auto stats = measureLevels (x.data(), (int) x.size());
    REQUIRE_THAT (stats.peak, WithinRel (0.5, 1e-3));
    REQUIRE_THAT (stats.rms, WithinRel (0.5 / std::sqrt (2.0), 1e-3));
    REQUIRE_THAT (gainToDb (0.5), WithinAbs (-6.0206, 1e-3));
    REQUIRE (gainToDb (0.0) == -200.0);
}

TEST_CASE ("Goertzel recovers tone amplitude and rejects other tones", "[analysis]")
{
    auto x = sine (1000.0, 0.25, 48000.0, 4800);  // exactly 100 cycles
    const auto y = sine (3000.0, 0.1, 48000.0, 4800);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += y[i];

    REQUIRE_THAT (toneAmplitude (x.data(), (int) x.size(), 1000.0, 48000.0), WithinRel (0.25, 1e-3));
    REQUIRE_THAT (toneAmplitude (x.data(), (int) x.size(), 3000.0, 48000.0), WithinRel (0.1, 1e-3));
    REQUIRE (toneAmplitude (x.data(), (int) x.size(), 2000.0, 48000.0) < 1e-4);
}

TEST_CASE ("Zero-crossing frequency estimate", "[analysis]")
{
    const auto x = sine (441.0, 0.8, 44100.0, 44100);
    REQUIRE_THAT (estimateFrequency (x.data(), (int) x.size(), 44100.0), WithinRel (441.0, 1e-4));

    const std::vector<float> silence (1000, 0.0f);
    REQUIRE (estimateFrequency (silence.data(), (int) silence.size(), 44100.0) == 0.0);
}
