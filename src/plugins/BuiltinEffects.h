#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace meridian
{

/** Meridian's own effects (Meridian EQ, Meridian Reverb), hosted through the
    normal plug-in path: they appear in the plug-in list, go in insert slots,
    bypass, save their settings in the session, compensate latency and open a
    window exactly like third-party plug-ins. The signal processing lives in
    the core library (dsp::ParametricEq, dsp::FdnReverb), where it is measured
    by unit tests.

    Identifiers are "meridian:eq" and "meridian:reverb"; the format is named
    "Built-in". */
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
