#pragma once

#include <juce_core/juce_core.h>

namespace meridian::timeformat
{

/** Session tempo and metre. Fixed for now (no tempo map yet). */
constexpr double tempoBpm = 120.0;
constexpr int beatsPerBar = 4;
constexpr int ticksPerBeat = 960;

/** "m:ss.mmm" */
inline juce::String minSecs (juce::int64 samples, double sampleRate)
{
    if (sampleRate <= 0.0)
        return "0:00.000";
    const double seconds = juce::jmax<double> (0.0, (double) samples / sampleRate);
    const auto totalMs = (juce::int64) std::floor (seconds * 1000.0);
    const auto minutes = totalMs / 60000;
    const auto secs = (totalMs / 1000) % 60;
    const auto ms = totalMs % 1000;
    return juce::String (minutes) + ":" + juce::String (secs).paddedLeft ('0', 2) + "." + juce::String (ms).paddedLeft ('0', 3);
}

inline double samplesPerBeat (double sampleRate) { return sampleRate * 60.0 / tempoBpm; }

/** "bar| beat| tick", 1-based bars and beats. */
inline juce::String barsBeats (juce::int64 samples, double sampleRate)
{
    if (sampleRate <= 0.0)
        return "1| 1| 000";
    const double beats = (double) juce::jmax<juce::int64> (0, samples) / samplesPerBeat (sampleRate);
    const auto wholeBeats = (juce::int64) std::floor (beats);
    const auto bar = wholeBeats / beatsPerBar + 1;
    const auto beat = wholeBeats % beatsPerBar + 1;
    const auto ticks = (int) std::floor ((beats - (double) wholeBeats) * ticksPerBeat);
    return juce::String (bar) + "| " + juce::String (beat) + "| " + juce::String (ticks).paddedLeft ('0', 3);
}

} // namespace meridian::timeformat
