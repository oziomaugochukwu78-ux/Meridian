// SMPTE timecode: labels at every rate, drop-frame numbering, round trips, parsing.

#include <midi/Timecode.h>

#include <catch2/catch_test_macros.hpp>

using namespace meridian::midi;

TEST_CASE ("Timecode: whole-frame rates count plainly", "[timecode]")
{
    CHECK (format (48000 * 3600, 48000.0, FrameRate::fps25) == "01:00:00:00");
    CHECK (format (72000, 48000.0, FrameRate::fps25) == "00:00:01:12");   // 1.5 s = frame 37.5 -> 37
    CHECK (format (47999, 48000.0, FrameRate::fps24) == "00:00:00:23");   // the last sample of second 0
    CHECK (format (48000, 48000.0, FrameRate::fps24) == "00:00:01:00");   // a frame's first sample is in it
    CHECK (format (0, 48000.0, FrameRate::fps30, 108000) == "01:00:00:00"); // a session starting at 1 h
}

TEST_CASE ("Timecode: 29.97 drop-frame skips 0 and 1 each minute except every tenth", "[timecode]")
{
    const auto r = FrameRate::fps29_97drop;
    CHECK (toString (fromFrameCount (1799, r), r) == "00:00:59;29");
    CHECK (toString (fromFrameCount (1800, r), r) == "00:01:00;02");      // ;00 and ;01 never appear
    CHECK (toString (fromFrameCount (17981, r), r) == "00:09:59;29");
    CHECK (toString (fromFrameCount (17982, r), r) == "00:10:00;00");     // the tenth minute keeps them
    // One hour of real time is 107892 frames at 29.97, and drop-frame labels it exactly 01:00:00;00.
    CHECK (format (48000 * 3600, 48000.0, r) == "01:00:00;00");
    // Non-drop 29.97 drifts: the same hour reads 3.6 s short.
    CHECK (format (48000 * 3600, 48000.0, FrameRate::fps29_97) == "00:59:56:12");
}

TEST_CASE ("Timecode: every frame number round-trips at every rate", "[timecode]")
{
    for (const auto r : { FrameRate::fps23_976, FrameRate::fps24, FrameRate::fps25, FrameRate::fps29_97,
                          FrameRate::fps29_97drop, FrameRate::fps30 })
    {
        bool same = true;
        for (int64_t f = 0; f < 40000 && same; f += 7)
            same = toFrameCount (fromFrameCount (f, r), r) == f;
        CHECK (same);
        // And sample positions: a frame's first sample falls in that frame, the one before it does not.
        for (int64_t f = 1; f < 2000 && same; f += 13)
        {
            const auto s = sampleAtFrame (f, 48000.0, r);
            same = frameAtSample (s, 48000.0, r) == f && frameAtSample (s - 1, 48000.0, r) == f - 1;
        }
        CHECK (same);
    }
}

TEST_CASE ("Timecode: parsing", "[timecode]")
{
    const auto tc = parse ("01:02:03:04", FrameRate::fps25);
    REQUIRE (tc.has_value());
    CHECK (toFrameCount (*tc, FrameRate::fps25) == ((3600 + 120 + 3) * 25 + 4));
    CHECK (parse ("00:01:00;02", FrameRate::fps29_97drop).has_value());
    CHECK (! parse ("00:01:00;01", FrameRate::fps29_97drop).has_value()); // a dropped number
    CHECK (parse ("00:10:00;00", FrameRate::fps29_97drop).has_value());
    CHECK (! parse ("00:00:00:25", FrameRate::fps25).has_value());          // only 0-24 at 25 fps
    CHECK (! parse ("24:00:00:00", FrameRate::fps25).has_value());
    CHECK (! parse ("1:2:3", FrameRate::fps25).has_value());
    CHECK (! parse ("aa:00:00:00", FrameRate::fps25).has_value());
    CHECK (frameRateFromName ("29.97df") == FrameRate::fps29_97drop);
    CHECK (! frameRateFromName ("31").has_value());
}
