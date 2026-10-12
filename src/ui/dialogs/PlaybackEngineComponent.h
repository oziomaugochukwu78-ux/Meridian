#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Setup > Playback Engine.

    Playback Engine (driver + device), Input Device (for drivers with
    separate inputs), H/W Buffer Size, Sample Rate, the Host Engine options,
    Delay Compensation Engine, and Disk Playback cache size. Changes apply
    immediately; options not implemented yet are shown disabled and say so. */
class PlaybackEngineComponent final : public juce::Component,
                                      private juce::ChangeListener
{
public:
    static void show (AppController&);

    explicit PlaybackEngineComponent (AppController&);
    ~PlaybackEngineComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct DeviceChoice
    {
        juce::String typeName;
        juce::String deviceName;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void populateEngines();
    void refresh();
    void applyEngine();
    void applyInput();
    void applyBufferSize();
    void applySampleRate();
    void applyCacheSize();
    void reportError (const juce::String&);

    AppController& app;
    std::vector<DeviceChoice> choices;

    juce::Label engineLabel, inputLabel, bufferLabel, rateLabel, hostHeader, delayLabel, diskHeader, cacheLabel, info, error;
    juce::ComboBox engine, input, bufferSize, sampleRate, delayCompensation, cacheSize;
    juce::ToggleButton ignoreErrors { "Ignore Errors During Playback/Record" },
                       minimizeLatency { "Minimize Additional I/O Latency" },
                       dynamicPlugins { "Dynamic Plug-In Processing" };
    juce::TextButton controlPanelButton { "Control Panel..." }, okButton { "OK" };
};

} // namespace meridian
