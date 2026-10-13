// Measured behaviour of the tuner's pitch detector.

#include <dsp/PitchDetector.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double pi = 3.14159265358979323846;

double centsBetween (double measured, double actual) { return 1200.0 * std::log2 (measured / actual); }

std::vector<float> sine (double hz, double rate, int n, double amplitude = 0.5, double phase = 0.3)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = (float) (amplitude * std::sin (2.0 * pi * hz * i / rate + phase));
    return x;
}

/** Band-limited sawtooth: every harmonic below Nyquist, amplitude 1/k. */
std::vector<float> sawtooth (double hz, double rate, int n)
{
    std::vector<float> x ((size_t) n, 0.0f);
    for (int k = 1; k * hz < rate / 2.0; ++k)
        for (int i = 0; i < n; ++i)
            x[(size_t) i] += (float) (0.3 / k * std::sin (2.0 * pi * k * hz * i / rate));
    return x;
}
} // namespace

TEST_CASE ("Pitch detector: sines from 30 Hz to 2 kHz read within 1 cent", "[pitch]")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        PitchDetector d;
        d.prepare (rate);
        double worst = 0.0;
        // About every half semitone at 48 kHz, every two at the other rates (keeps sanitizer runs short); off the note grid.
        const double step = std::abs (rate - 48000.0) < 1.0 ? 1.0293 : 1.1225;
        for (double hz = 30.0; hz <= 2000.0; hz *= step)
        {
            const auto x = sine (hz, rate, d.getWindowSamples());
            const auto r = d.detect (x.data());
            REQUIRE (r.found);
            worst = std::max (worst, std::abs (centsBetween (r.frequencyHz, hz)));
            CHECK (r.clarity > 0.9);
        }
        INFO ("rate " << rate << ": worst error " << worst << " cents");
        CHECK (worst < 1.0);
    }
}

TEST_CASE ("Pitch detector: a sawtooth reads its fundamental, not an octave off", "[pitch]")
{
    PitchDetector d;
    d.prepare (48000.0);
    for (const double hz : { 41.2, 82.41, 110.0, 196.0, 261.63, 440.0, 659.26, 1318.5 })
    {
        const auto x = sawtooth (hz, 48000.0, d.getWindowSamples());
        const auto r = d.detect (x.data());
        INFO (hz << " Hz read as " << r.frequencyHz);
        REQUIRE (r.found);
        CHECK (std::abs (centsBetween (r.frequencyHz, hz)) < 1.0);
    }
}

TEST_CASE ("Pitch detector: silence, a very quiet tone and white noise read as no pitch", "[pitch]")
{
    PitchDetector d;
    d.prepare (48000.0);
    const std::vector<float> silence ((size_t) d.getWindowSamples(), 0.0f);
    CHECK_FALSE (d.detect (silence.data()).found);

    const auto quiet = sine (440.0, 48000.0, d.getWindowSamples(), 0.0005); // about -69 dBFS RMS
    CHECK_FALSE (d.detect (quiet.data()).found);

    std::mt19937 random (1234);
    std::uniform_real_distribution<float> uniform (-0.5f, 0.5f);
    std::vector<float> noise ((size_t) d.getWindowSamples());
    for (int trial = 0; trial < 20; ++trial)
    {
        for (auto& s : noise)
            s = uniform (random);
        CHECK_FALSE (d.detect (noise.data()).found);
    }
}

TEST_CASE ("Pitch detector: nearest note and cents", "[pitch]")
{
    auto n = PitchDetector::nearestNote (440.0);
    CHECK (n.midi == 69);
    CHECK (n.cents == Approx (0.0).margin (1.0e-9));
    CHECK (PitchDetector::noteName (69) == "A4");
    CHECK (PitchDetector::noteName (60) == "C4");
    CHECK (PitchDetector::noteName (70) == "A#4");
    CHECK (PitchDetector::noteName (0) == "C-1");

    n = PitchDetector::nearestNote (1000.0); // 69 + 12 log2(1000/440) = 83.2131: B5 +21.3
    CHECK (n.midi == 83);
    CHECK (n.cents == Approx (21.309).margin (0.001));
    CHECK (PitchDetector::noteName (n.midi) == "B5");

    n = PitchDetector::nearestNote (440.0, 442.0); // tuned to A = 442: A4 is 7.85 cents flat
    CHECK (n.midi == 69);
    CHECK (n.cents == Approx (-7.85).margin (0.01));

    for (double hz = 50.0; hz < 4000.0; hz *= 1.01)
        REQUIRE (std::abs (PitchDetector::nearestNote (hz).cents) <= 50.0 + 1.0e-9);
}

TEST_CASE ("Sample history: the latest samples, oldest first, mixed to mono", "[pitch]")
{
    SampleHistory h;
    h.prepare (1000); // rounds up to 1024
    std::vector<float> out (8);
    CHECK_FALSE (h.copyLatest (out.data(), 8)); // nothing written yet

    std::vector<float> left (3000), right (3000);
    for (int i = 0; i < 3000; ++i)
    {
        left[(size_t) i] = (float) i;
        right[(size_t) i] = (float) i + 2.0f;
    }
    h.push (left.data(), right.data(), 1000);
    h.push (left.data() + 1000, right.data() + 1000, 2000); // wraps the ring twice
    REQUIRE (h.copyLatest (out.data(), 8));
    for (int i = 0; i < 8; ++i)
        CHECK (out[(size_t) i] == Approx (2992.0 + i + 1.0)); // (x + x + 2) / 2

    std::vector<float> tooMany (2048);
    CHECK_FALSE (h.copyLatest (tooMany.data(), 2048));

    h.push (left.data(), nullptr, 4); // mono: as is
    REQUIRE (h.copyLatest (out.data(), 4));
    CHECK (out[0] == Approx (0.0));
    CHECK (out[3] == Approx (3.0));
}
