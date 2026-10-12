#include "AudioImporter.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

namespace meridian
{

std::unique_ptr<juce::AudioFormatWriter> createSessionWriter (const juce::File& file, double sampleRate, int numChannels,
                                                              int bitDepth, bool aiff, juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> fileStream = file.createOutputStream();
    if (fileStream == nullptr || ! fileStream->openedOk())
    {
        error = "Could not create " + file.getFullPathName() + ".";
        return nullptr;
    }
    std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);

    auto options = juce::AudioFormatWriterOptions{}
                       .withSampleRate (sampleRate)
                       .withNumChannels (numChannels)
                       .withBitsPerSample (bitDepth);
    if (bitDepth == 32)
        options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiffFormat;
    juce::AudioFormat& format = aiff ? static_cast<juce::AudioFormat&> (aiffFormat) : wav;

    auto writer = format.createWriterFor (stream, options);
    if (writer == nullptr)
        error = "Could not write " + juce::String (bitDepth) + "-bit " + format.getFormatName() + " files.";
    return writer;
}

ImportResult importAudioFile (const juce::File& source, const juce::File& destination,
                              double sessionSampleRate, int bitDepth, bool aiff,
                              juce::AudioFormatManager& formatManager, const std::function<bool (double)>& progress)
{
    ImportResult result;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (source));
    if (reader == nullptr)
    {
        result.error = source.getFileName() + " is not an audio file this application can read.";
        return result;
    }
    if (reader->lengthInSamples <= 0)
    {
        result.error = source.getFileName() + " contains no audio.";
        return result;
    }

    const int numChannels = juce::jlimit (1, 2, (int) reader->numChannels);
    auto writer = createSessionWriter (destination, sessionSampleRate, numChannels, bitDepth, aiff, result.error);
    if (writer == nullptr)
        return result;

    constexpr int blockSize = 16384;
    const double ratio = reader->sampleRate / sessionSampleRate; // input samples per output sample
    const juce::int64 outputLength = (juce::int64) std::ceil ((double) reader->lengthInSamples / ratio);

    juce::AudioBuffer<float> block (numChannels, blockSize);
    bool cancelled = false;

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        for (juce::int64 pos = 0; pos < reader->lengthInSamples && ! cancelled; pos += blockSize)
        {
            const int n = (int) juce::jmin<juce::int64> (blockSize, reader->lengthInSamples - pos);
            reader->read (block.getArrayOfWritePointers(), numChannels, pos, n);
            writer->writeFromAudioSampleBuffer (block, 0, n);
            cancelled = progress && ! progress ((double) (pos + n) / (double) reader->lengthInSamples);
        }
    }
    else
    {
        // Anti-alias filter, applied once to each input sample as it enters the FIFO.
        using Biquad = juce::dsp::IIR::Filter<float>;
        std::vector<std::vector<std::unique_ptr<Biquad>>> filters ((size_t) numChannels);
        if (ratio > 1.0)
        {
            const auto designs = juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                (float) (0.45 * sessionSampleRate), reader->sampleRate, 16);
            for (auto& chain : filters)
                for (auto* coefficients : designs)
                    chain.push_back (std::make_unique<Biquad> (coefficients));
        }

        std::vector<juce::WindowedSincInterpolator> interpolators ((size_t) numChannels);
        std::vector<std::vector<float>> fifo ((size_t) numChannels);
        size_t fifoRead = 0;
        juce::int64 readerPosition = 0;

        auto refill = [&] (size_t minimumAvailable)
        {
            while (fifo[0].size() - fifoRead < minimumAvailable)
            {
                // Reading past the end yields silence, which flushes the filters.
                reader->read (block.getArrayOfWritePointers(), numChannels, readerPosition, blockSize);
                readerPosition += blockSize;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    float* data = block.getWritePointer (ch);
                    for (auto& stage : filters[(size_t) ch])
                        for (int i = 0; i < blockSize; ++i)
                            data[i] = stage->processSample (data[i]);
                    fifo[(size_t) ch].insert (fifo[(size_t) ch].end(), data, data + blockSize);
                }
            }

            if (fifoRead > (size_t) (4 * blockSize))
            {
                for (auto& f : fifo)
                    f.erase (f.begin(), f.begin() + (std::ptrdiff_t) fifoRead);
                fifoRead = 0;
            }
        };

        // The interpolator delays its output by its base latency (in input
        // samples); render that much extra and drop it from the front.
        const auto skip = (juce::int64) std::lround (juce::WindowedSincInterpolator::getBaseLatency() / ratio);
        juce::AudioBuffer<float> out (numChannels, blockSize);
        juce::int64 produced = 0;
        const juce::int64 totalToProduce = outputLength + skip;

        while (produced < totalToProduce && ! cancelled)
        {
            const int numOut = (int) juce::jmin<juce::int64> (blockSize, totalToProduce - produced);
            refill ((size_t) std::ceil (numOut * ratio) + 8);

            int used = 0;
            for (int ch = 0; ch < numChannels; ++ch)
                used = interpolators[(size_t) ch].process (ratio, fifo[(size_t) ch].data() + fifoRead,
                                                            out.getWritePointer (ch), numOut);
            fifoRead += (size_t) used;

            const juce::int64 dropHere = juce::jlimit<juce::int64> (0, numOut, skip - produced);
            if (numOut - dropHere > 0)
                writer->writeFromAudioSampleBuffer (out, (int) dropHere, numOut - (int) dropHere);

            produced += numOut;
            cancelled = progress && ! progress ((double) produced / (double) totalToProduce);
        }
    }

    writer.reset();

    if (cancelled)
    {
        destination.deleteFile();
        result.error = "Import cancelled.";
        return result;
    }

    result.ok = true;
    result.length = outputLength;
    result.numChannels = numChannels;
    return result;
}

} // namespace meridian
