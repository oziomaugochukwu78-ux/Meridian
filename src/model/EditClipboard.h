#pragma once

#include "Session.h"

#include <vector>

/** Cut, Copy, Paste and Duplicate in the Edit window.

    A copy holds, for each copied track (top to bottom), the clips inside the
    copied time span, trimmed to it, and the track's automation over the span
    ("automation follows edit"). Times in the copy are relative to the span's
    start, so it can be pasted anywhere.

    Paste in Slip, Grid and Spot modes replaces what is under the pasted span;
    in Shuffle mode it inserts, moving everything later (clips and automation)
    right by the span's length. Every function records into the given
    UndoManager, so the caller's transaction makes it one undo step. */
namespace meridian::clipboard
{

struct Lane
{
    int sourceTrackId = 0;
    bool midi = false;                       ///< copied from an instrument track
    juce::Array<juce::ValueTree> clips;      ///< deep copies; start relative to the span
    juce::ValueTree automation;              ///< AUTOMATION copy: lanes with points relative to the span
};

struct Contents
{
    juce::int64 length = 0;                  ///< the copied span, samples
    std::vector<Lane> lanes;

    bool isEmpty() const noexcept { return length <= 0 || lanes.empty(); }
};

/** Copies [start, end) of each track. */
Contents copyRange (const juce::Array<juce::ValueTree>& tracks, juce::int64 start, juce::int64 end);

/** Copies one clip (its span, with its track's automation over it). */
Contents copyClip (const juce::ValueTree& clip);

/** Removes [start, end) from a track's active playlist: clips crossing the
    edges are split there, and what lies inside is removed. Automation is untouched. */
void clearRange (Session&, juce::ValueTree track, juce::int64 start, juce::int64 end, juce::UndoManager*);

struct PasteResult
{
    int clipsPasted = 0;
    int lanesSkipped = 0;                    ///< audio onto an instrument track or MIDI onto an audio track
    juce::Array<int> trackIds;               ///< the tracks pasted onto, top to bottom
};

/** Pastes at `at`: lane i goes to tracks[i]; with fewer tracks than lanes the
    extra lanes go to the tracks below the last one, in session order. Aux
    tracks are skipped over. Automation is pasted too: volume, pan and mute
    always, sends where the target has that send, and plug-in controls only
    onto the track they were copied from. */
PasteResult paste (Session&, const Contents&, juce::Array<juce::ValueTree> tracks, juce::int64 at,
                   bool shuffle, juce::UndoManager*);

} // namespace meridian::clipboard
