#include <dsp/DelayLine.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using meridian::dsp::DelayLine;

namespace
{
std::vector<float> ramp (int n, float start)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = start + (float) i;
    return v;
}
} // namespace

TEST_CASE ("Delay line shifts a signal by exactly N samples across blocks", "[delay]")
{
    DelayLine line (100);
    const int delay = 37;
    std::vector<float> output;

    // Feed 1, 2, 3, ... in uneven blocks.
    float next = 1.0f;
    for (const int block : { 5, 64, 1, 30, 100 })
    {
        auto in = ramp (block, next);
        next += (float) block;
        std::vector<float> out ((size_t) block);
        const float* ip[] = { in.data() };
        float* op[] = { out.data() };
        line.process (ip, op, 1, block, delay);
        output.insert (output.end(), out.begin(), out.end());
    }

    for (size_t i = 0; i < output.size(); ++i)
        REQUIRE (output[i] == (i < (size_t) delay ? 0.0f : (float) (i - (size_t) delay + 1)));
}

TEST_CASE ("Zero delay passes through; delay is clamped to capacity", "[delay]")
{
    DelayLine line (8);
    auto in = ramp (16, 1.0f);
    std::vector<float> out (16);
    const float* ip[] = { in.data() };
    float* op[] = { out.data() };

    line.process (ip, op, 1, 16, 0);
    REQUIRE (out == in);

    line.reset();
    line.process (ip, op, 1, 16, 1000); // clamped to 8
    REQUIRE (out[7] == 0.0f);
    REQUIRE (out[8] == 1.0f);
}

TEST_CASE ("Stereo channels are delayed independently and identically", "[delay]")
{
    DelayLine line (10);
    auto left = ramp (20, 1.0f);
    auto right = ramp (20, 101.0f);
    std::vector<float> outL (20), outR (20);
    const float* ip[] = { left.data(), right.data() };
    float* op[] = { outL.data(), outR.data() };

    line.process (ip, op, 2, 20, 4);
    REQUIRE (outL[4] == 1.0f);
    REQUIRE (outR[4] == 101.0f);
    REQUIRE (outR[19] == 116.0f);
}
