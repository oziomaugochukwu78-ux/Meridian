#pragma once

#include "Session.h"

#include <midi/NoteSequence.h>
#include <midi/TempoMap.h>
#include <synth/DrumSynth.h>
#include <synth/PolySynth.h>

namespace meridian::midimodel
{

/** The session's tempo as a TempoMap (one tempo for now). */
midi::TempoMap tempoMap (const Session&);

/** A clip's NOTE children as a (sorted, validated) sequence. */
midi::NoteSequence readNotes (const juce::ValueTree& clip);

/** Adds one NOTE to a MIDI clip (undoable). Values are clamped like NoteSequence. */
juce::ValueTree addNote (juce::ValueTree clip, const midi::Note&, juce::UndoManager*);

/** Replaces every note of a MIDI clip (undoable). */
void writeNotes (juce::ValueTree clip, const midi::NoteSequence&, juce::UndoManager*);

/** An instrument track's SYNTH node as synth parameters (defaults for missing values). */
synth::PolySynth::Params readSynthParams (const juce::ValueTree& synthNode);

/** A drum track's SYNTH node as drum-machine parameters. */
synth::DrumSynth::Params readDrumParams (const juce::ValueTree& synthNode);

/** Waveform names as stored in the session: "sine", "saw", "square". */
juce::String waveformName (synth::PolySynth::Waveform);
synth::PolySynth::Waveform waveformFromName (const juce::String&);

} // namespace meridian::midimodel
