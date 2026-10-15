#pragma once

#include "Session.h"

/** Shuffle mode edits: clips on a track stay butted together. Removing
    material closes the gap (everything later moves left), lengthening a clip
    pushes everything later to the right, and moving a clip takes it out of
    the sequence and puts it back at another clip boundary.

    These work on a track's active playlist only. Delete and trim also move
    the track's automation after the edit, so it stays with the audio; a
    shuffle move leaves automation where it is, as a Slip move does.

    Every function records into the given UndoManager, so a caller's
    transaction makes the whole shuffle one undo step. */
namespace meridian::shuffle
{

/** Moves every clip in `clips` that starts at or after `from` by `delta`
    samples (clamped at 0), except `except`. */
void shiftClips (juce::ValueTree clips, juce::int64 from, juce::int64 delta, juce::UndoManager*,
                 const juce::ValueTree& except = {});

/** Moves the track's automation breakpoints at or after `from` by `delta`.
    When delta is negative, the time [from + delta, from) is gone, and so are
    the breakpoints in it. */
void shiftAutomation (juce::ValueTree track, juce::int64 from, juce::int64 delta, juce::UndoManager*);

/** Removes `clip` and closes its gap: later clips and automation move left by its length. */
void deleteClip (juce::ValueTree clip, juce::UndoManager*);

/** After [start, end) has been cleared on `track`, closes the gap: later clips
    and automation move left by end - start. */
void closeRange (juce::ValueTree track, juce::int64 start, juce::int64 end, juce::UndoManager*);

/** Takes `clip` out of its sequence (closing its gap) and inserts it at the
    clip boundary (another clip's start or end, or 0) nearest `target`, moving
    the clips from there on right by its length. Returns its new start. */
juce::int64 moveClip (juce::ValueTree clip, juce::int64 target, juce::UndoManager*);

} // namespace meridian::shuffle
