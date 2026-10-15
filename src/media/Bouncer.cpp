#include "Bouncer.h"
#include "Mp3Writer.h"

#include "AudioImporter.h"

#include <dsp/Dither.h>
#include <dsp/Loudness.h>

#include <cmath>

namespace meridian
{

std::unique_ptr<juce::AudioFormatWriter> createBounceWriter (const juce::File& file, double sampleRate, int numChannels,
                                                            int bitDepth, const juce::String& format, juce::String& error, int mp3Kbps)
{
    if (format == "mp3")
    {
        file.deleteFile();
        auto writer = createMp3Writer (file.createOutputStream(), sampleRate, numChannels, mp3Kbps, error);
        if (writer == nullptr && error.isEmpty())
            error = "Could not create " + file.getFullPathName() + ".";
        return writer;
    }
    if (format != "flac")
        return createSessionWriter (file, sampleRate, numChannels, bitDepth, format == "aiff", error);

    if (bitDepth != 16 && bitDepth != 24)
    {
        error = "FLAC stores 16 or 24 bits, not " + juce::String (bitDepth) + ".";
        return nullptr;
    }
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> fileStream = file.createOutputStream();
    if (fileStream == nullptr || ! fileStream->openedOk())
    {
        error = "Could not create " + file.getFullPathName() + ".";
        return nullptr;
    }
    std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);
    juce::FlacAudioFormat flac;
    auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                     .withSampleRate (sampleRate)
                                                     .withNumChannels (numChannels)
                                                     .withBitsPerSample (bitDepth));
    if (writer == nullptr)
        error = "Could not write " + juce::String (bitDepth) + "-bit FLAC files.";
    return writer;
}

namespace
{
/** Writes blocks to a file. For 16 and 24 bits Meridian quantises itself
    (rounding to the nearest step, or TPDF dither) and hands the writer exact
    integers: JUCE's own float-to-integer paths differ between formats (its
    FLAC writer truncates), and a bounce must be exact. */
struct FileOutput
{
    std::unique_ptr<juce::AudioFormatWriter> writer;
    int bits = 24;
    bool dither = false;
    bool shaped = false;  ///< with dither: noise shaping
    dsp::Dither ditherLeft { 0x1234567u }, ditherRight { 0x89abcdefu };
    std::vector<int> integers[2];

    void write (juce::AudioBuffer<float>& block, int start, int count)
    {
        if (bits != 16 && bits != 24)
        {
            writer->writeFromAudioSampleBuffer (block, start, count); // 32-bit float: as is
            return;
        }

        // Left-justified 32-bit integers, as AudioFormatWriter::write expects:
        // step k of a `bits` grid is k * 2^(32 - bits), so every format's shift is exact.
        const double steps = std::ldexp (1.0, bits - 1);
        const juce::int64 justify = juce::int64 (1) << (32 - bits);
        const int* channels[3] = { nullptr, nullptr, nullptr };
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& out = integers[ch];
            out.resize ((size_t) count);
            const auto* x = block.getReadPointer (ch, start);
            auto& d = ch == 0 ? ditherLeft : ditherRight;
            for (int i = 0; i < count; ++i)
            {
                const float q = ! dither ? dsp::Dither::round (x[i], bits)
                              : shaped   ? d.quantiseShaped (x[i], bits)
                                         : d.quantise (x[i], bits);
                out[(size_t) i] = (int) ((juce::int64) std::llround ((double) q * steps) * justify);
            }
            channels[ch] = out.data();
        }
        writer->write (channels, count);
    }
};

struct Measurement
{
    dsp::LoudnessMeter loudness;
    dsp::TruePeakMeter truePeak;
    double sumSquares[2] = { 0.0, 0.0 };
    dsp::LevelStats left, right;

    void prepare (double sampleRate)
    {
        loudness.prepare (sampleRate, 2);
        truePeak.prepare (2);
    }

    void add (const juce::AudioBuffer<float>& block, int start, int count)
    {
        const float* channels[2] = { block.getReadPointer (0, start), block.getReadPointer (1, start) };
        loudness.process (channels, 2, count);
        truePeak.process (channels, 2, count);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& stats = ch == 0 ? left : right;
            for (int i = 0; i < count; ++i)
            {
                const double x = channels[ch][i];
                stats.peak = juce::jmax (stats.peak, std::abs (x));
                sumSquares[ch] += x * x;
            }
        }
    }
};
} // namespace

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

    const auto format = request.format.isNotEmpty() ? request.format.toLowerCase() : juce::String (request.aiff ? "aiff" : "wav");
    // Normalising needs the whole mix measured first: render it to a 32-bit float file.
    const auto firstPassFile = request.normalise ? request.destination.getSiblingFile (request.destination.getFileNameWithoutExtension()
                                                                                       + ".unnormalised.wav")
                                                 : request.destination;
    // MP3 takes floats: nothing is quantised or dithered before the encoder.
    const bool mp3 = format == "mp3";
    const int bits = mp3 ? 32 : request.bitDepth;
    FileOutput output;
    output.writer = request.normalise ? createBounceWriter (firstPassFile, request.sampleRate, 2, 32, "wav", result.error)
                                      : createBounceWriter (request.destination, request.sampleRate, 2, bits, format, result.error,
                                                            request.mp3Kbps);
    output.bits = request.normalise ? 32 : bits;
    output.dither = request.dither && ! request.normalise && ! mp3;
    output.shaped = request.noiseShaping;
    if (output.writer == nullptr)
        return result;

    const int blockSize = juce::jmax (1, engine.getMaxBlockSize());
    const int latency = engine.getTotalLatencySamples(); // compensated plug-in latency + console

    auto& transport = engine.getTransport();
    transport.locate (request.start);
    transport.play();

    Measurement measured;
    measured.prepare (request.sampleRate);
    juce::AudioBuffer<float> block (2, blockSize);
    juce::int64 skipped = 0, written = 0;
    const juce::int64 total = length + latency;
    bool cancelled = false;
    const double passes = request.normalise ? 2.0 : 1.0;

    for (juce::int64 rendered = 0; rendered < total && ! cancelled;)
    {
        const int n = (int) std::min<juce::int64> (blockSize, total - rendered);
        engine.renderOffline (block.getArrayOfWritePointers(), 2, n);
        rendered += n;

        // Drop the master bus's latency from the head of the render.
        const int drop = (int) juce::jlimit<juce::int64> (0, n, latency - skipped);
        skipped += drop;
        const int keep = (int) std::min<juce::int64> (n - drop, length - written);
        if (keep <= 0)
            continue;

        measured.add (block, drop, keep); // before dither: what the mix is
        output.write (block, drop, keep);
        written += keep;

        if (progress && ! progress ((double) rendered / (double) total / passes))
            cancelled = true;
    }

    transport.stop();
    output.writer.reset();

    auto fail = [&] (const juce::String& message)
    {
        firstPassFile.deleteFile();
        request.destination.deleteFile();
        result.error = message;
        return result;
    };
    if (cancelled)
        return fail ("Bounce cancelled.");

    result.left = measured.left;
    result.right = measured.right;
    result.left.rms = std::sqrt (measured.sumSquares[0] / (double) std::max<juce::int64> (1, written));
    result.right.rms = std::sqrt (measured.sumSquares[1] / (double) std::max<juce::int64> (1, written));
    result.integratedLufs = measured.loudness.getIntegrated();
    result.loudnessRange = measured.loudness.getLoudnessRange();
    result.truePeakDbtp = measured.truePeak.getTruePeakDb();
    result.length = written;

    if (request.normalise)
    {
        if (result.integratedLufs <= dsp::LoudnessMeter::absoluteGate)
            return fail ("The mix is silent (below -70 LUFS), so it cannot be normalised.");

        // Gain to the target, but never past the true-peak ceiling. Loudness and
        // true peak both move by exactly the gain, so the result is known now.
        double gainDb = request.targetLufs - result.integratedLufs;
        if (result.truePeakDbtp + gainDb > request.ceilingDbtp)
        {
            gainDb = request.ceilingDbtp - result.truePeakDbtp;
            result.limitedByCeiling = true;
        }
        const auto gain = (float) std::pow (10.0, gainDb / 20.0);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (firstPassFile));
        FileOutput finalOutput;
        finalOutput.writer = createBounceWriter (request.destination, request.sampleRate, 2, bits, format, result.error,
                                                 request.mp3Kbps);
        finalOutput.bits = bits;
        finalOutput.dither = request.dither && ! mp3;
        finalOutput.shaped = request.noiseShaping;
        if (reader == nullptr || finalOutput.writer == nullptr)
            return fail (result.error.isNotEmpty() ? result.error : juce::String ("Could not read the first pass back."));

        for (juce::int64 pos = 0; pos < written && ! cancelled; pos += blockSize)
        {
            const int n = (int) std::min<juce::int64> (blockSize, written - pos);
            reader->read (&block, 0, n, pos, true, true);
            block.applyGain (0, n, gain);
            finalOutput.write (block, 0, n);
            if (progress && ! progress (0.5 + 0.5 * (double) (pos + n) / (double) written))
                cancelled = true;
        }
        finalOutput.writer.reset();
        reader.reset();
        firstPassFile.deleteFile();
        if (cancelled)
            return fail ("Bounce cancelled.");

        result.appliedGainDb = gainDb;
        result.integratedLufs += gainDb;
        result.truePeakDbtp += gainDb;
        for (auto* stats : { &result.left, &result.right })
        {
            stats->peak *= gain;
            stats->rms *= gain;
        }
    }

    result.ok = true;
    return result;
}

} // namespace meridian
