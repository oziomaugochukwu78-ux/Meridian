#pragma once

#include "InsertSlotButton.h"

#include "app/AppController.h"
#include "ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Vertical console strip for one track, top to bottom:

      INSERTS A-E / F-J   ten plug-in slots
      SENDS A-E           (planned; drawn disabled)
      I/O                 input selector, output
      AUTO                automation mode (off / read)
      PAN                 knob + readout ("<50", "0", "50>")
      R  S  M             record, solo, mute
      FADER + METER       console-taper fader, multi-segment meter, dB scale
      readout, name plate
*/
class ChannelStrip final : public juce::Component,
                           private juce::ValueTree::Listener,
                           private juce::AsyncUpdater
{
public:
    static constexpr int stripWidth = 104;

    ChannelStrip (AppController&, juce::ValueTree track);
    ~ChannelStrip() override;

    void updateMeter();

    // For the self test, which drives the strip's real controls.
    juce::Slider& getFader() noexcept          { return fader; }
    juce::Slider& getPanKnob() noexcept        { return panKnob; }
    juce::TextButton& getMuteButton() noexcept { return muteButton; }
    juce::String getPanReadout() const         { return panReadout.getText(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void handleAsyncUpdate() override { refreshInsertSlots(); }
    void refresh();
    void refreshInsertSlots();
    void populateInputs();
    void setProperty (const juce::Identifier&, const juce::var&, const juce::String& undoName);

    AppController& app;
    juce::ValueTree trackNode;

    juce::OwnedArray<InsertSlotButton> insertSlots;
    juce::ComboBox inputSelector, automationSelector;
    juce::Label outputLabel;
    juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label panReadout;
    juce::TextButton recordButton { "R" }, soloButton { "S" }, muteButton { "M" };
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    LevelMeter meter;
    juce::Label volumeReadout, namePlate;

    juce::Rectangle<int> insertsHeaderA, insertsHeaderB, sendsHeader, sendsArea, ioHeader, scaleArea;
    bool draggingFader = false;
};

} // namespace meridian
