#include "AutomationWriter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace meridian::dsp
{

AutomationWriter::AutomationWriter (AutomationMode m, std::vector<AutomationPoint> lane, double base, double tol, int64_t glideSamples)
    : mode (m), existingPoints (std::move (lane)), baseValue (base),
      tolerance (std::max (0.0, tol)), glide (std::max<int64_t> (0, glideSamples))
{
    std::stable_sort (existingPoints.begin(), existingPoints.end(),
                      [] (const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
}

bool AutomationWriter::recording (bool touched)
{
    switch (mode)
    {
        case AutomationMode::write: return true;
        case AutomationMode::latch: latched = latched || touched; return latched;
        case AutomationMode::touch: return touched;
        case AutomationMode::off:
        case AutomationMode::read:  break;
    }
    return false;
}

void AutomationWriter::update (int64_t position, double value, bool touched)
{
    if (mode == AutomationMode::off || mode == AutomationMode::read || ! std::isfinite (value))
        return;

    // A jump backwards (loop, locate) ends the stretch: nothing is drawn across it.
    if (position < lastPosition && ! stretches.empty())
        stretches.back().open = false;
    lastPosition = position;

    if (recording (touched))
    {
        if (stretches.empty() || ! stretches.back().open)
            stretches.push_back ({});
        auto& points = stretches.back().points;
        if (! points.empty() && points.back().time == position)
            points.back().value = value;
        else
            points.push_back ({ position, value });
    }
    else if (! stretches.empty())
    {
        stretches.back().open = false; // released (touch)
    }
}

std::vector<AutomationPoint> AutomationWriter::finish (int64_t stopPosition)
{
    // Write and latch hold the last value up to the stop; so does a touch still held.
    if (! stretches.empty() && stretches.back().open)
    {
        auto& points = stretches.back().points;
        if (stopPosition > points.back().time)
            points.push_back ({ stopPosition, points.back().value });
    }

    // Each recorded stretch replaces the lane over its span (and, after a touch,
    // over the glide back), in the order recorded: a later pass over the same
    // time (a loop) replaces the earlier one. The joins at each end take the
    // lane as it is so far, so the curve outside the span is unchanged.
    auto lane = existingPoints;
    for (const auto& s : stretches)
    {
        if (s.points.empty())
            continue;
        const auto from = s.points.front().time;
        const auto end = s.points.back().time;
        const bool glideBack = mode == AutomationMode::touch && glide > 0 && ! s.open;
        const auto to = end + (glideBack ? glide : 1);

        const AutomationCurve before (lane);
        auto valueBefore = [&] (int64_t t) { return before.isEmpty() ? baseValue : before.valueAt (t); };

        std::vector<AutomationPoint> next;
        for (const auto& p : lane)
            if (p.time < from - 1 || p.time > to)
                next.push_back (p);
        if (from > 0)
            next.push_back ({ from - 1, valueBefore (from - 1) });
        for (const auto& p : thin (s.points, tolerance))
            next.push_back (p);
        next.push_back ({ to, valueBefore (to) });

        std::stable_sort (next.begin(), next.end(), [] (const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
        lane.clear();
        for (const auto& p : next) // one point per time: the later one wins, as in AutomationCurve
        {
            if (! lane.empty() && lane.back().time == p.time)
                lane.back() = p;
            else
                lane.push_back (p);
        }
    }
    stretches.clear();
    return lane;
}

std::vector<AutomationPoint> AutomationWriter::thin (const std::vector<AutomationPoint>& points, double tol)
{
    if (points.size() <= 2)
        return points;

    std::vector<bool> keep (points.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> work { { 0, points.size() - 1 } };
    while (! work.empty())
    {
        const auto [a, b] = work.back();
        work.pop_back();
        double worst = -1.0;
        size_t worstIndex = a;
        const auto& pa = points[a];
        const auto& pb = points[b];
        const double span = (double) (pb.time - pa.time);
        for (size_t i = a + 1; i < b; ++i)
        {
            const double f = span > 0.0 ? (double) (points[i].time - pa.time) / span : 0.0;
            const double error = std::abs (points[i].value - (pa.value + f * (pb.value - pa.value)));
            if (error > worst)
            {
                worst = error;
                worstIndex = i;
            }
        }
        if (worst > tol)
        {
            keep[worstIndex] = true;
            work.push_back ({ a, worstIndex });
            work.push_back ({ worstIndex, b });
        }
    }

    std::vector<AutomationPoint> result;
    for (size_t i = 0; i < points.size(); ++i)
        if (keep[i])
            result.push_back (points[i]);
    return result;
}

} // namespace meridian::dsp
