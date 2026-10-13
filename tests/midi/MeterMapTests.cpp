// Time signatures, and the click following a tempo and meter map.

#include <dsp/Metronome.h>
#include <midi/MeterMap.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace meridian;
using Catch::Approx;

namespace
{
/** 4/4 for bars 0-1, 3/4 for bars 2-3, 6/8 from bar 4. */
midi::MeterMap mixedMeters()
{
    midi::MeterMap m;
    m.addChange (2, 3, 4);
    m.addChange (4, 6, 8);
    return m;
}
} // namespace

TEST_CASE ("Meter map: bars to ticks across meter changes", "[meter]")
{
    const auto m = mixedMeters();
    CHECK (m.barToTicks (0) == 0);
    CHECK (m.barToTicks (2) == 7680);            // two bars of 4/4
    CHECK (m.barToTicks (3) == 7680 + 2880);     // one bar of 3/4
    CHECK (m.barToTicks (4) == 13440);
    CHECK (m.barToTicks (5) == 13440 + 2880);    // 6/8: six eighths = 2880 ticks
    CHECK (m.barToTicks (-1) == -3840);          // before the start: the first meter extends back
}

TEST_CASE ("Meter map: bar, beat and tick of a position", "[meter]")
{
    const auto m = mixedMeters();
    const auto p = m.positionOf (10560 + 960 + 5); // bar 3 (3/4), beat 1, 5 ticks in
    CHECK (p.bar == 3);
    CHECK (p.beat == 1);
    CHECK (p.tick == 5);
    const auto q = m.positionOf (13440 + 480 * 5); // bar 4 (6/8), beat 5 (eighths)
    CHECK (q.bar == 4);
    CHECK (q.beat == 5);
    CHECK (q.tick == 0);
}

TEST_CASE ("Meter map: the next beat and whether it starts a bar", "[meter]")
{
    const auto m = mixedMeters();
    auto b = m.beatAtOrAfter (7680);          // exactly bar 2
    CHECK ((b.tick == 7680 && b.barStart));
    b = m.beatAtOrAfter (7681);               // just after: beat 2 of bar 2
    CHECK ((b.tick == 8640 && ! b.barStart));
    b = m.beatAtOrAfter (10560 - 1);          // last tick of bar 2: the next bar
    CHECK ((b.tick == 10560 && b.barStart));
    b = m.beatAtOrAfter (13440 + 1);          // in 6/8 the beats are eighths
    CHECK ((b.tick == 13440 + 480 && ! b.barStart));
}

TEST_CASE ("Meter map: invalid meters are made valid", "[meter]")
{
    midi::MeterMap m (0, 3);
    CHECK (m.getChanges().front().numerator == 1);
    CHECK (m.getChanges().front().denominator == 4);
    m.addChange (1, 99, 64);
    CHECK (m.meterAt (1).numerator == 32);
    CHECK (m.meterAt (1).denominator == 4);
}

TEST_CASE ("Click grid: every click on the exact sample of its beat, accents on bar starts", "[meter]")
{
    midi::TempoMap tempo (120.0);
    tempo.addChange (7680, 90.0);              // from bar 2
    const auto meter = mixedMeters();
    const double rate = 44100.0;
    const midi::ClickGrid grid (tempo, meter, rate);

    dsp::Metronome m;
    m.prepare (rate);
    const int total = (int) (rate * 12.0);
    std::vector<float> x ((size_t) total, 0.0f);
    for (int done = 0; done < total; done += 300)
        m.render (x.data() + done, std::min (300, total - done), done, grid);

    // Expected onsets: every beat tick, through the tempo map's own rounding.
    bool allExact = true;
    int beats = 0, accents = 0, accentsRight = 0;
    for (auto beat = meter.beatAtOrAfter (0);; beat = meter.beatAtOrAfter (beat.tick + 1))
    {
        const auto s = tempo.ticksToSamples (beat.tick, rate);
        if (s + 2000 >= total)
            break;
        allExact = allExact && x[(size_t) s] == 0.0f && x[(size_t) s + 1] != 0.0f && (s == 0 || std::abs (x[(size_t) s - 1]) < 0.01f);
        float peak = 0.0f;
        for (int i = 0; i < 1700; ++i)
            peak = std::max (peak, std::abs (x[(size_t) s + (size_t) i]));
        const bool loud = 20.0 * std::log10 (peak) > -8.0; // accent -6 dB, others -10 dB
        accents += beat.barStart ? 1 : 0;
        accentsRight += loud == beat.barStart ? 1 : 0;
        ++beats;
    }
    CHECK (allExact);
    CHECK (beats > 20);
    CHECK (accents >= 5);
    CHECK (accentsRight == beats);
}

TEST_CASE ("Click grid at a constant 120 BPM, 4/4 is bit-identical to the plain click", "[meter]")
{
    const midi::ClickGrid grid (midi::TempoMap (120.0), midi::MeterMap (4, 4), 48000.0);
    dsp::Metronome a, b;
    a.prepare (48000.0);
    b.prepare (48000.0);
    std::vector<float> x (48000 * 4, 0.0f), y (48000 * 4, 0.0f);
    for (int done = 0; done < (int) x.size(); done += 512)
    {
        const int n = std::min (512, (int) x.size() - done);
        a.render (x.data() + done, n, done);
        b.render (y.data() + done, n, done, grid);
    }
    CHECK (x == y);
}
