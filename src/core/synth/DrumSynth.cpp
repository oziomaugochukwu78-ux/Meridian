#include "DrumSynth.h"

#include <algorithm>
#include <cmath>

namespace meridian::synth
{
namespace
{
constexpr double twoPi = 6.283185307179586;
constexpr double endLevel = 1.0e-4; // -80 dB: a voice below this has finished

// The six square-wave "metal" oscillators of the classic analog hi-hat circuit (Hz).
constexpr double metalFreqs[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

double polyBlep (double t, double dtPhase) noexcept
{
    if (t < dtPhase)
    {
        t /= dtPhase;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dtPhase)
    {
        t = (t - 1.0) / dtPhase;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

/** Band-limited square wave; advances the phase (0..1) by `inc`. */
double square (double& phase, double inc) noexcept
{
    double v = phase < 0.5 ? 1.0 : -1.0;
    v += polyBlep (phase, inc);
    v -= polyBlep (std::fmod (phase + 0.5, 1.0), inc);
    phase += inc;
    phase -= std::floor (phase);
    return v;
}
} // namespace

//==============================================================================
DrumSynth::Sound DrumSynth::soundForNote (int pitch) noexcept
{
    switch (pitch)
    {
        case 35: case 36:                   return Sound::kick;
        case 37:                            return Sound::rim;
        case 38: case 40:                   return Sound::snare;
        case 39:                            return Sound::clap;
        case 42:                            return Sound::closedHat;
        case 44:                            return Sound::pedalHat;
        case 46:                            return Sound::openHat;
        case 41: case 43:                   return Sound::lowTom;
        case 45: case 47:                   return Sound::midTom;
        case 48: case 50:                   return Sound::highTom;
        case 49: case 52: case 55: case 57: return Sound::crash;
        case 51: case 53: case 59:          return Sound::ride;
        case 56:                            return Sound::cowbell;
        default:                            return Sound::none;
    }
}

const char* DrumSynth::nameForNote (int pitch) noexcept
{
    switch (soundForNote (pitch))
    {
        case Sound::kick:      return "Kick";
        case Sound::rim:       return "Side Stick";
        case Sound::snare:     return "Snare";
        case Sound::clap:      return "Clap";
        case Sound::closedHat: return "Closed Hat";
        case Sound::pedalHat:  return "Pedal Hat";
        case Sound::openHat:   return "Open Hat";
        case Sound::lowTom:    return "Low Tom";
        case Sound::midTom:    return "Mid Tom";
        case Sound::highTom:   return "High Tom";
        case Sound::crash:     return "Crash";
        case Sound::ride:      return "Ride";
        case Sound::cowbell:   return "Cowbell";
        case Sound::none:      break;
    }
    return nullptr;
}

//==============================================================================
void DrumSynth::Filter::set (double cutoffHz, double q, double rate) noexcept
{
    g = std::tan (3.141592653589793 * std::min (cutoffHz, 0.45 * rate) / rate);
    k = 1.0 / std::max (0.1, q);
    a1 = 1.0 / (1.0 + g * (g + k));
}

void DrumSynth::Filter::process (double x, double& low, double& band, double& high) noexcept
{
    const double v1 = a1 * (ic1 + g * (x - ic2));
    const double v2 = ic2 + g * v1;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    low = v2;
    band = v1;
    high = x - k * v1 - v2;
}

//==============================================================================
void DrumSynth::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    dt = 1.0 / sampleRate;
    setParams (params);
    reset();
}

void DrumSynth::setParams (const Params& p) noexcept
{
    const Params defaults;
    auto safe = [] (double value, double fallback, double lo, double hi)
    {
        return std::clamp (std::isfinite (value) ? value : fallback, lo, hi);
    };
    params.tuneSemitones = safe (p.tuneSemitones, defaults.tuneSemitones, -12.0, 12.0);
    params.decayScale = safe (p.decayScale, defaults.decayScale, 0.25, 4.0);
    params.tone = safe (p.tone, defaults.tone, 0.0, 1.0);
    params.gainDb = safe (p.gainDb, defaults.gainDb, -60.0, 12.0);
    kitGain = std::pow (10.0, params.gainDb / 20.0);
}

void DrumSynth::reset() noexcept
{
    for (auto& v : voices)
        v = Voice {};
}

int DrumSynth::getNumActiveVoices() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        n += v.active ? 1 : 0;
    return n;
}

double DrumSynth::tuneFactor() const noexcept
{
    return std::pow (2.0, params.tuneSemitones / 12.0);
}

double DrumSynth::nextNoise (Voice& v) noexcept
{
    v.noise ^= v.noise << 13;
    v.noise ^= v.noise >> 17;
    v.noise ^= v.noise << 5;
    return (double) v.noise / 2147483648.0 - 1.0;
}

void DrumSynth::noteOn (int pitch, float velocity) noexcept
{
    const auto sound = soundForNote (pitch);
    if (sound == Sound::none || ! (velocity > 0.0f))
        return;

    // A closed or pedal hi-hat chokes any ringing open hi-hat.
    if (sound == Sound::closedHat || sound == Sound::pedalHat)
        for (auto& v : voices)
            if (v.active && v.sound == Sound::openHat)
                v.choking = true;

    // A free voice, else the quietest-sounding (oldest) one.
    Voice* target = nullptr;
    for (auto& v : voices)
        if (! v.active)
        {
            target = &v;
            break;
        }
    if (target == nullptr)
        for (auto& v : voices)
            if (target == nullptr || v.age < target->age)
                target = &v;

    auto& v = *target;
    v = Voice {};
    v.sound = sound;
    v.active = true;
    v.age = ++hitCounter;
    v.amp = std::min (1.0f, velocity) * kitGain;
    v.noise = 0x9E3779B9u ^ (uint32_t) (pitch * 2654435761u); // deterministic per sound
    const double tf = tuneFactor(), ds = params.decayScale, tone = params.tone;

    double pan = 0.0;
    switch (sound)
    {
        case Sound::kick:
            v.pitchStart = 150.0 * tf;
            v.pitchEnd = 50.0 * tf;
            v.ampDecay = 0.13 * ds;
            break;
        case Sound::snare:
            v.freqs[0] = 185.0 * tf;
            v.freqs[1] = 330.0 * tf;
            v.ampDecay = 0.06 * ds;
            v.f1.set (1000.0 + 2500.0 * tone, 0.7, sampleRate);
            break;
        case Sound::rim:
            v.freqs[0] = 1700.0 * tf;
            v.freqs[1] = 510.0 * tf;
            v.ampDecay = 0.008 * ds;
            v.f1.set (3000.0, 0.7, sampleRate);
            break;
        case Sound::clap:
            v.ampDecay = 0.08 * ds;
            v.f1.set (1100.0 + 600.0 * tone, 1.2, sampleRate);
            pan = 0.05;
            break;
        case Sound::closedHat:
        case Sound::pedalHat:
        case Sound::openHat:
        case Sound::crash:
        case Sound::ride:
            for (int i = 0; i < 6; ++i)
                v.freqs[i] = metalFreqs[i] * tf;
            v.ampDecay = (sound == Sound::closedHat ? 0.012 : sound == Sound::pedalHat ? 0.02
                          : sound == Sound::openHat ? 0.12 : sound == Sound::crash ? 0.35 : 0.5) * ds;
            v.f1.set ((sound == Sound::crash ? 4500.0 : sound == Sound::ride ? 5500.0 : 7000.0) * (0.8 + 0.4 * tone), 0.7, sampleRate);
            v.f2.set (8000.0, 0.7, sampleRate);
            pan = sound == Sound::crash ? -0.25 : sound == Sound::ride ? 0.35 : 0.25;
            break;
        case Sound::lowTom:
        case Sound::midTom:
        case Sound::highTom:
            v.pitchEnd = (sound == Sound::lowTom ? 105.0 : sound == Sound::midTom ? 145.0 : 195.0) * tf;
            v.pitchStart = v.pitchEnd * 1.5;
            v.ampDecay = 0.13 * ds;
            pan = sound == Sound::lowTom ? -0.3 : sound == Sound::midTom ? 0.0 : 0.3;
            break;
        case Sound::cowbell:
            v.freqs[0] = 562.0 * tf;
            v.freqs[1] = 845.0 * tf;
            v.ampDecay = 0.1 * ds;
            v.f1.set (2000.0, 1.5, sampleRate);
            pan = 0.2;
            break;
        case Sound::none:
            break;
    }

    const double angle = (pan + 1.0) * 0.25 * 3.141592653589793; // constant-power pan
    v.panL = std::cos (angle);
    v.panR = std::sin (angle);
}

double DrumSynth::renderVoice (Voice& v) noexcept
{
    const double t = v.t;
    const double env = std::exp (-t / v.ampDecay);
    const double tone = params.tone;
    double low = 0.0, band = 0.0, high = 0.0, out = 0.0;

    switch (v.sound)
    {
        case Sound::kick:
        {
            const double f = v.pitchEnd + (v.pitchStart - v.pitchEnd) * std::exp (-t / 0.035);
            v.phase[0] += twoPi * f * dt;
            const double click = nextNoise (v) * std::exp (-t / 0.0025) * (0.15 + 0.35 * tone);
            const double body = std::sin (v.phase[0]) * env + click;
            out = std::tanh (1.6 * body) / std::tanh (1.6); // gentle drum-machine saturation
            break;
        }
        case Sound::snare:
        {
            const double bodyTone = 0.6 * std::sin (twoPi * v.freqs[0] * t) * std::exp (-t / 0.05)
                                  + 0.3 * std::sin (twoPi * v.freqs[1] * t) * std::exp (-t / 0.03);
            v.f1.process (nextNoise (v), low, band, high);
            out = (0.4 + 0.2 * (1.0 - tone)) * bodyTone + (0.6 + 0.4 * tone) * high * env;
            break;
        }
        case Sound::rim:
        {
            v.f1.process (nextNoise (v), low, band, high);
            out = 0.7 * std::sin (twoPi * v.freqs[0] * t) * std::exp (-t / 0.006)
                + 0.5 * std::sin (twoPi * v.freqs[1] * t) * std::exp (-t / 0.012)
                + 0.4 * high * std::exp (-t / 0.004);
            break;
        }
        case Sound::clap:
        {
            // Three quick bursts 10 ms apart, then the room tail.
            double e = 0.0;
            for (int b = 0; b < 3; ++b)
                if (t >= b * 0.010)
                    e = std::max (e, std::exp (-(t - b * 0.010) / 0.004));
            if (t >= 0.03)
                e = std::max (e, 0.6 * std::exp (-(t - 0.03) / v.ampDecay));
            v.f1.process (nextNoise (v), low, band, high);
            out = 2.2 * band * e;
            break;
        }
        case Sound::closedHat:
        case Sound::pedalHat:
        case Sound::openHat:
        case Sound::crash:
        case Sound::ride:
        {
            double metal = 0.0;
            for (int i = 0; i < 6; ++i)
                metal += square (v.phase[i], v.freqs[i] * dt);
            v.f1.process (metal / 6.0, low, band, high);
            const double metallic = high;
            v.f2.process (nextNoise (v), low, band, high);
            const double noiseMix = v.sound == Sound::crash ? 0.6 : v.sound == Sound::ride ? 0.25 : 0.45;
            out = ((1.0 - noiseMix) * metallic + noiseMix * high) * env * 1.6;
            if (v.sound == Sound::ride)
                out += 0.12 * std::sin (twoPi * 2900.0 * tuneFactor() * t) * std::exp (-t / 0.3); // the bell
            break;
        }
        case Sound::lowTom:
        case Sound::midTom:
        case Sound::highTom:
        {
            const double f = v.pitchEnd + (v.pitchStart - v.pitchEnd) * std::exp (-t / 0.06);
            v.phase[0] += twoPi * f * dt;
            out = std::sin (v.phase[0]) * env + nextNoise (v) * std::exp (-t / 0.003) * 0.2 * tone;
            break;
        }
        case Sound::cowbell:
        {
            const double bell = 0.5 * (square (v.phase[0], v.freqs[0] * dt) + square (v.phase[1], v.freqs[1] * dt));
            v.f1.process (bell, low, band, high);
            out = band * (0.6 * std::exp (-t / 0.015) + 0.4 * env) * 1.5;
            break;
        }
        case Sound::none:
            break;
    }

    if (v.choking)
        v.choke *= std::exp (-dt / 0.0022); // -60 dB in ~15 ms: a quick, click-free cut

    v.t += dt;
    // Finished once the envelope (and any choke) has decayed below -80 dB.
    const double tail = v.sound == Sound::clap ? std::exp (-std::max (0.0, t - 0.03) / v.ampDecay) : env;
    if (tail * v.choke < endLevel && t > 0.05)
        v.active = false;

    return out * v.amp * v.choke;
}

void DrumSynth::render (float* left, float* right, int numSamples) noexcept
{
    for (auto& v : voices)
    {
        if (! v.active)
            continue;
        for (int i = 0; i < numSamples && v.active; ++i)
        {
            const double s = renderVoice (v);
            left[i] += (float) (s * v.panL);
            right[i] += (float) (s * v.panR);
        }
    }
}

} // namespace meridian::synth
