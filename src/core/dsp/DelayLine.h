#pragma once

#include <algorithm>
#include <vector>

namespace meridian::dsp
{

/** Integer-sample delay for up to two channels, used for plug-in delay
    compensation. Allocated once on the message thread; process() is
    allocation-free and safe on the audio thread.

    The delay is passed per call rather than stored, so a render graph can
    change the amount while keeping the buffered history. */
class DelayLine
{
public:
    explicit DelayLine (int maximumDelay = 0) { setMaximumDelay (maximumDelay); }

    void setMaximumDelay (int maximumDelay)
    {
        size = std::max (1, maximumDelay + 1);
        for (auto& b : buffers)
            b.assign ((size_t) size, 0.0f);
        writeIndex = 0;
    }

    int getMaximumDelay() const noexcept { return size - 1; }

    void reset() noexcept
    {
        for (auto& b : buffers)
            std::fill (b.begin(), b.end(), 0.0f);
        writeIndex = 0;
    }

    /** out[ch][i] = in[ch][i - delay]. `in` and `out` must not overlap.
        delay is clamped to getMaximumDelay(). */
    void process (const float* const* in, float* const* out, int numChannels, int numSamples, int delay) noexcept
    {
        delay = std::clamp (delay, 0, size - 1);
        numChannels = std::clamp (numChannels, 1, 2);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* buffer = buffers[ch].data();
            int w = writeIndex;
            int r = w - delay;
            if (r < 0)
                r += size;

            for (int i = 0; i < numSamples; ++i)
            {
                buffer[w] = in[ch][i];
                out[ch][i] = buffer[r];
                if (++w == size) w = 0;
                if (++r == size) r = 0;
            }
        }

        writeIndex = (writeIndex + numSamples) % size;
    }

private:
    std::vector<float> buffers[2];
    int size = 1;
    int writeIndex = 0;
};

} // namespace meridian::dsp
