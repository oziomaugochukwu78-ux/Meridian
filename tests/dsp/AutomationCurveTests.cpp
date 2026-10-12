#include <dsp/AutomationCurve.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace meridian::dsp;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Automation interpolates linearly with sample accuracy", "[automation]")
{
    const AutomationCurve curve ({ { 100, 0.0 }, { 200, 1.0 } });

    REQUIRE (curve.valueAt (0) == 0.0);     // held before the first point
    REQUIRE (curve.valueAt (100) == 0.0);
    REQUIRE_THAT (curve.valueAt (150), WithinAbs (0.5, 1e-15));
    REQUIRE_THAT (curve.valueAt (199), WithinAbs (0.99, 1e-15));
    REQUIRE (curve.valueAt (200) == 1.0);
    REQUIRE (curve.valueAt (100000) == 1.0); // held after the last point
}

TEST_CASE ("Block render matches point evaluation across breakpoints", "[automation]")
{
    const AutomationCurve curve ({ { 300, 1.0 }, { 0, 0.0 }, { 10, 0.5 }, { 20, 0.25 } }); // unsorted on purpose

    std::vector<double> block (400);
    curve.render (-50, block.data(), int (block.size()));
    for (int i = 0; i < int (block.size()); ++i)
        REQUIRE (block[size_t (i)] == curve.valueAt (-50 + i));
}

TEST_CASE ("Duplicate times keep the last point written", "[automation]")
{
    const AutomationCurve curve ({ { 50, 0.1 }, { 50, 0.9 } });
    REQUIRE (curve.size() == 1);
    REQUIRE (curve.valueAt (50) == 0.9);
}
