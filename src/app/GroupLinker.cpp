#include "GroupLinker.h"

#include "model/Groups.h"

#include <dsp/FaderTaper.h>

#include <cmath>

namespace meridian
{

GroupLinker::~GroupLinker()
{
    attach (nullptr);
}

void GroupLinker::attach (Session* s)
{
    if (session != nullptr)
        session->getState().removeListener (this);
    session = s;
    faderDb.clear();
    if (session == nullptr)
        return;
    session->getState().addListener (this);
    for (const auto& t : session->getTracksNode())
        remember (t);
}

void GroupLinker::remember (const juce::ValueTree& t)
{
    faderDb[track::id (t)] = track::volumeDb (t);
}

void GroupLinker::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (ids::TRACK))
        remember (child);
}

void GroupLinker::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (session == nullptr || ! tree.hasType (ids::TRACK))
        return;
    const bool fader = property == ids::volumeDb;
    if (! fader && property != ids::mute && property != ids::solo && property != ids::recordArm && property != ids::automationMode)
        return;

    const int id = track::id (tree);
    const auto known = faderDb.find (id);
    const double before = known != faderDb.end() ? known->second : track::volumeDb (tree);
    if (fader)
        remember (tree);

    auto& um = session->getUndoManager();
    if (following || um.isPerformingUndoRedo())
        return;
    const auto followers = groups::linkedTracks (*session, id, true);
    if (followers.isEmpty())
        return;

    const juce::ScopedValueSetter<bool> leading (following, true);
    const double change = track::volumeDb (tree) - before;
    for (const int other : followers)
    {
        auto t = session->getTrackById (other);
        if (! t.isValid())
            continue;
        if (fader)
        {
            if (! std::isfinite (change) || juce::exactlyEqual (change, 0.0))
                continue;
            const double moved = juce::jlimit (dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, track::volumeDb (t) + change);
            t.setProperty (ids::volumeDb, moved, &um);
        }
        else if (property == ids::recordArm && track::isAux (t))
        {
            continue; // an Aux Input has no recording
        }
        else if (t[property] != tree[property])
        {
            t.setProperty (property, tree[property], &um);
        }
    }
}

} // namespace meridian
