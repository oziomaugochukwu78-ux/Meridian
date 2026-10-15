#include "TempoMap.h"

#include "../dsp/FloatCompare.h"

#include <algorithm>
#include <cmath>

namespace meridian::midi
{
namespace
{
double clampBpm (double bpm) noexcept
{
    return std::isfinite (bpm) ? std::clamp (bpm, TempoMap::minBpm, TempoMap::maxBpm) : 120.0;
}
} // namespace

TempoMap::TempoMap (double bpm)
{
    setTempo (bpm);
}

void TempoMap::setTempo (double bpm)
{
    changes.assign (1, Change { 0, clampBpm (bpm) });
}

void TempoMap::addChange (int64_t tick, double bpm, bool ramp)
{
    tick = std::max<int64_t> (0, tick);
    const auto at = std::lower_bound (changes.begin(), changes.end(), tick,
                                      [] (const Change& c, int64_t t) { return c.tick < t; });
    if (at != changes.end() && at->tick == tick)
    {
        at->bpm = clampBpm (bpm);
        at->ramp = ramp;
    }
    else
    {
        changes.insert (at, Change { tick, clampBpm (bpm), ramp });
    }
}

bool TempoMap::setRamp (int64_t tick, bool ramp)
{
    for (auto& c : changes)
        if (c.tick == tick)
        {
            c.ramp = ramp;
            return true;
        }
    return false;
}

double TempoMap::segmentEndBpm (size_t i) const noexcept
{
    return changes[i].ramp && i + 1 < changes.size() ? changes[i + 1].bpm : changes[i].bpm;
}

double TempoMap::secondsInto (size_t i, double ticks) const noexcept
{
    const double b0 = changes[i].bpm, b1 = segmentEndBpm (i);
    if (dsp::exactlyEqual (b1, b0))
        return ticks * secondsPerTick (b0);
    // bpm(t) = b0 + k t, with k = (b1 - b0) / length. The time per tick is
    // 60 / (TPQ bpm(t)); its integral is 60 / (TPQ k) * ln (bpm(t) / b0).
    const double k = (b1 - b0) / (double) (changes[i + 1].tick - changes[i].tick);
    return 60.0 / ((double) ticksPerQuarter * k) * std::log ((b0 + k * ticks) / b0);
}

double TempoMap::ticksInto (size_t i, double seconds) const noexcept
{
    const double b0 = changes[i].bpm, b1 = segmentEndBpm (i);
    if (dsp::exactlyEqual (b1, b0))
        return seconds / secondsPerTick (b0);
    const double k = (b1 - b0) / (double) (changes[i + 1].tick - changes[i].tick);
    return b0 * (std::exp (seconds * (double) ticksPerQuarter * k / 60.0) - 1.0) / k;
}

double TempoMap::bpmAt (int64_t tick) const noexcept
{
    size_t segment = 0;
    for (size_t i = 0; i < changes.size() && changes[i].tick <= tick; ++i)
        segment = i;
    const auto& c = changes[segment];
    if (! c.ramp || segment + 1 >= changes.size() || tick <= c.tick)
        return c.bpm;
    const auto& next = changes[segment + 1];
    return c.bpm + (next.bpm - c.bpm) * (double) (tick - c.tick) / (double) (next.tick - c.tick);
}

double TempoMap::ticksToSeconds (int64_t tick) const noexcept
{
    // Sum whole segments before `tick`, then the part of the segment it is in.
    double seconds = 0.0;
    for (size_t i = 0; i < changes.size(); ++i)
    {
        const auto segmentEnd = i + 1 < changes.size() ? changes[i + 1].tick : INT64_MAX;
        const auto end = std::min (tick, segmentEnd);
        if (end > changes[i].tick)
            seconds += secondsInto (i, (double) (end - changes[i].tick));
        if (tick <= segmentEnd)
            break;
    }
    // Negative ticks extrapolate at the first tempo.
    if (tick < 0)
        seconds = (double) tick * secondsPerTick (changes.front().bpm);
    return seconds;
}

int64_t TempoMap::secondsToTicks (double seconds) const noexcept
{
    if (! std::isfinite (seconds))
        return 0;
    if (seconds < 0.0)
        return (int64_t) std::llround (seconds / secondsPerTick (changes.front().bpm));

    double elapsed = 0.0;
    for (size_t i = 0; i < changes.size(); ++i)
    {
        if (i + 1 < changes.size())
        {
            const auto segmentSeconds = secondsInto (i, (double) (changes[i + 1].tick - changes[i].tick));
            if (seconds < elapsed + segmentSeconds)
                return changes[i].tick + (int64_t) std::llround (ticksInto (i, seconds - elapsed));
            elapsed += segmentSeconds;
        }
        else
        {
            return changes[i].tick + (int64_t) std::llround (ticksInto (i, seconds - elapsed));
        }
    }
    return 0;
}

int64_t TempoMap::ticksToSamples (int64_t tick, double sampleRate) const noexcept
{
    return (int64_t) std::llround (ticksToSeconds (tick) * sampleRate);
}

int64_t TempoMap::samplesToTicks (int64_t samples, double sampleRate) const noexcept
{
    return sampleRate > 0.0 ? secondsToTicks ((double) samples / sampleRate) : 0;
}

int64_t TempoMap::samplesFromOrigin (int64_t originSample, int64_t ticks, double sampleRate) const noexcept
{
    const auto originTick = samplesToTicks (originSample, sampleRate);
    const auto subTick = originSample - ticksToSamples (originTick, sampleRate);
    return ticksToSamples (originTick + ticks, sampleRate) + subTick;
}

int64_t TempoMap::ticksFromOrigin (int64_t originSample, int64_t sample, double sampleRate) const noexcept
{
    const auto originTick = samplesToTicks (originSample, sampleRate);
    const auto subTick = originSample - ticksToSamples (originTick, sampleRate);
    return samplesToTicks (sample - subTick, sampleRate) - originTick;
}

} // namespace meridian::midi
