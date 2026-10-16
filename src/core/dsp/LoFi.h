#pragma once

#include <cstdint>
#include <vector>

namespace meridian::dsp
{

/** The lo-fi record: tape wow and flutter, vinyl crackle and hiss, bit and
    sample-rate reduction, and a darker tone.

      wow       slow pitch drift, 0..1 = up to +/-12 cents at 0.5 Hz
      flutter   fast pitch waver, 0..1 = up to +/-4 cents at 6 Hz
      noise     vinyl, 0..1: crackle (random clicks, up to 30 a second) and hiss
      bits      16 = off, down to 4: quantisation steps
      downsample 1 = off, up to 8: holds each sample this many times
      toneHz    low-pass (12 dB/octave); 20000 = off
      mix       0 = dry, 1 = fully processed

    Wow and flutter read a delay line around a fixed centre (latencySamples),
    so the effect delays everything by that much; the dry signal takes the
    same delay, so mixing never smears. With every control off, the output is
    the input delayed by exactly latencySamples. The noise comes from a fixed
    seed: the same input gives the same output every time.

    prepare() allocates; process() does not. */
class LoFi
{
public:
    struct Params
    {
        double wow = 0.3, flutter = 0.3, noise = 0.2;
        int bits = 16;
        int downsample = 1;
        double toneHz = 8000.0;
        double mix = 1.0;
    };

    static constexpr double centreDelaySeconds = 0.010;
    static constexpr double maxWowCents = 12.0, wowHz = 0.5;
    static constexpr double maxFlutterCents = 4.0, flutterHz = 6.0;

    void prepare (double sampleRate);
    void setParams (const Params&) noexcept;
    void reset() noexcept;
    int getLatencySamples() const noexcept { return centre; }

    /** In place; right may be null (mono). */
    void process (float* left, float* right, int numSamples) noexcept;

private:
    float noiseSample() noexcept;

    Params params;
    double sampleRate = 48000.0;
    int centre = 480;
    std::vector<float> lineL, lineR;
    int write = 0;
    double wowPhase = 0.0, flutterPhase = 0.0, wowDepth = 0.0, flutterDepth = 0.0;
    uint32_t rng = 0x2545f491u;
    double crackle = 0.0;          ///< the current click's level, decaying
    double hissState = 0.0;
    double lp[2][2] = {};          ///< two one-pole stages per channel
    double lpCoefficient = 0.0;
    float held[2] = {};
    int holdCount = 0;
};

} // namespace meridian::dsp
