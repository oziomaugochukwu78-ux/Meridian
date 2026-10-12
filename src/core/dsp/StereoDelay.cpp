#include "StereoDelay.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }
} // namespace

void StereoDelay::prepare (double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    const auto size = (size_t) std::ceil (maxSeconds * sampleRate) + 2;
    lineL.assign (size, 0.0);
    lineR.assign (size, 0.0);
    update();
    reset();
}

void StereoDelay::reset() noexcept
{
    std::fill (lineL.begin(), lineL.end(), 0.0);
    std::fill (lineR.begin(), lineR.end(), 0.0);
    writePos = 0;
    stateL = stateR = 0.0;
}

void StereoDelay::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.timeMs = std::clamp (finiteOr (p.timeMs, defaults.timeMs), 1.0, maxSeconds * 1000.0);
    params.feedback = std::clamp (finiteOr (p.feedback, defaults.feedback), 0.0, 0.95);
    params.mix = std::clamp (finiteOr (p.mix, defaults.mix), 0.0, 1.0);
    params.highCutHz = std::clamp (finiteOr (p.highCutHz, defaults.highCutHz), 500.0, 20000.0);
    params.pingPong = p.pingPong;
    update();
}

void StereoDelay::update() noexcept
{
    if (lineL.empty())
        return;
    delaySamples = std::clamp ((int) std::lround (params.timeMs * sampleRate / 1000.0), 1, (int) lineL.size() - 1);
    // One-pole low-pass in the loop; 20 kHz means off (exactly transparent).
    cutCoef = params.highCutHz >= 20000.0 ? 0.0
                                          : std::exp (-2.0 * 3.141592653589793 * std::min (params.highCutHz, 0.45 * sampleRate) / sampleRate);
}

void StereoDelay::process (float* left, float* right, int numSamples) noexcept
{
    if (lineL.empty())
        return;
    const auto size = (int) lineL.size();
    const bool stereo = right != nullptr;
    const bool pingPong = params.pingPong && stereo;
    const double feedback = params.feedback, wet = params.mix, a = cutCoef;

    for (int n = 0; n < numSamples; ++n)
    {
        int readPos = writePos - delaySamples;
        if (readPos < 0)
            readPos += size;
        const double inL = left[n];
        const double inR = stereo ? (double) right[n] : inL;

        // The delayed signal, darkened by the loop's high-cut.
        stateL = (1.0 - a) * lineL[(size_t) readPos] + a * stateL;
        stateR = (1.0 - a) * lineR[(size_t) readPos] + a * stateR;
        const double echoL = stateL, echoR = stateR;

        if (pingPong)
        {
            // The input (summed) enters on the left; every repeat crosses to the
            // other side: left at 1x the time, right at 2x, left at 3x...
            lineL[(size_t) writePos] = 0.5 * (inL + inR) + feedback * echoR;
            lineR[(size_t) writePos] = feedback * echoL;
        }
        else
        {
            lineL[(size_t) writePos] = inL + feedback * echoL;
            lineR[(size_t) writePos] = inR + feedback * echoR;
        }
        if (++writePos >= size)
            writePos = 0;

        left[n] = (float) (inL + wet * echoL);
        if (stereo)
            right[n] = (float) (inR + wet * echoR);
    }
}

} // namespace meridian::dsp
