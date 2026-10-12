#pragma once

#include <cstdint>

namespace meridian::dsp
{

/** The click: a short sine burst on every beat, higher and louder on the
    first beat of each bar.

    Beat b of the timeline starts exactly on sample round(b * samplesPerBeat),
    whatever the block size; a click that starts near the end of a block
    continues seamlessly into the next. Rendering the same span in different
    block sizes gives bit-identical output (tested).

    Each click: sin(2 pi f t) * exp(-t / 6 ms), 40 ms long, starting at zero
    phase (no step at the onset). Accent 2 kHz, other beats 1 kHz. Each is
    scaled so its sampled peak is exactly its level (accent: levelDb; other
    beats: levelDb + unaccentedDb).

    Not allocating; render() is real-time safe. */
class Metronome
{
public:
    struct Params
    {
        double bpm = 120.0;
        int beatsPerBar = 4;
        double levelDb = -6.0;       ///< peak of the accented click
        double unaccentedDb = -4.0;  ///< other beats, relative to the accent
    };

    static constexpr double accentHz = 2000.0, beatHz = 1000.0;
    static constexpr double clickSeconds = 0.040, decaySeconds = 0.006;

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept { remaining = 0; }

    double getSamplesPerBeat() const noexcept { return samplesPerBeat; }

    /** First sample of beat `beat` on this grid. */
    int64_t beatStart (int64_t beat) const noexcept;

    /** Adds the clicks for timeline samples [position, position + numSamples)
        to `out` (mono). Beat 0 is at sample 0. */
    void render (float* out, int numSamples, int64_t position) noexcept;

private:
    void startClick (bool accent) noexcept;
    double samplePeak (double hz) const noexcept;

    Params params;
    double sampleRate = 48000.0, samplesPerBeat = 24000.0;
    int clickLength = 1920;
    double decayPerSample = 0.0, accentGain = 0.5, beatGain = 0.3;

    // The click in progress (it may span blocks).
    int remaining = 0, elapsed = 0;
    double frequency = beatHz, gain = 0.0, envelope = 1.0;
};

} // namespace meridian::dsp
