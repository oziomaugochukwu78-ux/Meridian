#include "MasterBusProcessor.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

MasterBusProcessor::MasterBusProcessor()
    : oversampler (2, 2, juce::dsp::Oversampling<double>::filterHalfBandFIREquiripple,
                   true,  // maximum-quality filters
                   true)  // integer latency, so the dry path can be aligned exactly
{
}

void MasterBusProcessor::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax (1, maxBlockSize);

    oversampler.initProcessing ((size_t) maxBlock);
    oversampler.reset();
    latency = (int) std::lround (oversampler.getLatencyInSamples());

    for (int ch = 0; ch < 2; ++ch)
    {
        delayLines[ch].assign ((size_t) juce::jmax (1, latency), 0.0);
        dryScratch[ch].assign ((size_t) maxBlock, 0.0);
        dcBlockers[ch].prepare (sampleRate);
    }
    wetGains.assign ((size_t) maxBlock, 0.0);
    delayWrite = 0;

    wetRamp.reset (consoleEnabled.load() ? 1.0 : 0.0);
    oversamplerHot = false;
    appliedDrive = -1.0;
    appliedCharacter = -1.0;
    appliedCurve = -1;
    updateCurveIfChanged();
}

void MasterBusProcessor::updateCurveIfChanged() noexcept
{
    const double drive = driveDb.load (std::memory_order_relaxed);
    const double asym = character.load (std::memory_order_relaxed);
    const int type = curveType.load (std::memory_order_relaxed);

    if (type != appliedCurve)
    {
        curve.setType (type == 1 ? dsp::SaturationCurve::Type::cubic : dsp::SaturationCurve::Type::tanh);
        appliedCurve = type;
    }
    if (drive != appliedDrive)
    {
        curve.setDriveDecibels (drive);
        appliedDrive = drive;
    }
    if (asym != appliedCharacter)
    {
        curve.setAsymmetry (asym);
        appliedCharacter = asym;
    }
}

void MasterBusProcessor::process (double* left, double* right, int numSamples) noexcept
{
    jassert (numSamples <= maxBlock);
    double* io[2] = { left, right };

    updateCurveIfChanged();

    // Dry path, delayed by the oversampler's latency.
    for (int ch = 0; ch < 2; ++ch)
    {
        double* dry = dryScratch[ch].data();
        if (latency == 0)
        {
            std::copy (io[ch], io[ch] + numSamples, dry);
            continue;
        }

        auto& line = delayLines[ch];
        int w = delayWrite;
        for (int i = 0; i < numSamples; ++i)
        {
            dry[i] = line[(size_t) w];
            line[(size_t) w] = io[ch][i];
            if (++w == latency)
                w = 0;
        }
    }
    if (latency > 0)
        delayWrite = (delayWrite + numSamples) % latency;

    wetRamp.setTarget (consoleEnabled.load (std::memory_order_relaxed) ? 1.0 : 0.0, (int) (0.05 * sampleRate));

    if (! wetRamp.isRamping() && wetRamp.getTarget() == 0.0)
    {
        // Console fully bypassed: output the aligned dry signal and let the
        // filters go cold, so re-engaging starts from a clean state.
        if (oversamplerHot)
        {
            oversampler.reset();
            dcBlockers[0].reset();
            dcBlockers[1].reset();
            oversamplerHot = false;
        }
        for (int ch = 0; ch < 2; ++ch)
            std::copy (dryScratch[ch].data(), dryScratch[ch].data() + numSamples, io[ch]);
        return;
    }

    oversamplerHot = true;

    juce::dsp::AudioBlock<double> block (io, 2, (size_t) numSamples);
    auto upsampled = oversampler.processSamplesUp (block);
    for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
        curve.processBlock (upsampled.getChannelPointer (ch), (int) upsampled.getNumSamples());
    oversampler.processSamplesDown (block);

    dcBlockers[0].process (left, numSamples);
    dcBlockers[1].process (right, numSamples);

    if (! wetRamp.isRamping())
        return; // fully engaged

    wetRamp.render (wetGains.data(), numSamples);
    for (int ch = 0; ch < 2; ++ch)
    {
        const double* dry = dryScratch[ch].data();
        double* out = io[ch];
        for (int i = 0; i < numSamples; ++i)
            out[i] = dry[i] + wetGains[(size_t) i] * (out[i] - dry[i]);
    }
}

} // namespace meridian
