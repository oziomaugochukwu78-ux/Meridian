// Turning recorded MIDI input into a clip.

#include <midi/MidiRecording.h>

#include <catch2/catch_test_macros.hpp>

using namespace meridian::midi;

namespace
{
constexpr double rate = 48000.0;
const TempoMap tempo120 (120.0); // a beat = 24000 samples = 960 ticks

RecordedEvent on (int64_t t, int pitch, float velocity = 0.8f) { return { t, pitch, velocity, true }; }
RecordedEvent off (int64_t t, int pitch) { return { t, pitch, 0.0f, false }; }
} // namespace

TEST_CASE ("MIDI recording: notes land on the ticks the player meant, after latency compensation", "[midirec]")
{
    // The engine received everything 1000 samples late.
    const int64_t late = 1000;
    const auto take = buildTake ({ on (24000 + late, 60), off (48000 + late, 60), on (48000 + late, 64, 0.5f), off (60000 + late, 64) },
                                 0 + late, 96000 + late, late, tempo120, rate);
    CHECK (take.clipStart == 0);
    CHECK (take.clipLength == 96000);
    REQUIRE (take.notes.size() == 2);
    CHECK (take.notes[0].pitch == 60);
    CHECK (take.notes[0].start == 960);
    CHECK (take.notes[0].length == 960);
    CHECK (take.notes[1].pitch == 64);
    CHECK (take.notes[1].start == 1920);
    CHECK (take.notes[1].length == 480);
    CHECK (take.notes[1].velocity == 0.5f);
}

TEST_CASE ("MIDI recording: ticks are counted from the clip start", "[midirec]")
{
    // Recording started at bar 2 (96000): a note on its downbeat is at tick 0 of the clip.
    const auto take = buildTake ({ on (96000, 62), off (108000, 62) }, 96000, 192000, 0, tempo120, rate);
    CHECK (take.clipStart == 96000);
    REQUIRE (take.notes.size() == 1);
    CHECK (take.notes[0].start == 0);
    CHECK (take.notes[0].length == 480);
}

TEST_CASE ("MIDI recording: held, retriggered and stray notes", "[midirec]")
{
    const auto take = buildTake ({ on (0, 60), on (12000, 60), off (30000, 61), { 36000, 62, 0.0f, true }, on (24000, 62), on (40000, 200) },
                                 0, 48000, 0, tempo120, rate);
    // 60 retriggered at 12000 (first ends there), second held to the stop; the stray
    // note-off for 61 does nothing; velocity-0 note-on ends 62; pitch 200 is ignored.
    REQUIRE (take.notes.size() == 3);
    CHECK ((take.notes[0].pitch == 60 && take.notes[0].start == 0 && take.notes[0].length == 480));
    CHECK ((take.notes[1].pitch == 60 && take.notes[1].start == 480 && take.notes[1].length == 1440));
    CHECK ((take.notes[2].pitch == 62 && take.notes[2].start == 960 && take.notes[2].length == 480));
}

TEST_CASE ("MIDI recording: events outside the take are clamped into it; a take never starts before zero", "[midirec]")
{
    const auto take = buildTake ({ on (100, 60), off (500000, 60) }, 2000, 50000, 3000, tempo120, rate);
    CHECK (take.clipStart == 0);
    CHECK (take.clipLength == 47000);
    REQUIRE (take.notes.size() == 1);
    CHECK (take.notes[0].start == 0);
    CHECK (take.notes[0].length == 1880); // 47000 samples
}
