#include "PolySynth.h"

#include <algorithm>
#include <cmath>

namespace meridian::synth
{
namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double silenceLevel = 1.0e-4; // -80 dB: a releasing voice below this is finished

double midiToHz (int pitch) noexcept { return 440.0 * std::pow (2.0, (pitch - 69) / 12.0); }

/** Coefficient for a one-pole exponential approach that falls by 60 dB in
    `seconds` (the usual definition of decay and release times; 60 dB is
    6.91 time constants). A releasing voice ends at -80 dB, ~1.33x later. */
double approachCoefficient (double seconds, double sampleRate) noexcept
{
    const double samples = std::max (1.0, seconds * sampleRate / 6.907755);
    return std::exp (-1.0 / samples);
}

/** polyBLEP residual: removes the discontinuity's aliasing from a naive
    waveform. t is the phase (0..1), dt the phase increment per sample. */
double polyBlep (double t, double dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}
} // namespace

/** Zeroth-order modified Bessel function of the first kind (for the Kaiser window). */
static double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1.0e-12 * sum)
            break;
    }
    return sum;
}

void PolySynth::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    internalRate = sampleRate * oversampling;

    // Decimation low-pass: cutoff at the output Nyquist (a quarter of the
    // internal rate), Kaiser window with beta 8 (~80 dB stopband), unity DC gain.
    constexpr int centre = (decimatorTaps - 1) / 2;
    constexpr double beta = 8.0;
    const double cutoff = 0.5 / oversampling; // in cycles per internal sample
    double sum = 0.0;
    for (int n = 0; n < decimatorTaps; ++n)
    {
        const double m = n - centre;
        const double sinc = m == 0 ? 2.0 * cutoff : std::sin (2.0 * pi * cutoff * m) / (pi * m);
        const double r = m / centre;
        fir[(size_t) n] = sinc * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
        sum += fir[(size_t) n];
    }
    for (auto& c : fir)
        c /= sum;

    setParams (params);
    reset();
}

void PolySynth::setParams (const Params& p) noexcept
{
    // Clamp into range; a non-finite value (from a damaged file, say) falls back to the default.
    const Params defaults;
    auto safe = [] (double value, double fallback, double lo, double hi)
    {
        return std::clamp (std::isfinite (value) ? value : fallback, lo, hi);
    };
    params = p;
    params.detuneCents = safe (p.detuneCents, defaults.detuneCents, 0.0, 50.0);
    params.attack = safe (p.attack, defaults.attack, 0.0005, 10.0);
    params.decay = safe (p.decay, defaults.decay, 0.001, 10.0);
    params.sustain = safe (p.sustain, defaults.sustain, 0.0, 1.0);
    params.release = safe (p.release, defaults.release, 0.001, 20.0);
    params.cutoffHz = safe (p.cutoffHz, defaults.cutoffHz, 20.0, 20000.0);
    params.resonance = safe (p.resonance, defaults.resonance, 0.0, 1.0);
    params.filterEnvOctaves = safe (p.filterEnvOctaves, defaults.filterEnvOctaves, -8.0, 8.0);
    params.gainDb = safe (p.gainDb, defaults.gainDb, -60.0, 12.0);

    voiceGain = std::pow (10.0, params.gainDb / 20.0);
    attackStep = 1.0 / std::max (1.0, params.attack * internalRate);
    decayCoef = approachCoefficient (params.decay, internalRate);
    releaseCoef = approachCoefficient (params.release, internalRate);

    for (auto& v : voices)
        if (v.active)
        {
            v.inc1 = midiToHz (v.pitch) / internalRate;
            v.inc2 = v.inc1 * std::pow (2.0, params.detuneCents / 1200.0);
        }
}

void PolySynth::startVoice (Voice& v, int pitch, float velocity) noexcept
{
    const bool wasSounding = v.active;
    v.active = true;
    v.releasing = false;
    v.sustained = false; // the key is down again
    v.pitch = pitch;
    v.velocity = velocity;
    v.age = ++noteCounter;
    v.inc1 = midiToHz (pitch) / internalRate;
    v.inc2 = v.inc1 * std::pow (2.0, params.detuneCents / 1200.0);
    v.stage = 0;
    if (! wasSounding)
    {
        // A fresh voice starts from rest; a stolen one keeps its level and
        // phase so the hand-over has no click.
        v.env = 0.0;
        v.filterEnv = 0.0;
        v.phase1 = 0.0;
        v.phase2 = 0.37; // decorrelate the two oscillators
        v.ic1 = v.ic2 = 0.0;
    }
}

void PolySynth::noteOn (int pitch, float velocity) noexcept
{
    if (pitch < 0 || pitch > 127 || ! (velocity > 0.0f))
    {
        noteOff (pitch);
        return;
    }

    // Retrigger the same pitch on the same voice; else a free voice; else steal.
    Voice* target = nullptr;
    for (auto& v : voices)
        if (v.active && v.pitch == pitch)
            target = &v;
    if (target == nullptr)
        for (auto& v : voices)
            if (! v.active)
            {
                target = &v;
                break;
            }
    if (target == nullptr)
    {
        // Prefer a voice already releasing (quietest first), else the oldest.
        for (auto& v : voices)
            if (v.releasing && (target == nullptr || v.env < target->env))
                target = &v;
        if (target == nullptr)
            for (auto& v : voices)
                if (target == nullptr || v.age < target->age)
                    target = &v;
    }
    startVoice (*target, pitch, std::min (velocity, 1.0f));
}

void PolySynth::noteOff (int pitch) noexcept
{
    for (auto& v : voices)
        if (v.active && v.pitch == pitch && ! v.releasing)
        {
            if (pedalDown)
            {
                v.sustained = true; // the pedal holds it
                continue;
            }
            v.releasing = true;
            v.stage = 2;
        }
}

void PolySynth::setSustain (bool down) noexcept
{
    if (pedalDown && ! down)
        for (auto& v : voices)
            if (v.active && v.sustained && ! v.releasing)
            {
                v.releasing = true;
                v.stage = 2;
            }
    if (! down)
        for (auto& v : voices)
            v.sustained = false;
    pedalDown = down;
}

void PolySynth::setPitchBend (float amount) noexcept
{
    bend = std::isfinite (amount) ? std::clamp (amount, -1.0f, 1.0f) : 0.0f;
}

void PolySynth::setModWheel (float amount) noexcept
{
    modulation = std::isfinite (amount) ? std::clamp (amount, 0.0f, 1.0f) : 0.0f;
}

void PolySynth::allNotesOff() noexcept
{
    pedalDown = false;
    bend = modulation = 0.0f;
    for (auto& v : voices)
    {
        v.sustained = false;
        if (v.active)
        {
            v.releasing = true;
            v.stage = 2;
        }
    }
}

void PolySynth::reset() noexcept
{
    for (auto& v : voices)
        v = Voice {};
    pedalDown = false;
    bend = modulation = 0.0f;
    vibratoPhase = 0.0;
    history.fill (0.0);
    historyPos = 0;
}

int PolySynth::getNumActiveVoices() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        n += v.active ? 1 : 0;
    return n;
}

double PolySynth::oscillator (double phase, double increment) const noexcept
{
    switch (params.waveform)
    {
        case Waveform::sine:
            return std::sin (2.0 * pi * phase);
        case Waveform::square:
        {
            double v = phase < 0.5 ? 1.0 : -1.0;
            v += polyBlep (phase, increment);
            v -= polyBlep (std::fmod (phase + 0.5, 1.0), increment);
            return v;
        }
        case Waveform::saw:
        default:
            return 2.0 * phase - 1.0 - polyBlep (phase, increment);
    }
}

void PolySynth::render (float* left, float* right, int numSamples) noexcept
{
    for (int done = 0; done < numSamples;)
    {
        const int n = std::min (chunk, numSamples - done);
        const int internalCount = n * oversampling;
        std::fill (internal.begin(), internal.begin() + internalCount, 0.0);

        // The wheels: one pitch factor per internal sample, shared by every voice.
        // At rest nothing is multiplied, so an unbent render is unchanged to the bit.
        const bool modulated = bend != 0.0f || modulation != 0.0f;
        const double vibratoStep = vibratoRateHz / internalRate;
        for (int i = 0; i < internalCount; ++i)
        {
            if (modulated)
            {
                const double semitones = bend * bendRangeSemitones
                                       + modulation * vibratoDepthSemitones * std::sin (2.0 * pi * vibratoPhase);
                pitchFactor[(size_t) i] = std::exp2 (semitones / 12.0);
            }
            vibratoPhase += vibratoStep;
            vibratoPhase -= std::floor (vibratoPhase);
        }
        renderVoices (internal.data(), internalCount, modulated);

        // Decimate: push every internal sample through the FIR, keep every
        // `oversampling`-th output. The doubled history keeps each window contiguous.
        for (int i = 0; i < n; ++i)
        {
            for (int phase = 0; phase < oversampling; ++phase)
            {
                const double x = internal[(size_t) (i * oversampling + phase)];
                history[(size_t) historyPos] = x;
                history[(size_t) (historyPos + decimatorTaps)] = x;
                if (++historyPos == decimatorTaps)
                    historyPos = 0;
            }

            // history[historyPos .. historyPos + taps) holds the newest `taps`
            // samples, oldest first; the filter is symmetric, so order is moot.
            double y = 0.0;
            const double* window = history.data() + historyPos;
            for (int t = 0; t < decimatorTaps; ++t)
                y += fir[(size_t) t] * window[t];

            left[done + i] += (float) y;
            right[done + i] += (float) y;
        }
        done += n;
    }
}

void PolySynth::renderVoices (double* out, int numSamples, bool pitchModulated) noexcept
{
    const double k = 2.0 - 1.9 * params.resonance; // SVF damping: 2 (none) .. 0.1 (strong)

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            // Envelopes: linear attack, exponential decay to sustain and release.
            if (v.stage == 0)
            {
                v.env += attackStep;
                if (v.env >= 1.0)
                {
                    v.env = 1.0;
                    v.stage = 1;
                }
                v.filterEnv = v.env;
            }
            else if (v.stage == 1)
            {
                v.env = params.sustain + (v.env - params.sustain) * decayCoef;
                v.filterEnv = v.filterEnv * decayCoef;
            }
            else
            {
                v.env *= releaseCoef;
                v.filterEnv *= releaseCoef;
                if (v.env < silenceLevel)
                {
                    v = Voice {};
                    break;
                }
            }

            const double inc1 = pitchModulated ? v.inc1 * pitchFactor[(size_t) i] : v.inc1;
            const double inc2 = pitchModulated ? v.inc2 * pitchFactor[(size_t) i] : v.inc2;
            double s = oscillator (v.phase1, inc1);
            if (params.detuneCents > 0.0)
                s = 0.5 * (s + oscillator (v.phase2, inc2));
            v.phase1 += inc1;
            v.phase1 -= std::floor (v.phase1);
            v.phase2 += inc2;
            v.phase2 -= std::floor (v.phase2);

            // Low-pass, cutoff opened by the filter envelope and by velocity.
            const double octaves = params.filterEnvOctaves * v.filterEnv * (0.5 + 0.5 * v.velocity);
            const double cutoff = std::min (params.cutoffHz * std::exp2 (octaves), 0.45 * internalRate);
            const double g = std::tan (pi * cutoff / internalRate);
            const double a1 = 1.0 / (1.0 + g * (g + k));
            const double v1 = a1 * (v.ic1 + g * (s - v.ic2));
            const double v2 = v.ic2 + g * v1;
            v.ic1 = 2.0 * v1 - v.ic1;
            v.ic2 = 2.0 * v2 - v.ic2;

            out[i] += v2 * v.env * v.velocity * voiceGain;
        }
    }
}

} // namespace meridian::synth
