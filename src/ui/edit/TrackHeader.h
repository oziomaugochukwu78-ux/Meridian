#pragma once

#include "EditState.h"

#include "app/AppController.h"
#include "ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The left-hand header of an Edit-window track: colour, name, record/solo/
    mute keys, fader readout and a meter. Right-click for track options. */
class TrackHeader final : public juce::Component,
                          private juce::ValueTree::Listener
{
public:
    TrackHeader (AppController&, EditState&, juce::ValueTree track);
    ~TrackHeader() override;

    const juce::ValueTree& getTrack() const noexcept { return trackNode; }

    /** Pulls the latest peaks from the engine (Edit window timer). */
    void updateMeter();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void refresh();
    void toggleProperty (const juce::Identifier&, const juce::String& undoName);
    void showMenu();

    AppController& app;
    EditState& editState;
    juce::ValueTree trackNode;

    juce::Label nameLabel;
    juce::TextButton recordButton { "R" }, soloButton { "S" }, muteButton { "M" };
    juce::Label infoLabel;
    LevelMeter meter;
};

} // namespace meridian
