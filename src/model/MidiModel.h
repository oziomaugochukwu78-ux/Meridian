#pragma once

#include "Session.h"

#include <midi/MeterMap.h>
#include <midi/NoteSequence.h>
#include <midi/TempoMap.h>
#include <synth/DrumSynth.h>
#include <synth/PolySynth.h>

namespace meridian::midimodel
{

/** The session's tempo: its starting tempo plus every TEMPO change. */
midi::TempoMap tempoMap (const Session&);

/** The session's time signatures (4/4 unless changed). */
midi::MeterMap meterMap (const Session&);

/** A tempo from `tick` on (tick 0 sets the session's starting tempo). Replaces a change at the same tick. Undoable. */
void setTempoChange (Session&, juce::int64 tick, double bpm, juce::UndoManager*);
/** A time signature from the start of `bar` (0-based). Replaces a change at the same bar. Undoable. */
void setMeterChange (Session&, juce::int64 bar, int numerator, int denominator, juce::UndoManager*);
/** Back to one tempo (the starting one) and 4/4. */
void clearTempoAndMeterChanges (Session&, juce::UndoManager*);

/** Makes the tempo at `tick` (0 = the starting tempo) glide to the next change's, or hold. False if
    there is no change there. Undoable. */
bool setTempoRamp (Session&, juce::int64 tick, bool ramp, juce::UndoManager*);

/** Removes the tempo change at `tick` (never the starting tempo). False if there is none. Undoable. */
bool removeTempoChange (Session&, juce::int64 tick, juce::UndoManager*);
/** Removes the time signature starting at `bar` (never bar 0's). False if there is none. Undoable. */
bool removeMeterChange (Session&, juce::int64 bar, juce::UndoManager*);

/** A clip's NOTE children as a (sorted, validated) sequence. */
midi::NoteSequence readNotes (const juce::ValueTree& clip);

/** Adds one NOTE to a MIDI clip (undoable). Values are clamped like NoteSequence. */
juce::ValueTree addNote (juce::ValueTree clip, const midi::Note&, juce::UndoManager*);

/** Replaces every note of a MIDI clip (undoable). */
void writeNotes (juce::ValueTree clip, const midi::NoteSequence&, juce::UndoManager*);

/** Where a MIDI clip holds the sustain pedal (normalised; empty if never). */
std::vector<midi::PedalRange> readSustain (const juce::ValueTree& clip);

/** Replaces a MIDI clip's sustain-pedal ranges (undoable); normalises them first. */
void writeSustain (juce::ValueTree clip, const std::vector<midi::PedalRange>&, juce::UndoManager*);

/** The wheels: midi::pitchBend (values -1..+1) or midi::modWheel (0..1). */
juce::Identifier controlNodeType (int control);
float controlLow (int control);

/** A MIDI clip's pitch-wheel or mod-wheel moves (normalised; empty if none). */
std::vector<midi::ControlPoint> readControl (const juce::ValueTree& clip, int control);

/** Replaces them (undoable); normalises them first. */
void writeControl (juce::ValueTree clip, int control, const std::vector<midi::ControlPoint>&, juce::UndoManager*);

/** An instrument track's SYNTH node as synth parameters (defaults for missing values). */
synth::PolySynth::Params readSynthParams (const juce::ValueTree& synthNode);

/** A drum track's SYNTH node as drum-machine parameters. */
synth::DrumSynth::Params readDrumParams (const juce::ValueTree& synthNode);

/** Waveform names as stored in the session: "sine", "saw", "square". */
juce::String waveformName (synth::PolySynth::Waveform);
synth::PolySynth::Waveform waveformFromName (const juce::String&);

} // namespace meridian::midimodel
