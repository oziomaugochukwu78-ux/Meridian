#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

namespace meridian
{

/** Elastic Audio: time compression/expansion and pitch shifting of clips.

    Rendering is offline: Rubber Band's highest-quality "finer" engine analyses
    the whole clip in a study pass and then renders it to a new file, which
    the clip then plays. This is the artifact-free path (the equivalent of a
    "rendered" elastic mode). It runs on a background thread and leaves the
    original file untouched, so the edit is undoable. */
struct TimeStretchRequest
{
    juce::File source;
    juce::int64 sourceStart = 0;
    juce::int64 sourceLength = 0;
    juce::File destination;

    double timeRatio = 1.0;        ///< output length / input length (2.0 = twice as long)
    double pitchSemitones = 0.0;   ///< -24 .. +24
    bool preserveFormants = false; ///< keep vocal character when pitch shifting

    double sampleRate = 48000.0;
    int bitDepth = 24;
    bool aiff = false;
};

struct TimeStretchResult
{
    bool ok = false;
    juce::String error;
    juce::int64 length = 0;
};

bool isTimeStretchAvailable() noexcept;
juce::String timeStretchEngineName();

/** Blocking; `progress` receives 0..1 and may return false to cancel. */
TimeStretchResult renderTimeStretch (const TimeStretchRequest&, juce::AudioFormatManager&,
                                     const std::function<bool (double)>& progress);

} // namespace meridian
