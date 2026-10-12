#pragma once

#include <dsp/DcBlocker.h>
#include <dsp/LinearRamp.h>
#include <dsp/SaturationCurve.h>

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <vector>

namespace meridian
{

/** The master bus "console": analog-style saturation in double precision.

    Signal flow, per block:

        sum ──┬── 4x oversample ── SaturationCurve ── downsample ── DC block ──┐
              │                                                                 ├── crossfade ── out
              └── delay (= oversampler latency) ───────────────────────────────┘

    The oversampler uses linear-phase FIR half-band filters, so the dry path
    can be time-aligned with a plain integer delay. That makes switching the
    console on and off a click-free, phase-coherent crossfade, and the bus
    latency the same whether the console is engaged or not.

    Parameters are atomics set from the message thread; process() runs on the
    audio thread. prepare() must not run concurrently with process(). */
class MasterBusProcessor
{
public:
    MasterBusProcessor();

    void prepare (double sampleRate, int maxBlockSize);

    /** Processes two channels of double audio in place. */
    void process (double* left, double* right, int numSamples) noexcept;

    /** Latency added by the bus, in samples at the session rate. */
    int getLatencySamples() const noexcept { return latency; }

    // Message thread -> audio thread
    std::atomic<bool> consoleEnabled { true };
    std::atomic<double> driveDb { 6.0 };      ///< 0 .. 24
    std::atomic<double> character { 0.35 };   ///< asymmetry 0 .. 1
    std::atomic<int> curveType { 0 };         ///< 0 = tanh, 1 = cubic

private:
    void updateCurveIfChanged() noexcept;

    juce::dsp::Oversampling<double> oversampler;
    dsp::SaturationCurve curve;
    dsp::DcBlocker dcBlockers[2];
    dsp::LinearRamp wetRamp;

    // Dry-path delay lines (ring buffers), one per channel.
    std::vector<double> delayLines[2];
    std::vector<double> dryScratch[2];
    std::vector<double> wetGains;
    int delayWrite = 0;
    int latency = 0;
    int maxBlock = 0;
    double sampleRate = 48000.0;
    bool oversamplerHot = false;

    double appliedDrive = -1.0;
    double appliedCharacter = -1.0;
    int appliedCurve = -1;
};

} // namespace meridian
