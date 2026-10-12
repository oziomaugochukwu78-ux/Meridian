#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** One send slot (A..E) on a channel strip.

    Empty: click to choose a destination bus (or create a new Aux track).
    Assigned: shows the bus name and a level bar; click to open the send
    panel (level, pan, pre/post-fader, mute, remove). */
class SendSlotButton final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    SendSlotButton (AppController&, juce::ValueTree track, int slotIndex);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void chooseTarget();
    void openPanel();

    AppController& app;
    juce::ValueTree trackNode;
    const int slot;
};

/** Pops up a menu of buses `track` may feed (no cycles) plus "New Aux Track".
    `onChosen` receives the chosen aux track id (a new aux is created first). */
void showBusMenu (AppController&, juce::Component& target, const juce::ValueTree& track, bool includeMaster,
                  std::function<void (int auxTrackId)> onChosen);

} // namespace meridian
