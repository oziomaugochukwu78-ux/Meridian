#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace meridian::dsp
{

/** Monophonic pitch detection (YIN: de Cheveigné and Kawahara, 2002), for the tuner.

    The difference function d(tau) = sum (x[j] - x[j + tau])^2 is normalised by
    its running mean; the period is the first dip below `threshold`, walked down
    to its minimum and refined by a parabola through d. A signal below -60 dBFS,
    or one with no clear period (noise, chords), reads as no pitch.

    Specification, checked by the unit tests:
      - sines from 30 Hz to 2 kHz read within 1 cent
      - harmonic-rich tones (sawtooth) read their fundamental, not an octave off
      - silence and white noise read as no pitch
      - nearestNote: 440 Hz is A4 (MIDI 69) at 0 cents; cents are within +/-50

    prepare() allocates; detect() does not. */
class PitchDetector
{
public:
    struct Result
    {
        bool found = false;
        double frequencyHz = 0.0;
        double clarity = 0.0; ///< 0..1: 1 - the normalised difference at the period
    };

    struct Note
    {
        int midi = 69;
        double cents = 0.0; ///< -50..+50 from the nearest equal-tempered note
    };

    static constexpr double threshold = 0.15;
    static constexpr double minHz = 30.0, maxHz = 2000.0;

    void prepare (double sampleRate);

    /** How many samples detect() reads (about 70 ms). */
    int getWindowSamples() const noexcept { return guard + window + maxTau + 2 + guard; }

    /** Analyses getWindowSamples() consecutive samples. */
    Result detect (const float* samples) noexcept;

    static Note nearestNote (double frequencyHz, double referenceHz = 440.0) noexcept;
    static std::string noteName (int midi); ///< 60 -> "C4", 69 -> "A4", 70 -> "A#4"

private:
    static constexpr size_t interpolationTaps = 32;
    static constexpr int guard = (int) interpolationTaps / 2; ///< samples kept either side for the interpolation

    /** d (tau) at a fractional lag; `input` as passed to detect(). */
    double differenceAt (const float* input, double lag) const noexcept;

    double sampleRate = 48000.0;
    int window = 0, minTau = 0, maxTau = 0;
    std::vector<double> difference, normalised;
};

/** The last few thousand samples of a signal, written by the audio thread and
    read from another (the tuner's display). Lock-free: the writer never waits;
    a reader that was overtaken while copying is told so and tries again later. */
class SampleHistory
{
public:
    /** Allocates; `capacity` is rounded up to a power of two. */
    void prepare (int capacity);
    void reset() noexcept { written.store (0, std::memory_order_release); }

    /** Audio thread: appends the mono mix of left (and right, if not null). */
    void push (const float* left, const float* right, int numSamples) noexcept;

    /** Any thread: the latest `numSamples`, oldest first. False if fewer have been
        written, if more than the capacity is asked for, or if the writer overtook the copy. */
    bool copyLatest (float* destination, int numSamples) const noexcept;

private:
    std::vector<std::atomic<float>> ring;
    uint64_t mask = 0;
    std::atomic<uint64_t> written { 0 };
};

} // namespace meridian::dsp
