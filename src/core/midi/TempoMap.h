#pragma once

#include <cstdint>
#include <vector>

namespace meridian::midi
{

/** Musical time resolution: ticks per quarter note (the industry-standard 960). */
inline constexpr int64_t ticksPerQuarter = 960;

/** Converts between musical time (ticks) and real time (seconds, samples).

    MIDI notes are stored in ticks, so changing the tempo moves them with the
    music, as in every DAW. The map is a list of tempo changes, the first
    always at tick 0. A change either holds its tempo until the next, or
    ramps: the tempo glides linearly (in BPM per tick) to the next change's.
    Conversions in both directions are exact (closed forms, no stepping) to
    within rounding to the nearest tick or sample.

    Not thread-safe: the engine renders from its own copy. */
class TempoMap
{
public:
    static constexpr double minBpm = 20.0, maxBpm = 999.0;

    struct Change
    {
        int64_t tick = 0;
        double bpm = 120.0;
        bool ramp = false; ///< glide linearly to the next change's tempo (ignored on the last)
    };

    explicit TempoMap (double bpm = 120.0);

    /** One tempo for the whole timeline (removes all changes). */
    void setTempo (double bpm);

    /** Adds a tempo change, replacing any change at the same tick. */
    void addChange (int64_t tick, double bpm, bool ramp = false);

    /** Makes the change at `tick` ramp to the next one (or hold). False if there is no change there. */
    bool setRamp (int64_t tick, bool ramp);

    const std::vector<Change>& getChanges() const noexcept { return changes; }

    double bpmAt (int64_t tick) const noexcept;

    double ticksToSeconds (int64_t tick) const noexcept;
    int64_t secondsToTicks (double seconds) const noexcept; ///< rounded to the nearest tick

    int64_t ticksToSamples (int64_t tick, double sampleRate) const noexcept;
    int64_t samplesToTicks (int64_t samples, double sampleRate) const noexcept;

    /** Content placed at a sample position (a MIDI clip's origin) keeps its
        musical timing: `ticks` after an origin at `originSample` falls where the
        music is that many ticks after the origin's own tick, under the tempo
        there, not under the tempo at the start of the session. An origin exactly
        on a tick lands its notes on exactly the samples ticksToSamples gives
        (where the click sounds); an origin between ticks keeps that sub-tick
        offset. */
    int64_t samplesFromOrigin (int64_t originSample, int64_t ticks, double sampleRate) const noexcept;

    /** The inverse: ticks from an origin at `originSample` to `sample` (nearest tick). */
    int64_t ticksFromOrigin (int64_t originSample, int64_t sample, double sampleRate) const noexcept;

private:
    static double secondsPerTick (double bpm) noexcept { return 60.0 / (bpm * (double) ticksPerQuarter); }

    /** Segment i: [changes[i].tick, next change), its end tempo (the next change's if it ramps). */
    double segmentEndBpm (size_t i) const noexcept;
    /** Seconds from the start of segment i to `ticks` into it (exact under a ramp). */
    double secondsInto (size_t i, double ticks) const noexcept;
    /** The inverse: ticks into segment i after `seconds` (not rounded). */
    double ticksInto (size_t i, double seconds) const noexcept;

    std::vector<Change> changes; // sorted by tick, first at tick 0
};

} // namespace meridian::midi
