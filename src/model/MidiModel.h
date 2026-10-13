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

/** An instrument track's SYNTH node as synth parameters (defaults for missing values). */
synth::PolySynth::Params readSynthParams (const juce::ValueTree& synthNode);

/** A drum track's SYNTH node as drum-machine parameters. */
synth::DrumSynth::Params readDrumParams (const juce::ValueTree& synthNode);

/** Waveform names as stored in the session: "sine", "saw", "square". */
juce::String waveformName (synth::PolySynth::Waveform);
synth::PolySynth::Waveform waveformFromName (const juce::String&);

} // namespace meridian::midimodel
