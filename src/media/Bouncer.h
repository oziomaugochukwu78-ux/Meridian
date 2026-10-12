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
    bool aiff = false;              ///< (older form of format = "aiff")
    juce::String format;            ///< "wav", "aiff" or "flac" (FLAC: 16 or 24 bits); empty = from `aiff`

    /** TPDF dither when writing 16 or 24 bits (dsp::Dither; seeded, so reproducible). */
    bool dither = false;

    /** Loudness normalisation: the whole mix is gained to targetLufs
        (integrated, EBU R128), but never so far that the true peak passes
        ceilingDbtp; then the result says it was limited by the ceiling.
        Renders a 32-bit float pass first, then writes the file. */
    bool normalise = false;
    double targetLufs = -14.0;
    double ceilingDbtp = -1.0;
};

struct BounceResult
{
    bool ok = false;
    juce::String error;
    juce::File file;
    juce::int64 length = 0;
    dsp::LevelStats left, right;    ///< measured on the mix as written (before quantisation)

    // Loudness of the file as written (EBU R128 / BS.1770-4).
    double integratedLufs = -200.0;
    double loudnessRange = 0.0;      ///< LU
    double truePeakDbtp = -200.0;
    double appliedGainDb = 0.0;      ///< normalisation gain
    bool limitedByCeiling = false;   ///< the target was not reached: the true peak would have passed the ceiling
};

/** Creates a writer: format "wav", "aiff" or "flac"; bitDepth 16, 24 or 32 (float; not FLAC). */
std::unique_ptr<juce::AudioFormatWriter> createBounceWriter (const juce::File&, double sampleRate, int numChannels, int bitDepth,
                                                            const juce::String& format, juce::String& error);

/** Renders [start, end) through `offlineEngine`, which must already be
    prepared, in offline mode, and hold the session's graph.

    The output is late by the compensated plug-in latency plus the console's
    oversampling filters; the render runs that much longer and drops the
    head, so the file is sample-aligned with the timeline. Blocking;
    `progress` may return false to cancel. */
BounceResult renderBounce (AudioEngine& offlineEngine, const BounceRequest&, const std::function<bool (double)>& progress);

} // namespace meridian
