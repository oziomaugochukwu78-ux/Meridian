#include <dsp/LinearRamp.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using meridian::dsp::LinearRamp;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Ramp lands exactly on target at its last sample", "[ramp]")
{
    LinearRamp ramp;
    ramp.reset (0.0);
    ramp.setTarget (1.0, 100);

    std::vector<double> out (100);
    ramp.render (out.data(), 100);

    REQUIRE (out.back() == 1.0);
    REQUIRE_THAT (out.front(), WithinAbs (0.01, 1e-12));
    REQUIRE_FALSE (ramp.isRamping());
}

TEST_CASE ("Ramp spans several blocks and then holds", "[ramp]")
{
    LinearRamp ramp;
    ramp.reset (1.0);
    ramp.setTarget (0.0, 10);

    std::vector<double> a (4), b (4), c (4);
    ramp.render (a.data(), 4);
    ramp.render (b.data(), 4);
    ramp.render (c.data(), 4);

    REQUIRE_THAT (a[3], WithinAbs (0.6, 1e-12));
    REQUIRE_THAT (b[3], WithinAbs (0.2, 1e-12));
    REQUIRE (c[1] == 0.0);
    REQUIRE (c[3] == 0.0);
}

TEST_CASE ("Retargeting mid-ramp continues without a jump", "[ramp]")
{
    LinearRamp ramp;
    ramp.reset (0.0);
    ramp.setTarget (1.0, 10);

    std::vector<double> first (5);
    ramp.render (first.data(), 5);
    const double before = first.back();

    ramp.setTarget (0.0, 10);
    std::vector<double> second (1);
    ramp.render (second.data(), 1);

    // One step of the new ramp away from where the old ramp stopped.
    REQUIRE (std::abs (second[0] - before) < 0.06);
    REQUIRE (second[0] < before);
}

TEST_CASE ("Zero-length ramp jumps immediately", "[ramp]")
{
    LinearRamp ramp;
    ramp.reset (0.25);
    ramp.setTarget (0.75, 0);
    REQUIRE (ramp.getCurrent() == 0.75);
    REQUIRE_FALSE (ramp.isRamping());
}

TEST_CASE ("Skip advances like render", "[ramp]")
{
    LinearRamp a, b;
    a.reset (0.0);
    b.reset (0.0);
    a.setTarget (2.0, 8);
    b.setTarget (2.0, 8);

    std::vector<double> scratch (5);
    a.render (scratch.data(), 5);
    b.skip (5);
    REQUIRE_THAT (a.getCurrent(), WithinAbs (b.getCurrent(), 1e-15));
}
