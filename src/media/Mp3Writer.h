#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace meridian
{

/** True if this build can write MP3 (LAME is compiled in). */
bool canWriteMp3();

/** An MP3 writer (LAME, constant bitrate, joint stereo, quality 2 of 0-9).
    It takes float samples, so nothing is quantised or dithered before the
    encoder. On close it writes the LAME/Xing header into the first frame,
    giving players the exact length and the encoder delay (gapless playback).

    Sample rates of 32, 44.1 and 48 kHz are kept; others are resampled by
    LAME to one of those. Returns null with `error` set if it cannot start. */
std::unique_ptr<juce::AudioFormatWriter> createMp3Writer (std::unique_ptr<juce::FileOutputStream>, double sampleRate,
                                                         int numChannels, int kbps, juce::String& error);

} // namespace meridian
