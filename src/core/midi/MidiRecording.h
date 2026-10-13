#pragma once

#include "NoteSequence.h"
#include "TempoMap.h"

#include <cstdint>
#include <vector>

namespace meridian::midi
{

/** One note-on or note-off captured while recording, at the timeline sample
    where the engine received it. Pitch sustainPedal is the pedal (on = down). */
struct RecordedEvent
{
    int64_t time = 0;
    int pitch = 60;
    float velocity = 0.0f; ///< 0..1; a note-on with velocity 0 is a note-off
    bool on = false;
};

/** A finished MIDI take: where its clip goes, the notes in it and where the
    sustain pedal was held (ticks from the clip start). */
struct RecordedTake
{
    int64_t clipStart = 0;
    int64_t clipLength = 0;
    std::vector<Note> notes;
    std::vector<PedalRange> sustain;
};

/** Turns captured events into a take.

    `compensation` is how late the engine received what the player meant
    (output latency, the engine's own latency, and the one block that live
    input is delayed by); every event and the clip itself move earlier by it.
    Notes pair note-on with the next note-off of the same pitch; a note-on
    for a pitch already sounding ends the previous one there; notes still held
    when recording stops end at the stop. The pedal pairs down with the next
    up the same way (a pedal still down at the stop ends there). Events outside
    the take are clamped into it; out-of-range pitches are ignored. */
RecordedTake buildTake (std::vector<RecordedEvent> events, int64_t recordStart, int64_t recordEnd,
                        int64_t compensation, const TempoMap&, double sampleRate);

} // namespace meridian::midi
