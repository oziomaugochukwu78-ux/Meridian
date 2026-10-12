#include <dsp/SaturationCurve.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using meridian::dsp::SaturationCurve;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace
{
SaturationCurve makeCurve (SaturationCurve::Type type, double driveDb, double asymmetry)
{
    SaturationCurve c;
    c.setType (type);
    c.setDriveDecibels (driveDb);
    c.setAsymmetry (asymmetry);
    return c;
}
} // namespace

TEST_CASE ("Saturation is level-matched for quiet signals", "[saturation]")
{
    for (const auto type : { SaturationCurve::Type::tanh, SaturationCurve::Type::cubic })
        for (const double drive : { 0.0, 6.0, 18.0 })
            for (const double asym : { 0.0, 0.5, 1.0 })
            {
                const auto c = makeCurve (type, drive, asym);
                REQUIRE (c.process (0.0) == 0.0);
                REQUIRE_THAT (c.process (1.0e-5), WithinRel (1.0e-5, 1.0e-3));
                REQUIRE_THAT (c.process (-1.0e-5), WithinRel (-1.0e-5, 1.0e-3));
            }
}

TEST_CASE ("Saturation compresses peaks more as drive rises", "[saturation]")
{
    const auto gentle = makeCurve (SaturationCurve::Type::tanh, 3.0, 0.0);
    const auto hard = makeCurve (SaturationCurve::Type::tanh, 18.0, 0.0);

    REQUIRE (std::abs (gentle.process (1.0)) < 1.0);
    REQUIRE (std::abs (hard.process (1.0)) < std::abs (gentle.process (1.0)));
}

TEST_CASE ("Symmetric curve is odd; asymmetry introduces even harmonics", "[saturation]")
{
    const auto symmetric = makeCurve (SaturationCurve::Type::tanh, 12.0, 0.0);
    REQUIRE_THAT (symmetric.process (0.7) + symmetric.process (-0.7), WithinAbs (0.0, 1e-15));

    const auto asymmetric = makeCurve (SaturationCurve::Type::tanh, 12.0, 1.0);
    REQUIRE (std::abs (asymmetric.process (0.7) + asymmetric.process (-0.7)) > 1.0e-3);
}

TEST_CASE ("Cubic curve output is bounded", "[saturation]")
{
    const auto c = makeCurve (SaturationCurve::Type::cubic, 12.0, 0.0);
    const double ceiling = c.process (1000.0);
    REQUIRE (c.process (2000.0) == ceiling);
    REQUIRE_THAT (c.process (-1000.0), WithinAbs (-ceiling, 1e-15));
}
