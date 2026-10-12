#include "Automation.h"

#include <algorithm>

namespace meridian::automation
{

juce::ValueTree volumeLane (const juce::ValueTree& t)
{
    return track::automation (t).getChildWithProperty (ids::parameter, "volume");
}

juce::ValueTree getOrCreateVolumeLane (juce::ValueTree t, juce::UndoManager* um)
{
    if (auto lane = volumeLane (t); lane.isValid())
        return lane;

    juce::ValueTree lane (ids::LANE);
    lane.setProperty (ids::parameter, "volume", nullptr);
    track::automation (t).appendChild (lane, um);
    return lane;
}

std::vector<Point> points (const juce::ValueTree& lane)
{
    std::vector<Point> result;
    for (const auto& p : lane)
        result.push_back ({ (juce::int64) p[ids::time], (double) p[ids::value] });
    std::stable_sort (result.begin(), result.end(), [] (const Point& a, const Point& b) { return a.time < b.time; });
    return result;
}

void addPoint (juce::ValueTree lane, juce::int64 time, double db, juce::UndoManager* um)
{
    db = juce::jlimit (-144.0, 12.0, db);
    if (auto existing = lane.getChildWithProperty (ids::time, time); existing.isValid())
    {
        existing.setProperty (ids::value, db, um);
        return;
    }

    juce::ValueTree p (ids::POINT);
    p.setProperty (ids::time, std::max<juce::int64> (0, time), nullptr);
    p.setProperty (ids::value, db, nullptr);
    lane.appendChild (p, um);
}

void removeRange (juce::ValueTree lane, juce::int64 from, juce::int64 to, juce::UndoManager* um)
{
    if (from > to)
        std::swap (from, to);
    for (int i = lane.getNumChildren(); --i >= 0;)
    {
        const auto t = (juce::int64) lane.getChild (i)[ids::time];
        if (t >= from && t <= to)
            lane.removeChild (i, um);
    }
}

bool removeNear (juce::ValueTree lane, juce::int64 time, juce::int64 tolerance, juce::UndoManager* um)
{
    int best = -1;
    juce::int64 bestDistance = tolerance + 1;
    for (int i = 0; i < lane.getNumChildren(); ++i)
    {
        const auto distance = std::abs ((juce::int64) lane.getChild (i)[ids::time] - time);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }
    if (best < 0)
        return false;
    lane.removeChild (best, um);
    return true;
}

void clear (juce::ValueTree t, juce::UndoManager* um)
{
    if (auto lane = volumeLane (t); lane.isValid())
        track::automation (t).removeChild (lane, um);
}

double valueAt (const std::vector<Point>& pts, juce::int64 time)
{
    if (pts.empty())
        return 0.0;
    if (time <= pts.front().time)
        return pts.front().db;
    if (time >= pts.back().time)
        return pts.back().db;

    const auto next = std::upper_bound (pts.begin(), pts.end(), time,
                                        [] (juce::int64 t, const Point& p) { return t < p.time; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    const double fraction = (double) (time - a.time) / (double) std::max<juce::int64> (1, b.time - a.time);
    return a.db + fraction * (b.db - a.db);
}

} // namespace meridian::automation
