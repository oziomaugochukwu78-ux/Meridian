#include "NewTracksComponent.h"

#include "DialogUtils.h"

namespace meridian
{

void NewTracksComponent::show (AppController& app)
{
    if (app.getSession() == nullptr)
        return;
    dialogs::launch (new NewTracksComponent (app), "New Tracks");
}

NewTracksComponent::NewTracksComponent (AppController& a) : app (a)
{
    for (auto* l : { &createLabel, &newLabel, &namedLabel })
        l->setFont (theme::font (13.0f));
    createLabel.setText ("Create", juce::dontSendNotification);
    newLabel.setText ("new", juce::dontSendNotification);
    namedLabel.setText ("named", juce::dontSendNotification);

    count.setText ("1");
    count.setInputRestrictions (3, "0123456789");
    count.setJustification (juce::Justification::centred);
    count.setSelectAllWhenFocused (true);

    format.addItem ("Mono", 1);
    format.addItem ("Stereo", 2);
    format.setSelectedId (1, juce::dontSendNotification);

    kind.addItem ("Audio Track", 1);
    kind.addItem ("Aux Input (bus)", 2);
    kind.addItem ("Instrument Track", 3);
    kind.addItem ("Drum Track", 4);
    kind.setSelectedId (1, juce::dontSendNotification);
    kind.onChange = [this]
    {
        // Aux inputs, instruments and drums are always stereo.
        const int k = kind.getSelectedId();
        const bool stereoOnly = k >= 2;
        format.setSelectedId (stereoOnly ? 2 : format.getSelectedId(), juce::dontSendNotification);
        format.setEnabled (! stereoOnly);
        const auto current = baseName.getText();
        if (current == "Audio" || current == "Aux" || current == "Synth" || current == "Drums")
            baseName.setText (k == 2 ? "Aux" : k == 3 ? "Synth" : k == 4 ? "Drums" : "Audio");
    };

    baseName.setText ("Audio");
    baseName.setSelectAllWhenFocused (true);

    count.onReturnKey = [this] { create(); };
    baseName.onReturnKey = [this] { create(); };
    cancelButton.onClick = [this] { dialogs::close (*this); };
    createButton.onClick = [this] { create(); };
    createButton.setColour (juce::TextButton::buttonColourId, theme::selectionEdge);
    createButton.setColour (juce::TextButton::textColourOffId, theme::textOnLight);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &createLabel, &count, &newLabel, &format, &kind, &namedLabel, &baseName, &cancelButton, &createButton })
        addAndMakeVisible (c);

    setSize (620, 110);
}

void NewTracksComponent::create()
{
    const int n = juce::jlimit (1, 128, count.getText().getIntValue());
    app.addTracks (n, format.getSelectedId() == 2 ? 2 : 1, baseName.getText().trim(),
                   kind.getSelectedId() == 2 ? Session::TrackKind::aux
                   : kind.getSelectedId() == 3 ? Session::TrackKind::instrument
                   : kind.getSelectedId() == 4 ? Session::TrackKind::drums : Session::TrackKind::audio);
    dialogs::close (*this);
}

void NewTracksComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
}

void NewTracksComponent::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto row = area.removeFromTop (26);
    createLabel.setBounds (row.removeFromLeft (50));
    count.setBounds (row.removeFromLeft (40));
    row.removeFromLeft (6);
    newLabel.setBounds (row.removeFromLeft (34));
    format.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (6);
    kind.setBounds (row.removeFromLeft (180));
    row.removeFromLeft (6);
    namedLabel.setBounds (row.removeFromLeft (46));
    baseName.setBounds (row);

    auto buttons = area.removeFromBottom (28);
    createButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
}

} // namespace meridian
