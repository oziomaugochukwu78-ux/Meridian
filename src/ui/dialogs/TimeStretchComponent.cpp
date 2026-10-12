#include "TimeStretchComponent.h"

#include "DialogUtils.h"
#include "media/TimeStretchEngine.h"

namespace meridian
{

void TimeStretchComponent::show (AppController& app, juce::ValueTree clip)
{
    dialogs::launch (new TimeStretchComponent (app, std::move (clip)), "Elastic Audio");
}

TimeStretchComponent::TimeStretchComponent (AppController& a, juce::ValueTree c) : app (a), clipNode (std::move (c))
{
    const double sampleRate = app.getSession() != nullptr ? app.getSession()->getSampleRate() : 48000.0;
    originalSeconds = (double) clip::length (clipNode) / sampleRate;

    dialogs::styleFieldLabel (percentLabel, "Length:");
    dialogs::styleFieldLabel (secondsLabel, "Seconds:");
    dialogs::styleFieldLabel (semitoneLabel, "Pitch (semitones):");
    dialogs::styleFieldLabel (centsLabel, "Fine (cents):");

    percent.setRange (12.5, 800.0, 0.1);
    percent.setSkewFactorFromMidPoint (100.0);
    percent.setValue (100.0, juce::dontSendNotification);
    percent.setTextValueSuffix (" %");
    percent.setDoubleClickReturnValue (true, 100.0);
    percent.onValueChange = [this] { syncFromPercent(); };

    seconds.setInputRestrictions (12, "0123456789.");
    seconds.onReturnKey = [this] { syncFromSeconds(); };
    seconds.onFocusLost = [this] { syncFromSeconds(); };

    semitones.setRange (-24.0, 24.0, 1.0);
    semitones.setValue (0.0, juce::dontSendNotification);
    cents.setRange (-100.0, 100.0, 1.0);
    cents.setValue (0.0, juce::dontSendNotification);

    engineLabel.setText ("Engine: " + timeStretchEngineName() + ", offline render. The original file is kept, so Undo restores it.",
                         juce::dontSendNotification);
    engineLabel.setFont (theme::font (11.0f));
    engineLabel.setColour (juce::Label::textColourId, theme::textDim);

    cancelButton.onClick = [this] { dialogs::close (*this); };
    renderButton.onClick = [this] { render(); };
    renderButton.setEnabled (isTimeStretchAvailable());
    renderButton.setColour (juce::TextButton::buttonColourId, theme::selectionEdge);
    renderButton.setColour (juce::TextButton::textColourOffId, theme::textOnLight);

    for (juce::Component* comp : std::initializer_list<juce::Component*> {
             &percentLabel, &percent, &secondsLabel, &seconds, &semitoneLabel, &semitones, &centsLabel, &cents,
             &preserveFormants, &engineLabel, &cancelButton, &renderButton })
        addAndMakeVisible (comp);

    syncFromPercent();
    setSize (520, 260);
}

void TimeStretchComponent::syncFromPercent()
{
    seconds.setText (juce::String (originalSeconds * percent.getValue() / 100.0, 3), false);
}

void TimeStretchComponent::syncFromSeconds()
{
    const double s = seconds.getText().getDoubleValue();
    if (s > 0.0 && originalSeconds > 0.0)
        percent.setValue (juce::jlimit (12.5, 800.0, 100.0 * s / originalSeconds), juce::dontSendNotification);
    syncFromPercent();
}

void TimeStretchComponent::render()
{
    const double ratio = percent.getValue() / 100.0;
    const double pitch = semitones.getValue() + cents.getValue() / 100.0;

    if (std::abs (ratio - 1.0) < 1.0e-4 && std::abs (pitch) < 1.0e-4)
    {
        dialogs::close (*this);
        return;
    }

    app.renderElasticAudio (clipNode, ratio, pitch, preserveFormants.getToggleState());
    dialogs::close (*this);
}

void TimeStretchComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawText ("Clip: " + clip::name (clipNode) + "   (" + juce::String (originalSeconds, 3) + " s)",
                getLocalBounds().reduced (16, 10).removeFromTop (20), juce::Justification::centredLeft);
}

void TimeStretchComponent::resized()
{
    auto area = getLocalBounds().reduced (16).withTrimmedTop (28);

    auto row = [&] (juce::Label& label, juce::Component& control, int width)
    {
        auto r = area.removeFromTop (26);
        label.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (8);
        control.setBounds (r.removeFromLeft (width));
        area.removeFromTop (6);
    };

    row (percentLabel, percent, 330);
    row (secondsLabel, seconds, 120);
    row (semitoneLabel, semitones, 120);
    row (centsLabel, cents, 120);
    preserveFormants.setBounds (area.removeFromTop (24).withTrimmedLeft (138));

    auto buttons = area.removeFromBottom (28);
    renderButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
    engineLabel.setBounds (area);
}

} // namespace meridian
