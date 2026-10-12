#pragma once

#include <array>
#include <vector>

namespace meridian::dsp
{

/** A look-ahead brickwall limiter (stereo-linked), for the end of a mix.

        detector:   the peak of each sample, and with true peak on, the
                    inter-sample peaks too (4x interpolation, as BS.1770
                    meters do, with a 32-tap Kaiser-windowed sinc per phase)
        gain:       target t = ceiling / peak (at most 1); a sliding minimum
                    over the look-ahead window, an exponential release, then
                    a moving average over the same window
        output:     the input, delayed by the look-ahead, times that gain

    Why the ceiling holds exactly: the averaged gain applied to sample k is a
    mean of values that are each at most min(t over a window containing k),
    so it never exceeds t[k], and |output| <= ceiling for every sample. The
    gain only ever ramps over the look-ahead window, so limiting adds no
    clicks.

    True peaks are held as accurately as they can be estimated. Measured
    against a 128-tap 16x reference: exact for programme material below
    16 kHz (at 48 kHz) and within 0.05 dB on a quarter-rate sine (the classic
    +3 dB inter-sample over). Full-band white noise, with energy right up to
    Nyquist, under-reads by about 0.6 dB, as every finite interpolator does.

    Latency is constant (getLatencySamples), whatever the settings, so delay
    compensation never has to change while playing. prepare() allocates;
    process() is real-time safe. */
class Limiter
{
public:
    struct Params
    {
        double inputGainDb = 0.0; ///< 0..24: drive into the limiter
        double ceilingDb = -1.0;  ///< -24..0: the output never exceeds this
        double releaseMs = 60.0;  ///< 1..1000
        bool truePeak = true;     ///< also hold inter-sample peaks under the ceiling
    };

    static constexpr int interpolationHalfLength = 16; ///< taps each side of the interval
    static constexpr double lookaheadMs = 1.5;

    void prepare (double sampleRate);
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    int getLatencySamples() const noexcept { return interpolationHalfLength + lookahead; }

    /** In place; right may be null (mono). */
    void process (float* left, float* right, int numSamples) noexcept;

    /** Current gain reduction in dB (0 or positive), for metering. */
    double getGainReductionDb() const noexcept { return reductionDb; }

    /** The largest inter-sample magnitude in the interval (k, k+1) from 2N
        samples x[k-N+1 .. k+N], using the same interpolator as the detector. */
    static double intervalPeak (const float* window2N) noexcept;

private:
    static constexpr int historySize = 2 * interpolationHalfLength;

    void update() noexcept;

    Params params;
    double sampleRate = 48000.0;
    int lookahead = 72;
    double inputGain = 1.0, ceiling = 1.0, releaseCoef = 0.0;

    // Input history for the interpolator (both channels), newest last.
    std::array<std::array<float, historySize>, 2> history {};
    double previousInterval = 0.0;

    // Sliding minimum over lookahead + 1 targets (monotonic queue).
    std::vector<long long> minIndex;
    std::vector<double> minValue;
    int minHead = 0, minCount = 0;
    long long sampleIndex = 0;

    double released = 1.0;
    std::vector<double> boxValues;
    double boxSum = 0.0;
    int boxPos = 0;

    std::vector<float> delayL, delayR;
    int delayPos = 0;
    double reductionDb = 0.0;
};

} // namespace meridian::dsp
