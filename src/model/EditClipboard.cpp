#include "EditClipboard.h"

#include "Automation.h"
#include "Routing.h"
#include "Shuffle.h"

#include <algorithm>

namespace meridian::clipboard
{
namespace
{
/** The lane's value at `time`: interpolated, or held for stepped lanes (mute). */
double laneValueAt (const juce::String& parameter, const std::vector<automation::Point>& pts, juce::int64 time)
{
    const auto range = automation::rangeOf (parameter);
    if (range.has_value() && range->stepped && ! pts.empty())
    {
        double held = pts.front().value;
        for (const auto& p : pts)
            if (p.time <= time)
                held = p.value;
        return held;
    }
    return automation::valueAt (pts, time);
}

/** The clip trimmed to [start, end), with its start made relative to `start`. Invalid if it misses the span. */
juce::ValueTree trimmedCopy (const juce::ValueTree& c, juce::int64 start, juce::int64 end)
{
    const auto s = clip::start (c), e = clip::end (c);
    const auto from = std::max (s, start), to = std::min (e, end);
    if (to <= from)
        return {};

    auto copy = c.createCopy();
    const auto length = to - from;
    copy.setProperty (ids::start, from - start, nullptr);
    copy.setProperty (ids::offset, clip::offset (c) + (from - s), nullptr);
    copy.setProperty (ids::length, length, nullptr);
    // A cut edge has no fade (as Separate leaves it); a kept fade still fits the clip.
    copy.setProperty (ids::fadeIn, from > s ? juce::int64 (0) : std::min (clip::fadeIn (c), length), nullptr);
    copy.setProperty (ids::fadeOut, to < e ? juce::int64 (0) : std::min (clip::fadeOut (c), length), nullptr);
    if (from > s)
        for (const auto& id : { ids::fadeInShape, ids::crossfadeIn, ids::extendedIn })
            copy.removeProperty (id, nullptr);
    if (to < e)
        for (const auto& id : { ids::fadeOutShape, ids::extendedOut })
            copy.removeProperty (id, nullptr);
    return copy;
}

/** The track's automation over [start, end], relative to start: each lane's
    value at both ends plus the breakpoints in between. */
juce::ValueTree copyAutomation (const juce::ValueTree& t, juce::int64 start, juce::int64 end)
{
    juce::ValueTree result (ids::AUTOMATION);
    for (const auto& lane : track::automation (t))
    {
        const auto parameter = lane[ids::parameter].toString();
        const auto pts = automation::points (lane);
        if (pts.empty())
            continue;
        juce::ValueTree copy (ids::LANE);
        copy.setProperty (ids::parameter, parameter, nullptr);
        std::vector<automation::Point> span { { 0, laneValueAt (parameter, pts, start) } };
        for (const auto& p : pts)
            if (p.time > start && p.time < end)
                span.push_back ({ p.time - start, p.value });
        span.push_back ({ end - start, laneValueAt (parameter, pts, end) });
        automation::setPoints (copy, span, nullptr);
        result.appendChild (copy, nullptr);
    }
    return result;
}

bool canTake (const juce::ValueTree& target, const Lane& lane)
{
    return ! track::isAux (target) && track::isInstrument (target) == lane.midi;
}

void pasteAutomation (juce::ValueTree target, const Lane& lane, juce::int64 at, juce::int64 length, juce::UndoManager* um)
{
    if (! lane.automation.isValid())
        return;
    for (const auto& copied : lane.automation)
    {
        const auto parameter = copied[ids::parameter].toString();
        const bool allowed = parameter == "volume" || parameter == "pan" || parameter == "mute"
                             || (automation::sendTarget (parameter) >= 0 && routing::findSend (target, automation::sendTarget (parameter)).isValid())
                             || (automation::pluginTarget (parameter).first >= 0 && track::id (target) == lane.sourceTrackId);
        const auto span = automation::points (copied);
        if (! allowed || span.empty())
            continue;

        // What plays just outside the pasted span stays as it was: the lane's own
        // values there, or the control's setting if the track had no lane yet.
        const auto existing = automation::points (automation::lane (target, parameter));
        const bool plugin = automation::pluginTarget (parameter).first >= 0;
        auto outside = [&] (juce::int64 time, double fallback)
        {
            if (! existing.empty())
                return laneValueAt (parameter, existing, time);
            return plugin ? fallback : automation::staticValue (target, parameter);
        };
        const double before = outside (at - 1, span.front().value);
        const double after = outside (at + length + 1, span.back().value);

        auto laneNode = automation::getOrCreateLane (target, parameter, um);
        automation::removeRange (laneNode, at, at + length, um);
        if (at > 0)
            automation::addPoint (laneNode, at - 1, before, um);
        automation::addPoint (laneNode, at + length + 1, after, um);
        for (const auto& p : span)
            automation::addPoint (laneNode, at + p.time, p.value, um);
    }
}
} // namespace

Contents copyRange (const juce::Array<juce::ValueTree>& tracks, juce::int64 start, juce::int64 end)
{
    Contents result;
    if (end <= start)
        return result;
    result.length = end - start;
    for (const auto& t : tracks)
    {
        if (track::isAux (t))
            continue;
        Lane lane;
        lane.sourceTrackId = track::id (t);
        lane.midi = track::isInstrument (t);
        for (const auto& c : track::clips (t))
            if (auto copy = trimmedCopy (c, start, end); copy.isValid())
                lane.clips.add (copy);
        lane.automation = copyAutomation (t, start, end);
        result.lanes.push_back (std::move (lane));
    }
    return result;
}

Contents copyClip (const juce::ValueTree& c)
{
    Contents result;
    const auto t = clip::owningTrack (c);
    if (! t.isValid() || clip::length (c) <= 0)
        return result;
    result.length = clip::length (c);
    Lane lane;
    lane.sourceTrackId = track::id (t);
    lane.midi = track::isInstrument (t);
    lane.clips.add (trimmedCopy (c, clip::start (c), clip::end (c)));
    lane.automation = copyAutomation (t, clip::start (c), clip::end (c));
    result.lanes.push_back (std::move (lane));
    return result;
}

void clearRange (Session& session, juce::ValueTree t, juce::int64 start, juce::int64 end, juce::UndoManager* um)
{
    if (end <= start)
        return;
    auto list = track::clips (t);
    for (const auto cut : { start, end })
    {
        juce::Array<juce::ValueTree> here; // copied first: splitting inserts siblings
        for (const auto& c : list)
            here.add (c);
        for (auto& c : here)
            session.splitClip (c, cut, um);
    }
    for (int i = list.getNumChildren(); --i >= 0;)
    {
        const auto c = list.getChild (i);
        if (clip::start (c) >= start && clip::end (c) <= end)
            list.removeChild (i, um);
    }
}

PasteResult paste (Session& session, const Contents& contents, juce::Array<juce::ValueTree> tracks, juce::int64 at,
                   bool shuffle, juce::UndoManager* um)
{
    PasteResult result;
    if (contents.isEmpty() || tracks.isEmpty())
        return result;
    at = std::max<juce::int64> (0, at);

    // Targets: the given tracks (no aux tracks), then the ones below them for any extra lanes.
    juce::Array<juce::ValueTree> targets;
    for (const auto& t : tracks)
        if (! track::isAux (t))
            targets.add (t);
    if (targets.isEmpty())
        return result;
    const auto all = session.getTracksNode();
    for (int i = all.indexOf (targets.getLast()) + 1; i < all.getNumChildren() && targets.size() < (int) contents.lanes.size(); ++i)
        if (! track::isAux (all.getChild (i)))
            targets.add (all.getChild (i));

    const auto length = contents.length;
    for (size_t i = 0; i < contents.lanes.size() && (int) i < targets.size(); ++i)
    {
        const auto& lane = contents.lanes[i];
        auto target = targets.getReference ((int) i);
        if (! canTake (target, lane))
        {
            ++result.lanesSkipped;
            continue;
        }

        if (shuffle)
        {
            // Open a gap: split what crosses `at`, then move everything from there on.
            juce::Array<juce::ValueTree> here;
            for (const auto& c : track::clips (target))
                here.add (c);
            for (auto& c : here)
                session.splitClip (c, at, um);
            shuffle::shiftClips (track::clips (target), at, length, um);
            shuffle::shiftAutomation (target, at, length, um);
        }
        else
        {
            clearRange (session, target, at, at + length, um);
        }

        auto list = track::clips (target);
        for (const auto& c : lane.clips)
        {
            auto copy = c.createCopy();
            copy.setProperty (ids::id, session.allocateId(), nullptr);
            copy.setProperty (ids::start, at + clip::start (c), nullptr);
            list.appendChild (copy, um);
            ++result.clipsPasted;
        }
        pasteAutomation (target, lane, at, length, um);
        result.trackIds.add (track::id (target));
    }
    return result;
}

} // namespace meridian::clipboard
