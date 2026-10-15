#include "Shuffle.h"

#include "Automation.h"

namespace meridian::shuffle
{

void shiftClips (juce::ValueTree clips, juce::int64 from, juce::int64 delta, juce::UndoManager* um, const juce::ValueTree& except)
{
    if (delta == 0)
        return;
    for (auto c : clips)
        if (c != except && clip::start (c) >= from)
            c.setProperty (ids::start, std::max<juce::int64> (0, clip::start (c) + delta), um);
}

void shiftAutomation (juce::ValueTree track, juce::int64 from, juce::int64 delta, juce::UndoManager* um)
{
    if (delta == 0)
        return;
    for (auto lane : track::automation (track))
    {
        if (! lane.hasType (ids::LANE))
            continue;
        std::vector<automation::Point> moved;
        bool changed = false;
        for (auto p : automation::points (lane))
        {
            if (p.time >= from)
            {
                p.time = std::max<juce::int64> (0, p.time + delta);
                changed = true;
            }
            else if (delta < 0 && p.time >= from + delta)
            {
                changed = true; // in the time that was removed
                continue;
            }
            moved.push_back (p);
        }
        if (changed)
            automation::setPoints (lane, moved, um);
    }
}

void deleteClip (juce::ValueTree clip, juce::UndoManager* um)
{
    auto clips = clip.getParent();
    auto track = clip::owningTrack (clip);
    if (! clips.isValid())
        return;

    const auto start = clip::start (clip), end = clip::end (clip);
    clips.removeChild (clip, um);
    shiftClips (clips, end, start - end, um);
    if (track.isValid())
        shiftAutomation (track, end, start - end, um);
}

void closeRange (juce::ValueTree track, juce::int64 start, juce::int64 end, juce::UndoManager* um)
{
    if (end <= start)
        return;
    shiftClips (track::clips (track), end, start - end, um);
    shiftAutomation (track, end, start - end, um);
}

juce::int64 moveClip (juce::ValueTree clip, juce::int64 target, juce::UndoManager* um)
{
    auto clips = clip.getParent();
    if (! clips.isValid())
        return clip::start (clip);

    const auto length = clip::length (clip);

    // Out of the sequence: what followed it closes up.
    shiftClips (clips, clip::end (clip), -length, um, clip);

    // Back in at the nearest boundary of the clips that remain.
    juce::int64 insertAt = 0;
    for (const auto& c : clips)
        if (c != clip)
            for (const auto edge : { clip::start (c), clip::end (c) })
                if (std::abs (edge - target) < std::abs (insertAt - target))
                    insertAt = edge;

    shiftClips (clips, insertAt, length, um, clip);
    clip.setProperty (ids::start, insertAt, um);
    return insertAt;
}

} // namespace meridian::shuffle
