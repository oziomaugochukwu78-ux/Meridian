#include "FaderTaper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace meridian::dsp
{
namespace
{
struct Breakpoint
{
    double position;
    double db;
};

// Strictly increasing in both columns, which keeps the map invertible.
constexpr std::array<Breakpoint, 9> breakpoints {{
    { 0.000, -144.0 },
    { 0.050,  -60.0 },
    { 0.150,  -40.0 },
    { 0.300,  -24.0 },
    { 0.450,  -12.0 },
    { 0.600,   -6.0 },
    { 0.750,    0.0 },
    { 0.875,    6.0 },
    { 1.000,   12.0 },
}};
} // namespace

double FaderTaper::positionToDb (double position) noexcept
{
    if (! (position > 0.0)) // also catches NaN
        return minDb;
    if (position >= 1.0)
        return maxDb;

    for (size_t i = 1; i < breakpoints.size(); ++i)
    {
        const auto& hi = breakpoints[i];
        if (position == hi.position)
            return hi.db; // exact landmarks (e.g. unity) must not pick up rounding error
        if (position < hi.position)
        {
            const auto& lo = breakpoints[i - 1];
            const double t = (position - lo.position) / (hi.position - lo.position);
            return lo.db + t * (hi.db - lo.db);
        }
    }
    return maxDb;
}

double FaderTaper::dbToPosition (double db) noexcept
{
    if (std::isnan (db) || db <= minDb)
        return 0.0;
    if (db >= maxDb)
        return 1.0;

    for (size_t i = 1; i < breakpoints.size(); ++i)
    {
        const auto& hi = breakpoints[i];
        if (db == hi.db)
            return hi.position;
        if (db < hi.db)
        {
            const auto& lo = breakpoints[i - 1];
            const double t = (db - lo.db) / (hi.db - lo.db);
            return lo.position + t * (hi.position - lo.position);
        }
    }
    return 1.0;
}

} // namespace meridian::dsp
