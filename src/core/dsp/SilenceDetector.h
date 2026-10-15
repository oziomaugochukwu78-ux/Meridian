#pragma once

#include <cstdint>
#include <vector>

namespace meridian::dsp
{

/** Finds the audible stretches of a signal, for Strip Silence.

    A sample is audible when its magnitude (on any channel) exceeds the
    threshold. Audible samples separated by less than `minSilence` samples of
    quiet belong to one region, so the gaps between words or notes stay. Each
    region is then widened by the pads (before the first and after the last
    audible sample), clipped to the signal, and regions the pads make touch
    are merged.

    Streaming: feed() any number of blocks in order, then regions(). No
    allocation per sample; the region list grows only when a region ends. */
class SilenceDetector
{
public:
    struct Region
    {
        int64_t start = 0, end = 0;   ///< [start, end), in samples from the first one fed
    };

    SilenceDetector (float thresholdGain, int64_t minSilence, int64_t padBefore, int64_t padAfter);

    /** Channels of `numSamples` samples each. */
    void feed (const float* const* channels, int numChannels, int numSamples, float gain = 1.0f);

    /** The audible regions of everything fed so far, padded and merged. */
    std::vector<Region> regions() const;

    int64_t samplesFed() const noexcept { return position; }

private:
    float threshold;
    int64_t minSilence, padBefore, padAfter;
    int64_t position = 0;
    std::vector<Region> found;   ///< unpadded, closed regions
    int64_t openStart = -1;      ///< start of the region still open, or -1
    int64_t lastAudible = -1;
};

} // namespace meridian::dsp
