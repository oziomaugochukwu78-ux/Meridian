#pragma once

namespace meridian::dsp
{

/** A feed-forward stereo-linked compressor.

        detector:      the louder channel's instantaneous level, in dB
        gain computer: threshold, ratio and a quadratic soft knee
                       (Giannoulis, Massberg & Reiss, JAES 2012)
        smoothing:     on the gain reduction, in dB: a release peak-hold
                       followed by the attack filter ("smooth decoupled"),
                       so a steady tone is held at its peak gain reduction
                       with almost no ripple
        output:        makeup gain, then a dry/wet mix (parallel compression)

    Specification, checked by the unit tests:
      - static curve: a steady level comes out at exactly the curve below
      - attack: a step into compression reaches 63% (1 - 1/e) of its final
        gain reduction after the attack time
      - release: likewise on the way back
      - ratio 1:1, makeup 0 dB, mix 100% passes audio bit for bit

    Not allocating; process() is real-time safe. */
class Compressor
{
public:
    struct Params
    {
        double thresholdDb = -18.0; ///< -60..0
        double ratio = 4.0;         ///< 1..20 (20 is effectively limiting)
        double attackMs = 10.0;     ///< 0.05..200
        double releaseMs = 150.0;   ///< 5..3000
        double kneeDb = 6.0;        ///< 0 (hard) .. 24
        double makeupDb = 0.0;      ///< 0..24
        double mix = 1.0;           ///< 0 (dry) .. 1 (fully compressed)
    };

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    /** In place; right may be null (mono). Both channels get the same gain. */
    void process (float* left, float* right, int numSamples) noexcept;

    /** The static curve: output level for a steady input level (both in dB). */
    double staticCurve (double inputDb) const noexcept;

    /** Current gain reduction in dB (0 or positive), for metering. */
    double getGainReductionDb() const noexcept { return smoothed; }

private:
    void update() noexcept;

    Params params;
    double sampleRate = 48000.0;
    double attackCoef = 0.0, releaseCoef = 0.0, makeupGain = 1.0;
    double held = 0.0, smoothed = 0.0; // gain reduction in dB, after release hold and after attack
};

} // namespace meridian::dsp
