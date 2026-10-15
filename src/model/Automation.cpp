#include "Automation.h"
#include "Routing.h"

#include <algorithm>
#include <cmath>

namespace meridian::automation
{

std::optional<Range> rangeOf (const juce::String& parameter)
{
    if (parameter == "volume")
        return Range { -144.0, 12.0, false };
    if (parameter == "pan")
        return Range { -1.0, 1.0, false };
    if (parameter == "mute")
        return Range { 0.0, 1.0, true };
    if (sendTarget (parameter) >= 0)
        return Range { -144.0, 12.0, false };
    if (pluginTarget (parameter).first >= 0)
        return Range { 0.0, 1.0, false };
    return std::nullopt;
}

juce::String sendParameter (int slot)                    { return "send:" + juce::String (slot); }
juce::String pluginParameter (int slot, int index)       { return "plugin:" + juce::String (slot) + ":" + juce::String (index); }

int sendTarget (const juce::String& parameter)
{
    if (! parameter.startsWith ("send:"))
        return -1;
    const auto rest = parameter.fromFirstOccurrenceOf (":", false, false);
    const int slot = rest.getIntValue();
    return rest.containsOnly ("0123456789") && rest.isNotEmpty() && slot < Session::maxSends ? slot : -1;
}

std::pair<int, int> pluginTarget (const juce::String& parameter)
{
    if (! parameter.startsWith ("plugin:"))
        return { -1, -1 };
    const auto parts = juce::StringArray::fromTokens (parameter.fromFirstOccurrenceOf (":", false, false), ":", {});
    if (parts.size() != 2 || ! parts[0].containsOnly ("0123456789") || ! parts[1].containsOnly ("0123456789")
        || parts[0].isEmpty() || parts[1].isEmpty())
        return { -1, -1 };
    const int slot = parts[0].getIntValue(), index = parts[1].getIntValue();
    return slot < Session::maxInserts && index < 100000 ? std::pair { slot, index } : std::pair { -1, -1 };
}

juce::ValueTree lane (const juce::ValueTree& t, const juce::String& parameter)
{
    return track::automation (t).getChildWithProperty (ids::parameter, parameter);
}

juce::ValueTree getOrCreateLane (juce::ValueTree t, const juce::String& parameter, juce::UndoManager* um)
{
    if (auto existing = lane (t, parameter); existing.isValid())
        return existing;

    juce::ValueTree created (ids::LANE);
    created.setProperty (ids::parameter, parameter, nullptr);
    track::automation (t).appendChild (created, um);
    return created;
}

void clearLane (juce::ValueTree t, const juce::String& parameter, juce::UndoManager* um)
{
    if (auto existing = lane (t, parameter); existing.isValid())
        track::automation (t).removeChild (existing, um);
}

juce::ValueTree volumeLane (const juce::ValueTree& t)                                  { return lane (t, "volume"); }
juce::ValueTree getOrCreateVolumeLane (juce::ValueTree t, juce::UndoManager* um)        { return getOrCreateLane (t, "volume", um); }
void clear (juce::ValueTree t, juce::UndoManager* um)                                   { clearLane (t, "volume", um); }

std::vector<Point> points (const juce::ValueTree& laneNode)
{
    std::vector<Point> result;
    for (const auto& p : laneNode)
        result.push_back ({ (juce::int64) p[ids::time], (double) p[ids::value] });
    std::stable_sort (result.begin(), result.end(), [] (const Point& a, const Point& b) { return a.time < b.time; });
    return result;
}

namespace
{
double fitToLane (const juce::ValueTree& laneNode, double value)
{
    const auto range = rangeOf (laneNode[ids::parameter].toString()).value_or (Range { -144.0, 12.0, false });
    if (! std::isfinite (value))
        value = range.min;
    value = juce::jlimit (range.min, range.max, value);
    return range.stepped ? std::round (value) : value;
}
} // namespace

void addPoint (juce::ValueTree laneNode, juce::int64 time, double value, juce::UndoManager* um)
{
    value = fitToLane (laneNode, value);
    if (auto existing = laneNode.getChildWithProperty (ids::time, time); existing.isValid())
    {
        existing.setProperty (ids::value, value, um);
        return;
    }

    juce::ValueTree p (ids::POINT);
    p.setProperty (ids::time, std::max<juce::int64> (0, time), nullptr);
    p.setProperty (ids::value, value, nullptr);
    laneNode.appendChild (p, um);
}

void setPoints (juce::ValueTree laneNode, const std::vector<Point>& newPoints, juce::UndoManager* um)
{
    laneNode.removeAllChildren (um);
    for (const auto& p : newPoints)
        addPoint (laneNode, p.time, p.value, um);
}

void removeRange (juce::ValueTree laneNode, juce::int64 from, juce::int64 to, juce::UndoManager* um)
{
    if (from > to)
        std::swap (from, to);
    for (int i = laneNode.getNumChildren(); --i >= 0;)
    {
        const auto t = (juce::int64) laneNode.getChild (i)[ids::time];
        if (t >= from && t <= to)
            laneNode.removeChild (i, um);
    }
}

bool removeNear (juce::ValueTree laneNode, juce::int64 time, juce::int64 tolerance, juce::UndoManager* um)
{
    int best = -1;
    juce::int64 bestDistance = tolerance + 1;
    for (int i = 0; i < laneNode.getNumChildren(); ++i)
    {
        const auto distance = std::abs ((juce::int64) laneNode.getChild (i)[ids::time] - time);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }
    if (best < 0)
        return false;
    laneNode.removeChild (best, um);
    return true;
}

double valueAt (const std::vector<Point>& pts, juce::int64 time)
{
    if (pts.empty())
        return 0.0;
    if (time <= pts.front().time)
        return pts.front().value;
    if (time >= pts.back().time)
        return pts.back().value;

    const auto next = std::upper_bound (pts.begin(), pts.end(), time,
                                        [] (juce::int64 t, const Point& p) { return t < p.time; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    const double fraction = (double) (time - a.time) / (double) std::max<juce::int64> (1, b.time - a.time);
    return a.value + fraction * (b.value - a.value);
}

juce::String shownParameter (const juce::ValueTree& track, const juce::String& wanted)
{
    if (wanted == "pan" || wanted == "mute")
        return wanted;
    if (const int slot = sendTarget (wanted); slot >= 0 && routing::findSend (track, slot).isValid())
        return wanted;
    if (const auto target = pluginTarget (wanted); target.first >= 0)
        for (const auto& insert : track::inserts (track))
            if ((int) insert[ids::slot] == target.first)
                return wanted;
    return "volume";
}

juce::String shortName (const juce::String& parameter)
{
    if (const int slot = sendTarget (parameter); slot >= 0)
        return "send " + juce::String::charToString ((juce::juce_wchar) ('A' + slot));
    return parameter;
}

double staticValue (const juce::ValueTree& track, const juce::String& parameter)
{
    if (parameter == "pan")
        return track::pan (track);
    if (parameter == "mute")
        return track::mute (track) ? 1.0 : 0.0;
    if (const int slot = sendTarget (parameter); slot >= 0)
        if (const auto s = routing::findSend (track, slot); s.isValid())
            return send::levelDb (s);
    return track::volumeDb (track);
}

bool coalesceTrim (juce::ValueTree track, juce::UndoManager* um)
{
    auto lane = volumeLane (track);
    const double trim = track::volumeDb (track);
    if (! lane.isValid() || juce::exactlyEqual (trim, 0.0))
        return false;
    auto pts = points (lane);
    for (auto& p : pts)
        p.value = p.value <= -144.0 ? p.value : p.value + trim; // silence stays silence
    setPoints (lane, pts, um); // clamps to the lane's range
    track.setProperty (ids::volumeDb, 0.0, um);
    return true;
}

} // namespace meridian::automation
