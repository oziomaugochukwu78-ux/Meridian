#include <midi/TempoMap.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace meridian::midi;
using Catch::Approx;

TEST_CASE ("Constant tempo: a quarter note at 120 BPM is half a second", "[tempo]")
{
    TempoMap map (120.0);
    CHECK (map.ticksToSeconds (ticksPerQuarter) == Approx (0.5));
    CHECK (map.ticksToSamples (ticksPerQuarter * 4, 48000.0) == 96000); // one 4/4 bar = 2 s
    CHECK (map.secondsToTicks (0.5) == ticksPerQuarter);
    CHECK (map.samplesToTicks (24000, 48000.0) == ticksPerQuarter);
}

TEST_CASE ("Tick <-> sample conversion round-trips exactly", "[tempo]")
{
    TempoMap map (97.0); // an awkward tempo
    map.addChange (ticksPerQuarter * 8, 143.5);
    for (int64_t tick = 0; tick < ticksPerQuarter * 32; tick += 37)
    {
        const auto samples = map.ticksToSamples (tick, 48000.0);
        // At 48 kHz a sample is shorter than a tick at any of these tempos,
        // so converting back must land on the same tick.
        REQUIRE (map.samplesToTicks (samples, 48000.0) == tick);
    }
}

TEST_CASE ("Tempo changes are piecewise constant", "[tempo]")
{
    TempoMap map (120.0);
    map.addChange (ticksPerQuarter * 4, 60.0); // after one bar, half speed

    CHECK (map.bpmAt (0) == Approx (120.0));
    CHECK (map.bpmAt (ticksPerQuarter * 4 - 1) == Approx (120.0));
    CHECK (map.bpmAt (ticksPerQuarter * 4) == Approx (60.0));

    // Bar 1 at 120 BPM = 2 s, then each quarter takes 1 s.
    CHECK (map.ticksToSeconds (ticksPerQuarter * 4) == Approx (2.0));
    CHECK (map.ticksToSeconds (ticksPerQuarter * 6) == Approx (4.0));
    CHECK (map.secondsToTicks (4.0) == ticksPerQuarter * 6);
    CHECK (map.secondsToTicks (1.0) == ticksPerQuarter * 2);
}

TEST_CASE ("Tempo changes replace, sort and clamp", "[tempo]")
{
    TempoMap map (120.0);
    map.addChange (1920, 90.0);
    map.addChange (960, 100.0);
    map.addChange (1920, 95.0); // replaces
    map.addChange (3000, 5000.0);

    REQUIRE (map.getChanges().size() == 4);
    CHECK (map.getChanges()[1].tick == 960);
    CHECK (map.getChanges()[2].bpm == Approx (95.0));
    CHECK (map.getChanges()[3].bpm == Approx (TempoMap::maxBpm));

    map.setTempo (0.0);
    CHECK (map.getChanges().size() == 1);
    CHECK (map.bpmAt (0) == Approx (TempoMap::minBpm));
}
