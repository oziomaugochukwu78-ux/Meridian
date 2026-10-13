#pragma once

#include <juce_data_structures/juce_data_structures.h>

/** Schema of the session document.

    The session is a juce::ValueTree: a headless, observable, undoable tree that
    the UI, the engine bridge and the remote-control layer all read and mutate
    on the message thread. The audio thread never touches it; it renders from
    immutable snapshots built from it (see engine/RenderGraph.h).

    SESSION  name sampleRate bitDepth fileFormat formatVersion nextId
             masterVolumeDb consoleEnabled consoleDrive consoleCharacter consoleCurve
      TRACKS
        TRACK  id name kind channels colour volumeDb pan mute solo recordArm
               inputChannel automationMode outputBus
          CLIPS
            CLIP  id name file start length offset sourceLength gainDb fadeIn fadeOut
          INSERTS
            INSERT  slot description state bypass
          SENDS
            SEND  slot target levelDb pan preFader mute

    kind is "audio" (plays clips, records), "aux" (an Aux Input: a stereo bus
    fed by sends and by tracks whose outputBus names it) or "instrument" (a
    stereo track whose MIDI clips play its built-in synth). outputBus is the id
    of the aux track this track feeds, or 0 for the master bus. A SEND's target
    is an aux track id. Routing never forms a cycle (see model/Routing.h).
          AUTOMATION
            LANE  parameter
              POINT  time value
          SYNTH  engine ("synth" or "drums") gainDb
                 synth: waveform detune attack decay sustain release cutoff resonance filterEnv
                 drums: tune decayScale tone        (instrument tracks only)

    MIDI clips are CLIP nodes with kind="midi" on instrument tracks. They have
    no file; start/length/offset are timeline samples as for audio clips, and
    their notes are positioned in ticks (960 per quarter note) from the clip's
    content origin (start - offset), so they follow the session tempo:
            CLIP  kind="midi" ...
              NOTES
                NOTE  pitch velocity start length       (ticks)

    SESSION also holds tempo (BPM, default 120), loop playback (loopEnabled,
    loopStart, loopEnd in samples), punch recording (punchEnabled, punchIn, punchOut), the click (clickEnabled, clickLevelDb),
    countInBars (0 = none; counts in before recording), and markers:
      MARKERS
        MARKER  number name start      (start in samples)
      TEMPO_MAP                        (tempo changes after the start; SESSION.tempo is the first)
        TEMPO  tick bpm
      METER_MAP                        (time signatures; 4/4 unless changed)
        METER  bar numerator denominator

    All times are in samples at the session sample rate. Child order of CLIPS is
    z-order: later clips cover earlier ones where they overlap.
*/
namespace meridian::ids
{
#define MERIDIAN_ID(name) inline const juce::Identifier name { #name };

// Node types
MERIDIAN_ID (SESSION)
MERIDIAN_ID (TRACKS)
MERIDIAN_ID (TRACK)
MERIDIAN_ID (CLIPS)
MERIDIAN_ID (CLIP)
MERIDIAN_ID (INSERTS)
MERIDIAN_ID (INSERT)
MERIDIAN_ID (SENDS)
MERIDIAN_ID (SEND)
MERIDIAN_ID (AUTOMATION)
MERIDIAN_ID (LANE)
MERIDIAN_ID (POINT)
MERIDIAN_ID (NOTES)
MERIDIAN_ID (NOTE)
MERIDIAN_ID (SYNTH)
MERIDIAN_ID (MARKERS)
MERIDIAN_ID (MARKER)
MERIDIAN_ID (TEMPO_MAP)
MERIDIAN_ID (TEMPO)
MERIDIAN_ID (METER_MAP)
MERIDIAN_ID (METER)

// SESSION
MERIDIAN_ID (name)
MERIDIAN_ID (sampleRate)
MERIDIAN_ID (bitDepth)
MERIDIAN_ID (fileFormat)
MERIDIAN_ID (formatVersion)
MERIDIAN_ID (nextId)
MERIDIAN_ID (masterVolumeDb)
MERIDIAN_ID (consoleEnabled)
MERIDIAN_ID (consoleDrive)
MERIDIAN_ID (consoleCharacter)
MERIDIAN_ID (consoleCurve)
MERIDIAN_ID (tempo)
MERIDIAN_ID (loopEnabled)
MERIDIAN_ID (loopStart)
MERIDIAN_ID (loopEnd)
MERIDIAN_ID (punchEnabled)
MERIDIAN_ID (punchIn)
MERIDIAN_ID (punchOut)
MERIDIAN_ID (clickEnabled)
MERIDIAN_ID (clickLevelDb)
MERIDIAN_ID (countInBars)
MERIDIAN_ID (number)       // MARKER: 1, 2, 3... (unique in the session)
MERIDIAN_ID (tick)         // TEMPO: where the change happens, in ticks (960 per quarter note)
MERIDIAN_ID (bpm)
MERIDIAN_ID (bar)          // METER: the 0-based bar the change starts
MERIDIAN_ID (numerator)
MERIDIAN_ID (denominator)

// TRACK
MERIDIAN_ID (id)
MERIDIAN_ID (channels)
MERIDIAN_ID (colour)
MERIDIAN_ID (volumeDb)
MERIDIAN_ID (pan)
MERIDIAN_ID (mute)
MERIDIAN_ID (solo)
MERIDIAN_ID (recordArm)
MERIDIAN_ID (inputChannel)
MERIDIAN_ID (automationMode)
MERIDIAN_ID (kind)
MERIDIAN_ID (outputBus)

// SEND
MERIDIAN_ID (target)
MERIDIAN_ID (levelDb)
MERIDIAN_ID (preFader)

// CLIP
MERIDIAN_ID (file)
MERIDIAN_ID (start)
MERIDIAN_ID (length)
MERIDIAN_ID (offset)
MERIDIAN_ID (sourceLength)
MERIDIAN_ID (gainDb)
MERIDIAN_ID (fadeIn)
MERIDIAN_ID (fadeOut)

// INSERT
MERIDIAN_ID (slot)
MERIDIAN_ID (description)
MERIDIAN_ID (state)
MERIDIAN_ID (bypass)

// LANE / POINT
MERIDIAN_ID (parameter)
MERIDIAN_ID (time)
MERIDIAN_ID (value)

// NOTE (start and length reuse the CLIP identifiers, in ticks)
MERIDIAN_ID (pitch)
MERIDIAN_ID (velocity)

// SYNTH
MERIDIAN_ID (waveform)
MERIDIAN_ID (detune)
MERIDIAN_ID (attack)
MERIDIAN_ID (decay)
MERIDIAN_ID (sustain)
MERIDIAN_ID (release)
MERIDIAN_ID (cutoff)
MERIDIAN_ID (resonance)
MERIDIAN_ID (filterEnv)
MERIDIAN_ID (engine)        // "synth" (PolySynth) or "drums" (DrumSynth)
MERIDIAN_ID (tune)          // drums: semitones
MERIDIAN_ID (decayScale)    // drums
MERIDIAN_ID (tone)          // drums: 0 dark .. 1 bright

#undef MERIDIAN_ID
} // namespace meridian::ids
