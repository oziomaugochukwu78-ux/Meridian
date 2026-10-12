// Measured behaviour of the click (metronome).

#include <dsp/Metronome.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
double db (double g) { return 20.0 * std::log10 (std::max (g, 1.0e-15)); }

Metronome make (double rate, double bpm, int beatsPerBar = 4)
{
    Metronome m;
    m.prepare (rate);
    Metronome::Params p;
    p.bpm = bpm;
    p.beatsPerBar = beatsPerBar;
    m.setParams (p);
    return m;
}

std::vector<float> renderInBlocks (Metronome m, int total, int blockSize, int64_t start = 0)
{
    std::vector<float> out ((size_t) total, 0.0f);
    for (int done = 0; done < total; done += blockSize)
        m.render (out.data() + done, std::min (blockSize, total - done), start + done);
    return out;
}

float peakIn (const std::vector<float>& x, int64_t from, int64_t to)
{
    float peak = 0.0f;
    for (auto i = from; i < to && i < (int64_t) x.size(); ++i)
        peak = std::max (peak, std::abs (x[(size_t) i]));
    return peak;
}

int signChanges (const std::vector<float>& x, int64_t from, int length)
{
    int changes = 0;
    for (auto i = from + 1; i < from + length; ++i)
        changes += (x[(size_t) i - 1] < 0.0f) != (x[(size_t) i] < 0.0f) ? 1 : 0;
    return changes;
}
} // namespace

TEST_CASE ("Metronome: every click starts on its exact beat sample, silence between", "[metronome]")
{
    for (const auto& [rate, bpm] : { std::pair { 48000.0, 120.0 }, std::pair { 44100.0, 97.0 }, std::pair { 96000.0, 133.3 } })
    {
        auto m = make (rate, bpm);
        const int total = (int) (rate * 6.0);
        const auto x = renderInBlocks (m, total, total);
        const int length = (int) std::lround (Metronome::clickSeconds * rate);
        bool exact = true;
        for (int64_t b = 0; m.beatStart (b + 1) < total; ++b)
        {
            const auto onset = m.beatStart (b);
            CHECK (onset == (int64_t) std::llround ((double) b * 60.0 * rate / bpm));
            // Zero phase at the onset, sound right after it, nothing from the end of the click to the next beat.
            exact = exact && x[(size_t) onset] == 0.0f && x[(size_t) onset + 1] != 0.0f
                    && peakIn (x, onset + length, m.beatStart (b + 1)) == 0.0f
                    && (onset == 0 || x[(size_t) onset - 1] == 0.0f);
        }
        CHECK (exact);
    }
}

TEST_CASE ("Metronome: output is bit-identical whatever the block size", "[metronome]")
{
    auto m = make (44100.0, 97.0);
    const int total = 44100 * 4;
    const auto reference = renderInBlocks (m, total, total, 1234);
    for (const int block : { 1, 7, 64, 511, 4096 })
        CHECK (renderInBlocks (m, total, block, 1234) == reference);
}

TEST_CASE ("Metronome: levels, accent pattern and pitch", "[metronome]")
{
    auto m = make (48000.0, 120.0, 3); // 3/4: accents on beats 0, 3, 6
    const auto x = renderInBlocks (m, 48000 * 4, 512);
    const int length = (int) std::lround (Metronome::clickSeconds * 48000.0);
    for (int64_t b = 0; b < 7; ++b)
    {
        const auto onset = m.beatStart (b);
        const bool accent = b % 3 == 0;
        CHECK (db (peakIn (x, onset, onset + length)) == Approx (accent ? -6.0 : -10.0).margin (1.0e-4));
        // Sign changes over the 40 ms click: 2 per cycle, 2 kHz -> 160, 1 kHz -> 80.
        CHECK (signChanges (x, onset, length) == Approx (accent ? 160.0 : 80.0).margin (2.0));
    }
}

TEST_CASE ("Metronome: rendering from any position finds the next beat", "[metronome]")
{
    auto m = make (48000.0, 120.0);
    const int64_t start = m.beatStart (5) - 100;
    const auto x = renderInBlocks (m, 2000, 2000, start);
    CHECK (peakIn (x, 0, 100) == 0.0f);
    CHECK (x[100] == 0.0f);
    CHECK (x[101] != 0.0f);
    CHECK (db (peakIn (x, 100, 2000)) == Approx (-10.0).margin (1.0e-4)); // beat 5 of 4/4: not an accent
}

TEST_CASE ("Metronome: invalid settings stay safe", "[metronome]")
{
    Metronome m;
    m.prepare (std::numeric_limits<double>::quiet_NaN());
    Metronome::Params p;
    p.bpm = 0.0;
    p.beatsPerBar = -2;
    p.levelDb = std::numeric_limits<double>::infinity();
    m.setParams (p);
    std::vector<float> x (48000, 0.0f);
    m.render (x.data(), (int) x.size(), -500);
    CHECK (std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v) && std::abs (v) <= 1.0f; }));
}
