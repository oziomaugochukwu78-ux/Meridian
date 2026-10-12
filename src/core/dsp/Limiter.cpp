#include "Limiter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace meridian::dsp
{
namespace
{
constexpr int N = Limiter::interpolationHalfLength;

double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1.0e-17)
            break;
    }
    return sum;
}

/** 4x interpolator: phases 1..3 (a quarter, half and three quarters of the way
    from x[k] to x[k+1]), 2N taps each over x[k-N+1 .. k+N], each phase
    normalised to unity gain at DC. */
struct Interpolator
{
    std::array<std::array<double, 2 * N>, 3> taps {};

    Interpolator()
    {
        constexpr double beta = 8.0, pi = 3.141592653589793;
        for (int p = 0; p < 3; ++p)
        {
            double sum = 0.0;
            for (int j = 0; j < 2 * N; ++j)
            {
                const double a = (p + 1) / 4.0 + (N - 1 - j); // distance from tap to the interpolated point
                const double sinc = std::sin (pi * a) / (pi * a);
                const double r = a / N;
                const double window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
                taps[(size_t) p][(size_t) j] = sinc * window;
                sum += sinc * window;
            }
            for (auto& t : taps[(size_t) p])
                t /= sum;
        }
    }
};

const Interpolator& interpolator()
{
    static const Interpolator instance;
    return instance;
}
} // namespace

double Limiter::intervalPeak (const float* x) noexcept
{
    const auto& taps = interpolator().taps;
    double peak = 0.0;
    for (const auto& phase : taps)
    {
        double y = 0.0;
        for (int j = 0; j < 2 * N; ++j)
            y += phase[(size_t) j] * x[j];
        peak = std::max (peak, std::abs (y));
    }
    return peak;
}

void Limiter::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    lookahead = std::max (1, (int) std::lround (lookaheadMs * sampleRate / 1000.0));
    (void) interpolator(); // build the table here, not on the audio thread

    minIndex.assign ((size_t) lookahead + 2, 0);
    minValue.assign ((size_t) lookahead + 2, 1.0);
    boxValues.assign ((size_t) lookahead + 1, 1.0);
    delayL.assign ((size_t) getLatencySamples() + 1, 0.0f);
    delayR.assign ((size_t) getLatencySamples() + 1, 0.0f);
    update();
    reset();
}

void Limiter::reset() noexcept
{
    for (auto& h : history)
        h.fill (0.0f);
    previousInterval = 0.0;
    minHead = minCount = 0;
    sampleIndex = 0;
    released = 1.0;
    std::fill (boxValues.begin(), boxValues.end(), 1.0);
    boxSum = (double) boxValues.size();
    boxPos = 0;
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    delayPos = 0;
    reductionDb = 0.0;
}

void Limiter::setParams (const Params& p) noexcept
{
    const Params defaults;
    params.inputGainDb = std::clamp (finiteOr (p.inputGainDb, defaults.inputGainDb), 0.0, 24.0);
    params.ceilingDb = std::clamp (finiteOr (p.ceilingDb, defaults.ceilingDb), -24.0, 0.0);
    params.releaseMs = std::clamp (finiteOr (p.releaseMs, defaults.releaseMs), 1.0, 1000.0);
    params.truePeak = p.truePeak;
    update();
}

void Limiter::update() noexcept
{
    inputGain = std::pow (10.0, params.inputGainDb / 20.0);
    ceiling = std::pow (10.0, params.ceilingDb / 20.0);
    releaseCoef = std::exp (-1000.0 / (params.releaseMs * sampleRate));
}

void Limiter::process (float* left, float* right, int numSamples) noexcept
{
    if (delayL.empty())
        return;

    const int channels = right != nullptr ? 2 : 1;
    const int window = lookahead + 1;
    const auto queueSize = (int) minValue.size();
    const auto delaySize = (int) delayL.size();
    double gain = 1.0;

    for (int n = 0; n < numSamples; ++n)
    {
        // Detector: the peak of sample k = n - N, and of the intervals either side of it.
        double interval = 0.0, centre = 0.0;
        for (int c = 0; c < channels; ++c)
        {
            auto& h = history[(size_t) c];
            std::memmove (h.data(), h.data() + 1, sizeof (float) * (historySize - 1));
            h[historySize - 1] = c == 0 ? left[n] : right[n];
            centre = std::max (centre, (double) std::abs (h[N - 1]));
            if (params.truePeak)
                interval = std::max (interval, intervalPeak (h.data()));
        }
        const double peak = std::max (centre, std::max (interval, previousInterval)) * inputGain;
        previousInterval = interval;
        const double target = peak > ceiling ? ceiling / peak : 1.0;

        // Sliding minimum of the targets over the look-ahead window.
        while (minCount > 0 && minValue[(size_t) ((minHead + minCount - 1) % queueSize)] >= target)
            --minCount;
        const auto back = (size_t) ((minHead + minCount) % queueSize);
        minIndex[back] = sampleIndex;
        minValue[back] = target;
        ++minCount;
        while (minIndex[(size_t) minHead] <= sampleIndex - window)
        {
            minHead = (minHead + 1) % queueSize;
            --minCount;
        }
        const double held = minValue[(size_t) minHead];
        ++sampleIndex;

        // Release: down at once, back up exponentially (never above the held value).
        released = held < released ? held : held + releaseCoef * (released - held);

        // Moving average over the same window: the gain ramps over the look-ahead.
        boxSum += released - boxValues[(size_t) boxPos];
        boxValues[(size_t) boxPos] = released;
        if (++boxPos >= window)
        {
            boxPos = 0;
            boxSum = 0.0; // re-sum exactly once per window, so rounding never accumulates
            for (const auto v : boxValues)
                boxSum += v;
        }
        gain = std::min (1.0, boxSum / window);

        // The audio, delayed by the detector and look-ahead latency.
        delayL[(size_t) delayPos] = left[n];
        if (right != nullptr)
            delayR[(size_t) delayPos] = right[n];
        if (++delayPos >= delaySize)
            delayPos = 0;
        const double g = gain * inputGain;
        left[n] = (float) (delayL[(size_t) delayPos] * g);
        if (right != nullptr)
            right[n] = (float) (delayR[(size_t) delayPos] * g);
    }

    reductionDb = gain < 1.0 ? -20.0 * std::log10 (gain) : 0.0;
}

} // namespace meridian::dsp
