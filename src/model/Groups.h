#pragma once

#include "Session.h"

/** Mix and edit groups: tracks that move together.

      SESSION
        GROUPS
          GROUP  id name active mix edit members      (members: track ids, "3,5,9")

    A mix group links its members' faders (relatively: moving one fader moves
    the others by the same number of dB), mute, solo, record arm and
    automation mode. An edit group links selection: selecting a time range or
    a track name on one member selects it on all of them, so every edit
    (cut, paste, fades, nudge, separate...) applies to the whole group. A group
    can be both. An inactive group links nothing until it is made active again.

    The linking itself happens in app/GroupLinker (mix) and the Edit window
    (edit); this namespace is the document part. */
namespace meridian::groups
{

juce::ValueTree groupsNode (const juce::ValueTree& sessionState);   ///< may be invalid (no groups yet)

/** A new group of these tracks (aux tracks too), at least two of them.
    Returns the GROUP node, or an invalid tree if there were fewer than two. */
juce::ValueTree create (Session&, const juce::String& name, const juce::Array<int>& trackIds, bool mix, bool edit,
                        juce::UndoManager*);

void remove (Session&, const juce::ValueTree& group, juce::UndoManager*);
juce::ValueTree findByName (const Session&, const juce::String& name);   ///< case-insensitive
juce::ValueTree findById (const Session&, int groupId);

juce::Array<int> members (const juce::ValueTree& group);
inline bool isActive (const juce::ValueTree& g)  { return g.getProperty (ids::active, true); }
inline bool linksMix (const juce::ValueTree& g)  { return g.getProperty (ids::mix, true); }
inline bool linksEdit (const juce::ValueTree& g) { return g.getProperty (ids::edit, true); }

/** The other tracks linked to `trackId` through its active mix (or edit) groups. */
juce::Array<int> linkedTracks (const Session&, int trackId, bool mix);

/** `trackIds` plus every track linked to them through active edit groups. */
juce::Array<int> expandForEdit (const Session&, const juce::Array<int>& trackIds);

/** The names of the groups a track is in ("Drums, Vox"), for its header. */
juce::String namesFor (const Session&, int trackId);

/** Drops members that are not tracks of the session (or repeated) and groups
    left with fewer than two members (run when a session is opened). Returns
    what it did. Until then such members are simply ignored. */
juce::StringArray repair (Session&);
/** A malformed group (unreadable members, no name), or empty. */
juce::String findProblem (const Session&);

} // namespace meridian::groups
