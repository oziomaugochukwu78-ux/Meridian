#pragma once

#include "app/AppController.h"
#include "ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The master fader strip, with the analog console section on top:
    on/off, drive, character (asymmetry → even harmonics) and curve. */
class MasterStrip final : public juce::Component
{
public:
    static constexpr int stripWidth = 132;

    explicit MasterStrip (AppController&);

    /** Re-reads every control from the session (after a session change). */
    void refresh();
    void updateMeter();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void store (const juce::Identifier&, const juce::var&, const juce::String& undoName);

    AppController& app;

    juce::TextButton consoleButton { "CONSOLE" };
    juce::Slider driveKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider characterKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::ComboBox curveSelector;
    juce::Label driveLabel, characterLabel, latencyLabel;

    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    LevelMeter meter { 2 };
    juce::Label volumeReadout, namePlate;
    juce::Rectangle<int> consoleHeader, scaleArea;
    bool draggingFader = false;
};

} // namespace meridian
