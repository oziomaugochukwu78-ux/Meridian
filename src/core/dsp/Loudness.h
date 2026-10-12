#pragma once

#include "Biquad.h"

#include <array>
#include <cstdint>
#include <vector>

namespace meridian::dsp
{

/** Loudness and true peak, as in ITU-R BS.1770-4 and EBU R128.

    LoudnessMeter
      K-weighting (the two-stage filter of BS.1770, designed for any sample
      rate from its analogue prototype; at 48 kHz it reproduces the
      standard's coefficient table) -> mean square per 100 ms sub-block ->
        momentary  (M): the last 400 ms
        short-term (S): the last 3 s
        integrated (I): 400 ms blocks every 100 ms, gated at -70 LUFS
                        (absolute) and 10 LU below their mean (relative)
        loudness range (LRA, EBU Tech 3342): the spread between the 10th and
                        95th percentile of short-term loudness, gated at
                        -70 LUFS and 20 LU below their mean
      Gating uses fixed histograms (0.01 LU bins) that also keep the exact
      energy of each bin, so an hour of audio needs no more memory than a
      second, and the gated means are exact.

    TruePeakMeter
      The largest magnitude of the signal reconstructed at 4x the sample
      rate (sample points and three points between each pair), dBTP.

    Checked against the EBU Tech 3341 and 3342 test signals in the tests.
    Neither class allocates after prepare(). */

/** BS.1770 K-weighting: a high shelf (+4 dB above ~1.7 kHz) then the RLB high-pass. */
class KWeighting
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept { shelf.reset(); highPass.reset(); }
    double process (double x) noexcept { return highPass.process (shelf.process (x)); }

    const Biquad::Coefficients& getShelf() const noexcept { return shelfCoefficients; }
    const Biquad::Coefficients& getHighPass() const noexcept { return highPassCoefficients; }

private:
    Biquad shelf, highPass;
    Biquad::Coefficients shelfCoefficients, highPassCoefficients;
};

class LoudnessMeter
{
public:
    static constexpr int maxChannels = 2;
    static constexpr double absoluteGate = -70.0;      ///< LUFS
    static constexpr double relativeGate = -10.0;      ///< LU, integrated
    static constexpr double rangeRelativeGate = -20.0; ///< LU, loudness range
    static constexpr double silence = -200.0;          ///< reported when there is nothing to measure

    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;

    /** Adds audio (any block size). Channels beyond maxChannels are ignored. */
    void process (const float* const* channels, int numChannels, int numSamples) noexcept;

    /** For split metering (the audio thread K-weights, another thread keeps the
        statistics): adds one finished 100 ms sub-block, as its K-weighted mean
        square summed over channels. */
    void addSubBlock (double meanSquare) noexcept;

    double getMomentary() const noexcept;      ///< LUFS, last 400 ms
    double getShortTerm() const noexcept;      ///< LUFS, last 3 s
    double getIntegrated() const noexcept;     ///< LUFS, gated, since reset
    double getLoudnessRange() const noexcept;  ///< LU
    double getMaxMomentary() const noexcept { return maxMomentary; }
    double getMaxShortTerm() const noexcept { return maxShortTerm; }

    /** Loudness of a mean square (sum over channels): -0.691 + 10 log10 (z). */
    static double loudnessOf (double meanSquare) noexcept;

private:
    struct Histogram
    {
        static constexpr double low = -70.0, high = 10.0, step = 0.01;
        static constexpr int bins = (int) ((high - low) / step) + 1;
        std::vector<uint32_t> counts;
        std::vector<double> energies;
        uint64_t total = 0;
        void clear();
        void add (double meanSquare, double loudness);
    };

    void endSubBlock() noexcept;
    double windowMeanSquare (int subBlocks) const noexcept;
    static double gatedMean (const Histogram&, double gateLoudness) noexcept;

    double sampleRate = 48000.0;
    int channels = 2;
    int subBlockLength = 4800, subBlockFill = 0;
    std::array<KWeighting, maxChannels> weighting;
    std::array<double, maxChannels> sum {};

    // The last 30 sub-blocks (3 s), as mean squares summed over channels.
    std::array<double, 30> recent {};
    int recentCount = 0, recentPos = 0;

    Histogram blocks;     // 400 ms blocks, for integrated loudness
    Histogram shortTerms; // 3 s windows, for the loudness range
    double maxMomentary = silence, maxShortTerm = silence;
};

class TruePeakMeter
{
public:
    void prepare (int numChannels) noexcept;
    void reset() noexcept;
    /** Starts a new maximum but keeps the signal history, so a reset while
        audio runs does not see a false step from silence. */
    void resetPeak() noexcept { peak = samplePeak = 0.0; }
    void process (const float* const* channels, int numChannels, int numSamples) noexcept;

    double getTruePeak() const noexcept { return peak; }   ///< linear
    double getTruePeakDb() const noexcept;                  ///< dBTP
    double getSamplePeak() const noexcept { return samplePeak; }

private:
    static constexpr int historySize = 32; // the interpolator's span (Limiter::intervalPeak)
    int channels = 2;
    std::array<std::array<float, historySize>, 2> history {};
    double peak = 0.0, samplePeak = 0.0;
};

} // namespace meridian::dsp
