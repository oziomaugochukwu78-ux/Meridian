#pragma once

#include "EditState.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Edit-window toolbar: edit modes (Shuffle / Slip / Spot / Grid), the edit
    tools (Zoomer, Trim, Selector, Grabber, Smart Tool, Pencil) and zoom. */
class EditToolbar final : public juce::Component,
                          private juce::ChangeListener
{
public:
    explicit EditToolbar (EditState&);
    ~EditToolbar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();

    EditState& state;
    juce::OwnedArray<juce::TextButton> modeButtons, toolButtons;
    juce::TextButton zoomOut { "-" }, zoomIn { "+" };
    juce::Label gridLabel;
};

} // namespace meridian
