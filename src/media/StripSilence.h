#pragma once

#include "model/Session.h"

#include <juce_audio_formats/juce_audio_formats.h>

/** Strip Silence: removes the quiet parts of the audio clips in a time range,
    leaving one clip per audible stretch (see dsp::SilenceDetector). The
    clips' own audio is analysed, at their clip gain; fades are ignored.
    The kept clips play exactly what they played before, so only silence
    (below the threshold) goes. */
namespace meridian::stripsilence
{

struct Params
{
    double thresholdDb = -48.0;        ///< quieter than this is silence
    double minSilenceSeconds = 0.25;   ///< shorter quiet stretches are kept (pauses inside a phrase)
    double padBeforeSeconds = 0.005;   ///< kept before each audible stretch (the attack)
    double padAfterSeconds = 0.05;     ///< kept after it (the decay)
};

struct Result
{
    int clipsAnalysed = 0;
    int clipsMade = 0;                 ///< audible stretches left as clips
    juce::int64 samplesRemoved = 0;
    juce::StringArray unreadable;      ///< clips whose audio could not be read (left alone)
};

/** Strips [start, end) of a track's active playlist, recording into `um`. */
Result apply (Session&, juce::AudioFormatManager&, juce::ValueTree track, juce::int64 start, juce::int64 end,
              const Params&, juce::UndoManager*);

} // namespace meridian::stripsilence
