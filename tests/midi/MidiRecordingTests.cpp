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

TEST_CASE ("MIDI recording: the sustain pedal becomes ranges, compensated like notes", "[midirec]")
{
    // Pedal down at beat 1, up at beat 3; down again at beat 4 and held to the stop; a stray
    // pedal-up and a repeated pedal-down change nothing. All received 1000 samples late.
    const int64_t late = 1000;
    const auto pedal = [] (int64_t t, bool down) { return RecordedEvent { t, sustainPedal, 0.0f, down }; };
    const auto take = buildTake ({ pedal (10000 + late, false), pedal (24000 + late, true), on (24000 + late, 60),
                                   pedal (36000 + late, true), off (30000 + late, 60), pedal (72000 + late, false),
                                   pedal (96000 + late, true), { 100000 + late, -5, 1.0f, true } },
                                 late, 120000 + late, late, tempo120, rate);
    REQUIRE (take.sustain.size() == 2);
    CHECK ((take.sustain[0].start == 960 && take.sustain[0].length == 1920));
    CHECK ((take.sustain[1].start == 3840 && take.sustain[1].length == 960)); // held until the stop (120000)
    REQUIRE (take.notes.size() == 1); // the pedal is not a note, and pitch -5 is ignored
    CHECK ((take.notes[0].start == 960 && take.notes[0].length == 240));
}

TEST_CASE ("Pedal ranges: sorted, merged, clamped", "[midirec]")
{
    const auto r = normalisedPedal ({ { 1000, 500 }, { -200, 400 }, { 1200, 1000 }, { 2200, 10 }, { 5000, 0 } });
    REQUIRE (r.size() == 3);
    CHECK ((r[0].start == 0 && r[0].length == 200));    // the part before zero is cut
    CHECK ((r[1].start == 1000 && r[1].length == 1210)); // overlapping and touching ranges merge
    CHECK ((r[2].start == 5000 && r[2].length == 1));    // at least a tick
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

TEST_CASE ("MIDI recording: pitch-wheel and mod-wheel moves become control points on the meant ticks", "[midirec]")
{
    const int64_t late = 1000;
    const auto take = buildTake ({ on (24000 + late, 60), { 36000 + late, pitchBend, 1.0f, true }, { 48000 + late, pitchBend, 0.0f, true },
                                   { 36000 + late, modWheel, 0.25f, true }, { 42000 + late, modWheel, 2.0f, true }, off (60000 + late, 60) },
                                 late, 96000 + late, late, tempo120, rate);
    REQUIRE (take.pitchBend.size() == 2);
    CHECK (take.pitchBend[0].tick == 1440);
    CHECK (take.pitchBend[0].value == 1.0f);
    CHECK (take.pitchBend[1].tick == 1920);
    CHECK (take.pitchBend[1].value == 0.0f);
    REQUIRE (take.modWheel.size() == 2);
    CHECK (take.modWheel[0].tick == 1440);
    CHECK (take.modWheel[1].value == 1.0f); // clamped into 0..1
    CHECK (take.notes.size() == 1);
}
