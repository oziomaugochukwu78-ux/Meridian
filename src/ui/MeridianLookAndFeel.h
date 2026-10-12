#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Dark console styling for every standard JUCE control.

    Faders: a recessed slot with a flat, grooved cap.
    Knobs: a small dark knob with an arc drawn from the centre (pan-style).
    Buttons: flat, squared, coloured only when engaged, the way console
    mute/solo/record keys light up. */
class MeridianLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MeridianLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont (juce::Label&) override;
};

} // namespace meridian
