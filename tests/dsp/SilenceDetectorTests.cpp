// Strip Silence's detector: region edges to the sample, gaps kept, pads, merging, streaming.

#include <dsp/SilenceDetector.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace meridian::dsp;

namespace
{
/** Silence with bursts of a 0.5-amplitude square wave (never near zero) over [start, end). */
std::vector<float> bursts (int length, std::initializer_list<std::pair<int, int>> spans)
{
    std::vector<float> x ((size_t) length, 0.0f);
    for (const auto& [start, end] : spans)
        for (int i = start; i < end; ++i)
            x[(size_t) i] = (i % 2 == 0) ? 0.5f : -0.5f;
    return x;
}

std::vector<SilenceDetector::Region> detect (const std::vector<float>& x, int64_t minSilence, int64_t before, int64_t after,
                                             int blockSize = 1 << 20, float gain = 1.0f)
{
    SilenceDetector d (0.01f, minSilence, before, after);
    for (size_t at = 0; at < x.size(); at += (size_t) blockSize)
    {
        const float* channel = x.data() + at;
        d.feed (&channel, 1, (int) std::min (x.size() - at, (size_t) blockSize), gain);
    }
    return d.regions();
}
} // namespace

TEST_CASE ("SilenceDetector: regions start and end exactly at the audible samples", "[silence]")
{
    const auto r = detect (bursts (1000, { { 100, 200 }, { 600, 700 } }), 50, 0, 0);
    REQUIRE (r.size() == 2);
    CHECK (r[0].start == 100);
    CHECK (r[0].end == 200);
    CHECK (r[1].start == 600);
    CHECK (r[1].end == 700);
}

TEST_CASE ("SilenceDetector: gaps shorter than the minimum silence stay inside a region", "[silence]")
{
    // A 49-sample gap is kept (minimum 50); a 50-sample gap splits.
    const auto kept = detect (bursts (1000, { { 100, 200 }, { 249, 300 } }), 50, 0, 0);
    REQUIRE (kept.size() == 1);
    CHECK (kept[0].end == 300);
    const auto split = detect (bursts (1000, { { 100, 200 }, { 250, 300 } }), 50, 0, 0);
    CHECK (split.size() == 2);
}

TEST_CASE ("SilenceDetector: pads widen regions, stop at the signal edges, and merge regions they join", "[silence]")
{
    const auto r = detect (bursts (1000, { { 5, 200 }, { 300, 400 }, { 980, 1000 } }), 50, 10, 100);
    REQUIRE (r.size() == 2);
    CHECK (r[0].start == 0);      // 5 - 10, clipped
    CHECK (r[0].end == 500);      // 200 + 100 passes 300 - 10: merged, then 400 + 100
    CHECK (r[1].start == 970);
    CHECK (r[1].end == 1000);     // clipped
}

TEST_CASE ("SilenceDetector: the result does not depend on the block size", "[silence]")
{
    const auto x = bursts (5000, { { 17, 900 }, { 1500, 1501 }, { 3000, 4999 } });
    const auto whole = detect (x, 100, 3, 7);
    for (const int block : { 1, 7, 64, 1000 })
    {
        const auto r = detect (x, 100, 3, 7, block);
        REQUIRE (r.size() == whole.size());
        for (size_t i = 0; i < r.size(); ++i)
        {
            CHECK (r[i].start == whole[i].start);
            CHECK (r[i].end == whole[i].end);
        }
    }
}

TEST_CASE ("SilenceDetector: the clip gain scales the signal against the threshold", "[silence]")
{
    // 0.5 at -40 dB gain is 0.005: below the 0.01 threshold, so nothing is audible.
    CHECK (detect (bursts (1000, { { 100, 200 } }), 50, 0, 0, 1 << 20, 0.01f).empty());
    CHECK (detect (bursts (1000, { { 100, 200 } }), 50, 0, 0, 1 << 20, 0.0f).empty());
    CHECK (detect (std::vector<float> (1000, 0.0f), 50, 0, 0).empty());
}
