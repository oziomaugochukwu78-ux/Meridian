#include "MidiModel.h"

namespace meridian::midimodel
{

midi::TempoMap tempoMap (const Session& session)
{
    return midi::TempoMap (session.getTempo());
}

midi::NoteSequence readNotes (const juce::ValueTree& clip)
{
    midi::NoteSequence sequence;
    for (const auto& n : clip::notes (clip))
        if (n.hasType (ids::NOTE))
            sequence.add ({ (int) n[ids::pitch], (float) (double) n.getProperty (ids::velocity, 0.8),
                            // via juce::int64: on Linux int64_t is long, which juce::var does not convert to directly
                            (int64_t) (juce::int64) n[ids::start],
                            (int64_t) (juce::int64) n.getProperty (ids::length, (juce::int64) midi::ticksPerQuarter) });
    return sequence;
}

namespace
{
juce::ValueTree makeNoteNode (const midi::Note& note)
{
    // Clamp exactly as NoteSequence does, so the document never holds an invalid note.
    midi::NoteSequence one;
    const auto& n = one.getNotes()[one.add (note)];
    juce::ValueTree node (ids::NOTE);
    node.setProperty (ids::pitch, n.pitch, nullptr);
    node.setProperty (ids::velocity, (double) n.velocity, nullptr);
    node.setProperty (ids::start, (juce::int64) n.start, nullptr);
    node.setProperty (ids::length, (juce::int64) n.length, nullptr);
    return node;
}

juce::ValueTree getOrCreateNotesNode (juce::ValueTree clip, juce::UndoManager* um)
{
    auto notes = clip::notes (clip);
    if (! notes.isValid())
    {
        notes = juce::ValueTree (ids::NOTES);
        clip.appendChild (notes, um);
    }
    return notes;
}
} // namespace

juce::ValueTree addNote (juce::ValueTree clip, const midi::Note& note, juce::UndoManager* um)
{
    auto node = makeNoteNode (note);
    getOrCreateNotesNode (clip, um).appendChild (node, um);
    return node;
}

void writeNotes (juce::ValueTree clip, const midi::NoteSequence& sequence, juce::UndoManager* um)
{
    auto notes = getOrCreateNotesNode (clip, um);
    notes.removeAllChildren (um);
    for (const auto& n : sequence.getNotes())
        notes.appendChild (makeNoteNode (n), um);
}

juce::String waveformName (synth::PolySynth::Waveform w)
{
    switch (w)
    {
        case synth::PolySynth::Waveform::sine:   return "sine";
        case synth::PolySynth::Waveform::square: return "square";
        case synth::PolySynth::Waveform::saw:
        default:                                 return "saw";
    }
}

synth::PolySynth::Waveform waveformFromName (const juce::String& name)
{
    if (name.equalsIgnoreCase ("sine"))   return synth::PolySynth::Waveform::sine;
    if (name.equalsIgnoreCase ("square")) return synth::PolySynth::Waveform::square;
    return synth::PolySynth::Waveform::saw;
}

synth::PolySynth::Params readSynthParams (const juce::ValueTree& s)
{
    synth::PolySynth::Params p; // defaults for anything missing; setParams clamps the rest
    p.waveform = waveformFromName (s.getProperty (ids::waveform, "saw").toString());
    p.detuneCents = s.getProperty (ids::detune, p.detuneCents);
    p.attack = s.getProperty (ids::attack, p.attack);
    p.decay = s.getProperty (ids::decay, p.decay);
    p.sustain = s.getProperty (ids::sustain, p.sustain);
    p.release = s.getProperty (ids::release, p.release);
    p.cutoffHz = s.getProperty (ids::cutoff, p.cutoffHz);
    p.resonance = s.getProperty (ids::resonance, p.resonance);
    p.filterEnvOctaves = s.getProperty (ids::filterEnv, p.filterEnvOctaves);
    p.gainDb = s.getProperty (ids::gainDb, p.gainDb);
    return p;
}

synth::DrumSynth::Params readDrumParams (const juce::ValueTree& s)
{
    synth::DrumSynth::Params p; // setParams clamps
    p.tuneSemitones = s.getProperty (ids::tune, p.tuneSemitones);
    p.decayScale = s.getProperty (ids::decayScale, p.decayScale);
    p.tone = s.getProperty (ids::tone, p.tone);
    p.gainDb = s.getProperty (ids::gainDb, p.gainDb);
    return p;
}

} // namespace meridian::midimodel
