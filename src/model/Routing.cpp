#include "Routing.h"

#include <set>

namespace meridian::routing
{

juce::Array<int> directDestinations (const Session& session, const juce::ValueTree& t)
{
    juce::Array<int> result;
    auto addIfAux = [&] (int targetId)
    {
        const auto target = session.getTrackById (targetId);
        if (targetId != 0 && target.isValid() && track::isAux (target))
            result.addIfNotAlreadyThere (targetId);
    };

    addIfAux (track::outputBus (t));
    for (const auto& s : track::sends (t))
        addIfAux (send::target (s));
    return result;
}

bool feeds (const Session& session, int fromId, int toId)
{
    std::set<int> visited;
    juce::Array<int> stack { fromId };

    while (! stack.isEmpty())
    {
        const int current = stack.removeAndReturn (stack.size() - 1);
        if (! visited.insert (current).second)
            continue;

        for (const int next : directDestinations (session, session.getTrackById (current)))
        {
            if (next == toId)
                return true;
            stack.add (next);
        }
    }
    return false;
}

bool wouldCreateCycle (const Session& session, const juce::ValueTree& source, int targetId)
{
    const int sourceId = track::id (source);
    return targetId == sourceId || feeds (session, targetId, sourceId);
}

juce::Array<juce::ValueTree> validTargets (const Session& session, const juce::ValueTree& source)
{
    juce::Array<juce::ValueTree> result;
    for (const auto& t : session.getTracksNode())
        if (track::isAux (t) && ! wouldCreateCycle (session, source, track::id (t)))
            result.add (t);
    return result;
}

juce::ValueTree findSend (const juce::ValueTree& t, int slot)
{
    return track::sends (t).getChildWithProperty (ids::slot, slot);
}

bool setSend (Session& session, juce::ValueTree t, int slot, int targetId, juce::UndoManager* um)
{
    if (! juce::isPositiveAndBelow (slot, Session::maxSends) || wouldCreateCycle (session, t, targetId))
        return false;

    if (auto existing = findSend (t, slot); existing.isValid())
    {
        existing.setProperty (ids::target, targetId, um);
        return true;
    }

    juce::ValueTree s (ids::SEND);
    s.setProperty (ids::slot, slot, nullptr);
    s.setProperty (ids::target, targetId, nullptr);
    s.setProperty (ids::levelDb, 0.0, nullptr);
    s.setProperty (ids::pan, 0.0, nullptr);
    s.setProperty (ids::preFader, false, nullptr);
    s.setProperty (ids::mute, false, nullptr);
    track::sends (t).appendChild (s, um);
    return true;
}

void removeSend (juce::ValueTree t, int slot, juce::UndoManager* um)
{
    if (auto existing = findSend (t, slot); existing.isValid())
        track::sends (t).removeChild (existing, um);
}

bool setOutput (Session& session, juce::ValueTree t, int targetId, juce::UndoManager* um)
{
    if (targetId != 0 && wouldCreateCycle (session, t, targetId))
        return false;
    t.setProperty (ids::outputBus, targetId, um);
    return true;
}

} // namespace meridian::routing
