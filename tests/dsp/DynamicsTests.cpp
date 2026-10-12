// Measured behaviour of the built-in compressor and limiter.

#include <dsp/Biquad.h>
#include <dsp/Compressor.h>
#include <dsp/Limiter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;
constexpr double pi = 3.14159265358979323846;

double db (double g) { return 20.0 * std::log10 (std::max (g, 1.0e-15)); }
double gainOf (double dB) { return std::pow (10.0, dB / 20.0); }

std::vector<float> noise (int n, uint32_t seed, float amplitude)
{
    std::vector<float> x ((size_t) n);
    for (auto& v : x)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        v = (float) (seed / 2147483648.0 - 1.0) * amplitude;
    }
    return x;
}

/** Noise shaped like real programme material: nothing above 16 kHz (8th-order
    low-pass), peak-normalised to `amplitude`. */
std::vector<float> programme (int n, uint32_t seed, float amplitude)
{
    auto x = noise (n, seed, 1.0f);
    std::array<Biquad, 4> lowPass;
    for (auto& f : lowPass)
        f.setCoefficients (Biquad::design (Biquad::Type::lowPass, 16000.0, 0.7071, 0.0, rate));
    float peak = 0.0f;
    for (auto& v : x)
    {
        double y = v;
        for (auto& f : lowPass)
            y = f.process (y);
        v = (float) y;
        peak = std::max (peak, std::abs (v));
    }
    for (auto& v : x)
        v *= amplitude / peak;
    return x;
}

Compressor makeCompressor (double threshold, double ratio, double knee, double attackMs, double releaseMs)
{
    Compressor c;
    c.prepare (rate);
    Compressor::Params p;
    p.thresholdDb = threshold;
    p.ratio = ratio;
    p.kneeDb = knee;
    p.attackMs = attackMs;
    p.releaseMs = releaseMs;
    c.setParams (p);
    return c;
}

/** Output level (dB) for a steady (DC) input at `inputDb`, after 1 s. */
double steadyOutputDb (Compressor& c, double inputDb)
{
    c.reset();
    std::vector<float> x ((size_t) rate, (float) gainOf (inputDb));
    c.process (x.data(), nullptr, (int) x.size());
    return db (x.back());
}

/** True peak by independent means: 16x band-limited interpolation with a long
    (128-tap) Kaiser-windowed sinc, a much finer and longer filter than the
    limiter's own detector. */
double truePeakDb (const std::vector<float>& x, size_t from)
{
    constexpr int half = 64, steps = 16;
    constexpr double beta = 10.0;
    auto i0 = [] (double v)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    std::vector<double> kernel ((size_t) (steps * 2 * half));
    for (int s = 0; s < steps; ++s)
        for (int j = 0; j < 2 * half; ++j)
        {
            const double a = (double) s / steps + (half - 1 - j);
            const double sinc = std::abs (a) < 1.0e-12 ? 1.0 : std::sin (pi * a) / (pi * a);
            const double r = a / half;
            kernel[(size_t) (s * 2 * half + j)] = sinc * i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (beta);
        }

    double peak = 0.0;
    for (size_t k = std::max (from, (size_t) half); k + half < x.size(); ++k)
        for (int s = 0; s < steps; ++s)
        {
            double y = 0.0;
            const double* h = kernel.data() + s * 2 * half;
            for (int j = 0; j < 2 * half; ++j)
                y += h[j] * x[k - (size_t) half + 1 + (size_t) j];
            peak = std::max (peak, std::abs (y));
        }
    return db (peak);
}

Limiter makeLimiter (double inputGainDb, double ceilingDb, double releaseMs, bool truePeak)
{
    Limiter l;
    l.prepare (rate);
    Limiter::Params p;
    p.inputGainDb = inputGainDb;
    p.ceilingDb = ceilingDb;
    p.releaseMs = releaseMs;
    p.truePeak = truePeak;
    l.setParams (p);
    return l;
}
} // namespace

//==============================================================================
TEST_CASE ("Compressor: the static curve is threshold + overshoot / ratio", "[compressor]")
{
    auto c = makeCompressor (-20.0, 4.0, 0.0, 1.0, 50.0);
    CHECK (c.staticCurve (-30.0) == Approx (-30.0).margin (1.0e-12)); // below threshold: untouched
    CHECK (c.staticCurve (-8.0) == Approx (-17.0).margin (1.0e-12));  // 12 dB over -> 3 dB over

    // Measured on steady signals across the whole range.
    for (double in = -40.0; in <= 0.0; in += 2.0)
        CHECK (steadyOutputDb (c, in) == Approx (c.staticCurve (in)).margin (0.01));
}

TEST_CASE ("Compressor: the soft knee is smooth and meets both straight lines", "[compressor]")
{
    auto c = makeCompressor (-20.0, 4.0, 12.0, 1.0, 50.0);
    CHECK (c.staticCurve (-20.0) == Approx (-20.0 - 0.75 * 36.0 / 24.0).margin (1.0e-12)); // 1.125 dB at threshold
    CHECK (c.staticCurve (-26.0) == Approx (-26.0).margin (1.0e-12));                      // knee start: no reduction
    CHECK (c.staticCurve (-14.0) == Approx (-20.0 + 6.0 / 4.0).margin (1.0e-12));          // knee end: on the ratio line

    // Slope rises monotonically from 1 to 1/ratio through the knee, with no step.
    double previousSlope = 1.0 + 1.0e-9;
    for (double in = -30.0; in <= -10.0; in += 0.25)
    {
        const double slope = (c.staticCurve (in + 0.01) - c.staticCurve (in)) / 0.01;
        CHECK (slope <= previousSlope + 1.0e-6);
        CHECK (slope >= 0.25 - 1.0e-6);
        previousSlope = slope;
    }
}

TEST_CASE ("Compressor: attack and release times are the 63% times", "[compressor]")
{
    // Step from below threshold into 15 dB of gain reduction.
    auto c = makeCompressor (-20.0, 4.0, 0.0, 5.0, 1000.0);
    const int stepAt = 4800;
    std::vector<float> x ((size_t) rate, 1.0f);
    std::fill (x.begin(), x.begin() + stepAt, (float) gainOf (-40.0));
    const auto in = x;
    c.process (x.data(), nullptr, (int) x.size());
    int reached = -1;
    for (int i = stepAt; i < (int) x.size() && reached < 0; ++i)
        if (-db (x[(size_t) i] / in[(size_t) i]) >= 15.0 * (1.0 - std::exp (-1.0)))
            reached = i - stepAt;
    CHECK (reached / rate * 1000.0 == Approx (5.0).margin (0.05));

    // Back down below threshold: the reduction falls to 1/e of 15 dB after the release time.
    auto d = makeCompressor (-20.0, 4.0, 0.0, 0.05, 100.0);
    std::vector<float> y ((size_t) rate, (float) gainOf (-40.0));
    std::fill (y.begin(), y.begin() + stepAt, 1.0f);
    const auto yin = y;
    d.process (y.data(), nullptr, (int) y.size());
    reached = -1;
    for (int i = stepAt; i < (int) y.size() && reached < 0; ++i)
        if (-db (y[(size_t) i] / yin[(size_t) i]) <= 15.0 * std::exp (-1.0))
            reached = i - stepAt;
    CHECK (reached / rate * 1000.0 == Approx (100.0).margin (1.0));
}

TEST_CASE ("Compressor: a steady tone is compressed by its peak, without ripple", "[compressor]")
{
    auto c = makeCompressor (-18.0, 4.0, 0.0, 10.0, 150.0);
    std::vector<float> x ((size_t) rate * 2);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (0.5 * std::sin (2.0 * pi * 1000.0 * (double) i / rate)); // -6.02 dB peak
    c.process (x.data(), nullptr, (int) x.size());
    float peak = 0.0f;
    for (size_t i = (size_t) rate; i < x.size(); ++i)
        peak = std::max (peak, std::abs (x[i]));
    CHECK (db (peak) == Approx (c.staticCurve (db (0.5))).margin (0.1)); // -18 + 12/4 = -15 dB
}

TEST_CASE ("Compressor: 1:1, mix 0 and makeup behave exactly", "[compressor]")
{
    auto original = noise (48000, 11, 0.9f);

    auto unity = makeCompressor (-40.0, 1.0, 6.0, 1.0, 50.0);
    auto x = original;
    unity.process (x.data(), nullptr, (int) x.size());
    CHECK (x == original);

    auto dry = makeCompressor (-40.0, 10.0, 0.0, 1.0, 50.0);
    auto p = dry.getParams();
    p.mix = 0.0;
    dry.setParams (p);
    x = original;
    dry.process (x.data(), nullptr, (int) x.size());
    CHECK (x == original);

    auto makeup = makeCompressor (0.0, 4.0, 0.0, 1.0, 50.0);
    p = makeup.getParams();
    p.makeupDb = 6.0;
    makeup.setParams (p);
    CHECK (steadyOutputDb (makeup, -20.0) == Approx (-14.0).margin (0.001));

    // Parallel: half dry, half compressed.
    auto parallel = makeCompressor (-20.0, 4.0, 0.0, 1.0, 50.0);
    p = parallel.getParams();
    p.mix = 0.5;
    parallel.setParams (p);
    CHECK (steadyOutputDb (parallel, -8.0) == Approx (-8.0 + db (0.5 + 0.5 * gainOf (-9.0))).margin (0.01));
}

TEST_CASE ("Compressor: invalid settings stay safe", "[compressor]")
{
    Compressor c;
    c.prepare (rate);
    Compressor::Params p;
    p.thresholdDb = std::numeric_limits<double>::quiet_NaN();
    p.ratio = -3.0;
    p.attackMs = 0.0;
    p.releaseMs = std::numeric_limits<double>::infinity();
    c.setParams (p);
    auto x = noise (4800, 3, 1.0f);
    c.process (x.data(), nullptr, (int) x.size());
    CHECK (std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v) && std::abs (v) <= 1.0f; }));
}

//==============================================================================
TEST_CASE ("Limiter: below the ceiling, audio passes bit for bit (delayed by its latency)", "[limiter]")
{
    auto l = makeLimiter (0.0, -1.0, 60.0, true);
    CHECK (l.getLatencySamples() == Limiter::interpolationHalfLength + 72);

    const auto original = noise (48000, 5, 0.3f); // peaks under -10 dBFS, true peaks well under -1
    auto left = original, right = original;
    l.process (left.data(), right.data(), (int) left.size());
    const auto latency = (size_t) l.getLatencySamples();
    bool exact = true;
    for (size_t i = latency; i < left.size(); ++i)
        exact = exact && left[i] == original[i - latency] && right[i] == original[i - latency];
    CHECK (exact);
}

TEST_CASE ("Limiter: no sample ever exceeds the ceiling", "[limiter]")
{
    for (const bool truePeak : { false, true })
    {
        auto l = makeLimiter (18.0, -1.0, 60.0, truePeak);
        auto left = noise (96000, 21, 0.9f), right = noise (96000, 77, 0.9f);
        // Bursts and silence, so the gain moves constantly.
        for (size_t i = 0; i < left.size(); ++i)
            if ((i / 6000) % 3 == 2)
                left[i] = right[i] = 0.0f;
        l.process (left.data(), right.data(), (int) left.size());
        float peak = 0.0f;
        for (size_t i = 0; i < left.size(); ++i)
            peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
        CHECK (db (peak) <= -1.0 + 1.0e-5);
        if (! truePeak) // (in true-peak mode the sample peaks sit lower, under the inter-sample ones)
            CHECK (db (peak) >= -1.01); // and it is actually loud, not just turned down
    }
}

TEST_CASE ("Limiter: true-peak mode holds inter-sample peaks under the ceiling", "[limiter]")
{
    // A sine at a quarter of the sample rate, phased so every sample is at 0.707:
    // sample peak -3 dBFS, true peak 0 dBFS. The classic inter-sample over.
    std::vector<float> tone ((size_t) rate / 2);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = (float) std::sin (2.0 * pi * 0.25 * (double) i + pi / 4.0);
    const size_t settled = (size_t) rate / 10;
    CHECK (truePeakDb (tone, settled) == Approx (0.0).margin (0.01));

    auto samplePeakOnly = makeLimiter (0.0, -1.0, 60.0, false);
    auto x = tone;
    samplePeakOnly.process (x.data(), nullptr, (int) x.size());
    CHECK (truePeakDb (x, settled) > -0.1); // proves the test can fail: sample peaks are under, true peaks are over

    auto truePeak = makeLimiter (0.0, -1.0, 60.0, true);
    x = tone;
    truePeak.process (x.data(), nullptr, (int) x.size());
    CHECK (truePeakDb (x, settled) <= -1.0 + 0.05);
    CHECK (truePeakDb (x, settled) >= -1.2);

    // Loud, dense programme: true peaks stay within 0.2 dB of the ceiling.
    auto loud = makeLimiter (12.0, -1.0, 60.0, true);
    auto n = programme (24000, 99, 0.9f);
    loud.process (n.data(), nullptr, (int) n.size());
    CHECK (truePeakDb (n, 2400) <= -1.0 + 0.2);
    CHECK (truePeakDb (n, 2400) >= -1.3); // and it is actually loud
}

TEST_CASE ("Limiter: true-peak detector accuracy against the 16x reference", "[limiter]")
{
    // The documented accuracy: exact on programme below 16 kHz; on full-band
    // white noise (energy up to Nyquist) an under-read of about 0.6 dB.
    constexpr int half = Limiter::interpolationHalfLength;
    auto estimate = [] (const std::vector<float>& x)
    {
        double peak = 0.0;
        for (size_t k = half; k + half + 1 < x.size(); ++k)
            peak = std::max ({ peak, (double) std::abs (x[k]), Limiter::intervalPeak (x.data() + k - half + 1) });
        return db (peak);
    };
    const auto music = programme (24000, 99, 0.9f);
    CHECK (estimate (music) == Approx (truePeakDb (music, 0)).margin (0.05));
    const auto white = noise (24000, 99, 0.9f);
    CHECK (truePeakDb (white, 0) - estimate (white) < 0.8);
}

TEST_CASE ("Limiter: the gain ramps over the look-ahead, so limiting cannot click", "[limiter]")
{
    auto l = makeLimiter (0.0, -6.0, 50.0, false);
    // A quiet DC level, then a sudden step to full scale.
    std::vector<float> x (24000, 0.1f);
    std::fill (x.begin() + 12000, x.end(), 1.0f);
    const auto in = x;
    l.process (x.data(), nullptr, (int) x.size());
    const auto latency = (size_t) l.getLatencySamples();
    double maxStep = 0.0, previous = 1.0;
    for (size_t i = latency; i < x.size(); ++i)
    {
        const double g = x[i] / in[i - latency];
        maxStep = std::max (maxStep, std::abs (g - previous));
        previous = g;
    }
    CHECK (maxStep <= 1.0 / 73.0 + 1.0e-6); // at most 1/(look-ahead + 1) of full scale per sample
    CHECK (db (x.back()) == Approx (-6.0).margin (0.001));
}

TEST_CASE ("Limiter: gain recovers after a peak at the release rate", "[limiter]")
{
    auto l = makeLimiter (0.0, -6.0, 50.0, false);
    std::vector<float> x (48000, 0.1f);
    x[4800] = 1.0f; // one full-scale spike: 6 dB of reduction, then release
    const auto in = x;
    l.process (x.data(), nullptr, (int) x.size());
    const auto latency = (size_t) l.getLatencySamples();
    const double reduced = -db (x[4800 + latency] / in[4800]);
    CHECK (reduced == Approx (6.0).margin (0.001));
    // Five release time constants later (250 ms), 6 dB of reduction is down to 0.04 dB.
    const double later = -db (x[4800 + latency + 12000 + 73] / in[4800 + 12000 + 73]);
    CHECK (later < 0.06);
    CHECK (later > 0.0);
}

TEST_CASE ("Limiter: invalid settings stay safe", "[limiter]")
{
    Limiter l;
    l.prepare (rate);
    Limiter::Params p;
    p.inputGainDb = std::numeric_limits<double>::infinity();
    p.ceilingDb = std::numeric_limits<double>::quiet_NaN();
    p.releaseMs = -5.0;
    l.setParams (p);
    auto x = noise (4800, 3, 1.0f);
    l.process (x.data(), nullptr, (int) x.size());
    CHECK (std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v) && std::abs (v) <= 1.0f; }));
}
