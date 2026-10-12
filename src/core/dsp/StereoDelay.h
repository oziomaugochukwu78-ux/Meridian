#pragma once

#include <vector>

namespace meridian::dsp
{

/** A stereo echo.

        out = dry * in + mix * echoes
        echoes: the input delayed by `time`, fed back at `feedback` (each
        repeat that much quieter), through a one-pole high-cut in the loop
        (repeats get darker, like tape); ping-pong sends each repeat to the
        other side.

    Specification, checked by the unit tests:
      - the first echo arrives exactly `time` after the input (to the sample)
      - each further repeat is exactly `feedback` times the one before
        (high-cut off), and alternates sides in ping-pong
      - mix 0 passes the input bit for bit; feedback stays stable to 0.95

    prepare() allocates (up to maxSeconds); process() is real-time safe. */
class StereoDelay
{
public:
    struct Params
    {
        double timeMs = 375.0;      ///< 1..2000
        double feedback = 0.35;     ///< 0..0.95 (linear gain per repeat)
        double mix = 0.25;          ///< 0..1 (wet level; the dry signal stays at unity)
        double highCutHz = 20000.0; ///< 500..20000 in the feedback loop (20000 = off)
        bool pingPong = false;
    };

    static constexpr double maxSeconds = 2.0;

    void prepare (double sampleRate);
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    /** In place; right may be null (mono: no ping-pong). */
    void process (float* left, float* right, int numSamples) noexcept;

    int getDelaySamples() const noexcept { return delaySamples; }

private:
    void update() noexcept;

    Params params;
    double sampleRate = 48000.0;
    std::vector<double> lineL, lineR;
    int writePos = 0, delaySamples = 18000;
    double cutCoef = 0.0, stateL = 0.0, stateR = 0.0;
};

} // namespace meridian::dsp
