#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Elastic Audio dialog for one clip: new length (as a percentage or in
    seconds), pitch shift in semitones and cents, and formant preservation.
    Rendering happens in the background; the clip switches over when done. */
class TimeStretchComponent final : public juce::Component
{
public:
    static void show (AppController&, juce::ValueTree clip);

    TimeStretchComponent (AppController&, juce::ValueTree clip);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void syncFromPercent();
    void syncFromSeconds();
    void render();

    AppController& app;
    juce::ValueTree clipNode;
    double originalSeconds = 0.0;

    juce::Label percentLabel, secondsLabel, semitoneLabel, centsLabel, engineLabel;
    juce::Slider percent { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextEditor seconds;
    juce::Slider semitones { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Slider cents { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::ToggleButton preserveFormants { "Preserve formants (vocals)" };
    juce::TextButton cancelButton { "Cancel" }, renderButton { "Render" };
};

} // namespace meridian
