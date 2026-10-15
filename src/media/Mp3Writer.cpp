#include "Mp3Writer.h"

#if MERIDIAN_WITH_LAME
 #include <lame.h>
#endif

namespace meridian
{

#if MERIDIAN_WITH_LAME

namespace
{
class Mp3Writer final : public juce::AudioFormatWriter
{
public:
    Mp3Writer (juce::OutputStream* out, double rate, int channels, lame_t encoder)
        : juce::AudioFormatWriter (out, "MP3 file", rate, (unsigned int) channels, 32), lame (encoder)
    {
        usesFloatingPointData = true; // write() receives floats
    }

    ~Mp3Writer() override
    {
        // The encoder's last frames, then the header frame it reserved at the start.
        bytes.resize (7200);
        const int tail = lame_encode_flush (lame, bytes.data(), (int) bytes.size());
        if (tail > 0)
            output->write (bytes.data(), (size_t) tail);
        bytes.resize (8192);
        const auto header = lame_get_lametag_frame (lame, bytes.data(), bytes.size());
        if (header > 0 && header <= bytes.size())
        {
            const auto end = output->getPosition();
            if (output->setPosition (0))
            {
                output->write (bytes.data(), header);
                output->setPosition (end);
            }
        }
        output->flush();
        lame_close (lame);
    }

    bool write (const int** samples, int numSamples) override
    {
        if (numSamples <= 0)
            return true;
        const auto* left = reinterpret_cast<const float*> (samples[0]);
        const auto* right = numChannels > 1 && samples[1] != nullptr ? reinterpret_cast<const float*> (samples[1]) : left;
        if (left == nullptr)
            return false;
        // LAME's worst case: 1.25 x samples + 7200 bytes.
        bytes.resize ((size_t) numSamples + (size_t) numSamples / 4 + 7200);
        const int written = lame_encode_buffer_ieee_float (lame, left, right, numSamples, bytes.data(), (int) bytes.size());
        return written >= 0 && (written == 0 || output->write (bytes.data(), (size_t) written));
    }

private:
    lame_t lame;
    std::vector<unsigned char> bytes;
};
} // namespace

bool canWriteMp3() { return true; }

std::unique_ptr<juce::AudioFormatWriter> createMp3Writer (std::unique_ptr<juce::FileOutputStream> stream, double sampleRate,
                                                         int numChannels, int kbps, juce::String& error)
{
    if (stream == nullptr || stream->failedToOpen())
    {
        error = "Could not create the MP3 file.";
        return nullptr;
    }
    stream->setPosition (0);
    stream->truncate();

    auto* lame = lame_init();
    if (lame == nullptr)
    {
        error = "The MP3 encoder could not start.";
        return nullptr;
    }
    const auto rate = (int) std::lround (sampleRate);
    lame_set_in_samplerate (lame, rate);
    lame_set_out_samplerate (lame, rate == 32000 || rate == 44100 || rate == 48000 ? rate : 0); // 0: LAME picks
    lame_set_num_channels (lame, juce::jlimit (1, 2, numChannels));
    lame_set_mode (lame, numChannels > 1 ? JOINT_STEREO : MONO);
    lame_set_VBR (lame, vbr_off);
    lame_set_brate (lame, juce::jlimit (32, 320, kbps));
    lame_set_quality (lame, 2);
    lame_set_bWriteVbrTag (lame, 1);
    if (lame_init_params (lame) < 0)
    {
        lame_close (lame);
        error = "The MP3 encoder does not accept " + juce::String (rate) + " Hz at " + juce::String (kbps) + " kbps.";
        return nullptr;
    }
    return std::make_unique<Mp3Writer> (stream.release(), sampleRate, juce::jlimit (1, 2, numChannels), lame);
}

#else

bool canWriteMp3() { return false; }

std::unique_ptr<juce::AudioFormatWriter> createMp3Writer (std::unique_ptr<juce::FileOutputStream>, double, int, int, juce::String& error)
{
    error = "This build of Meridian has no MP3 encoder (built without LAME).";
    return nullptr;
}

#endif

} // namespace meridian
