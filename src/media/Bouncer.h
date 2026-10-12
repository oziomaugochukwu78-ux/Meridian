#pragma once

#include "engine/AudioEngine.h"

#include <dsp/SignalAnalysis.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

namespace meridian
{

/** Bounce (mixdown) of a timeline range to a stereo audio file. */
struct BounceRequest
{
    juce::int64 start = 0;          ///< timeline samples
    juce::int64 end = 0;
    juce::File destination;
    double sampleRate = 48000.0;
    int bitDepth = 24;              ///< 16, 24 or 32 (float)
    bool aiff = false;
};

struct BounceResult
{
    bool ok = false;
    juce::String error;
    juce::File file;
    juce::int64 length = 0;
    dsp::LevelStats left, right;    ///< measured on the rendered (pre-quantisation) signal
};

/** Renders [start, end) through `offlineEngine`, which must already be
    prepared, in offline mode, and hold the session's graph.

    The output is late by the compensated plug-in latency plus the console's
    oversampling filters; the render runs that much longer and drops the
    head, so the file is sample-aligned with the timeline. Blocking;
    `progress` may return false to cancel. */
BounceResult renderBounce (AudioEngine& offlineEngine, const BounceRequest&, const std::function<bool (double)>& progress);

} // namespace meridian
