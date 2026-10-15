#include "Fades.h"

#include <algorithm>
#include <vector>

namespace meridian::fades
{
namespace
{
juce::int64 extendedIn (const juce::ValueTree& c)  { return std::max<juce::int64> (0, (juce::int64) c.getProperty (ids::extendedIn, 0)); }
juce::int64 extendedOut (const juce::ValueTree& c) { return std::max<juce::int64> (0, (juce::int64) c.getProperty (ids::extendedOut, 0)); }
juce::String shapeName (Shape s)                   { return s == Shape::equalGain ? "gain" : "power"; }

/** Source audio available past the clip's end, and before its start. */
juce::int64 handleAfter (const juce::ValueTree& c)  { return std::max<juce::int64> (0, clip::sourceLength (c) - (clip::offset (c) + clip::length (c))); }
juce::int64 handleBefore (const juce::ValueTree& c) { return std::max<juce::int64> (0, clip::offset (c)); }

/** Undoes the incoming side of a crossfade: the clip starts at the join again, with no fade-in. */
void restoreIn (juce::ValueTree c, juce::UndoManager* um)
{
    const auto back = extendedIn (c);
    if (back > 0)
    {
        c.setProperty (ids::start, clip::start (c) + back, um);
        c.setProperty (ids::offset, clip::offset (c) + back, um);
        c.setProperty (ids::length, clip::length (c) - back, um);
    }
    c.setProperty (ids::fadeIn, 0, um);
    c.removeProperty (ids::extendedIn, um);
    c.removeProperty (ids::crossfadeIn, um);
    c.removeProperty (ids::fadeInShape, um);
}

/** Undoes the outgoing side: the clip ends at the join again, with no fade-out. */
void restoreOut (juce::ValueTree c, juce::UndoManager* um)
{
    if (const auto forward = extendedOut (c); forward > 0)
        c.setProperty (ids::length, clip::length (c) - forward, um);
    c.setProperty (ids::fadeOut, 0, um);
    c.removeProperty (ids::extendedOut, um);
    c.removeProperty (ids::fadeOutShape, um);
}

/** Where a crossfaded pair butted before the crossfade. */
juce::int64 joinOfIncoming (const juce::ValueTree& c)  { return clip::start (c) + extendedIn (c); }
juce::int64 joinOfOutgoing (const juce::ValueTree& c)  { return clip::end (c) - extendedOut (c); }

std::vector<juce::ValueTree> audioClipsByStart (const juce::ValueTree& t)
{
    std::vector<juce::ValueTree> result;
    for (const auto& c : track::clips (t))
        if (! clip::isMidi (c) && clip::length (c) > 0)
            result.push_back (c);
    std::stable_sort (result.begin(), result.end(),
                      [] (const juce::ValueTree& a, const juce::ValueTree& b) { return clip::start (a) < clip::start (b); });
    return result;
}

void setFadeIn (juce::ValueTree c, juce::int64 length, Shape shape, juce::UndoManager* um)
{
    length = juce::jlimit<juce::int64> (0, clip::length (c) - clip::fadeOut (c), length);
    c.setProperty (ids::fadeIn, length, um);
    c.setProperty (ids::fadeInShape, shapeName (shape), um);
}

void setFadeOut (juce::ValueTree c, juce::int64 length, Shape shape, juce::UndoManager* um)
{
    length = juce::jlimit<juce::int64> (0, clip::length (c) - clip::fadeIn (c), length);
    c.setProperty (ids::fadeOut, length, um);
    c.setProperty (ids::fadeOutShape, shapeName (shape), um);
}
} // namespace

bool isCrossfadeIn (const juce::ValueTree& c)
{
    return (bool) c.getProperty (ids::crossfadeIn, false);
}

bool crossfade (juce::ValueTree a, juce::ValueTree b, juce::int64 from, juce::int64 to, Shape shape,
                juce::UndoManager* um, bool* wasShortened)
{
    if (! a.isValid() || ! b.isValid() || clip::isMidi (a) || clip::isMidi (b) || a.getParent() != b.getParent())
        return false;
    const auto join = clip::end (a);
    if (clip::start (b) != join)
        return false;
    if (from > to)
        std::swap (from, to);
    from = std::min (from, join);
    to = std::max (to, join);

    const auto wantBefore = join - from, wantAfter = to - join;
    auto before = std::min (wantBefore, handleBefore (b));
    auto after = std::min (wantAfter, handleAfter (a));

    // Each clip keeps its other fade: the crossfade fits in what is left of the clip.
    const auto roomA = clip::length (a) - clip::fadeIn (a);
    const auto roomB = clip::length (b) - clip::fadeOut (b);
    const auto room = std::max<juce::int64> (0, std::min (roomA, roomB));
    if (before + after > room)
    {
        const double scale = (double) room / (double) (before + after);
        before = (juce::int64) std::floor ((double) before * scale);
        after = std::min (room - before, (juce::int64) std::floor ((double) after * scale));
    }
    const auto length = before + after;
    if (length <= 0)
        return false;
    if (wasShortened != nullptr)
        *wasShortened = before < wantBefore || after < wantAfter;

    // The incoming clip must sit above the outgoing one.
    auto list = a.getParent();
    if (const int ib = list.indexOf (b), ia = list.indexOf (a); ib < ia)
        list.moveChild (ib, ia, um);

    a.setProperty (ids::length, clip::length (a) + after, um);
    a.setProperty (ids::fadeOut, length, um);
    a.setProperty (ids::extendedOut, after, um);
    a.setProperty (ids::fadeOutShape, shapeName (shape), um);

    b.setProperty (ids::start, clip::start (b) - before, um);
    b.setProperty (ids::offset, clip::offset (b) - before, um);
    b.setProperty (ids::length, clip::length (b) + before, um);
    b.setProperty (ids::fadeIn, length, um);
    b.setProperty (ids::extendedIn, before, um);
    b.setProperty (ids::crossfadeIn, true, um);
    b.setProperty (ids::fadeInShape, shapeName (shape), um);
    return true;
}

bool removeFades (juce::ValueTree c, juce::UndoManager* um)
{
    if (! c.isValid() || clip::isMidi (c))
        return false;
    bool changed = clip::fadeIn (c) > 0 || clip::fadeOut (c) > 0;
    const auto list = c.getParent();

    if (isCrossfadeIn (c))
    {
        const auto join = joinOfIncoming (c);
        for (const auto& other : list)
            if (other != c && extendedOut (other) > 0 && joinOfOutgoing (other) == join)
                restoreOut (other, um);
        restoreIn (c, um);
        changed = true;
    }
    if (extendedOut (c) > 0)
    {
        const auto join = joinOfOutgoing (c);
        for (const auto& other : list)
            if (other != c && isCrossfadeIn (other) && joinOfIncoming (other) == join)
                restoreIn (other, um);
        restoreOut (c, um);
        changed = true;
    }
    c.setProperty (ids::fadeIn, 0, um);
    c.setProperty (ids::fadeOut, 0, um);
    c.removeProperty (ids::fadeInShape, um);
    c.removeProperty (ids::fadeOutShape, um);
    return changed;
}

Result apply (juce::ValueTree t, juce::int64 start, juce::int64 end, Shape shape, juce::int64 batchLength, juce::UndoManager* um)
{
    Result result;
    if (end <= start || ! t.isValid())
        return result;

    // A crossfade already over a join in the range is replaced: give back the butted clips first.
    for (const auto& c : audioClipsByStart (t))
        if (isCrossfadeIn (c) && joinOfIncoming (c) >= start && joinOfIncoming (c) <= end)
            removeFades (c, um);

    struct Edge
    {
        enum Kind { join, freeStart, freeEnd } kind;
        juce::int64 at;
        juce::ValueTree first, second; // join: outgoing, incoming; otherwise the clip
    };
    const auto clips = audioClipsByStart (t);
    std::vector<Edge> edges;
    for (const auto& c : clips)
    {
        const auto s = clip::start (c), e = clip::end (c);
        juce::ValueTree before, after;
        for (const auto& other : clips)
        {
            if (other == c)
                continue;
            if (clip::end (other) == s)
                before = other;
            if (clip::start (other) == e)
                after = other;
        }
        // A side already in a crossfade is not a free edge.
        if (! before.isValid() && ! isCrossfadeIn (c) && s >= start && s <= end)
            edges.push_back ({ Edge::freeStart, s, c, {} });
        if (after.isValid() && e >= start && e <= end)
            edges.push_back ({ Edge::join, e, c, after });
        else if (! after.isValid() && extendedOut (c) == 0 && e >= start && e <= end)
            edges.push_back ({ Edge::freeEnd, e, c, {} });
    }

    const bool single = edges.size() == 1;
    for (const auto& edge : edges)
    {
        auto clipNode = edge.first;
        switch (edge.kind)
        {
            case Edge::join:
            {
                bool shortened = false;
                const auto from = single ? start : edge.at - batchLength / 2;
                const auto to = single ? end : from + batchLength;
                if (crossfade (edge.first, edge.second, from, to, shape, um, &shortened))
                {
                    ++result.crossfades;
                    result.shortened += shortened ? 1 : 0;
                }
                else
                {
                    ++result.impossible;
                }
                break;
            }
            case Edge::freeStart:
            {
                const auto length = single ? std::min (end, clip::end (clipNode)) - clip::start (clipNode) : batchLength;
                if (length > 0)
                {
                    setFadeIn (clipNode, length, shape, um);
                    ++result.fadeIns;
                }
                break;
            }
            case Edge::freeEnd:
            {
                const auto length = single ? clip::end (clipNode) - std::max (start, clip::start (clipNode)) : batchLength;
                if (length > 0)
                {
                    setFadeOut (clipNode, length, shape, um);
                    ++result.fadeOuts;
                }
                break;
            }
        }
    }
    return result;
}

} // namespace meridian::fades
