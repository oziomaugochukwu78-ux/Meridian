#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** One insert slot on a channel strip (A..J).

    Empty slot: click to choose a plug-in.
    Filled slot: click to open its editor; right-click to bypass, replace or
    remove it. A bypassed insert is shown blue; a plug-in that failed to load
    shows its name in red brackets. */
class InsertSlotButton final : public juce::Component,
                               public juce::SettableTooltipClient
{
public:
    InsertSlotButton (AppController&, juce::ValueTree track, int slotIndex);

    void refresh();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void showPluginChooser();
    void showSlotMenu();

    AppController& app;
    juce::ValueTree trackNode;
    const int slot;
};

} // namespace meridian
