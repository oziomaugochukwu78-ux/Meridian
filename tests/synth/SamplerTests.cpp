// The built-in sampler: exact playback at the root, transposition by speed, rate
// conversion, velocity, one-shots vs keys, the hi-hat choke, zones.

#include <synth/Sampler.h>
#include <dsp/SignalAnalysis.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace meridian;
using namespace meridian::synth;
using Catch::Approx;

namespace
{
constexpr double pi = 3.14159265358979323846;

std::shared_ptr<Sample> sine (double frequency, double rate, int length, float amplitude = 0.5f)
{
    auto s = std::make_shared<Sample>();
    s->sampleRate = rate;
    s->left.resize ((size_t) length);
    for (int i = 0; i < length; ++i)
        s->left[(size_t) i] = amplitude * (float) std::sin (2.0 * pi * frequency * i / rate);
    return s;
}

struct Rig
{
    Sampler sampler;
    ZoneMap map;
    std::vector<float> left, right;

    explicit Rig (bool oneShot, double attack = 0.0, double release = 0.25)
    {
        sampler.prepare (48000.0);
        Sampler::Params p;
        p.oneShot = oneShot;
        p.attack = attack;
        p.release = release;
        p.gainDb = 0.0;
        sampler.setParams (p);
    }

    void use (std::shared_ptr<const Sample> s, int root = 60, int low = 0, int high = 127)
    {
        map.zones.push_back ({ std::move (s), root, low, high, 0.0 });
        sampler.setZones (&map);
    }

    void run (int n)
    {
        left.assign ((size_t) n, 0.0f);
        right.assign ((size_t) n, 0.0f);
        sampler.render (left.data(), right.data(), n);
    }
};
} // namespace

TEST_CASE ("Sampler: the root note plays the sample exactly, mono to both sides", "[sampler]")
{
    Rig rig (true);
    const auto s = sine (1000.0, 48000.0, 4800);
    rig.use (s);
    rig.sampler.noteOn (60, 1.0f);
    rig.run (6000);
    double worst = 0.0;
    for (int i = 0; i < 4800; ++i)
        worst = std::max (worst, (double) std::abs (rig.left[(size_t) i] - s->left[(size_t) i]) + std::abs (rig.right[(size_t) i] - s->left[(size_t) i]));
    CHECK (worst < 1.0e-6);
    CHECK (dsp::measureLevels (rig.left.data() + 4810, 1000).peak < 1.0e-9);  // then silence
    CHECK (rig.sampler.getNumActiveVoices() == 0);                           // and the voice is free
}

TEST_CASE ("Sampler: an octave up plays twice as fast; the pitch follows the note", "[sampler]")
{
    Rig rig (true);
    rig.use (sine (440.0, 48000.0, 48000));
    rig.sampler.noteOn (72, 1.0f);
    rig.run (30000);
    CHECK (dsp::estimateFrequency (rig.left.data() + 1000, 20000, 48000.0) == Approx (880.0).margin (0.5));
    CHECK (dsp::measureLevels (rig.left.data() + 24100, 4000).peak < 1.0e-6);   // half the length
    Rig fifth (true);
    fifth.use (sine (440.0, 48000.0, 48000));
    fifth.sampler.noteOn (67, 1.0f);
    fifth.run (20000);
    CHECK (dsp::estimateFrequency (fifth.left.data() + 1000, 15000, 48000.0) == Approx (440.0 * std::pow (2.0, 7.0 / 12.0)).margin (0.5));
}

TEST_CASE ("Sampler: a 44.1 kHz sample plays at its own pitch in a 48 kHz session", "[sampler]")
{
    Rig rig (true);
    rig.use (sine (441.0, 44100.0, 44100));
    rig.sampler.noteOn (60, 1.0f);
    rig.run (40000);
    CHECK (dsp::estimateFrequency (rig.left.data() + 1000, 30000, 48000.0) == Approx (441.0).margin (0.5));
    CHECK (dsp::measureLevels (rig.left.data() + 1000, 30000).peak == Approx (0.5).epsilon (0.002));
}

TEST_CASE ("Sampler: velocity sets the level along velocity^1.5", "[sampler]")
{
    Rig rig (true);
    rig.use (sine (1000.0, 48000.0, 9600));
    rig.sampler.noteOn (60, 0.5f);
    rig.run (9000);
    CHECK (dsp::measureLevels (rig.left.data(), 9000).peak == Approx (0.5 * std::pow (0.5, 1.5)).epsilon (0.002));
}

TEST_CASE ("Sampler: one-shots ignore note-off; keys fade over the release", "[sampler]")
{
    Rig drums (true);
    drums.use (sine (1000.0, 48000.0, 48000));
    drums.sampler.noteOn (60, 1.0f);
    drums.run (1000);
    drums.sampler.noteOff (60);
    drums.run (4800);
    CHECK (dsp::measureLevels (drums.left.data() + 4000, 800).peak == Approx (0.5).epsilon (0.01));

    Rig keys (false, 0.0, 0.1);
    keys.use (sine (1000.0, 48000.0, 48000));
    keys.sampler.noteOn (60, 1.0f);
    keys.run (1000);
    keys.sampler.noteOff (60);
    keys.run (4800);                                  // 100 ms release: -60 dB at its end
    const double at50ms = dsp::gainToDb (dsp::measureLevels (keys.left.data() + 2350, 100).peak / 0.5);
    CHECK (at50ms == Approx (-30.0).margin (1.0));
    keys.run (480);
    CHECK (keys.sampler.getNumActiveVoices() == 0);

    Rig pedal (false, 0.0, 0.1);                      // the sustain pedal holds a released key
    pedal.use (sine (1000.0, 48000.0, 48000));
    pedal.sampler.setSustain (true);
    pedal.sampler.noteOn (60, 1.0f);
    pedal.sampler.noteOff (60);
    pedal.run (9600);
    CHECK (dsp::measureLevels (pedal.left.data() + 9000, 600).peak == Approx (0.5).epsilon (0.01));
    pedal.sampler.setSustain (false);
    pedal.run (9600);
    CHECK (pedal.sampler.getNumActiveVoices() == 0);
}

TEST_CASE ("Sampler: a closed hi-hat chokes a ringing open hat", "[sampler]")
{
    Rig rig (true);
    rig.use (sine (1000.0, 48000.0, 96000));
    rig.sampler.noteOn (46, 1.0f);
    rig.run (1000);
    rig.sampler.noteOn (42, 1.0f);
    rig.run (480);                                    // the choke's 5 ms fade
    CHECK (rig.sampler.getNumActiveVoices() == 1);    // only the closed hat is left
}

TEST_CASE ("Sampler: zones pick the sample; notes outside every zone are silent", "[sampler]")
{
    Rig rig (true);
    rig.use (sine (200.0, 48000.0, 9600), 36, 36, 36);
    rig.use (sine (3000.0, 48000.0, 9600), 38, 38, 38);
    rig.sampler.noteOn (38, 1.0f);
    rig.run (9000);
    CHECK (dsp::estimateFrequency (rig.left.data(), 9000, 48000.0) == Approx (3000.0).margin (2.0));
    rig.sampler.noteOn (50, 1.0f);
    rig.run (1000);
    CHECK (rig.sampler.getNumActiveVoices() == 0);

    ZoneMap other;                                    // a new map silences voices playing the old one
    rig.sampler.noteOn (36, 1.0f);
    rig.sampler.setZones (&other);
    rig.run (100);
    CHECK (dsp::measureLevels (rig.left.data(), 100).peak < 1.0e-9);
}
