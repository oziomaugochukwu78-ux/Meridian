#include "TempoMap.h"

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

void TempoMap::addChange (int64_t tick, double bpm)
{
    tick = std::max<int64_t> (0, tick);
    const auto at = std::lower_bound (changes.begin(), changes.end(), tick,
                                      [] (const Change& c, int64_t t) { return c.tick < t; });
    if (at != changes.end() && at->tick == tick)
        at->bpm = clampBpm (bpm);
    else
        changes.insert (at, Change { tick, clampBpm (bpm) });
}

double TempoMap::bpmAt (int64_t tick) const noexcept
{
    double bpm = changes.front().bpm;
    for (const auto& c : changes)
    {
        if (c.tick > tick)
            break;
        bpm = c.bpm;
    }
    return bpm;
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
            seconds += (double) (end - changes[i].tick) * secondsPerTick (changes[i].bpm);
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
        const auto spt = secondsPerTick (changes[i].bpm);
        if (i + 1 < changes.size())
        {
            const auto segmentSeconds = (double) (changes[i + 1].tick - changes[i].tick) * spt;
            if (seconds < elapsed + segmentSeconds)
                return changes[i].tick + (int64_t) std::llround ((seconds - elapsed) / spt);
            elapsed += segmentSeconds;
        }
        else
        {
            return changes[i].tick + (int64_t) std::llround ((seconds - elapsed) / spt);
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
