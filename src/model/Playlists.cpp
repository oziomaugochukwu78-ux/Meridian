#include "Playlists.h"

namespace meridian::playlists
{
namespace
{
juce::ValueTree getOrCreatePlaylists (juce::ValueTree track, juce::UndoManager* um)
{
    auto list = track::playlists (track);
    if (! list.isValid())
    {
        list = juce::ValueTree (ids::PLAYLISTS);
        track.appendChild (list, um);
    }
    return list;
}

/** Moves every clip of `from` into `to` (both CLIPS lists), keeping their order. */
void moveClips (juce::ValueTree from, juce::ValueTree to, juce::UndoManager* um)
{
    while (from.getNumChildren() > 0)
    {
        auto c = from.getChild (0);
        from.removeChild (0, um);
        to.appendChild (c, um);
    }
}

/** Stores the active playlist's clips as an alternate named after it. */
void storeActive (juce::ValueTree track, juce::UndoManager* um)
{
    juce::ValueTree stored (ids::PLAYLIST);
    stored.setProperty (ids::name, activeName (track), nullptr);
    stored.appendChild (juce::ValueTree (ids::CLIPS), nullptr);
    getOrCreatePlaylists (track, um).appendChild (stored, um);
    moveClips (track::clips (track), stored.getChildWithName (ids::CLIPS), um);
}
} // namespace

juce::String activeName (const juce::ValueTree& track)
{
    const auto stored = track[ids::playlist].toString();
    return stored.isNotEmpty() ? stored : track::name (track) + ".01";
}

juce::StringArray names (const juce::ValueTree& track)
{
    juce::StringArray result;
    for (const auto& p : track::playlists (track))
        result.add (p[ids::name].toString());
    result.add (activeName (track));
    result.sort (true);
    return result;
}

juce::ValueTree find (const juce::ValueTree& track, const juce::String& name)
{
    for (const auto& p : track::playlists (track))
        if (p[ids::name].toString() == name)
            return p;
    return {};
}

juce::String nextName (const juce::ValueTree& track)
{
    const auto taken = names (track);
    for (int n = 2;; ++n)
    {
        const auto candidate = track::name (track) + "." + juce::String (n).paddedLeft ('0', 2);
        if (! taken.contains (candidate))
            return candidate;
    }
}

juce::String create (Session& session, juce::ValueTree track, bool duplicate, juce::UndoManager* um)
{
    const auto name = nextName (track);
    juce::Array<juce::ValueTree> copies;
    if (duplicate)
        for (const auto& c : track::clips (track))
        {
            auto copy = c.createCopy();
            copy.setProperty (ids::id, session.allocateId(), nullptr);
            copies.add (copy);
        }

    storeActive (track, um);
    for (auto& copy : copies)
        track::clips (track).appendChild (copy, um);
    track.setProperty (ids::playlist, name, um);
    return name;
}

bool activate (juce::ValueTree track, const juce::String& name, juce::UndoManager* um)
{
    auto target = find (track, name);
    if (! target.isValid())
        return name == activeName (track); // already active: nothing to do

    storeActive (track, um);
    moveClips (target.getChildWithName (ids::CLIPS), track::clips (track), um);
    track::playlists (track).removeChild (target, um);
    track.setProperty (ids::playlist, name, um);
    return true;
}

bool remove (juce::ValueTree track, const juce::String& name, juce::UndoManager* um)
{
    auto target = find (track, name);
    if (! target.isValid())
        return false;
    track::playlists (track).removeChild (target, um);
    return true;
}

bool rename (juce::ValueTree track, const juce::String& oldName, const juce::String& newName, juce::UndoManager* um)
{
    const auto clean = newName.trim();
    if (clean.isEmpty() || names (track).contains (clean))
        return false;
    if (oldName == activeName (track))
    {
        track.setProperty (ids::playlist, clean, um);
        return true;
    }
    auto target = find (track, oldName);
    if (! target.isValid())
        return false;
    target.setProperty (ids::name, clean, um);
    return true;
}

int promote (Session& session, juce::ValueTree track, const juce::String& name, juce::int64 start, juce::int64 end,
             juce::UndoManager* um)
{
    const auto source = find (track, name);
    if (! source.isValid() || end <= start)
        return -1;
    auto active = track::clips (track);

    // Clear the range on the active playlist: split at both edges, remove what is inside.
    for (const auto cut : { start, end })
    {
        juce::Array<juce::ValueTree> here;
        for (const auto& c : active)
            here.add (c);
        for (auto& c : here)
            session.splitClip (c, cut, um);
    }
    juce::Array<juce::ValueTree> inside;
    for (const auto& c : active)
        if (clip::start (c) >= start && clip::end (c) <= end)
            inside.add (c);
    for (auto& c : inside)
        active.removeChild (c, um);

    // Copy in the alternate's audio for the range, each clip trimmed to it. Added in the
    // alternate's own order, so its overlaps keep the same z-order.
    int copied = 0;
    for (const auto& c : source.getChildWithName (ids::CLIPS))
    {
        const auto s = clip::start (c), e = clip::end (c);
        const auto from = std::max (s, start), to = std::min (e, end);
        if (to <= from)
            continue;
        auto copy = c.createCopy();
        copy.setProperty (ids::id, session.allocateId(), nullptr);
        copy.setProperty (ids::start, from, nullptr);
        copy.setProperty (ids::offset, clip::offset (c) + (from - s), nullptr);
        copy.setProperty (ids::length, to - from, nullptr);
        // A fade is kept only where the copy keeps the clip's own edge, and never longer than the copy.
        copy.setProperty (ids::fadeIn, from == s ? std::min (clip::fadeIn (c), to - from) : (juce::int64) 0, nullptr);
        copy.setProperty (ids::fadeOut, to == e ? std::min (clip::fadeOut (c), to - from - clip::fadeIn (copy)) : (juce::int64) 0,
                          nullptr);
        active.appendChild (copy, um);
        ++copied;
    }
    return copied;
}

} // namespace meridian::playlists
