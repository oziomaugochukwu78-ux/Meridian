#include <dsp/PanLaw.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace meridian::dsp;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Mono pan is constant power across the whole field", "[pan]")
{
    for (double pan = -1.0; pan <= 1.0; pan += 0.05)
    {
        const auto g = monoPan (pan);
        REQUIRE_THAT (g.left * g.left + g.right * g.right, WithinAbs (1.0, 1e-12));
    }
}

TEST_CASE ("Mono pan centre is -3 dB per side, extremes are hard", "[pan]")
{
    const auto centre = monoPan (0.0);
    REQUIRE_THAT (centre.left, WithinAbs (std::sqrt (0.5), 1e-12));
    REQUIRE_THAT (centre.right, WithinAbs (std::sqrt (0.5), 1e-12));

    const auto left = monoPan (-1.0);
    REQUIRE_THAT (left.left, WithinAbs (1.0, 1e-12));
    REQUIRE_THAT (left.right, WithinAbs (0.0, 1e-12));

    const auto clamped = monoPan (5.0);
    REQUIRE_THAT (clamped.right, WithinAbs (1.0, 1e-12));
}

TEST_CASE ("Stereo balance passes centre untouched and attenuates one side", "[pan]")
{
    const auto centre = stereoBalance (0.0);
    REQUIRE (centre.left == 1.0);
    REQUIRE (centre.right == 1.0);

    const auto right = stereoBalance (1.0);
    REQUIRE_THAT (right.left, WithinAbs (0.0, 1e-12));
    REQUIRE (right.right == 1.0);

    const auto halfRight = stereoBalance (0.5);
    REQUIRE (halfRight.right == 1.0);
    REQUIRE (halfRight.left < 1.0);
    REQUIRE (halfRight.left > 0.0);
}
