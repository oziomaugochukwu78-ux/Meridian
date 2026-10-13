#pragma once

#include "Session.h"

/** Playlists: alternate versions of a track's clips, and comping between them.

    A track always has one active playlist, its CLIPS: the engine plays it and
    the Edit window edits it, exactly as for a track without playlists. The
    others wait under PLAYLISTS, each a PLAYLIST with a name and its own CLIPS.
    The active playlist's name is TRACK.playlist ("<track>.01" if never set).

    Switching moves the active clips into a PLAYLIST and the chosen one's into
    CLIPS, so ids stay unique and nothing is copied. Comping (promote) copies a
    time range of an alternate onto the active playlist, replacing what was
    there: the classic way to build one performance from several takes.

    Every function is one undoable edit when given the session's UndoManager. */
namespace meridian::playlists
{

/** The active playlist's name. */
juce::String activeName (const juce::ValueTree& track);

/** Every playlist name, the active one included, sorted (so "Vocal.01", "Vocal.02"...). */
juce::StringArray names (const juce::ValueTree& track);

/** The alternate playlist with this name (invalid if none, or if it is the active one). */
juce::ValueTree find (const juce::ValueTree& track, const juce::String& name);

/** The next free name: "<track>.02", "<track>.03"... */
juce::String nextName (const juce::ValueTree& track);

/** Stores the active playlist and makes a new one active: empty, or (duplicate) a copy of
    it with new clip ids. Returns the new playlist's name. */
juce::String create (Session&, juce::ValueTree track, bool duplicate, juce::UndoManager*);

/** Makes `name` the active playlist (the current one is stored). False if there is none by that name. */
bool activate (juce::ValueTree track, const juce::String& name, juce::UndoManager*);

/** Deletes an alternate playlist (never the active one). */
bool remove (juce::ValueTree track, const juce::String& name, juce::UndoManager*);

/** Renames a playlist (active or not). False if `newName` is empty or taken. */
bool rename (juce::ValueTree track, const juce::String& oldName, const juce::String& newName, juce::UndoManager*);

/** Comping: what `name` has in [start, end) replaces what the active playlist has there.
    The active clips are split at start and end and the part inside removed; the
    alternate's clips are copied in, trimmed to the range (a copy keeps its fade only
    where it keeps its own edge). Returns the number of clips copied, or -1 if there is
    no such playlist or the range is empty. */
int promote (Session&, juce::ValueTree track, const juce::String& name, juce::int64 start, juce::int64 end, juce::UndoManager*);

} // namespace meridian::playlists
