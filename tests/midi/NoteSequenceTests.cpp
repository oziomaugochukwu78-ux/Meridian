#include <midi/NoteSequence.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace meridian::midi;

namespace
{
bool sortedAndValid (const NoteSequence& s)
{
    const auto& n = s.getNotes();
    for (size_t i = 0; i < n.size(); ++i)
    {
        if (n[i].pitch < 0 || n[i].pitch > 127 || ! (n[i].velocity > 0.0f) || n[i].velocity > 1.0f
            || n[i].length < 1 || n[i].start < 0)
            return false;
        if (i > 0 && (n[i - 1].start > n[i].start || (n[i - 1].start == n[i].start && n[i - 1].pitch > n[i].pitch)))
            return false;
    }
    return true;
}
} // namespace

TEST_CASE ("Notes are kept sorted by start, then pitch", "[notes]")
{
    NoteSequence s;
    s.add ({ 64, 0.8f, 960, 480 });
    s.add ({ 60, 0.8f, 0, 480 });
    s.add ({ 67, 0.8f, 960, 480 });
    s.add ({ 62, 0.8f, 960, 480 });

    REQUIRE (s.size() == 4);
    CHECK (s.getNotes()[0].pitch == 60);
    CHECK (s.getNotes()[1].pitch == 62);
    CHECK (s.getNotes()[3].pitch == 67);
    CHECK (sortedAndValid (s));
    CHECK (s.endTick() == 1440);
}

TEST_CASE ("add() returns the index of the note it inserted, through every reallocation", "[notes]")
{
    // Regression: the index was once computed as insert(...) - begin() in one
    // expression, which GCC may evaluate begin() first, before the vector grows.
    NoteSequence s;
    for (int i = 0; i < 200; ++i)
    {
        const int pitch = (i * 37) % 128;
        const int64_t start = ((i * 7919) % 50) * 120; // out of order, with ties
        const auto index = s.add ({ pitch, 0.8f, start, 100 });
        REQUIRE (index < s.size());
        CHECK (s.getNotes()[index].pitch == pitch);
        CHECK (s.getNotes()[index].start == start);
    }
}

TEST_CASE ("Invalid note values are clamped, never stored", "[notes]")
{
    NoteSequence s;
    s.add ({ 200, 5.0f, -50, 0 });
    s.add ({ -3, 0.0f, 10, -7 });
    CHECK (sortedAndValid (s));
    CHECK (s.getNotes()[0].start == 0);
    CHECK (s.getNotes()[0].pitch == 127);
    CHECK (s.getNotes()[1].pitch == 0);
    CHECK (s.getNotes()[1].length == 1);
}

TEST_CASE ("Range queries find every overlapping note and nothing else", "[notes]")
{
    NoteSequence s;
    s.add ({ 60, 0.8f, 0, 960 });     // [0, 960)
    s.add ({ 62, 0.8f, 900, 200 });   // [900, 1100)
    s.add ({ 64, 0.8f, 1100, 100 });  // [1100, 1200)
    s.add ({ 65, 0.8f, 5000, 100 });

    std::vector<int> found;
    s.forEachOverlapping (950, 1100, [&] (const Note& n) { found.push_back (n.pitch); });
    CHECK (found == std::vector<int> { 60, 62 }); // 64 starts exactly at the end: not included
}

TEST_CASE ("Quantize snaps starts to the grid, with partial strength", "[notes]")
{
    NoteSequence s;
    s.add ({ 60, 0.8f, 230, 100 });  // nearest 240 (a sixteenth = 240 ticks)
    s.add ({ 62, 0.8f, 470, 100 });  // nearest 480
    s.add ({ 64, 0.8f, 0, 100 });

    s.quantize (240, 1.0);
    CHECK (s.getNotes()[1].start == 240);
    CHECK (s.getNotes()[2].start == 480);

    NoteSequence half;
    half.add ({ 60, 0.8f, 200, 100 }); // 40 ticks early
    half.quantize (240, 0.5);
    CHECK (half.getNotes()[0].start == 220); // half the distance
    CHECK (sortedAndValid (half));
}

TEST_CASE ("Transpose shifts pitches and keeps them in range", "[notes]")
{
    NoteSequence s;
    s.add ({ 60, 0.8f, 0, 100 });
    s.add ({ 125, 0.8f, 100, 100 });
    s.transpose (5);
    CHECK (s.getNotes()[0].pitch == 65);
    CHECK (s.getNotes()[1].pitch == 127);
    s.transpose (-200);
    CHECK (std::all_of (s.getNotes().begin(), s.getNotes().end(), [] (const Note& n) { return n.pitch == 0; }));
}
