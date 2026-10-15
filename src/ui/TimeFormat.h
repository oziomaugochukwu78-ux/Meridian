#pragma once

#include <juce_core/juce_core.h>

#include <midi/MeterMap.h>

#include <cmath>
#include <optional>

namespace meridian::timeformat
{

/** Default tempo, and the metre (4/4 throughout for now; no meter map yet). */
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

inline double samplesPerBeat (double sampleRate, double bpm = tempoBpm)
{
    return sampleRate * 60.0 / (bpm > 0.0 ? bpm : tempoBpm);
}

/** "bar| beat| tick", 1-based bars and beats. */
inline juce::String barsBeats (juce::int64 samples, double sampleRate, double bpm = tempoBpm)
{
    if (sampleRate <= 0.0)
        return "1| 1| 000";
    const double beats = (double) std::max<juce::int64> (0, samples) / samplesPerBeat (sampleRate, bpm);
    const auto wholeBeats = (juce::int64) std::floor (beats);
    const auto bar = wholeBeats / beatsPerBar + 1;
    const auto beat = wholeBeats % beatsPerBar + 1;
    const auto ticks = (int) std::floor ((beats - (double) wholeBeats) * ticksPerBeat);
    return juce::String (bar) + "| " + juce::String (beat) + "| " + juce::String (ticks).paddedLeft ('0', 3);
}

/** "bar| beat| tick" under a tempo map and a meter map (beats are the meter's own
    unit: eighths in 6/8). */
inline juce::String barsBeats (juce::int64 samples, double sampleRate, const midi::TempoMap& tempo, const midi::MeterMap& meter)
{
    if (sampleRate <= 0.0)
        return "1| 1| 000";
    const auto p = meter.positionOf (tempo.samplesToTicks (std::max<juce::int64> (0, samples), sampleRate));
    return juce::String (p.bar + 1) + "| " + juce::String (p.beat + 1) + "| " + juce::String (p.tick).paddedLeft ('0', 3);
}

/** A typed time, as a sample position: "m:ss.mmm" or "h:mm:ss.mmm", plain seconds
    ("12.5"), or samples ("48000 smp"). Nothing if the text is not a time. */
inline std::optional<juce::int64> parseTime (const juce::String& text, double sampleRate)
{
    const auto t = text.trim();
    if (t.isEmpty() || sampleRate <= 0.0)
        return std::nullopt;

    for (const char* suffix : { "samples", "smp" })
    {
        if (! t.endsWithIgnoreCase (suffix))
            continue;
        const auto digits = t.dropLastCharacters (juce::String (suffix).length()).trim();
        if (digits.isEmpty() || digits.length() > 15 || ! digits.containsOnly ("0123456789"))
            return std::nullopt;
        return digits.getLargeIntValue();
    }

    // Fields separated by ':'; only the last (seconds) may have a fraction, and
    // every field after the first must be below 60.
    const auto fields = juce::StringArray::fromTokens (t, ":", {});
    if (fields.size() > 3)
        return std::nullopt;

    double seconds = 0.0;
    for (int i = 0; i < fields.size(); ++i)
    {
        const auto& f = fields[i];
        const bool last = i == fields.size() - 1;
        if (f.isEmpty() || f == "." || f.length() > 12 || ! f.containsOnly (last ? "0123456789." : "0123456789")
            || f.indexOfChar ('.') != f.lastIndexOfChar ('.'))
            return std::nullopt;
        const double value = f.getDoubleValue();
        if (i > 0 && value >= 60.0)
            return std::nullopt;
        seconds = seconds * 60.0 + value;
    }
    return (juce::int64) std::llround (seconds * sampleRate);
}

} // namespace meridian::timeformat
