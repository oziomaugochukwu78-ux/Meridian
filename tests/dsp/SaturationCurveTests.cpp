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

TEST_CASE ("Drive below 0 dB keeps unity small-signal gain and moves the knee above full scale; it stops at -12 dB", "[saturation]")
{
    for (const auto type : { SaturationCurve::Type::tanh, SaturationCurve::Type::cubic })
        for (const double asym : { 0.0, 0.3, 1.0 })
        {
            const auto c = makeCurve (type, -12.0, asym);
            REQUIRE (c.process (0.0) == 0.0);
            REQUIRE_THAT (c.process (1.0e-5), WithinRel (1.0e-5, 1.0e-3));
        }

    // At -12 dB, a full-scale peak is only gently compressed (well under 1 dB);
    // at +6 dB the same peak is squashed by several dB.
    const auto gentle = makeCurve (SaturationCurve::Type::tanh, -12.0, 0.0);
    const auto driven = makeCurve (SaturationCurve::Type::tanh, 6.0, 0.0);
    CHECK (20.0 * std::log10 (gentle.process (1.0)) > -0.5);
    CHECK (20.0 * std::log10 (driven.process (1.0)) < -3.0);

    CHECK_THAT (makeCurve (SaturationCurve::Type::tanh, -40.0, 0.0).getDriveGain(), WithinRel (std::pow (10.0, -12.0 / 20.0), 1.0e-12));
}

TEST_CASE ("measure() reports a setting's character: Warm Tape 0.67 % with H2 leading, Gritty 4.6 % with H3 leading", "[saturation]")
{
    const auto warm = SaturationCurve::measure (SaturationCurve::Type::tanh, -12.0, 0.30);
    CHECK_THAT (warm.thdPercent, WithinAbs (0.666, 0.02));
    CHECK_THAT (warm.h2OverH3Db, WithinAbs (14.2, 0.3));

    const auto gritty = SaturationCurve::measure (SaturationCurve::Type::tanh, 4.0, 0.05);
    CHECK_THAT (gritty.thdPercent, WithinAbs (4.58, 0.05));
    CHECK (gritty.h2OverH3Db < -15.0);
}
