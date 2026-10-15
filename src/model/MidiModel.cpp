#include "MidiModel.h"

namespace meridian::midimodel
{

midi::TempoMap tempoMap (const Session& session)
{
    midi::TempoMap map (session.getTempo());
    for (const auto& change : session.getState().getChildWithName (ids::TEMPO_MAP))
        if (change.hasType (ids::TEMPO) && (juce::int64) change[ids::tick] > 0)
            map.addChange ((juce::int64) change[ids::tick], (double) change[ids::bpm]);
    return map;
}

namespace
{
juce::ValueTree getOrCreateChild (juce::ValueTree parent, const juce::Identifier& type, juce::UndoManager* um)
{
    auto child = parent.getChildWithName (type);
    if (! child.isValid())
    {
        child = juce::ValueTree (type);
        parent.appendChild (child, um);
    }
    return child;
}
} // namespace

void setTempoChange (Session& session, juce::int64 tick, double bpm, juce::UndoManager* um)
{
    bpm = std::isfinite (bpm) ? juce::jlimit (midi::TempoMap::minBpm, midi::TempoMap::maxBpm, bpm) : 120.0;
    if (tick <= 0)
    {
        session.getState().setProperty (ids::tempo, bpm, um);
        return;
    }
    auto map = getOrCreateChild (session.getState(), ids::TEMPO_MAP, um);
    if (auto existing = map.getChildWithProperty (ids::tick, tick); existing.isValid())
    {
        existing.setProperty (ids::bpm, bpm, um);
        return;
    }
    juce::ValueTree change (ids::TEMPO);
    change.setProperty (ids::tick, tick, nullptr);
    change.setProperty (ids::bpm, bpm, nullptr);
    map.appendChild (change, um);
}

void setMeterChange (Session& session, juce::int64 bar, int numerator, int denominator, juce::UndoManager* um)
{
    // Store what MeterMap will use (it makes invalid meters valid).
    midi::MeterMap check;
    check.addChange (std::max<juce::int64> (0, bar), numerator, denominator);
    const auto valid = check.meterAt (std::max<juce::int64> (0, bar));

    auto map = getOrCreateChild (session.getState(), ids::METER_MAP, um);
    auto change = map.getChildWithProperty (ids::bar, (juce::int64) valid.bar);
    if (! change.isValid())
    {
        change = juce::ValueTree (ids::METER);
        change.setProperty (ids::bar, (juce::int64) valid.bar, nullptr); // int64_t is long on Linux: ambiguous as a var
        change.setProperty (ids::numerator, valid.numerator, nullptr);
        change.setProperty (ids::denominator, valid.denominator, nullptr);
        map.appendChild (change, um);
        return;
    }
    change.setProperty (ids::numerator, valid.numerator, um);
    change.setProperty (ids::denominator, valid.denominator, um);
}

bool removeTempoChange (Session& session, juce::int64 tick, juce::UndoManager* um)
{
    auto map = session.getState().getChildWithName (ids::TEMPO_MAP);
    auto change = tick > 0 ? map.getChildWithProperty (ids::tick, tick) : juce::ValueTree();
    if (! change.isValid())
        return false;
    map.removeChild (change, um);
    return true;
}

bool removeMeterChange (Session& session, juce::int64 bar, juce::UndoManager* um)
{
    auto map = session.getState().getChildWithName (ids::METER_MAP);
    auto change = bar > 0 ? map.getChildWithProperty (ids::bar, bar) : juce::ValueTree();
    if (! change.isValid())
        return false;
    map.removeChild (change, um);
    return true;
}

void clearTempoAndMeterChanges (Session& session, juce::UndoManager* um)
{
    for (const auto* type : { &ids::TEMPO_MAP, &ids::METER_MAP })
        if (auto node = session.getState().getChildWithName (*type); node.isValid())
            session.getState().removeChild (node, um);
}

midi::MeterMap meterMap (const Session& session)
{
    midi::MeterMap map;
    for (const auto& change : session.getState().getChildWithName (ids::METER_MAP))
        if (change.hasType (ids::METER))
            map.addChange ((juce::int64) change[ids::bar], (int) change[ids::numerator], (int) change[ids::denominator]);
    return map;
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

std::vector<midi::PedalRange> readSustain (const juce::ValueTree& clip)
{
    std::vector<midi::PedalRange> ranges;
    for (const auto& p : clip.getChildWithName (ids::SUSTAIN))
        if (p.hasType (ids::PEDAL))
            ranges.push_back ({ (int64_t) (juce::int64) p[ids::start], (int64_t) (juce::int64) p.getProperty (ids::length, 1) });
    return midi::normalisedPedal (std::move (ranges));
}

void writeSustain (juce::ValueTree clip, const std::vector<midi::PedalRange>& ranges, juce::UndoManager* um)
{
    if (auto old = clip.getChildWithName (ids::SUSTAIN); old.isValid())
        clip.removeChild (old, um);
    const auto clean = midi::normalisedPedal (ranges);
    if (clean.empty())
        return;
    juce::ValueTree list (ids::SUSTAIN);
    for (const auto& r : clean)
    {
        juce::ValueTree pedal (ids::PEDAL);
        pedal.setProperty (ids::start, (juce::int64) r.start, nullptr);
        pedal.setProperty (ids::length, (juce::int64) r.length, nullptr);
        list.appendChild (pedal, nullptr);
    }
    clip.appendChild (list, um);
}

juce::Identifier controlNodeType (int control)
{
    return control == midi::pitchBend ? ids::PITCHBEND : ids::MODWHEEL;
}

float controlLow (int control)
{
    return control == midi::pitchBend ? -1.0f : 0.0f;
}

std::vector<midi::ControlPoint> readControl (const juce::ValueTree& clip, int control)
{
    std::vector<midi::ControlPoint> points;
    for (const auto& p : clip.getChildWithName (controlNodeType (control)))
        if (p.hasType (ids::CC))
            points.push_back ({ (int64_t) (juce::int64) p[ids::start], (float) (double) p[ids::value] });
    return midi::normalisedControl (std::move (points), controlLow (control), 1.0f);
}

void writeControl (juce::ValueTree clip, int control, const std::vector<midi::ControlPoint>& points, juce::UndoManager* um)
{
    if (auto old = clip.getChildWithName (controlNodeType (control)); old.isValid())
        clip.removeChild (old, um);
    const auto clean = midi::normalisedControl (points, controlLow (control), 1.0f);
    if (clean.empty())
        return;
    juce::ValueTree list (controlNodeType (control));
    for (const auto& p : clean)
    {
        juce::ValueTree cc (ids::CC);
        cc.setProperty (ids::start, (juce::int64) p.tick, nullptr);
        cc.setProperty (ids::value, (double) p.value, nullptr);
        list.appendChild (cc, nullptr);
    }
    clip.appendChild (list, um);
}

namespace
{
juce::ValueTree makeNoteNode (const midi::Note& note)
{
    // Clamp exactly as NoteSequence does, so the document never holds an invalid note.
    midi::NoteSequence one;
    const auto index = one.add (note);
    const auto& n = one.getNotes()[index];
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
