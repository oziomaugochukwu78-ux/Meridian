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
        TRACK  id name channels colour volumeDb pan mute solo recordArm
               inputChannel automationMode
          CLIPS
            CLIP  id name file start length offset sourceLength gainDb fadeIn fadeOut
          INSERTS
            INSERT  slot description state bypass
          AUTOMATION
            LANE  parameter
              POINT  time value

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
MERIDIAN_ID (AUTOMATION)
MERIDIAN_ID (LANE)
MERIDIAN_ID (POINT)

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

#undef MERIDIAN_ID
} // namespace meridian::ids
