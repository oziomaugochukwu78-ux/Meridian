#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

namespace meridian
{

/** Creates a writer for a file in the session's audio format. */
std::unique_ptr<juce::AudioFormatWriter> createSessionWriter (const juce::File& file, double sampleRate, int numChannels,
                                                              int bitDepth, bool aiff, juce::String& error);

struct ImportResult
{
    bool ok = false;
    juce::String error;
    juce::int64 length = 0;   ///< in session samples
    int numChannels = 0;
};

/** Copies an audio file into the session (Pro Tools-style "copy on import"),
    converting format, bit depth and sample rate to the session's.

    Sample-rate conversion uses a windowed-sinc interpolator; when lowering the
    rate the signal is first low-pass filtered at 90 % of the new Nyquist
    frequency so nothing aliases. Files with more than two channels keep the
    first two.

    Blocking: call from a background thread. `progress` receives 0..1 and may
    return false to cancel. */
ImportResult importAudioFile (const juce::File& source, const juce::File& destination,
                              double sessionSampleRate, int bitDepth, bool aiff,
                              juce::AudioFormatManager&, const std::function<bool (double)>& progress);

} // namespace meridian
