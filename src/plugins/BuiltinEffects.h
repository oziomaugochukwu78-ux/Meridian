#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace meridian
{

/** Implemented by Meridian's dynamics (Compressor, Limiter, Gate, De-Esser): how far the
    effect is turning the signal down, for the meter in its window and the AI tools.
    Written by the audio thread at the end of each block; read from any thread. */
class GainReductionMeter
{
public:
    virtual ~GainReductionMeter() = default;

    /** dB, 0 or positive, as of the end of the last processed block. */
    float getGainReductionDb() const noexcept { return reduction.load (std::memory_order_relaxed); }

protected:
    void reportGainReduction (double db) noexcept { reduction.store (db > 0.0 ? (float) db : 0.0f, std::memory_order_relaxed); }

private:
    std::atomic<float> reduction { 0.0f };
};

/** Meridian's own effects (EQ, Compressor, Limiter, Gate, De-Esser, Delay, Reverb), hosted through
    the normal plug-in path: they appear in the plug-in list, go in insert
    slots, bypass, save their settings in the session, compensate latency and
    open a window exactly like third-party plug-ins. The signal processing
    lives in the core library (dsp::ParametricEq, dsp::Compressor,
    dsp::Limiter, dsp::FdnReverb), where it is measured by unit tests.

    Identifiers are "meridian:eq", "meridian:compressor", "meridian:limiter",
    "meridian:gate", "meridian:deesser", "meridian:delay" and "meridian:reverb";
    the format is named "Built-in". */
class BuiltinEffectFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "Built-in";

    /** Descriptions of every built-in effect (added to the known-plug-in list at start-up). */
    static juce::Array<juce::PluginDescription> getDescriptions();

    juce::String getName() const override { return formatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& identifier) override;
    bool fileMightContainThisPluginType (const juce::String& identifier) override { return identifier.startsWith ("meridian:"); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& identifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate,
                               int initialBufferSize, PluginCreationCallback) override;
};

} // namespace meridian
