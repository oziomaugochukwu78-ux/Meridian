// Measured behaviour of the built-in synthesiser: pitch, timing, envelopes,
// dynamics, polyphony and aliasing.

#include <dsp/SignalAnalysis.h>
#include <synth/PolySynth.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
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

TEST_CASE ("Sustain pedal: released keys ring at full level until the pedal lifts", "[synth]")
{
    auto p = pureSine();
    p.release = 0.05;
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (p);

    // Pedal down, A4 pressed and released: it keeps sounding at its sustain level.
    s.setSustain (true);
    s.noteOn (69, 1.0f);
    render (s, 4800);
    s.noteOff (69);
    const auto held = render (s, 24000);
    CHECK (peak (held, 12000) == Approx (1.0).margin (0.01));
    CHECK (s.getNumActiveVoices() == 1);

    // A key still down when the pedal lifts keeps sounding; the released one stops.
    s.noteOn (76, 1.0f);
    render (s, 4800);
    s.setSustain (false);
    render (s, (int) (0.2 * rate)); // A4's 50 ms release is long over
    CHECK (s.getNumActiveVoices() == 1);
    const auto stillHeld = render (s, 9600);
    CHECK (dsp::toneAmplitude (stillHeld.data(), 9600, 659.2551138, rate) == Approx (1.0).margin (0.01));
    CHECK (dsp::toneAmplitude (stillHeld.data(), 9600, 440.0, rate) < 0.01); // A4 gone (what is left is E5's leakage)

    // Pressing a sustained key again restarts it held by the key, not the pedal.
    s.noteOff (76);
    render (s, (int) (0.2 * rate));
    CHECK (s.getNumActiveVoices() == 0);
    s.setSustain (true);
    s.noteOn (60, 1.0f);
    s.noteOff (60);
    s.noteOn (60, 1.0f); // re-struck under the pedal
    s.setSustain (false);
    render (s, (int) (0.2 * rate));
    CHECK (s.getNumActiveVoices() == 1); // its key is down

    // allNotesOff (stop, locate) lifts the pedal too: nothing can hang.
    s.setSustain (true);
    s.allNotesOff();
    CHECK_FALSE (s.isSustainDown());
    s.noteOn (62, 1.0f);
    s.noteOff (62);
    render (s, (int) (0.3 * rate));
    CHECK (s.getNumActiveVoices() == 0);
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

TEST_CASE ("Pitch bend: the wheel's ends are exactly two semitones away, halfway one", "[synth][bend]")
{
    for (const auto& [amount, semitones] : { std::pair { 1.0f, 2.0 }, std::pair { -1.0f, -2.0 }, std::pair { 0.5f, 1.0 } })
    {
        synth::PolySynth s;
        s.prepare (rate);
        s.setParams (pureSine());
        s.noteOn (69, 1.0f);
        s.setPitchBend (amount);
        render (s, 4800);
        const auto block = render (s, 48000);
        CHECK (dsp::estimateFrequency (block.data(), (int) block.size(), rate) == Approx (440.0 * std::exp2 (semitones / 12.0)).margin (0.03));
    }
}

TEST_CASE ("Pitch bend and mod wheel at rest change nothing, to the bit", "[synth][bend]")
{
    synth::PolySynth plain, centred;
    for (auto* s : { &plain, &centred })
    {
        s->prepare (rate);
        s->setParams (pureSine());
        s->noteOn (64, 0.8f);
    }
    centred.setPitchBend (0.0f);
    centred.setModWheel (0.0f);
    CHECK (render (plain, 9600) == render (centred, 9600));
}

TEST_CASE ("Mod wheel: vibrato of exactly half a semitone either way around the note", "[synth][bend]")
{
    synth::PolySynth s;
    s.prepare (rate);
    s.setParams (pureSine());
    s.noteOn (69, 1.0f);
    s.setModWheel (1.0f);
    render (s, 4800);
    const auto block = render (s, 96000); // 2 s: 11 vibrato cycles

    // The frequency of every cycle, from upward zero crossings (interpolated).
    std::vector<double> crossings;
    for (size_t i = 1; i < block.size(); ++i)
        if (block[i - 1] < 0.0f && block[i] >= 0.0f)
            crossings.push_back ((double) (i - 1) + block[i - 1] / (block[i - 1] - block[i]));
    double low = 1.0e9, high = 0.0;
    for (size_t k = 1; k < crossings.size(); ++k)
    {
        const double hz = rate / (crossings[k] - crossings[k - 1]);
        low = std::min (low, hz);
        high = std::max (high, hz);
    }
    const double depth = synth::PolySynth::vibratoDepthSemitones;
    CHECK (high == Approx (440.0 * std::exp2 (depth / 12.0)).margin (0.2));
    CHECK (low == Approx (440.0 * std::exp2 (-depth / 12.0)).margin (0.2));
    CHECK (dsp::estimateFrequency (block.data(), (int) block.size(), rate) == Approx (440.0).margin (0.1));

    s.allNotesOff();
    CHECK (s.getPitchBend() == 0.0f);
    CHECK (s.getModWheel() == 0.0f);
}
