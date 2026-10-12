// Measured behaviour of the built-in drum machine.

#include <dsp/SignalAnalysis.h>
#include <synth/DrumSynth.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace meridian;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

/** Renders `seconds` after hitting `pitch` (and optionally a second note later). */
Stereo hit (int pitch, double seconds, float velocity = 1.0f, synth::DrumSynth::Params params = {},
            int secondPitch = -1, double secondAt = 0.0)
{
    params.gainDb = 0.0;
    synth::DrumSynth d;
    d.prepare (rate);
    d.setParams (params);
    const int total = (int) (seconds * rate), split = secondPitch >= 0 ? (int) (secondAt * rate) : total;
    Stereo out { std::vector<float> ((size_t) total, 0.0f), std::vector<float> ((size_t) total, 0.0f) };
    d.noteOn (pitch, velocity);
    d.render (out.l.data(), out.r.data(), split);
    if (secondPitch >= 0)
    {
        d.noteOn (secondPitch, velocity);
        d.render (out.l.data() + split, out.r.data() + split, total - split);
    }
    return out;
}

double peak (const std::vector<float>& x, double from, double to)
{
    double p = 0.0;
    for (size_t i = (size_t) (from * rate); i < std::min (x.size(), (size_t) (to * rate)); ++i)
        p = std::max (p, (double) std::abs (x[i]));
    return p;
}

double db (double gain) { return 20.0 * std::log10 (std::max (gain, 1.0e-12)); }

/** Sign changes per second: a simple, robust brightness measure. */
double zeroCrossingRate (const std::vector<float>& x, double from, double to)
{
    int crossings = 0;
    const auto a = (size_t) (from * rate), b = std::min (x.size(), (size_t) (to * rate));
    for (size_t i = a + 1; i < b; ++i)
        crossings += (x[i - 1] < 0.0f) != (x[i] < 0.0f) ? 1 : 0;
    return crossings / (to - from);
}
} // namespace

TEST_CASE ("General MIDI drum map", "[drums]")
{
    using S = synth::DrumSynth::Sound;
    CHECK (synth::DrumSynth::soundForNote (36) == S::kick);
    CHECK (synth::DrumSynth::soundForNote (38) == S::snare);
    CHECK (synth::DrumSynth::soundForNote (42) == S::closedHat);
    CHECK (synth::DrumSynth::soundForNote (46) == S::openHat);
    CHECK (synth::DrumSynth::soundForNote (49) == S::crash);
    CHECK (synth::DrumSynth::soundForNote (60) == S::none);
    CHECK (std::string (synth::DrumSynth::nameForNote (39)) == "Clap");
    CHECK (synth::DrumSynth::nameForNote (61) == nullptr);
}

TEST_CASE ("Kick: pitch drops to its tuned fundamental, and tune moves it", "[drums]")
{
    const auto kick = hit (36, 1.0);
    const double early = dsp::estimateFrequency (kick.l.data(), (int) (0.03 * rate), rate);
    const double settled = dsp::estimateFrequency (kick.l.data() + (int) (0.15 * rate), (int) (0.2 * rate), rate);
    INFO ("early " << early << " Hz, settled " << settled << " Hz");
    CHECK (early > 75.0);
    CHECK (settled == Approx (50.0).margin (2.5));

    synth::DrumSynth::Params up;
    up.tuneSemitones = 12.0;
    const auto high = hit (36, 1.0, 1.0f, up);
    CHECK (dsp::estimateFrequency (high.l.data() + (int) (0.15 * rate), (int) (0.2 * rate), rate) == Approx (100.0).margin (5.0));
}

TEST_CASE ("Kick, snare and hi-hat have their characteristic brightness", "[drums]")
{
    const double kick = zeroCrossingRate (hit (36, 0.1).l, 0.005, 0.05);
    const double snare = zeroCrossingRate (hit (38, 0.1).l, 0.005, 0.05);
    const double hat = zeroCrossingRate (hit (42, 0.1).l, 0.001, 0.03);
    INFO ("zero crossings per second: kick " << kick << ", snare " << snare << ", hat " << hat);
    CHECK (kick < 600.0);
    CHECK (snare > 2000.0);
    CHECK (hat > 8000.0);
}

TEST_CASE ("Closed hi-hat is short, open hi-hat rings, decay scale stretches tails", "[drums]")
{
    const auto closed = hit (42, 0.5), open = hit (46, 0.5);
    const double closedTail = db (peak (closed.l, 0.15, 0.2) / peak (closed.l, 0.0, 0.02));
    const double openTail = db (peak (open.l, 0.15, 0.2) / peak (open.l, 0.0, 0.02));
    INFO ("150-200 ms after the hit: closed " << closedTail << " dB, open " << openTail << " dB");
    CHECK (closedTail < -60.0);
    CHECK (openTail > -30.0);

    synth::DrumSynth::Params longer;
    longer.decayScale = 2.0;
    const auto snare = hit (38, 0.6), longSnare = hit (38, 0.6, 1.0f, longer);
    CHECK (db (peak (longSnare.l, 0.25, 0.3)) - db (peak (snare.l, 0.25, 0.3)) > 10.0);
}

TEST_CASE ("A closed hi-hat chokes a ringing open hi-hat", "[drums]")
{
    const auto open = hit (46, 0.4);
    const auto choked = hit (46, 0.4, 1.0f, {}, 42, 0.05);
    const double start = peak (open.l, 0.0, 0.02);
    const double free = db (peak (open.l, 0.2, 0.3) / start);
    const double cut = db (peak (choked.l, 0.2, 0.3) / start);
    INFO ("200-300 ms: open alone " << free << " dB, choked " << cut << " dB");
    CHECK (free > -40.0);
    CHECK (cut < -60.0);
}

TEST_CASE ("Drum level follows velocity linearly; renders are deterministic", "[drums]")
{
    const auto full = hit (38, 0.3, 1.0f), half = hit (38, 0.3, 0.5f), again = hit (38, 0.3, 1.0f);
    CHECK (peak (half.l, 0.0, 0.3) / peak (full.l, 0.0, 0.3) == Approx (0.5).margin (1.0e-6));
    CHECK (full.l == again.l);
}

TEST_CASE ("Unmapped notes are silent; note-off does not cut one-shots", "[drums]")
{
    const auto none = hit (60, 0.2);
    CHECK (peak (none.l, 0.0, 0.2) == 0.0);

    synth::DrumSynth d;
    d.prepare (rate);
    d.noteOn (49, 1.0f);
    d.noteOff (49);
    std::vector<float> l (24000, 0.0f), r (24000, 0.0f);
    d.render (l.data(), r.data(), 24000);
    CHECK (peak (l, 0.3, 0.5) > 0.01); // the crash still rings half a second later
}

TEST_CASE ("Every drum is finite and bounded; hi-hats sit right of centre", "[drums]")
{
    for (int pitch = 35; pitch <= 59; ++pitch)
    {
        const auto x = hit (pitch, 0.5);
        const bool finite = std::all_of (x.l.begin(), x.l.end(), [] (float v) { return std::isfinite (v) && std::abs (v) < 2.0f; });
        INFO ("note " << pitch);
        CHECK (finite);
    }
    const auto hat = hit (42, 0.1);
    double left = 0.0, right = 0.0;
    for (size_t i = 0; i < hat.l.size(); ++i)
    {
        left += hat.l[i] * hat.l[i];
        right += hat.r[i] * hat.r[i];
    }
    CHECK (right > left);
}
