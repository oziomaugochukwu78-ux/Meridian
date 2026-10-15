#include "SilenceDetector.h"

#include <algorithm>
#include <cmath>

namespace meridian::dsp
{

SilenceDetector::SilenceDetector (float thresholdGain, int64_t minimumSilence, int64_t before, int64_t after)
    : threshold (std::max (0.0f, thresholdGain)),
      minSilence (std::max<int64_t> (1, minimumSilence)),
      padBefore (std::max<int64_t> (0, before)),
      padAfter (std::max<int64_t> (0, after))
{
}

void SilenceDetector::feed (const float* const* channels, int numChannels, int numSamples, float gain)
{
    const float limit = gain > 0.0f ? threshold / gain : 0.0f;
    for (int i = 0; i < numSamples; ++i, ++position)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            peak = std::max (peak, std::abs (channels[ch][i]));
        if (! (peak > limit) || gain <= 0.0f)
            continue;

        // A quiet run of at least minSilence samples ends the region before it.
        if (openStart >= 0 && position - lastAudible - 1 >= minSilence)
        {
            found.push_back ({ openStart, lastAudible + 1 });
            openStart = -1;
        }
        if (openStart < 0)
            openStart = position;
        lastAudible = position;
    }
}

std::vector<SilenceDetector::Region> SilenceDetector::regions() const
{
    auto raw = found;
    if (openStart >= 0)
        raw.push_back ({ openStart, lastAudible + 1 });

    std::vector<Region> result;
    for (const auto& r : raw)
    {
        Region padded { std::max<int64_t> (0, r.start - padBefore), std::min (position, r.end + padAfter) };
        if (! result.empty() && padded.start <= result.back().end)
            result.back().end = std::max (result.back().end, padded.end);
        else
            result.push_back (padded);
    }
    return result;
}

} // namespace meridian::dsp
