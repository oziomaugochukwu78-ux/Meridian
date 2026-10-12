#include "Metronome.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
constexpr double twoPi = 6.283185307179586;
double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }
} // namespace

void Metronome::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    setParams (params);
    reset();
}

double Metronome::samplePeak (double hz) const noexcept
{
    double peak = 0.0, env = 1.0;
    for (int i = 0; i < clickLength; ++i, env *= decayPerSample)
        peak = std::max (peak, std::abs (std::sin (twoPi * hz * i / sampleRate) * env));
    return peak > 0.0 ? peak : 1.0;
}

void Metronome::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.bpm = std::clamp (finiteOr (p.bpm, defaults.bpm), 20.0, 400.0);
    params.beatsPerBar = std::clamp (p.beatsPerBar, 1, 32);
    params.levelDb = std::clamp (finiteOr (p.levelDb, defaults.levelDb), -60.0, 0.0);
    params.unaccentedDb = std::clamp (finiteOr (p.unaccentedDb, defaults.unaccentedDb), -24.0, 0.0);

    samplesPerBeat = 60.0 * sampleRate / params.bpm;
    clickLength = std::max (1, (int) std::lround (clickSeconds * sampleRate));
    decayPerSample = std::exp (-1.0 / (decaySeconds * sampleRate));

    // Scaled so the sampled peak of each click is exactly its level.
    const double level = std::pow (10.0, params.levelDb / 20.0);
    accentGain = level / samplePeak (accentHz);
    beatGain = level * std::pow (10.0, params.unaccentedDb / 20.0) / samplePeak (beatHz);
}

int64_t Metronome::beatStart (int64_t beat) const noexcept
{
    return (int64_t) std::llround ((double) beat * samplesPerBeat);
}

void Metronome::startClick (bool accent) noexcept
{
    remaining = clickLength;
    elapsed = 0;
    envelope = 1.0;
    frequency = accent ? accentHz : beatHz;
    gain = accent ? accentGain : beatGain;
}

void Metronome::render (float* out, int numSamples, int64_t position) noexcept
{
    // The first beat at or after `position`.
    auto beat = (int64_t) std::floor ((double) position / samplesPerBeat);
    while (beatStart (beat) < position)
        ++beat;

    int done = 0;
    while (done < numSamples)
    {
        const auto nextOnset = beatStart (beat) - position; // offset in this block (>= done)
        const int until = (int) std::min<int64_t> (numSamples, nextOnset);

        // Continue the click in progress up to the next onset or the block end.
        for (int i = done; i < until && remaining > 0; ++i, --remaining, ++elapsed, envelope *= decayPerSample)
            out[i] += (float) (gain * std::sin (twoPi * frequency * elapsed / sampleRate) * envelope);
        done = until;

        if (done < numSamples) // an onset in this block
        {
            const int bar = params.beatsPerBar;
            startClick (((beat % bar) + bar) % bar == 0);
            ++beat;
        }
    }
}

} // namespace meridian::dsp
