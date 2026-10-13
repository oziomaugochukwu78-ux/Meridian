#pragma once

#include "TempoMap.h"

#include <cstdint>
#include <vector>

namespace meridian::midi
{

/** Time signatures over the session: a list of meter changes, each at the
    start of a bar, the first always at bar 0 (4/4 unless changed).

    A bar of n/d lasts n beats of a 1/d note: 960 * 4 / d ticks per beat.
    Bars are counted from 0 here; the user sees bar 1 for bar 0. */
class MeterMap
{
public:
    struct Change
    {
        int64_t bar = 0;
        int numerator = 4;
        int denominator = 4; ///< 1, 2, 4, 8, 16 or 32
    };

    MeterMap (int numerator = 4, int denominator = 4);

    /** Adds a change at the start of `bar`, replacing any at the same bar.
        Invalid values are clamped (numerator 1..32, denominator a power of two 1..32). */
    void addChange (int64_t bar, int numerator, int denominator);
    const std::vector<Change>& getChanges() const noexcept { return changes; }

    const Change& meterAt (int64_t bar) const noexcept;
    static int64_t ticksPerBeat (const Change& c) noexcept { return ticksPerQuarter * 4 / c.denominator; }
    static int64_t ticksPerBar (const Change& c) noexcept { return ticksPerBeat (c) * c.numerator; }

    /** First tick of `bar` (bars before 0 extend the first meter backwards). */
    int64_t barToTicks (int64_t bar) const noexcept;

    struct Position
    {
        int64_t bar = 0;      ///< 0-based
        int beat = 0;         ///< 0-based within the bar
        int64_t tick = 0;     ///< within the beat
    };
    /** Bar, beat and tick of a musical position (ticks from the session start). */
    Position positionOf (int64_t tick) const noexcept;

    /** The first beat at or after `tick`: its tick, and whether it starts a bar. */
    struct Beat
    {
        int64_t tick = 0;
        bool barStart = false;
    };
    Beat beatAtOrAfter (int64_t tick) const noexcept;

private:
    static Change sanitised (Change) noexcept;
    std::vector<Change> changes; // sorted by bar, first at bar 0
};

/** Where the click's beats fall, in samples, under a tempo map and a meter
    map: beat k sounds on exactly the sample TempoMap::ticksToSamples gives for
    its tick, the same rounding MIDI notes use, so the click and the notes
    never drift apart. Built on the message thread; immutable afterwards, so
    the audio thread may read it (it allocates nothing). */
class ClickGrid
{
public:
    ClickGrid (TempoMap tempo, MeterMap meter, double sampleRate);

    struct Beat
    {
        int64_t sample = 0;
        bool accent = false; ///< the first beat of a bar
    };
    /** The first beat at or after `samplePosition` (timeline samples). */
    Beat beatAtOrAfter (int64_t samplePosition) const noexcept;

    const TempoMap& getTempo() const noexcept { return tempo; }
    const MeterMap& getMeter() const noexcept { return meter; }
    double getSampleRate() const noexcept { return sampleRate; }

private:
    TempoMap tempo;
    MeterMap meter;
    double sampleRate;
};

} // namespace meridian::midi
