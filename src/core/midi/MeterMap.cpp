#include "MeterMap.h"

#include <algorithm>
#include <climits>

namespace meridian::midi
{
namespace
{
/** Floor division (rounds towards minus infinity, also for negative numbers). */
int64_t floorDiv (int64_t a, int64_t b) noexcept
{
    const auto q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
} // namespace

MeterMap::MeterMap (int numerator, int denominator)
{
    changes.push_back (sanitised ({ 0, numerator, denominator }));
}

MeterMap::Change MeterMap::sanitised (Change c) noexcept
{
    c.bar = std::max<int64_t> (0, c.bar);
    c.numerator = std::clamp (c.numerator, 1, 32);
    const bool powerOfTwo = c.denominator >= 1 && c.denominator <= 32 && (c.denominator & (c.denominator - 1)) == 0;
    if (! powerOfTwo)
        c.denominator = 4;
    return c;
}

void MeterMap::addChange (int64_t bar, int numerator, int denominator)
{
    const auto c = sanitised ({ bar, numerator, denominator });
    const auto at = std::lower_bound (changes.begin(), changes.end(), c.bar,
                                      [] (const Change& x, int64_t b) { return x.bar < b; });
    if (at != changes.end() && at->bar == c.bar)
        *at = c;
    else
        changes.insert (at, c);
}

const MeterMap::Change& MeterMap::meterAt (int64_t bar) const noexcept
{
    const Change* current = &changes.front();
    for (const auto& c : changes)
    {
        if (c.bar > bar)
            break;
        current = &c;
    }
    return *current;
}

int64_t MeterMap::barToTicks (int64_t bar) const noexcept
{
    int64_t ticks = 0;
    for (size_t i = 0; i < changes.size(); ++i)
    {
        const auto& c = changes[i];
        const auto endBar = i + 1 < changes.size() ? changes[i + 1].bar : INT64_MAX;
        if (bar < endBar)
            return ticks + (bar - c.bar) * ticksPerBar (c);
        ticks += (endBar - c.bar) * ticksPerBar (c);
    }
    return ticks;
}

MeterMap::Position MeterMap::positionOf (int64_t tick) const noexcept
{
    int64_t start = 0;
    for (size_t i = 0; i < changes.size(); ++i)
    {
        const auto& c = changes[i];
        const bool last = i + 1 == changes.size();
        const auto end = last ? INT64_MAX : start + (changes[i + 1].bar - c.bar) * ticksPerBar (c);
        if (tick < end || last)
        {
            const auto rel = tick - start;
            const auto barOffset = floorDiv (rel, ticksPerBar (c));
            const auto inBar = rel - barOffset * ticksPerBar (c);
            return { c.bar + barOffset, (int) (inBar / ticksPerBeat (c)), inBar % ticksPerBeat (c) };
        }
        start = end;
    }
    return {};
}

MeterMap::Beat MeterMap::beatAtOrAfter (int64_t tick) const noexcept
{
    auto p = positionOf (tick);
    const auto& m = meterAt (p.bar);
    if (p.tick != 0 && ++p.beat >= m.numerator)
    {
        ++p.bar;
        p.beat = 0;
    }
    const auto& meter = meterAt (p.bar);
    return { barToTicks (p.bar) + p.beat * ticksPerBeat (meter), p.beat == 0 };
}

//==============================================================================
ClickGrid::ClickGrid (TempoMap t, MeterMap m, double rate)
    : tempo (std::move (t)), meter (std::move (m)), sampleRate (rate > 0.0 ? rate : 48000.0)
{
}

ClickGrid::Beat ClickGrid::beatAtOrAfter (int64_t samplePosition) const noexcept
{
    // Start just before the tick that sample rounds to, then step to the first beat
    // whose sample (by the same rounding as MIDI notes) is not earlier.
    // (A few ticks of margin: at very fast tempos and low rates a tick is under a sample.)
    auto beat = meter.beatAtOrAfter (tempo.samplesToTicks (samplePosition, sampleRate) - 4);
    auto sample = tempo.ticksToSamples (beat.tick, sampleRate);
    while (sample < samplePosition)
    {
        beat = meter.beatAtOrAfter (beat.tick + 1);
        sample = tempo.ticksToSamples (beat.tick, sampleRate);
    }
    return { sample, beat.barStart };
}

} // namespace meridian::midi
