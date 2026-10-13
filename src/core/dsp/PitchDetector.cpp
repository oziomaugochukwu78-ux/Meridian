#include "PitchDetector.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace meridian::dsp
{

namespace
{
constexpr double pi = 3.14159265358979323846;

/** Weights for reading a signal between samples: x(n + f) = sum w[k] x[n + k - (taps/2 - 1)],
    a Blackman-windowed sinc, accurate far closer to Nyquist than a polynomial. */
template <size_t taps>
void fractionalWeights (double f, std::array<double, taps>& w) noexcept
{
    constexpr int half = (int) taps / 2;
    for (int k = 0; k < (int) taps; ++k)
    {
        const double t = f - (double) (k - (half - 1));
        const double sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (pi * t) / (pi * t);
        const double u = (t + (double) half) / (double) taps; // 0..1 across the kernel
        const double blackman = 0.42 - 0.5 * std::cos (2.0 * pi * u) + 0.08 * std::cos (4.0 * pi * u);
        w[(size_t) k] = sinc * blackman;
    }
}
} // namespace

void PitchDetector::prepare (double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    minTau = std::max (2, (int) std::floor (sampleRate / maxHz) - 2);
    maxTau = (int) std::ceil (sampleRate / minHz) + 2;
    window = maxTau; // at least one period of the lowest note
    difference.assign ((size_t) maxTau + 2, 0.0);
    normalised.assign ((size_t) maxTau + 2, 1.0);
}

double PitchDetector::differenceAt (const float* x, double lag) const noexcept
{
    // d at a fractional lag, the delayed signal read band-limited between samples.
    std::array<double, interpolationTaps> w {};
    const auto whole = (int) std::floor (lag);
    fractionalWeights (lag - (double) whole, w);
    double d = 0.0;
    for (int j = 0; j < window; ++j)
    {
        const float* at = x + guard + j + whole - ((int) interpolationTaps / 2 - 1);
        double delayed = 0.0;
        for (size_t k = 0; k < interpolationTaps; ++k)
            delayed += w[k] * (double) at[k];
        const double delta = (double) x[guard + j] - delayed;
        d += delta * delta;
    }
    return d;
}

PitchDetector::Result PitchDetector::detect (const float* input) noexcept
{
    Result result;
    if (window <= 0)
        return result;

    // Level gate: below -60 dBFS RMS there is nothing to tune.
    double energy = 0.0;
    for (int j = 0; j < getWindowSamples(); ++j)
        energy += (double) input[j] * (double) input[j];
    if (energy / (double) getWindowSamples() < 1.0e-6)
        return result;
    const float* x = input + guard; // room before and after for the interpolation

    // Difference function and its cumulative-mean normalisation.
    double running = 0.0;
    normalised[0] = 1.0;
    for (int tau = 1; tau <= maxTau; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double delta = (double) x[j] - (double) x[j + tau];
            d += delta * delta;
        }
        difference[(size_t) tau] = d;
        running += d;
        normalised[(size_t) tau] = running > 0.0 ? d * (double) tau / running : 1.0;
    }

    // The first dip below the threshold, walked down to its minimum.
    int tau = -1;
    for (int t = minTau; t < maxTau; ++t)
        if (normalised[(size_t) t] < threshold)
        {
            tau = t;
            while (tau + 1 < maxTau && normalised[(size_t) tau + 1] < normalised[(size_t) tau])
                ++tau;
            break;
        }
    if (tau < 0)
        return result;

    // A parabola through d at tau - 1, tau, tau + 1 places the period between samples.
    const double a = difference[(size_t) tau - 1], b = difference[(size_t) tau], c = difference[(size_t) tau + 1];
    const double curvature = a - 2.0 * b + c;
    const double shift = curvature > 0.0 ? std::clamp (0.5 * (a - c) / curvature, -1.0, 1.0) : 0.0;
    double period = (double) tau + shift;

    // That parabola is biased when d is sharp (a bright tone with a short period): refit it
    // through d read between samples, a quarter and then a sixteenth of a sample either side.
    for (const double step : { 0.25, 0.0625 })
    {
        const double lo = differenceAt (input, period - step), mid = differenceAt (input, period),
                     hi = differenceAt (input, period + step);
        const double bend = lo - 2.0 * mid + hi;
        if (bend > 0.0)
            period += std::clamp (0.5 * (lo - hi) / bend, -1.0, 1.0) * step;
    }

    result.found = true;
    result.frequencyHz = sampleRate / period;
    result.clarity = std::clamp (1.0 - normalised[(size_t) tau], 0.0, 1.0);
    return result;
}

PitchDetector::Note PitchDetector::nearestNote (double frequencyHz, double referenceHz) noexcept
{
    Note note;
    if (! (frequencyHz > 0.0) || ! (referenceHz > 0.0))
        return note;
    const double exact = 69.0 + 12.0 * std::log2 (frequencyHz / referenceHz);
    note.midi = (int) std::lround (exact);
    note.cents = (exact - (double) note.midi) * 100.0;
    return note;
}

std::string PitchDetector::noteName (int midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int pitchClass = ((midi % 12) + 12) % 12;
    const int octave = (midi - pitchClass) / 12 - 1;
    return std::string (names[pitchClass]) + std::to_string (octave);
}

//==============================================================================
void SampleHistory::prepare (int capacity)
{
    uint64_t size = 1;
    while (size < (uint64_t) std::max (capacity, 1))
        size <<= 1;
    ring = std::vector<std::atomic<float>> ((size_t) size);
    for (auto& s : ring)
        s.store (0.0f, std::memory_order_relaxed);
    mask = size - 1;
    written.store (0, std::memory_order_release);
}

void SampleHistory::push (const float* left, const float* right, int numSamples) noexcept
{
    if (ring.empty())
        return;
    auto at = written.load (std::memory_order_relaxed);
    for (int i = 0; i < numSamples; ++i, ++at)
        ring[(size_t) (at & mask)].store (right != nullptr ? 0.5f * (left[i] + right[i]) : left[i], std::memory_order_relaxed);
    written.store (at, std::memory_order_release);
}

bool SampleHistory::copyLatest (float* destination, int numSamples) const noexcept
{
    if (numSamples <= 0 || (uint64_t) numSamples > ring.size())
        return false;
    const auto end = written.load (std::memory_order_acquire);
    if (end < (uint64_t) numSamples)
        return false;
    const auto begin = end - (uint64_t) numSamples;
    for (int i = 0; i < numSamples; ++i)
        destination[i] = ring[(size_t) ((begin + (uint64_t) i) & mask)].load (std::memory_order_relaxed);
    // Overtaken if the writer has since reused any slot we read (the fence keeps
    // the copy's loads before this check, as in a seqlock).
    std::atomic_thread_fence (std::memory_order_acquire);
    return written.load (std::memory_order_relaxed) - begin <= ring.size();
}

} // namespace meridian::dsp
