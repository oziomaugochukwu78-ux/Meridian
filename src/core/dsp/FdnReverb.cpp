#include "FdnReverb.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
// Base line lengths (ms) at size 1: mutually prime sample counts at common rates,
// spread so the echoes never line up.
constexpr double baseMs[FdnReverb::numLines] = { 31.7, 37.1, 41.9, 47.3, 53.9, 59.3, 67.1, 73.7 };
constexpr double maxSize = 2.0;
constexpr double diffuserMs[4] = { 4.77, 3.59, 5.31, 2.89 }; // left: 0,1  right: 2,3
constexpr double diffuserGain = 0.6;

// Output tap signs: two different patterns give two decorrelated outputs.
constexpr double leftSigns[FdnReverb::numLines]  = { 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr double rightSigns[FdnReverb::numLines] = { 1, 1, -1, -1, 1, 1, -1, -1 };

double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }

/** In-place orthonormal 8-point Hadamard transform (energy preserving). */
void hadamard8 (std::array<double, FdnReverb::numLines>& x) noexcept
{
    for (int h = 1; h < 8; h *= 2)
        for (int i = 0; i < 8; i += h * 2)
            for (int j = i; j < i + h; ++j)
            {
                const double a = x[(size_t) j], b = x[(size_t) (j + h)];
                x[(size_t) j] = a + b;
                x[(size_t) (j + h)] = a - b;
            }
    constexpr double norm = 0.35355339059327373; // 1 / sqrt(8)
    for (auto& v : x)
        v *= norm;
}
} // namespace

double FdnReverb::AllPass::process (double x, double g) noexcept
{
    const double delayed = buffer[(size_t) pos];
    const double v = x + g * delayed;
    buffer[(size_t) pos] = v;
    if (++pos >= (int) buffer.size())
        pos = 0;
    return delayed - g * v;
}

void FdnReverb::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxLineLength = (int) std::ceil (baseMs[numLines - 1] * maxSize * sampleRate / 1000.0) + 2;
    for (auto& line : lines)
        line.buffer.assign ((size_t) maxLineLength, 0.0);
    for (size_t i = 0; i < diffusers.size(); ++i)
        diffusers[i].buffer.assign ((size_t) std::max (1, (int) std::lround (diffuserMs[i] * sampleRate / 1000.0)), 0.0);
    const auto maxPredelay = (size_t) std::ceil (0.25 * sampleRate) + 1;
    predelayL.assign (maxPredelay, 0.0);
    predelayR.assign (maxPredelay, 0.0);
    update();
    reset();
}

void FdnReverb::reset() noexcept
{
    for (auto& line : lines)
    {
        std::fill (line.buffer.begin(), line.buffer.end(), 0.0);
        line.pos = 0;
    }
    for (auto& d : diffusers)
    {
        std::fill (d.buffer.begin(), d.buffer.end(), 0.0);
        d.pos = 0;
    }
    std::fill (predelayL.begin(), predelayL.end(), 0.0);
    std::fill (predelayR.begin(), predelayR.end(), 0.0);
    predelayPos = 0;
    lowState.fill (0.0);
}

void FdnReverb::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.size = std::clamp (finiteOr (p.size, defaults.size), 0.3, maxSize);
    params.decay = std::clamp (finiteOr (p.decay, defaults.decay), 0.2, 20.0);
    params.damping = std::clamp (finiteOr (p.damping, defaults.damping), 0.0, 1.0);
    params.predelayMs = std::clamp (finiteOr (p.predelayMs, defaults.predelayMs), 0.0, 250.0);
    params.mix = std::clamp (finiteOr (p.mix, defaults.mix), 0.0, 1.0);
    params.width = std::clamp (finiteOr (p.width, defaults.width), 0.0, 1.0);
    update();
}

void FdnReverb::update() noexcept
{
    if (maxLineLength <= 0)
        return; // not prepared yet: prepare() calls update()

    for (int i = 0; i < numLines; ++i)
    {
        auto& line = lines[(size_t) i];
        line.length = std::clamp ((int) std::lround (baseMs[i] * params.size * sampleRate / 1000.0), 1, maxLineLength);
        if (line.pos >= line.length)
            line.pos = 0;
        // Exact RT60: each pass through this line must lose 60 dB * (length / RT60 samples).
        gains[(size_t) i] = std::pow (10.0, -3.0 * line.length / (params.decay * sampleRate));
    }

    // Damping: a one-pole low-pass in each loop (unity gain at DC, so the decay time
    // above holds for low frequencies; highs die away faster as damping rises).
    const double cutoff = 18000.0 * std::pow (0.05, params.damping); // 18 kHz .. 900 Hz
    dampCoef = std::exp (-2.0 * 3.141592653589793 * std::min (cutoff, 0.45 * sampleRate) / sampleRate);

    predelaySamples = std::clamp ((int) std::lround (params.predelayMs * sampleRate / 1000.0), 0, (int) predelayL.size() - 1);
}

void FdnReverb::process (float* left, float* right, int numSamples) noexcept
{
    if (maxLineLength <= 0)
        return;

    const double wet = params.mix, dry = 1.0 - params.mix;
    const double damp = params.damping > 0.0 ? dampCoef : 0.0;
    const auto predelaySize = (int) predelayL.size();
    std::array<double, numLines> taps {};

    for (int n = 0; n < numSamples; ++n)
    {
        const double inL = left[n];
        const double inR = right != nullptr ? (double) right[n] : inL;

        // Pre-delay.
        predelayL[(size_t) predelayPos] = inL;
        predelayR[(size_t) predelayPos] = inR;
        int readPos = predelayPos - predelaySamples;
        if (readPos < 0)
            readPos += predelaySize;
        double l = predelayL[(size_t) readPos], r = predelayR[(size_t) readPos];
        if (++predelayPos >= predelaySize)
            predelayPos = 0;

        // Input diffusion: smear the attack into a dense wash.
        l = diffusers[1].process (diffusers[0].process (l, diffuserGain), diffuserGain);
        r = diffusers[3].process (diffusers[2].process (r, diffuserGain), diffuserGain);

        // Read every line, damp, apply its decay gain, mix.
        double outL = 0.0, outR = 0.0;
        for (size_t i = 0; i < numLines; ++i)
        {
            const double x = lines[i].read();
            outL += leftSigns[i] * x;
            outR += rightSigns[i] * x;
            lowState[i] = (1.0 - damp) * x + damp * lowState[i];
            taps[i] = lowState[i] * gains[i];
        }
        hadamard8 (taps);
        for (size_t i = 0; i < numLines; ++i)
            lines[i].write (taps[i] + 0.5 * ((i % 2 == 0) ? l : r));

        // Stereo output with width (mid/side), then the dry/wet mix.
        outL *= 0.35;
        outR *= 0.35;
        const double mid = 0.5 * (outL + outR), side = 0.5 * (outL - outR) * params.width;
        const double wetL = mid + side, wetR = mid - side;

        if (right != nullptr)
        {
            left[n] = (float) (dry * inL + wet * wetL);
            right[n] = (float) (dry * inR + wet * wetR);
        }
        else
        {
            left[n] = (float) (dry * inL + wet * mid); // mono: the mid of the stereo tail
        }
    }
}

} // namespace meridian::dsp
