#pragma once

#include <cstdint>
#include <vector>

namespace meridian::midi
{

/** Musical time resolution: ticks per quarter note (the industry-standard 960). */
inline constexpr int64_t ticksPerQuarter = 960;

/** Converts between musical time (ticks) and real time (seconds, samples).

    MIDI notes are stored in ticks, so changing the tempo moves them with the
    music, as in every DAW. The map is piecewise constant: a list of tempo
    changes, the first always at tick 0. Conversions in both directions are
    exact to within rounding to the nearest tick or sample.

    Not thread-safe: the engine renders from its own copy. */
class TempoMap
{
public:
    static constexpr double minBpm = 20.0, maxBpm = 999.0;

    struct Change
    {
        int64_t tick = 0;
        double bpm = 120.0;
    };

    explicit TempoMap (double bpm = 120.0);

    /** One tempo for the whole timeline (removes all changes). */
    void setTempo (double bpm);

    /** Adds a tempo change, replacing any change at the same tick. */
    void addChange (int64_t tick, double bpm);

    const std::vector<Change>& getChanges() const noexcept { return changes; }

    double bpmAt (int64_t tick) const noexcept;

    double ticksToSeconds (int64_t tick) const noexcept;
    int64_t secondsToTicks (double seconds) const noexcept; ///< rounded to the nearest tick

    int64_t ticksToSamples (int64_t tick, double sampleRate) const noexcept;
    int64_t samplesToTicks (int64_t samples, double sampleRate) const noexcept;

private:
    static double secondsPerTick (double bpm) noexcept { return 60.0 / (bpm * (double) ticksPerQuarter); }

    std::vector<Change> changes; // sorted by tick, first at tick 0
};

} // namespace meridian::midi
