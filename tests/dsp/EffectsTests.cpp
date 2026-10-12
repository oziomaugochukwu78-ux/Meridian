// Measured behaviour of the built-in EQ and reverb.

#include <dsp/Biquad.h>
#include <dsp/FdnReverb.h>
#include <dsp/ParametricEq.h>
#include <dsp/SignalAnalysis.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;
constexpr double pi = 3.14159265358979323846;

double db (double g) { return 20.0 * std::log10 (std::max (g, 1.0e-15)); }

/** Gain (dB) of the EQ at `hz`, measured with a 1 s sine after a 0.5 s settle. */
double measuredGain (ParametricEq& eq, double hz)
{
    eq.reset();
    std::vector<float> x ((size_t) (1.5 * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (0.25 * std::sin (2.0 * pi * hz * (double) i / rate));
    float* ch[1] = { x.data() };
    eq.process (ch, 1, (int) x.size());
    return db (toneAmplitude (x.data() + (size_t) (0.5 * rate), (int) rate, hz, rate) / 0.25);
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

/** RT60 from an impulse response: Schroeder backward integration, line fit -5..-35 dB. */
double rt60 (const std::vector<double>& ir)
{
    std::vector<double> edc (ir.size());
    double sum = 0.0;
    for (size_t i = ir.size(); i-- > 0;)
    {
        sum += ir[i] * ir[i];
        edc[i] = sum;
    }
    size_t a = 0, b = 0;
    for (size_t i = 0; i < edc.size(); ++i)
    {
        const double level = 10.0 * std::log10 (edc[i] / edc[0]);
        if (a == 0 && level <= -5.0)  a = i;
        if (b == 0 && level <= -35.0) { b = i; break; }
    }
    if (b <= a)
        return 0.0;
    // Least-squares slope (dB per sample) between a and b.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = (double) (b - a);
    for (size_t i = a; i < b; ++i)
    {
        const double y = 10.0 * std::log10 (edc[i] / edc[0]);
        sx += (double) i; sy += y; sxx += (double) i * (double) i; sxy += (double) i * y;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return -60.0 / slope / rate;
}

std::vector<double> impulseResponse (FdnReverb::Params p, double seconds, bool rightChannel = false)
{
    FdnReverb reverb;
    reverb.prepare (rate);
    p.mix = 1.0;
    reverb.setParams (p);
    const int n = (int) (seconds * rate);
    std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
    l[0] = r[0] = 1.0f;
    reverb.process (l.data(), r.data(), n);
    std::vector<double> out ((size_t) n);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = rightChannel ? r[i] : l[i];
    return out;
}
} // namespace

//==============================================================================
TEST_CASE ("EQ: a flat EQ is bit-transparent", "[eq]")
{
    ParametricEq eq;
    eq.prepare (rate);
    auto x = noise (48000, 7);
    const auto original = x;
    float* ch[1] = { x.data() };
    eq.process (ch, 1, (int) x.size());
    CHECK (x == original);
}

TEST_CASE ("EQ: a peaking band boosts exactly its gain at its frequency", "[eq]")
{
    ParametricEq eq;
    eq.prepare (rate);
    ParametricEq::Params p;
    p.peaks[1] = { 1000.0, 6.0, 1.0 };
    eq.setParams (p);
    CHECK (db (eq.responseAt (1000.0)) == Approx (6.0).margin (1.0e-9));
    CHECK (measuredGain (eq, 1000.0) == Approx (6.0).margin (0.05));
    CHECK (measuredGain (eq, 100.0) == Approx (0.0).margin (0.1));   // far away: untouched
}

TEST_CASE ("EQ: measured response matches the design everywhere", "[eq]")
{
    ParametricEq eq;
    eq.prepare (rate);
    ParametricEq::Params p;
    p.highPassOn = true;
    p.highPassHz = 40.0;
    p.lowShelf = { 120.0, 3.0, 1.0 };
    p.peaks = { { { 300.0, -4.0, 2.0 }, { 1500.0, 5.0, 0.7 }, { 5000.0, -6.0, 4.0 } } };
    p.highShelf = { 9000.0, 2.5, 0.8 };
    p.lowPassOn = true;
    p.lowPassHz = 16000.0;
    p.outputDb = -1.5;
    eq.setParams (p);
    for (const double hz : { 60.0, 120.0, 300.0, 700.0, 1500.0, 3000.0, 5000.0, 9000.0, 14000.0 })
    {
        INFO (hz << " Hz");
        CHECK (measuredGain (eq, hz) == Approx (db (eq.responseAt (hz))).margin (0.05));
    }
}

TEST_CASE ("EQ: high-pass is -3 dB at its cutoff and falls 12 dB per octave", "[eq]")
{
    ParametricEq eq;
    eq.prepare (rate);
    ParametricEq::Params p;
    p.highPassOn = true;
    p.highPassHz = 100.0;
    eq.setParams (p);
    CHECK (db (eq.responseAt (100.0)) == Approx (-3.0103).margin (0.02));
    CHECK (measuredGain (eq, 100.0) == Approx (-3.01).margin (0.1));
    CHECK (measuredGain (eq, 25.0) == Approx (-24.0).margin (1.0));  // two octaves down
    CHECK (measuredGain (eq, 1000.0) == Approx (0.0).margin (0.05));
}

TEST_CASE ("EQ: invalid settings fall back to safe values", "[eq]")
{
    ParametricEq eq;
    eq.prepare (rate);
    ParametricEq::Params p;
    p.peaks[0] = { std::nan (""), 1.0e9, -5.0 };
    p.outputDb = INFINITY;
    eq.setParams (p);
    auto x = noise (4800, 3);
    float* ch[1] = { x.data() };
    eq.process (ch, 1, (int) x.size());
    CHECK (std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); }));
}

//==============================================================================
TEST_CASE ("Reverb: mix 0 passes the dry signal through exactly", "[reverb]")
{
    FdnReverb reverb;
    reverb.prepare (rate);
    FdnReverb::Params p;
    p.mix = 0.0;
    reverb.setParams (p);
    auto l = noise (24000, 11), r = noise (24000, 12);
    const auto l0 = l, r0 = r;
    reverb.process (l.data(), r.data(), 24000);
    CHECK (l == l0);
    CHECK (r == r0);
}

TEST_CASE ("Reverb: the tail decays in the time it is set to (RT60)", "[reverb]")
{
    for (const double decay : { 0.8, 2.0, 4.0 })
    {
        FdnReverb::Params p;
        p.decay = decay;
        p.damping = 0.0;
        p.predelayMs = 0.0;
        const double measured = rt60 (impulseResponse (p, decay * 1.2 + 0.3));
        INFO ("set " << decay << " s, measured " << measured << " s");
        CHECK (measured == Approx (decay).epsilon (0.1));
    }
}

TEST_CASE ("Reverb: damping makes high frequencies die away faster", "[reverb]")
{
    FdnReverb::Params p;
    p.decay = 2.5;
    p.damping = 0.8;
    const auto ir = impulseResponse (p, 3.5);

    auto band = [&] (Biquad::Type type, double hz)
    {
        Biquad filter;
        filter.setCoefficients (Biquad::design (type, hz, 0.707, 0.0, rate));
        std::vector<double> out (ir.size());
        for (size_t i = 0; i < ir.size(); ++i)
            out[i] = filter.process (ir[i]);
        return rt60 (out);
    };
    const double low = band (Biquad::Type::lowPass, 400.0), high = band (Biquad::Type::highPass, 6000.0);
    INFO ("RT60 below 400 Hz " << low << " s, above 6 kHz " << high << " s");
    CHECK (low == Approx (2.5).epsilon (0.15));
    CHECK (high < 0.6 * low);
}

TEST_CASE ("Reverb: wide stereo tail, collapsible to mono with width 0", "[reverb]")
{
    auto correlation = [] (const std::vector<double>& a, const std::vector<double>& b)
    {
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = (size_t) (0.05 * rate); i < a.size(); ++i)
        {
            ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i];
        }
        return ab / std::sqrt (aa * bb);
    };
    FdnReverb::Params p;
    p.decay = 1.5;
    const double wide = correlation (impulseResponse (p, 1.5), impulseResponse (p, 1.5, true));
    p.width = 0.0;
    const double mono = correlation (impulseResponse (p, 1.5), impulseResponse (p, 1.5, true));
    INFO ("L/R correlation: width 1 " << wide << ", width 0 " << mono);
    CHECK (std::abs (wide) < 0.3);
    CHECK (mono > 0.999);
}

TEST_CASE ("Reverb: pre-delay holds the tail back", "[reverb]")
{
    FdnReverb::Params p;
    p.predelayMs = 50.0;
    const auto ir = impulseResponse (p, 0.2);
    size_t first = 0;
    while (first < ir.size() && std::abs (ir[first]) < 1.0e-9)
        ++first;
    CHECK (first >= (size_t) (0.050 * rate));
}

TEST_CASE ("Reverb: stable and finite at the most extreme settings", "[reverb]")
{
    FdnReverb reverb;
    reverb.prepare (rate);
    FdnReverb::Params p;
    p.size = 2.0;
    p.decay = 20.0;
    p.damping = 0.0;
    p.mix = 1.0;
    reverb.setParams (p);
    auto l = noise ((int) (11 * rate), 5), r = noise ((int) (11 * rate), 6);
    std::fill (l.begin() + (long) rate, l.end(), 0.0f); // 1 s of noise, then 10 s of tail
    std::fill (r.begin() + (long) rate, r.end(), 0.0f);
    reverb.process (l.data(), r.data(), (int) l.size());
    double peak = 0.0;
    bool finite = true;
    for (size_t i = 0; i < l.size(); ++i)
    {
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        peak = std::max (peak, (double) std::abs (l[i]));
    }
    CHECK (finite);
    CHECK (peak < 4.0);
}
