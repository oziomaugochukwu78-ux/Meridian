#include "AutomationCurve.h"

#include <algorithm>

namespace meridian::dsp
{

AutomationCurve::AutomationCurve (std::vector<AutomationPoint> newPoints)
    : points (std::move (newPoints))
{
    std::stable_sort (points.begin(), points.end(),
                      [] (const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });

    // For equal times keep the last-written point (stable sort preserved input order).
    std::vector<AutomationPoint> unique;
    unique.reserve (points.size());
    for (const auto& p : points)
    {
        if (! unique.empty() && unique.back().time == p.time)
            unique.back() = p;
        else
            unique.push_back (p);
    }
    points = std::move (unique);
}

double AutomationCurve::valueAt (int64_t time) const noexcept
{
    double v = 0.0;
    render (time, &v, 1);
    return v;
}

void AutomationCurve::render (int64_t startTime, double* dst, int numSamples) const noexcept
{
    if (points.empty())
    {
        std::fill (dst, dst + numSamples, 0.0);
        return;
    }

    // First breakpoint strictly after startTime; the active segment is [next-1, next].
    auto next = std::upper_bound (points.begin(), points.end(), startTime,
                                  [] (int64_t t, const AutomationPoint& p) { return t < p.time; });

    for (int i = 0; i < numSamples; ++i)
    {
        const int64_t t = startTime + i;

        while (next != points.end() && next->time <= t)
            ++next;

        if (next == points.begin())
        {
            dst[i] = points.front().value;
        }
        else if (next == points.end())
        {
            dst[i] = points.back().value;
        }
        else
        {
            const auto& a = *(next - 1);
            const auto& b = *next;
            const double frac = static_cast<double> (t - a.time) / static_cast<double> (b.time - a.time);
            dst[i] = a.value + frac * (b.value - a.value);
        }
    }
}

} // namespace meridian::dsp
