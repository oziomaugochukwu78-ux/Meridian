#include "PluginHost.h"

#include "BuiltinEffects.h"

#include "ui/Theme.h"

namespace meridian
{
namespace
{
constexpr const char* knownPluginsKey = "knownPlugins";

class PluginManagerWindow final : public juce::DocumentWindow
{
public:
    PluginManagerWindow (PluginHost& host, juce::PropertiesFile& settings, std::function<void()> onClose)
        : juce::DocumentWindow ("Plug-in Manager", theme::panel, juce::DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new juce::PluginListComponent (host.getFormatManager(), host.getKnownPlugins(),
                                                        host.getDeadMansPedalFile(), &settings, true),
                         false);
        setResizable (true, false);
        centreWithSize (760, 520);
        setVisible (true);
    }

    void closeButtonPressed() override { closeCallback(); }

private:
    std::function<void()> closeCallback;
};
} // namespace

PluginHost::PluginHost (juce::PropertiesFile& s) : settings (s)
{
    juce::addDefaultFormatsToManager (formatManager);
    formatManager.addFormat (std::make_unique<BuiltinEffectFormat>());

    if (auto xml = settings.getXmlValue (knownPluginsKey))
        knownPlugins.recreateFromXml (*xml);

    // Meridian's own effects are always available, without a scan.
    for (const auto& d : BuiltinEffectFormat::getDescriptions())
        if (knownPlugins.getTypeForIdentifierString (d.createIdentifierString()) == nullptr)
            knownPlugins.addType (d);

    knownPlugins.addChangeListener (this);
}

PluginHost::~PluginHost()
{
    knownPlugins.removeChangeListener (this);
    managerWindow.reset();
}

juce::File PluginHost::getDeadMansPedalFile() const
{
    return settings.getFile().getSiblingFile ("PluginScanCrashes.txt");
}

void PluginHost::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = knownPlugins.createXml())
    {
        settings.setValue (knownPluginsKey, xml.get());
        settings.saveIfNeeded();
    }
}

void PluginHost::showPluginManager()
{
    if (managerWindow == nullptr)
        managerWindow = std::make_unique<PluginManagerWindow> (*this, settings, [this] {
            // Defer: we are inside the window's own callback.
            juce::MessageManager::callAsync ([this] { managerWindow.reset(); });
        });

    managerWindow->toFront (true);
}

juce::Array<juce::PluginDescription> PluginHost::getEffectPlugins() const
{
    juce::Array<juce::PluginDescription> effects;
    for (const auto& d : knownPlugins.getTypes())
        if (! d.isInstrument)
            effects.add (d);
    return effects;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description,
                                                                      int numChannels, double sampleRate,
                                                                      int blockSize, juce::String& error)
{
    auto instance = formatManager.createPluginInstance (description, sampleRate, blockSize, error);
    if (instance == nullptr)
    {
        if (error.isEmpty())
            error = "Could not load " + description.name + ".";
        return nullptr;
    }

    auto layout = instance->getBusesLayout();
    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
    {
        error = description.name + " has no audio input, so it cannot be used as an insert on an audio track.";
        return nullptr;
    }

    const auto set = numChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    layout.inputBuses.getReference (0) = set;
    layout.outputBuses.getReference (0) = set;
    for (int i = 1; i < layout.inputBuses.size(); ++i)
        layout.inputBuses.getReference (i) = juce::AudioChannelSet::disabled();
    for (int i = 1; i < layout.outputBuses.size(); ++i)
        layout.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();

    if (! instance->setBusesLayout (layout))
    {
        error = description.name + " does not support " + (numChannels == 1 ? "mono" : "stereo")
                + " operation, which this track needs.";
        return nullptr;
    }

    instance->prepareToPlay (sampleRate, blockSize);
    return instance;
}

} // namespace meridian
