#pragma once

#include "Biquad.h"

namespace meridian::dsp
{

/** A de-esser (stereo-linked): a dynamic high shelf.

    The detector listens to the signal above `frequencyHz` (a 2nd-order
    high-pass) with a 1 ms RMS envelope, scaled so a steady sine reads its
    peak amplitude whatever the sample rate. When that level rises above
    `thresholdDb`, a high shelf at `frequencyHz` is turned down by
    (level - threshold) * 3/4 dB (4:1), at most `rangeDb`. Attack 0.5 ms,
    release 60 ms. Below the frequency the signal is left alone; with
    nothing to reduce the shelf is at 0 dB and the output is the input.

    Specification, checked by the unit tests:
      - a loud tone below the frequency passes bit for bit
      - a tone in the band is turned down by the shelf's designed response
        for the computed reduction
      - the reduction never exceeds the range
      - in a mixture, the low part is untouched and the high part goes down

    Not allocating; process() is real-time safe. */
class DeEsser
{
public:
    struct Params
    {
        double thresholdDb = -30.0;  ///< -60..0
        double frequencyHz = 6000.0; ///< 2000..12000
        double rangeDb = 12.0;       ///< 0..24: the most it will turn the band down
    };

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    /** Current reduction of the shelf, in dB (0 or positive). */
    double getGainReductionDb() const noexcept { return reductionDb; }

private:
    void update() noexcept;
    void setShelf (double gainDb) noexcept;

    static constexpr int coefficientInterval = 16; ///< the shelf follows the reduction every 16 samples

    Params params;
    double sampleRate = 48000.0;
    Biquad detectL, detectR, shelfL, shelfR;
    double rmsCoef = 0.0, attackCoef = 0.0, releaseCoef = 0.0;
    double meanSquare = 0.0, heldDb = 0.0, reductionDb = 0.0, shelfDb = 0.0;
    int untilUpdate = 0;
};

} // namespace meridian::dsp
