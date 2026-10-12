#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Top bar shared by both windows: transport keys, the main and sub counters,
    Edit/Mix window switches, and status (CPU, disk, AI remote, background jobs). */
class TransportBar final : public juce::Component,
                           private juce::Timer
{
public:
    TransportBar (AppController&, std::function<void (WindowKind)> showWindow, std::function<WindowKind()> currentWindow);
    ~TransportBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class TransportButton;

    void timerCallback() override;

    AppController& app;
    std::function<void (WindowKind)> showWindow;
    std::function<WindowKind()> currentWindow;

    std::unique_ptr<TransportButton> rtzButton, stopButton, playButton, recordButton;
    juce::TextButton editButton { "EDIT" }, mixButton { "MIX" };

    juce::String mainCounter, subCounter, statusText;
    juce::Rectangle<int> counterArea, statusArea;
    float cpuLoad = 0.0f;
    int diskWarningFrames = 0;
};

} // namespace meridian
