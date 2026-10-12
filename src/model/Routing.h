#pragma once

#include "Session.h"

namespace meridian::routing
{

/** Ids of the aux tracks this track feeds directly: its output bus (if not
    the master) and every send target. Missing targets are ignored. */
juce::Array<int> directDestinations (const Session&, const juce::ValueTree& track);

/** True if audio from track `fromId` reaches track `toId` through outputs
    and sends (directly or via other buses). */
bool feeds (const Session&, int fromId, int toId);

/** True if routing `source` into aux `targetId` would make audio loop back
    into itself (or targets itself). Such routes are refused everywhere. */
bool wouldCreateCycle (const Session&, const juce::ValueTree& source, int targetId);

/** Aux tracks `source` may route to (output or send) without a cycle. */
juce::Array<juce::ValueTree> validTargets (const Session&, const juce::ValueTree& source);

juce::ValueTree findSend (const juce::ValueTree& track, int slot);

/** Creates or retargets the send in `slot` (undoable). Returns false, and
    changes nothing, if the route would create a cycle. */
bool setSend (Session&, juce::ValueTree track, int slot, int targetId, juce::UndoManager*);
void removeSend (juce::ValueTree track, int slot, juce::UndoManager*);

/** Sets the track's output (0 = master). Refuses cycles. */
bool setOutput (Session&, juce::ValueTree track, int targetId, juce::UndoManager*);

} // namespace meridian::routing
