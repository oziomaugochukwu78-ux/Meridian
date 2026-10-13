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

TEST_CASE ("Content at an origin keeps its musical timing under the tempo there", "[tempo]")
{
    // 120 BPM for one bar, then 60 BPM. A clip starting at bar 2 (2 s): its first
    // quarter note lasts 1 s (60 BPM), not 0.5 s (the tempo at the session start).
    TempoMap map (120.0);
    map.addChange (ticksPerQuarter * 4, 60.0);
    const auto bar2 = map.ticksToSamples (ticksPerQuarter * 4, 48000.0);
    CHECK (bar2 == 96000);
    CHECK (map.samplesFromOrigin (bar2, ticksPerQuarter, 48000.0) == 96000 + 48000);
    CHECK (map.ticksFromOrigin (bar2, 96000 + 48000, 48000.0) == ticksPerQuarter);

    // An origin on a tick lands every note on the sample ticksToSamples gives (the click's).
    TempoMap awkward (97.0);
    awkward.addChange (ticksPerQuarter * 8, 143.5);
    const int64_t originTick = 1234;
    const auto origin = awkward.ticksToSamples (originTick, 48000.0);
    for (int64_t t = 0; t < ticksPerQuarter * 16; t += 41)
    {
        REQUIRE (awkward.samplesFromOrigin (origin, t, 48000.0) == awkward.ticksToSamples (originTick + t, 48000.0));
        REQUIRE (awkward.ticksFromOrigin (origin, awkward.samplesFromOrigin (origin, t, 48000.0), 48000.0) == t);
    }

    // At one tempo, an origin between ticks shifts its notes by exactly its offset.
    TempoMap steady (120.0);
    for (int64_t t = 0; t < ticksPerQuarter * 8; t += 53)
        REQUIRE (steady.samplesFromOrigin (1000 + 7, t, 48000.0) - steady.samplesFromOrigin (1000, t, 48000.0) == 7);
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
