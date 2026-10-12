#include "Loudness.h"

#include "Limiter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace meridian::dsp
{
namespace
{
constexpr double pi = 3.141592653589793;
} // namespace

//==============================================================================
void KWeighting::prepare (double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;

    // Stage 1: the head's acoustic effect, a high shelf (BS.1770-4 prototype values).
    {
        constexpr double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (pi * f0 / fs);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelfCoefficients = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                              2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
    }
    // Stage 2: the RLB high-pass (numerator 1, -2, 1 exactly, as in the standard).
    {
        constexpr double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (pi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        highPassCoefficients = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
    }
    shelf.setCoefficients (shelfCoefficients);
    highPass.setCoefficients (highPassCoefficients);
    reset();
}

//==============================================================================
void LoudnessMeter::Histogram::clear()
{
    counts.assign ((size_t) bins, 0);
    energies.assign ((size_t) bins, 0.0);
    total = 0;
}

void LoudnessMeter::Histogram::add (double meanSquare, double loudness)
{
    const auto index = (size_t) std::clamp ((int) std::floor ((loudness - low) / step), 0, bins - 1);
    ++counts[index];
    energies[index] += meanSquare;
    ++total;
}

double LoudnessMeter::loudnessOf (double meanSquare) noexcept
{
    return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : silence;
}

void LoudnessMeter::prepare (double rate, int numChannels)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    channels = std::clamp (numChannels, 1, maxChannels);
    subBlockLength = std::max (1, (int) std::lround (0.1 * sampleRate));
    for (auto& w : weighting)
        w.prepare (sampleRate);
    blocks.clear();
    shortTerms.clear();
    reset();
}

void LoudnessMeter::reset() noexcept
{
    for (auto& w : weighting)
        w.reset();
    sum.fill (0.0);
    subBlockFill = 0;
    recent.fill (0.0);
    recentCount = recentPos = 0;
    std::fill (blocks.counts.begin(), blocks.counts.end(), 0u);
    std::fill (blocks.energies.begin(), blocks.energies.end(), 0.0);
    blocks.total = 0;
    std::fill (shortTerms.counts.begin(), shortTerms.counts.end(), 0u);
    std::fill (shortTerms.energies.begin(), shortTerms.energies.end(), 0.0);
    shortTerms.total = 0;
    maxMomentary = maxShortTerm = silence;
}

void LoudnessMeter::process (const float* const* input, int numChannels, int numSamples) noexcept
{
    const int used = std::min ({ numChannels, channels, maxChannels });
    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < used; ++c)
        {
            const double y = weighting[(size_t) c].process (input[c][i]);
            sum[(size_t) c] += y * y;
        }
        if (++subBlockFill == subBlockLength)
            endSubBlock();
    }
}

void LoudnessMeter::endSubBlock() noexcept
{
    // Channel weights are 1 for left and right (BS.1770-4), so the sum is the total.
    double meanSquare = 0.0;
    for (int c = 0; c < channels; ++c)
        meanSquare += sum[(size_t) c] / subBlockLength;
    sum.fill (0.0);
    subBlockFill = 0;
    addSubBlock (meanSquare);
}

void LoudnessMeter::addSubBlock (double meanSquare) noexcept
{
    if (blocks.counts.empty())
        return; // not prepared
    recent[(size_t) recentPos] = meanSquare;
    recentPos = (recentPos + 1) % (int) recent.size();
    recentCount = std::min (recentCount + 1, (int) recent.size());

    if (recentCount >= 4) // a 400 ms gating block, every 100 ms (75 % overlap)
    {
        const double block = windowMeanSquare (4);
        const double loudness = loudnessOf (block);
        maxMomentary = std::max (maxMomentary, loudness);
        if (loudness >= absoluteGate)
            blocks.add (block, loudness);
    }
    if (recentCount >= 30) // a 3 s short-term window, every 100 ms
    {
        const double window = windowMeanSquare (30);
        const double loudness = loudnessOf (window);
        maxShortTerm = std::max (maxShortTerm, loudness);
        if (loudness >= absoluteGate)
            shortTerms.add (window, loudness);
    }
}

double LoudnessMeter::windowMeanSquare (int subBlocks) const noexcept
{
    double total = 0.0;
    for (int i = 1; i <= subBlocks; ++i)
        total += recent[(size_t) ((recentPos - i + (int) recent.size()) % (int) recent.size())];
    return total / subBlocks;
}

double LoudnessMeter::getMomentary() const noexcept  { return recentCount >= 4 ? loudnessOf (windowMeanSquare (4)) : silence; }
double LoudnessMeter::getShortTerm() const noexcept  { return recentCount >= 30 ? loudnessOf (windowMeanSquare (30)) : silence; }

double LoudnessMeter::gatedMean (const Histogram& h, double gateLoudness) noexcept
{
    double energy = 0.0;
    uint64_t count = 0;
    for (int i = 0; i < Histogram::bins; ++i)
    {
        const double centre = Histogram::low + (i + 0.5) * Histogram::step;
        if (centre < gateLoudness || h.counts[(size_t) i] == 0)
            continue;
        energy += h.energies[(size_t) i];
        count += h.counts[(size_t) i];
    }
    return count > 0 ? energy / (double) count : 0.0;
}

double LoudnessMeter::getIntegrated() const noexcept
{
    if (blocks.total == 0)
        return silence;
    const double ungated = loudnessOf (gatedMean (blocks, absoluteGate));
    return loudnessOf (gatedMean (blocks, ungated + relativeGate));
}

double LoudnessMeter::getLoudnessRange() const noexcept
{
    if (shortTerms.total == 0)
        return 0.0;
    const double gate = loudnessOf (gatedMean (shortTerms, absoluteGate)) + rangeRelativeGate;

    uint64_t count = 0;
    for (int i = 0; i < Histogram::bins; ++i)
        if (Histogram::low + (i + 0.5) * Histogram::step >= gate)
            count += shortTerms.counts[(size_t) i];
    if (count == 0)
        return 0.0;

    // Percentiles of the gated short-term distribution, from the bins.
    auto percentile = [&] (double fraction)
    {
        const auto target = (uint64_t) std::ceil (fraction * (double) count);
        uint64_t seen = 0;
        for (int i = 0; i < Histogram::bins; ++i)
        {
            const double centre = Histogram::low + (i + 0.5) * Histogram::step;
            if (centre < gate)
                continue;
            seen += shortTerms.counts[(size_t) i];
            if (seen >= std::max<uint64_t> (1, target))
                return centre;
        }
        return Histogram::high;
    };
    return percentile (0.95) - percentile (0.10);
}

//==============================================================================
void TruePeakMeter::prepare (int numChannels) noexcept
{
    channels = std::clamp (numChannels, 1, 2);
    reset();
}

void TruePeakMeter::reset() noexcept
{
    for (auto& h : history)
        h.fill (0.0f);
    peak = samplePeak = 0.0;
}

void TruePeakMeter::process (const float* const* input, int numChannels, int numSamples) noexcept
{
    static_assert (historySize == 2 * Limiter::interpolationHalfLength);
    const int used = std::min (numChannels, channels);
    for (int c = 0; c < used; ++c)
    {
        auto& h = history[(size_t) c];
        for (int i = 0; i < numSamples; ++i)
        {
            std::memmove (h.data(), h.data() + 1, sizeof (float) * (historySize - 1));
            h[historySize - 1] = input[c][i];
            const double x = std::abs ((double) input[c][i]);
            samplePeak = std::max (samplePeak, x);
            peak = std::max ({ peak, x, Limiter::intervalPeak (h.data()) });
        }
    }
}

double TruePeakMeter::getTruePeakDb() const noexcept
{
    return peak > 0.0 ? 20.0 * std::log10 (peak) : LoudnessMeter::silence;
}

} // namespace meridian::dsp
