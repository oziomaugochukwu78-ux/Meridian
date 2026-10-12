// Measured behaviour of the built-in synthesiser: pitch, timing, envelopes,
// dynamics, polyphony and aliasing.

#include <dsp/SignalAnalysis.h>
#include <synth/PolySynth.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace meridian;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;

/** A pure test tone: sine, no detune, filter wide open and static. */
synth::PolySynth::Params pureSine()
{
    synth::PolySynth::Params p;
    p.waveform = synth::PolySynth::Waveform::sine;
    p.detuneCents = 0.0;
    p.attack = 0.001;
    p.decay = 0.1;
    p.sustain = 1.0;
    p.release = 0.05;
    p.cutoffHz = 20000.0;
    p.filterEnvOctaves = 0.0;
    p.resonance = 0.0;
    p.gainDb = 0.0;
    return p;
}

std::vector<float> render (synth::PolySynth& s, int numSamples)
{
    std::vector<float> l ((size_t) numSamples, 0.0f), r ((size_t) numSamples, 0.0f);
    s.render (l.data(), r.data(), numSamples);
    return l;
}

double peak (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
{
    double p = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        p = std::max (p, (double) std::abs (x[i]));
    return p;
}
} // namespace

TEST_CASE ("Synth plays in tune", "[synth]")
{
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (pureSine());

    for (const auto& [pitch, hz] : { std::pair { 69, 440.0 }, { 60, 261.6255653 }, { 81, 880.0 }, { 45, 110.0 } })
    {
        s.reset();
        s.noteOn (pitch, 1.0f);
        render (s, 4800); // let the attack settle
        const auto block = render (s, 48000);
        CHECK (dsp::estimateFrequency (block.data(), (int) block.size(), rate) == Approx (hz).margin (0.02));
    }
}

TEST_CASE ("Notes are timed exactly: output is the ideal tone delayed by the reported latency", "[synth]")
{
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (pureSine());

    const auto before = render (s, 256);
    CHECK (peak (before) == 0.0); // nothing before the note

    s.noteOn (69, 1.0f);
    const auto after = render (s, 9600);

    // Once the 1 ms attack has passed, measure the delay of the output against
    // a sine that started exactly at the note-on sample: correlate with sin and
    // cos over whole cycles (6000 samples = 55 cycles of 440 Hz) and convert the
    // phase to samples. It must be the reported latency, plus the small phase
    // lag of the synth's own (fully open) low-pass filter: well under a sample.
    const int latency = synth::PolySynth::getLatencySamples();
    const double w = 2.0 * 3.14159265358979323846 * 440.0 / rate;
    double i = 0.0, q = 0.0;
    for (int n = 3600; n < 9600; ++n)
    {
        i += after[(size_t) n] * std::sin (w * n);
        q += after[(size_t) n] * std::cos (w * n);
    }
    const double delay = -std::atan2 (q, i) / w;          // samples
    const double amplitude = 2.0 * std::hypot (i, q) / 6000.0;
    INFO ("reported latency " << latency << ", measured delay " << delay << " samples, amplitude " << amplitude);
    CHECK (latency == 16);
    CHECK (delay > latency - 0.05);
    CHECK (delay < latency + 0.3);
    CHECK (amplitude == Approx (1.0).margin (0.005));
}

TEST_CASE ("Velocity scales level linearly", "[synth]")
{
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (pureSine());

    auto level = [&] (float velocity)
    {
        s.reset();
        s.noteOn (69, velocity);
        render (s, 4800);
        const auto block = render (s, 48000);
        return dsp::toneAmplitude (block.data(), (int) block.size(), 440.0, rate);
    };

    const double full = level (1.0f);
    CHECK (full == Approx (1.0).margin (0.01));       // gain 0 dB, sustain 1: unity sine
    CHECK (level (0.5f) / full == Approx (0.5).margin (0.005));
}

TEST_CASE ("Attack and release take the time they are set to", "[synth]")
{
    auto p = pureSine();
    p.attack = 0.1;
    p.release = 0.2;
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (p);

    s.noteOn (69, 1.0f);
    const auto attack = render (s, (int) (0.15 * rate));
    CHECK (peak (attack, 0, 480) == Approx (0.1).margin (0.01));               // linear: 10% of the way after 10 ms
    CHECK (peak (attack, 2340, 2460) == Approx (0.5).margin (0.03));           // halfway (50 ms): half level
    CHECK (peak (attack, 5040, 7200) == Approx (1.0).margin (0.01));           // after 100 ms: full

    s.noteOff (69);
    const auto release = render (s, (int) (0.3 * rate));
    // Release 0.2 s = -60 dB in 0.2 s; the voice ends at -80 dB (~0.27 s).
    CHECK (peak (release, (size_t) (0.19 * rate), (size_t) (0.21 * rate)) == Approx (0.001).margin (0.0005)); // -60 dB at 0.2 s
    CHECK (s.getNumActiveVoices() == 0);
    CHECK (peak (release, (size_t) (0.28 * rate)) == 0.0);                    // and truly silent afterwards
    CHECK (peak (release, 0, 480) > 0.9);                                     // no jump at note-off (no click)
}

TEST_CASE ("Polyphony is limited and voice stealing never drops the newest note", "[synth]")
{
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (pureSine());

    for (int pitch = 30; pitch < 30 + 40; ++pitch)
        s.noteOn (pitch, 0.5f);
    CHECK (s.getNumActiveVoices() == synth::PolySynth::maxVoices);

    // The newest note must be sounding: its pitch is present in the output.
    render (s, 4800);
    const auto block = render (s, 48000);
    const double newest = dsp::toneAmplitude (block.data(), (int) block.size(), 440.0 * std::pow (2.0, (69 - 69) / 12.0), rate);
    CHECK (newest > 0.4); // pitch 69 (440 Hz) was the last of the 40

    s.allNotesOff();
    render (s, (int) rate);
    CHECK (s.getNumActiveVoices() == 0);
}

namespace
{
/** Level (dB re the fundamental) of the saw's `harmonic`-th harmonic folded
    back below Nyquist, for a note at `pitch`. */
double foldedHarmonicDb (int pitch, int harmonic)
{
    auto p = pureSine();
    p.waveform = synth::PolySynth::Waveform::saw;
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (p);
    s.noteOn (pitch, 1.0f);
    render (s, 4800);
    const auto block = render (s, 48000);

    const double f0 = 440.0 * std::pow (2.0, (pitch - 69) / 12.0);
    const double fundamental = dsp::toneAmplitude (block.data(), (int) block.size(), f0, rate);
    const double folded = rate - harmonic * f0; // mirrored around Nyquist (24 kHz)
    const double alias = dsp::toneAmplitude (block.data(), (int) block.size(), folded, rate);
    return 20.0 * std::log10 (std::max (alias, 1.0e-12) / fundamental);
}
} // namespace

TEST_CASE ("Band-limited saw keeps aliasing far below a naive saw", "[synth]")
{
    // A naive saw's n-th harmonic is at 1/n of the fundamental (-20 log10 n dB),
    // and above Nyquist it folds back, inharmonic, into the audible range.
    // C7 (2093 Hz): the 14th harmonic (29.3 kHz) folds to 18.7 kHz; naive -22.9 dB.
    const double c7 = foldedHarmonicDb (96, 14);
    // C5 (523 Hz): the 55th harmonic (28.8 kHz) folds to 19.2 kHz; naive -34.8 dB.
    const double c5 = foldedHarmonicDb (72, 55);
    INFO ("C7 folded 14th harmonic: " << c7 << " dB;  C5 folded 55th harmonic: " << c5 << " dB");
    CHECK (c7 < -60.0);
    CHECK (c5 < -60.0);
}

TEST_CASE ("Output is always finite, even at extreme settings", "[synth]")
{
    synth::PolySynth::Params p;
    p.resonance = 1.0;
    p.cutoffHz = 20000.0;
    p.filterEnvOctaves = 8.0;
    p.attack = 0.0;
    p.gainDb = 12.0;
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (p);
    for (int pitch = 0; pitch <= 127; pitch += 9)
        s.noteOn (pitch, 1.0f);
    const auto block = render (s, 96000);
    CHECK (std::all_of (block.begin(), block.end(), [] (float x) { return std::isfinite (x); }));
}
