#include <dsp/FaderTaper.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>

using meridian::dsp::FaderTaper;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Fader taper landmarks", "[fader]")
{
    REQUIRE (FaderTaper::positionToDb (0.0) == FaderTaper::minDb);
    REQUIRE (FaderTaper::positionToDb (FaderTaper::unityPosition) == 0.0);
    REQUIRE (FaderTaper::positionToDb (1.0) == FaderTaper::maxDb);
    REQUIRE (FaderTaper::dbToPosition (-std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE (FaderTaper::dbToPosition (0.0) == FaderTaper::unityPosition);
}

TEST_CASE ("Fader taper is monotonic and invertible", "[fader]")
{
    double previous = -1.0e9;
    for (int i = 1; i <= 1000; ++i)
    {
        const double position = i / 1000.0;
        const double db = FaderTaper::positionToDb (position);
        REQUIRE (db > previous);
        previous = db;
        REQUIRE_THAT (FaderTaper::dbToPosition (db), WithinAbs (position, 1e-12));
    }
}
