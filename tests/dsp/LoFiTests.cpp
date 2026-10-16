// The lo-fi effect: exact when everything is off, wow measured in cents, vinyl
// noise deterministic and at its level, bit reduction on the grid, the tone filter.

#include <dsp/FloatCompare.h>
#include <dsp/LoFi.h>
#include <dsp/SignalAnalysis.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace meridian::dsp;

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

std::vector<float> sine (double frequency, int length, float amplitude = 0.5f)
{
    std::vector<float> x ((size_t) length);
    for (int i = 0; i < length; ++i)
        x[(size_t) i] = amplitude * (float) std::sin (2.0 * pi * frequency * i / rate);
    return x;
}

LoFi::Params off()
{
    LoFi::Params p;
    p.wow = p.flutter = p.noise = 0.0;
    p.bits = 16;
    p.downsample = 1;
    p.toneHz = 20000.0;
    p.mix = 1.0;
    return p;
}

std::vector<float> run (LoFi::Params p, std::vector<float> x)
{
    LoFi fx;
    fx.prepare (rate);
    fx.setParams (p);
    for (size_t at = 0; at < x.size(); at += 512)
        fx.process (x.data() + at, nullptr, (int) std::min<size_t> (512, x.size() - at));
    return x;
}
} // namespace

TEST_CASE ("LoFi: with every control off it is a pure delay of getLatencySamples", "[lofi]")
{
    const auto in = sine (1000.0, 9600);
    const auto out = run (off(), in);
    LoFi probe;
    probe.prepare (rate);
    const int d = probe.getLatencySamples();
    CHECK (d == 480);
    bool exact = true;
    for (size_t i = (size_t) d; i < in.size(); ++i)
        exact = exact && exactlyEqual (out[i], in[i - (size_t) d]);
    CHECK (exact);

    // Mix 0 is the same pure delay, whatever the other controls say.
    auto everything = off();
    everything.wow = everything.flutter = everything.noise = 1.0;
    everything.bits = 6;
    everything.toneHz = 1000.0;
    everything.mix = 0.0;
    const auto dry = run (everything, in);
    bool dryExact = true;
    for (size_t i = (size_t) d; i < in.size(); ++i)
        dryExact = dryExact && exactlyEqual (dry[i], in[i - (size_t) d]);
    CHECK (dryExact);
}

TEST_CASE ("LoFi: full wow bends the pitch by +/-12 cents", "[lofi]")
{
    auto p = off();
    p.wow = 1.0;
    const auto out = run (p, sine (1000.0, (int) (4.5 * rate)));
    double low = 1.0e9, high = 0.0;
    for (int w = 0; w < 80; ++w) // 50 ms windows over 4 s, past the line's start
    {
        const auto f = estimateFrequency (out.data() + 2400 + w * 2400, 2400, rate);
        low = std::min (low, f);
        high = std::max (high, f);
    }
    const double centsUp = 1200.0 * std::log2 (high / 1000.0), centsDown = 1200.0 * std::log2 (1000.0 / low);
    CHECK (centsUp > 10.5);
    CHECK (centsUp < 13.0);
    CHECK (centsDown > 10.5);
    CHECK (centsDown < 13.0);
}

TEST_CASE ("LoFi: vinyl noise is repeatable, quiet hiss with clicks above it", "[lofi]")
{
    auto p = off();
    p.noise = 1.0;
    const std::vector<float> silence ((size_t) (2.0 * rate), 0.0f);
    const auto a = run (p, silence), b = run (p, silence);
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i)
        same = exactlyEqual (a[i], b[i]);
    CHECK (same);
    const auto stats = measureLevels (a.data(), (int) a.size());
    CHECK (gainToDb (stats.rms) > -60.0);
    CHECK (gainToDb (stats.rms) < -30.0);
    CHECK (stats.peak > 0.015);   // the clicks
}

TEST_CASE ("LoFi: bit reduction puts every sample on the grid; the tone control darkens", "[lofi]")
{
    auto p = off();
    p.bits = 8;
    const auto crushed = run (p, sine (440.0, 9600));
    bool onGrid = true;
    for (const float y : crushed)
        onGrid = onGrid && std::abs (y * 128.0 - std::round (y * 128.0)) < 1.0e-4;
    CHECK (onGrid);

    auto dark = off();
    dark.toneHz = 1000.0;
    const auto high = run (dark, sine (10000.0, 9600));
    const auto passed = run (dark, sine (200.0, 9600));
    CHECK (gainToDb (measureLevels (high.data() + 4800, 4800).peak / 0.5) < -30.0);
    CHECK (gainToDb (measureLevels (passed.data() + 4800, 4800).peak / 0.5) > -1.0);
}
