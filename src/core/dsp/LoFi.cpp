#include "LoFi.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{
namespace
{
constexpr double twoPi = 6.28318530717958647692;

/** Delay-line swing (in samples) that bends the pitch by `cents` at `hz`. */
double swingFor (double cents, double hz, double rate) noexcept
{
    const double ratio = std::pow (2.0, cents / 1200.0) - 1.0;
    return ratio * rate / (twoPi * hz);
}

float readLine (const std::vector<float>& line, double position) noexcept
{
    const auto size = (int) line.size();
    const auto i = (int) std::floor (position);
    const float t = (float) (position - (double) i);
    auto at = [&] (int k) { return line[(size_t) (((k % size) + size) % size)]; };
    const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
} // namespace

void LoFi::prepare (double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    centre = (int) std::lround (centreDelaySeconds * sampleRate);
    // Room for the centre delay, the widest wow swing either side, and interpolation.
    const auto swing = swingFor (maxWowCents, wowHz, sampleRate) + swingFor (maxFlutterCents, flutterHz, sampleRate);
    const auto size = (size_t) (2 * centre + (int) std::ceil (swing) + 8);
    lineL.assign (size, 0.0f);
    lineR.assign (size, 0.0f);
    setParams (params);
    reset();
}

void LoFi::setParams (const Params& p) noexcept
{
    params = p;
    params.wow = std::clamp (p.wow, 0.0, 1.0);
    params.flutter = std::clamp (p.flutter, 0.0, 1.0);
    params.noise = std::clamp (p.noise, 0.0, 1.0);
    params.bits = std::clamp (p.bits, 4, 16);
    params.downsample = std::clamp (p.downsample, 1, 8);
    params.toneHz = std::clamp (p.toneHz, 500.0, 20000.0);
    params.mix = std::clamp (p.mix, 0.0, 1.0);
    // Wow can never swing past the centre delay: the read point stays behind the write point.
    wowDepth = std::min (swingFor (maxWowCents * params.wow, wowHz, sampleRate), (double) centre * 0.75);
    flutterDepth = swingFor (maxFlutterCents * params.flutter, flutterHz, sampleRate);
    lpCoefficient = std::exp (-twoPi * params.toneHz / sampleRate);
}

void LoFi::reset() noexcept
{
    std::fill (lineL.begin(), lineL.end(), 0.0f);
    std::fill (lineR.begin(), lineR.end(), 0.0f);
    write = 0;
    wowPhase = flutterPhase = 0.0;
    rng = 0x2545f491u;
    crackle = hissState = 0.0;
    for (auto& channel : lp)
        channel[0] = channel[1] = 0.0;
    held[0] = held[1] = 0.0f;
    holdCount = 0;
}

float LoFi::noiseSample() noexcept
{
    // xorshift32: fast, deterministic
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float) ((double) rng / 4294967295.0 * 2.0 - 1.0);
}

void LoFi::process (float* left, float* right, int numSamples) noexcept
{
    if (lineL.empty())
        return;
    const auto size = (int) lineL.size();
    const double wowStep = twoPi * wowHz / sampleRate, flutterStep = twoPi * flutterHz / sampleRate;
    const bool filtering = params.toneHz < 19999.0;
    const double steps = std::ldexp (1.0, params.bits - 1);
    const bool crushing = params.bits < 16;
    const double clicksPerSample = 30.0 * params.noise / sampleRate;
    const double clickDecay = std::exp (-1.0 / (0.0006 * sampleRate));
    const double hissLevel = 0.004 * params.noise;     // about -48 dBFS at full noise
    const auto wet = (float) params.mix, dry = 1.0f - wet;

    for (int i = 0; i < numSamples; ++i)
    {
        lineL[(size_t) write] = left[i];
        lineR[(size_t) write] = right != nullptr ? right[i] : left[i];

        const double offset = (double) centre + wowDepth * std::sin (wowPhase) + flutterDepth * std::sin (flutterPhase);
        const double readPosition = (double) write - offset;
        const auto dryIndex = (size_t) (((write - centre) % size + size) % size);
        float out[2] = { readLine (lineL, readPosition), readLine (lineR, readPosition) };
        const float dryIn[2] = { lineL[dryIndex], lineR[dryIndex] };
        wowPhase += wowStep;
        flutterPhase += flutterStep;
        if (wowPhase > twoPi) wowPhase -= twoPi;
        if (flutterPhase > twoPi) flutterPhase -= twoPi;
        write = (write + 1) % size;

        // Vinyl: hiss (softened white noise) plus clicks at random moments.
        float vinyl = 0.0f;
        if (params.noise > 0.0)
        {
            hissState = 0.7 * hissState + 0.3 * (double) noiseSample();
            if ((double) (noiseSample() * 0.5f + 0.5f) < clicksPerSample)
                crackle = (noiseSample() > 0.0f ? 1.0 : -1.0) * (0.02 + 0.08 * (double) (noiseSample() * 0.5f + 0.5f)) * params.noise;
            vinyl = (float) (hissState * hissLevel + crackle);
            crackle *= clickDecay;
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            double x = out[ch] + vinyl;
            if (filtering)
            {
                lp[ch][0] = (1.0 - lpCoefficient) * x + lpCoefficient * lp[ch][0];
                lp[ch][1] = (1.0 - lpCoefficient) * lp[ch][0] + lpCoefficient * lp[ch][1];
                x = lp[ch][1];
            }
            if (crushing)
                x = std::round (x * steps) / steps;
            out[ch] = (float) x;
        }
        if (params.downsample > 1)
        {
            if (holdCount == 0)
            {
                held[0] = out[0];
                held[1] = out[1];
            }
            holdCount = (holdCount + 1) % params.downsample;
            out[0] = held[0];
            out[1] = held[1];
        }

        left[i] = dry * dryIn[0] + wet * out[0];
        if (right != nullptr)
            right[i] = dry * dryIn[1] + wet * out[1];
    }
}

} // namespace meridian::dsp
