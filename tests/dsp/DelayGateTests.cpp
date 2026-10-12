// Measured behaviour of the built-in delay and gate.

#include <dsp/Gate.h>
#include <dsp/StereoDelay.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;

StereoDelay makeDelay (double ms, double feedback, double mix, double highCut = 20000.0, bool pingPong = false)
{
    StereoDelay d;
    d.prepare (rate);
    StereoDelay::Params p;
    p.timeMs = ms;
    p.feedback = feedback;
    p.mix = mix;
    p.highCutHz = highCut;
    p.pingPong = pingPong;
    d.setParams (p);
    return d;
}

std::vector<float> noise (int n, uint32_t seed)
{
    std::vector<float> x ((size_t) n);
    for (auto& v : x)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        v = (float) (seed / 2147483648.0 - 1.0) * 0.5f;
    }
    return x;
}

Gate makeGate (double threshold, double hysteresis, double holdMs)
{
    Gate g;
    g.prepare (rate);
    Gate::Params p;
    p.thresholdDb = threshold;
    p.hysteresisDb = hysteresis;
    p.holdMs = holdMs;
    p.attackMs = 0.1;
    p.releaseMs = 20.0;
    p.rangeDb = -80.0;
    g.setParams (p);
    return g;
}
} // namespace

TEST_CASE ("Delay: echoes arrive exactly on time, each repeat exactly `feedback` quieter", "[delay]")
{
    auto d = makeDelay (10.0, 0.5, 1.0); // 480 samples
    std::vector<float> l (2000, 0.0f), r (2000, 0.0f);
    l[0] = 1.0f;
    d.process (l.data(), r.data(), (int) l.size());
    CHECK (l[0] == 1.0f);
    CHECK (l[480] == 1.0f);
    CHECK (l[960] == 0.5f);
    CHECK (l[1440] == 0.25f);
    CHECK (std::all_of (l.begin() + 1, l.begin() + 480, [] (float v) { return v == 0.0f; }));
    CHECK (std::all_of (r.begin(), r.end(), [] (float v) { return v == 0.0f; })); // nothing leaks across
}

TEST_CASE ("Delay: ping-pong repeats alternate sides", "[delay]")
{
    auto d = makeDelay (10.0, 0.5, 1.0, 20000.0, true);
    std::vector<float> l (2000, 0.0f), r (2000, 0.0f);
    l[0] = 1.0f; // the summed input (0.5) enters on the left
    d.process (l.data(), r.data(), (int) l.size());
    CHECK (l[480] == 0.5f);
    CHECK (r[480] == 0.0f);
    CHECK (r[960] == 0.25f);
    CHECK (l[960] == 0.0f);
    CHECK (l[1440] == 0.125f);
    CHECK (r[1440] == 0.0f);
}

TEST_CASE ("Delay: mix 0 passes audio bit for bit; the high-cut darkens each repeat", "[delay]")
{
    auto dry = makeDelay (100.0, 0.9, 0.0);
    auto original = noise (48000, 9);
    auto l = original, r = original;
    dry.process (l.data(), r.data(), (int) l.size());
    CHECK (l == original);
    CHECK (r == original);

    // With a 2 kHz high-cut in the loop, each repeat of a click loses more than feedback^2 of its energy.
    auto dark = makeDelay (10.0, 0.9, 1.0, 2000.0);
    std::vector<float> x (2400, 0.0f), y (2400, 0.0f);
    x[0] = 1.0f;
    dark.process (x.data(), y.data(), (int) x.size());
    auto energy = [&] (int from) { double e = 0; for (int i = from; i < from + 480; ++i) e += (double) x[(size_t) i] * x[(size_t) i]; return e; };
    CHECK (energy (960) < 0.81 * energy (480));
    CHECK (energy (1440) < 0.81 * energy (960));
}

TEST_CASE ("Delay: stable at the highest feedback; invalid settings stay safe", "[delay]")
{
    auto d = makeDelay (50.0, 0.95, 1.0);
    auto x = noise (48000, 3);
    std::vector<float> silence (48000 * 4, 0.0f);
    auto y = x;
    d.process (x.data(), y.data(), (int) x.size());
    auto tailL = silence, tailR = silence;
    d.process (tailL.data(), tailR.data(), (int) tailL.size());
    float worst = 0.0f;
    for (const auto v : tailL) worst = std::max (worst, std::abs (v));
    CHECK (std::isfinite (worst));
    CHECK (worst < 10.0f);
    double early = 0, late = 0;
    for (int i = 0; i < 48000; ++i) { early += tailL[(size_t) i] * tailL[(size_t) i]; late += tailL[(size_t) (i + 3 * 48000)] * tailL[(size_t) (i + 3 * 48000)]; }
    CHECK (late < early); // decaying, not growing

    StereoDelay bad;
    bad.prepare (rate);
    StereoDelay::Params p;
    p.timeMs = std::numeric_limits<double>::quiet_NaN();
    p.feedback = 7.0;
    p.mix = -1.0;
    bad.setParams (p);
    auto z = noise (4800, 4);
    bad.process (z.data(), nullptr, (int) z.size());
    CHECK (std::all_of (z.begin(), z.end(), [] (float v) { return std::isfinite (v); }));
}

TEST_CASE ("Gate: open, audio passes bit for bit; closed, exactly the range lower", "[gate]")
{
    auto g = makeGate (-40.0, 4.0, 0.0);
    std::vector<float> loud (9600, 0.5f);
    g.process (loud.data(), nullptr, (int) loud.size());
    CHECK (std::all_of (loud.begin() + 480, loud.end(), [] (float v) { return v == 0.5f; })); // after the 0.1 ms attack settles

    std::vector<float> quiet (48000, 0.001f); // -60 dB: below the -44 dB close level
    g.process (quiet.data(), nullptr, (int) quiet.size());
    CHECK (20.0 * std::log10 (quiet.back() / 0.001) == Approx (-80.0).margin (1.0e-6));
    CHECK (! g.isOpen());
}

TEST_CASE ("Gate: holds open for exactly the hold time after the level drops", "[gate]")
{
    auto g = makeGate (-40.0, 4.0, 50.0); // 2400 samples
    std::vector<float> loud (4800, 0.5f);
    g.process (loud.data(), nullptr, (int) loud.size());
    std::vector<float> quiet (4800, 0.001f);
    g.process (quiet.data(), nullptr, (int) quiet.size());
    CHECK (quiet[2399] == 0.001f);  // the last held sample: still unity
    CHECK (quiet[2401] < 0.001f);   // then the release has begun
}

TEST_CASE ("Gate: hysteresis stops chatter on a level hovering at the threshold", "[gate]")
{
    // Alternating every 10 ms between 2 dB above and 2 dB below the -40 dB threshold.
    auto hovering = [] (Gate g)
    {
        int closings = 0;
        bool wasOpen = false;
        for (int block = 0; block < 100; ++block)
        {
            std::vector<float> x (480, (float) std::pow (10.0, (block % 2 == 0 ? -38.0 : -42.0) / 20.0));
            for (auto& v : x)
            {
                g.process (&v, nullptr, 1);
                if (wasOpen && ! g.isOpen())
                    ++closings;
                wasOpen = g.isOpen();
            }
        }
        return closings;
    };
    CHECK (hovering (makeGate (-40.0, 4.0, 0.0)) == 0);  // -42 dB is above the -44 dB close level
    CHECK (hovering (makeGate (-40.0, 0.0, 0.0)) > 40);  // without hysteresis it chatters (the test can fail)
}
