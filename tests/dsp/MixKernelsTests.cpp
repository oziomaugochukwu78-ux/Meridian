#include <dsp/MixKernels.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace meridian::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{
struct Bus
{
    explicit Bus (int n) : sum (size_t (n), 0.0), comp (size_t (n), 0.0) {}
    CompensatedChannel view() { return { sum.data(), comp.data() }; }
    std::vector<double> resolved()
    {
        std::vector<double> out (sum.size());
        resolve (view(), out.data(), int (out.size()));
        return out;
    }
    std::vector<double> sum, comp;
};

// Lengths chosen to exercise both the SIMD body and the scalar tail.
constexpr int blockLengths[] = { 1, 2, 3, 4, 5, 7, 8, 64, 67 };
} // namespace

TEST_CASE ("Compensated bus recovers a value that plain double summing loses", "[mix]")
{
    for (const int n : blockLengths)
    {
        Bus bus (n);
        const std::vector<float> ones (size_t (n), 1.0f);

        // +1e16, +1, -1e16. With plain doubles 1e16 + 1 rounds back to 1e16
        // (doubles are spaced 2 apart there), so the naive answer is 0.
        accumulateScaled (ones.data(), 1.0e16, bus.view(), n);
        accumulateScaled (ones.data(), 1.0, bus.view(), n);
        accumulateScaled (ones.data(), -1.0e16, bus.view(), n);

        const double naive = (1.0e16 + 1.0) - 1.0e16;
        REQUIRE (naive == 0.0);

        for (const double v : bus.resolved())
            REQUIRE (v == 1.0);
    }
}

TEST_CASE ("Compensated bus keeps many tiny contributions next to a loud one", "[mix]")
{
    constexpr int n = 16;
    Bus bus (n);
    const std::vector<float> ones (n, 1.0f);

    accumulateScaled (ones.data(), 1.0, bus.view(), n);
    for (int i = 0; i < 1000; ++i)
        accumulateScaled (ones.data(), 1.0e-17, bus.view(), n); // each one is below 1 ulp of 1.0

    for (const double v : bus.resolved())
        REQUIRE_THAT (v, WithinAbs (1.0 + 1.0e-14, 4.0e-16));
}

TEST_CASE ("Compensated summing does not depend on track order", "[mix]")
{
    std::mt19937 rng (1234);
    std::uniform_real_distribution<float> sample (-1.0f, 1.0f);
    std::uniform_real_distribution<double> gainDb (-90.0, 12.0);

    constexpr int numTracks = 256;
    constexpr int n = 37;
    std::vector<std::vector<float>> tracks (numTracks, std::vector<float> (n));
    std::vector<double> gains (numTracks);
    for (int t = 0; t < numTracks; ++t)
    {
        for (auto& s : tracks[size_t (t)])
            s = sample (rng);
        gains[size_t (t)] = std::pow (10.0, gainDb (rng) / 20.0);
    }

    Bus forward (n), backward (n);
    for (int t = 0; t < numTracks; ++t)
        accumulateScaled (tracks[size_t (t)].data(), gains[size_t (t)], forward.view(), n);
    for (int t = numTracks - 1; t >= 0; --t)
        accumulateScaled (tracks[size_t (t)].data(), gains[size_t (t)], backward.view(), n);

    // Both orders carry the exact sum to ~106 bits; collapsing to one double can
    // differ by at most the final rounding step.
    const auto a = forward.resolved();
    const auto b = backward.resolved();
    for (int i = 0; i < n; ++i)
        REQUIRE_THAT (a[size_t (i)], WithinULP (b[size_t (i)], 1));
}

TEST_CASE ("Per-sample gain accumulation matches the scalar definition", "[mix]")
{
    for (const int n : blockLengths)
    {
        std::vector<float> src (static_cast<size_t> (n));
        std::vector<double> gains (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            src[size_t (i)] = 0.25f * float (i % 5) - 0.5f;
            gains[size_t (i)] = 0.1 * i;
        }

        Bus bus (n);
        accumulateScaled (src.data(), gains.data(), bus.view(), n);
        const auto out = bus.resolved();
        for (int i = 0; i < n; ++i)
            REQUIRE (out[size_t (i)] == double (src[size_t (i)]) * gains[size_t (i)]);
    }
}

TEST_CASE ("Ramp, multiply, convert and peak helpers", "[mix]")
{
    for (const int n : blockLengths)
    {
        std::vector<double> ramp (static_cast<size_t> (n));
        fillLinearRamp (ramp.data(), 0.5, 0.25, n);
        for (int i = 0; i < n; ++i)
            REQUIRE (ramp[size_t (i)] == 0.5 + 0.25 * i);

        multiplyInPlace (ramp.data(), 2.0, n);
        REQUIRE (ramp[0] == 1.0);

        std::vector<float> asFloat (static_cast<size_t> (n));
        convertToFloat (ramp.data(), asFloat.data(), n);
        for (int i = 0; i < n; ++i)
            REQUIRE (asFloat[size_t (i)] == float (ramp[size_t (i)]));

        std::vector<float> signal (size_t (n), 0.1f);
        signal[size_t (n - 1)] = -0.9f;
        REQUIRE (peakMagnitude (signal.data(), n) == 0.9f);
    }
}

TEST_CASE ("dB conversions honour the silence floor", "[mix]")
{
    const double db[] = { 0.0, -6.0, 6.0, -200.0 };
    double gains[4];
    decibelsToGain (db, gains, 4, -144.0);
    REQUIRE_THAT (gains[0], WithinRel (1.0, 1e-12));
    REQUIRE_THAT (gains[1], WithinRel (0.501187233627, 1e-9));
    REQUIRE_THAT (gains[2], WithinRel (1.995262314969, 1e-9));
    REQUIRE (gains[3] == 0.0);

    double back[4];
    gainToDecibels (gains, back, 4, -144.0);
    REQUIRE_THAT (back[1], WithinAbs (-6.0, 1e-9));
    REQUIRE (back[3] == -144.0);
}
