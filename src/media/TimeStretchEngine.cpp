#include "TimeStretchEngine.h"

#include "AudioImporter.h"

#if MERIDIAN_WITH_RUBBERBAND
 #include <rubberband/RubberBandStretcher.h>
#endif

#include <cmath>

namespace meridian
{

bool isTimeStretchAvailable() noexcept
{
    return MERIDIAN_WITH_RUBBERBAND != 0;
}

juce::String timeStretchEngineName()
{
#if MERIDIAN_WITH_RUBBERBAND
    return "Rubber Band Library (R3 finer engine)";
#else
    return "unavailable (built without Rubber Band)";
#endif
}

TimeStretchResult renderTimeStretch (const TimeStretchRequest& request, juce::AudioFormatManager& formatManager,
                                     const std::function<bool (double)>& progress)
{
    TimeStretchResult result;

#if ! MERIDIAN_WITH_RUBBERBAND
    juce::ignoreUnused (request, formatManager, progress);
    result.error = "This build has no time-stretching engine. Rebuild with MERIDIAN_WITH_RUBBERBAND=ON.";
    return result;
#else
    if (request.timeRatio < 0.125 || request.timeRatio > 8.0)
    {
        result.error = "Time ratio must be between 12.5 % and 800 %.";
        return result;
    }
    if (std::abs (request.pitchSemitones) > 24.0)
    {
        result.error = "Pitch shift must be within two octaves.";
        return result;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (request.source));
    if (reader == nullptr)
    {
        result.error = "Could not read " + request.source.getFileName() + ".";
        return result;
    }

    const int numChannels = juce::jlimit (1, 2, (int) reader->numChannels);
    const auto start = juce::jlimit<juce::int64> (0, reader->lengthInSamples, request.sourceStart);
    const auto length = juce::jlimit<juce::int64> (0, reader->lengthInSamples - start, request.sourceLength);
    if (length <= 0)
    {
        result.error = "The clip has no audio to process.";
        return result;
    }

    auto writer = createSessionWriter (request.destination, request.sampleRate, numChannels,
                                       request.bitDepth, request.aiff, result.error);
    if (writer == nullptr)
        return result;

    using RB = RubberBand::RubberBandStretcher;
    int options = RB::OptionProcessOffline | RB::OptionEngineFiner;
    if (request.preserveFormants)
        options |= RB::OptionFormantPreserved;
    if (numChannels == 2)
        options |= RB::OptionChannelsTogether; // keep the stereo image coherent

    RB stretcher ((size_t) reader->sampleRate, (size_t) numChannels, options,
                  request.timeRatio, std::pow (2.0, request.pitchSemitones / 12.0));

    constexpr int blockSize = 8192;
    stretcher.setExpectedInputDuration ((size_t) length);
    stretcher.setMaxProcessSize (blockSize);

    juce::AudioBuffer<float> input (numChannels, blockSize);
    juce::AudioBuffer<float> output (numChannels, blockSize);
    const double expectedOutput = (double) length * request.timeRatio;
    juce::int64 written = 0;

    auto report = [&] (double fraction) { return ! progress || progress (juce::jlimit (0.0, 1.0, fraction)); };

    auto drain = [&]
    {
        for (;;)
        {
            const int available = stretcher.available();
            if (available <= 0)
                return;
            const auto got = stretcher.retrieve (output.getArrayOfWritePointers(),
                                                 (size_t) juce::jmin (available, blockSize));
            writer->writeFromAudioSampleBuffer (output, 0, (int) got);
            written += (juce::int64) got;
        }
    };

    // Pass 1: study the whole clip (gives the offline engine its look-ahead).
    for (juce::int64 pos = 0; pos < length; pos += blockSize)
    {
        const int n = (int) std::min<juce::int64> (blockSize, length - pos);
        reader->read (input.getArrayOfWritePointers(), numChannels, start + pos, n);
        stretcher.study (input.getArrayOfReadPointers(), (size_t) n, pos + n >= length);
        if (! report (0.3 * (double) (pos + n) / (double) length))
        {
            result.error = "Cancelled.";
            break;
        }
    }

    // Pass 2: process and collect.
    for (juce::int64 pos = 0; pos < length && result.error.isEmpty(); pos += blockSize)
    {
        const int n = (int) std::min<juce::int64> (blockSize, length - pos);
        reader->read (input.getArrayOfWritePointers(), numChannels, start + pos, n);
        stretcher.process (input.getArrayOfReadPointers(), (size_t) n, pos + n >= length);
        drain();
        if (! report (0.3 + 0.7 * (double) written / juce::jmax (1.0, expectedOutput)))
            result.error = "Cancelled.";
    }
    drain();

    writer.reset();

    if (result.error.isNotEmpty())
    {
        request.destination.deleteFile();
        return result;
    }

    result.ok = true;
    result.length = written;
    return result;
#endif
}

} // namespace meridian
