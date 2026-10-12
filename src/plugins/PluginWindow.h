#pragma once

#include "engine/RenderGraph.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace meridian
{

/** A floating window showing a plug-in's editor (or a generic parameter
    editor when the plug-in has none). One window per insert; opening it again
    brings it to the front. The window closes itself when its insert is
    removed, so an editor never outlives its plug-in. */
class PluginWindow final : public juce::DocumentWindow,
                           private juce::Timer
{
public:
    static void open (std::shared_ptr<InsertSlot> slot, const juce::String& title);
    static void closeAll();

    ~PluginWindow() override;
    void closeButtonPressed() override;

private:
    PluginWindow (std::shared_ptr<InsertSlot>, const juce::String& title);
    void timerCallback() override;

    std::shared_ptr<InsertSlot> slot;

    static juce::OwnedArray<PluginWindow>& openWindows();
};

} // namespace meridian
