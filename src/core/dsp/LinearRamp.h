#pragma once

#include "MixKernels.h"

#include <algorithm>

namespace meridian::dsp
{

/** A gain value that glides linearly to new targets.

    Used for every gain change on the audio path (fader moves, mutes, remote
    SET_VOLUME commands), so a jump in gain becomes a short ramp and never an
    audible click. Audio-thread only; not thread-safe by design.
*/
class LinearRamp
{
public:
    void reset (double value) noexcept
    {
        current = target = value;
        increment = 0.0;
        remaining = 0;
    }

    /** Starts a ramp from the current value to newTarget over rampSamples.
        Re-targeting mid-ramp continues smoothly from wherever the ramp is now. */
    void setTarget (double newTarget, int rampSamples) noexcept
    {
        if (newTarget == target)
            return;

        target = newTarget;

        if (rampSamples <= 0)
        {
            reset (newTarget);
            return;
        }

        remaining = rampSamples;
        increment = (target - current) / static_cast<double> (rampSamples);
    }

    bool isRamping() const noexcept     { return remaining > 0; }
    double getCurrent() const noexcept  { return current; }
    double getTarget() const noexcept   { return target; }

    /** Writes the per-sample values for the next numSamples and advances.
        Sample k holds the value after k + 1 steps, so a ramp of length N ends
        exactly on the target at its last sample. */
    void render (double* dst, int numSamples) noexcept
    {
        const int rampPart = std::min (numSamples, remaining);

        if (rampPart > 0)
        {
            fillLinearRamp (dst, current + increment, increment, rampPart);
            remaining -= rampPart;
            current = remaining == 0 ? target : current + increment * rampPart;
            if (remaining == 0)
                dst[rampPart - 1] = target; // land exactly: a ramp to 0 must be true silence, not -6e-17
        }

        std::fill (dst + rampPart, dst + numSamples, current);
    }

    /** Advances without producing values (e.g. while a track is silent). */
    void skip (int numSamples) noexcept
    {
        const int rampPart = std::min (numSamples, remaining);
        remaining -= rampPart;
        current = remaining == 0 ? target : current + increment * rampPart;
    }

private:
    double current = 0.0;
    double target = 0.0;
    double increment = 0.0;
    int remaining = 0;
};

} // namespace meridian::dsp
