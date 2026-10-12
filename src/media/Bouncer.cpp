#include "Bouncer.h"

#include "AudioImporter.h"

#include <cmath>

namespace meridian
{

BounceResult renderBounce (AudioEngine& engine, const BounceRequest& request, const std::function<bool (double)>& progress)
{
    BounceResult result;
    result.file = request.destination;

    const auto length = request.end - request.start;
    if (length <= 0)
    {
        result.error = "Nothing to bounce: the range is empty.";
        return result;
    }

    auto writer = createSessionWriter (request.destination, request.sampleRate, 2, request.bitDepth, request.aiff, result.error);
    if (writer == nullptr)
        return result;

    const int blockSize = juce::jmax (1, engine.getMaxBlockSize());
    const int latency = engine.getTotalLatencySamples(); // compensated plug-in latency + console

    auto& transport = engine.getTransport();
    transport.locate (request.start);
    transport.play();

    juce::AudioBuffer<float> block (2, blockSize);
    double sumSquares[2] = { 0.0, 0.0 };
    juce::int64 skipped = 0, written = 0;
    const juce::int64 total = length + latency;
    bool cancelled = false;

    for (juce::int64 rendered = 0; rendered < total && ! cancelled;)
    {
        const int n = (int) juce::jmin<juce::int64> (blockSize, total - rendered);
        engine.renderOffline (block.getArrayOfWritePointers(), 2, n);
        rendered += n;

        // Drop the master bus's latency from the head of the render.
        const int drop = (int) juce::jlimit<juce::int64> (0, n, latency - skipped);
        skipped += drop;
        const int keep = (int) juce::jmin<juce::int64> (n - drop, length - written);
        if (keep <= 0)
            continue;

        for (int ch = 0; ch < 2; ++ch)
        {
            const float* data = block.getReadPointer (ch, drop);
            auto& stats = ch == 0 ? result.left : result.right;
            for (int i = 0; i < keep; ++i)
            {
                const double x = data[i];
                stats.peak = juce::jmax (stats.peak, std::abs (x));
                sumSquares[ch] += x * x;
            }
        }

        writer->writeFromAudioSampleBuffer (block, drop, keep);
        written += keep;

        if (progress && ! progress ((double) rendered / (double) total))
            cancelled = true;
    }

    transport.stop();
    writer.reset();

    if (cancelled)
    {
        request.destination.deleteFile();
        result.error = "Bounce cancelled.";
        return result;
    }

    result.left.rms = std::sqrt (sumSquares[0] / (double) juce::jmax<juce::int64> (1, written));
    result.right.rms = std::sqrt (sumSquares[1] / (double) juce::jmax<juce::int64> (1, written));
    result.length = written;
    result.ok = true;
    return result;
}

} // namespace meridian
