#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

namespace meridian
{

/** Third-party plugin hosting: format registry, scanned-plugin database and
    instantiation into a track's channel layout.

    Formats: VST3 and LV2 everywhere, Audio Units on macOS (via JUCE).
    CLAP is not hosted by JUCE; adding it means an AudioPluginFormat subclass
    built on the CLAP SDK, registered in the constructor alongside the others. */
class PluginHost final : private juce::ChangeListener
{
public:
    explicit PluginHost (juce::PropertiesFile& settings);
    ~PluginHost() override;

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() noexcept          { return knownPlugins; }

    /** Records plugins that crashed during a scan so they are skipped next time. */
    juce::File getDeadMansPedalFile() const;

    /** Opens (or brings forward) the Plug-in Manager window, where plugins are
        scanned and the list is managed. */
    void showPluginManager();

    /** Effects only, sorted by manufacturer, for building an insert menu. */
    juce::Array<juce::PluginDescription> getEffectPlugins() const;

    /** Creates an instance configured for numChannels in / numChannels out
        (aux and sidechain buses disabled), prepared for playback. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&, int numChannels,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::PropertiesFile& settings;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::DocumentWindow> managerWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace meridian
