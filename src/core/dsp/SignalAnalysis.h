#pragma once

// Measurements used to verify rendered audio objectively (self-test, bounce
// reports): levels, tone amplitude and frequency. Not used on the audio thread.

namespace meridian::dsp
{

struct LevelStats
{
    double peak = 0.0; ///< largest |x|
    double rms = 0.0;
};

LevelStats measureLevels (const float* samples, int numSamples) noexcept;

/** 20*log10(gain), floored at -200 dB so silence is still a number. */
double gainToDb (double gain) noexcept;

/** Amplitude (peak, linear) of the sinusoidal component at `frequency`
    (Goertzel). Exact when the block holds a whole number of cycles. */
double toneAmplitude (const float* samples, int numSamples, double frequency, double sampleRate) noexcept;

/** Fundamental frequency from interpolated rising zero crossings.
    Returns 0 if fewer than two crossings are found. */
double estimateFrequency (const float* samples, int numSamples, double sampleRate) noexcept;

} // namespace meridian::dsp
