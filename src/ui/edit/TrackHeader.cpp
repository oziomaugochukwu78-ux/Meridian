#include "TrackHeader.h"

#include "model/Automation.h"
#include "ui/Theme.h"

namespace meridian
{
namespace
{
juce::String formatDb (double db)
{
    if (db <= -144.0)
        return "-inf";
    return (db > 0.0 ? "+" : "") + juce::String (db, 1);
}

void styleKey (juce::TextButton& b, juce::Colour onColour, const juce::String& tooltip)
{
    b.setClickingTogglesState (false);
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOnId, theme::textOnLight);
    b.setTooltip (tooltip);
}
} // namespace

TrackHeader::TrackHeader (AppController& a, EditState& s, juce::ValueTree t)
    : app (a), editState (s), trackNode (std::move (t)), meter (track::channels (trackNode))
{
    nameLabel.setFont (theme::font (13.0f, true));
    nameLabel.setEditable (false, true, false);
    nameLabel.setTooltip ("Double-click to rename");
    nameLabel.onTextChange = [this]
    {
        const auto newName = nameLabel.getText().trim();
        if (newName.isEmpty())
        {
            refresh();
            return;
        }
        if (auto* um = app.getUndoManager())
        {
            um->beginNewTransaction ("Rename Track");
            trackNode.setProperty (ids::name, newName, um);
        }
    };
    addAndMakeVisible (nameLabel);

    styleKey (recordButton, theme::record, "Record enable");
    styleKey (soloButton, theme::solo, "Solo");
    styleKey (muteButton, theme::mute, "Mute");
    recordButton.onClick = [this] { toggleProperty (ids::recordArm, "Record Enable"); };
    soloButton.onClick = [this] { toggleProperty (ids::solo, "Solo"); };
    muteButton.onClick = [this] { toggleProperty (ids::mute, "Mute"); };
    for (auto* b : { &recordButton, &soloButton, &muteButton })
        addAndMakeVisible (b);

    infoLabel.setFont (theme::font (11.0f));
    infoLabel.setColour (juce::Label::textColourId, theme::textDim);
    infoLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (infoLabel);

    addAndMakeVisible (meter);

    trackNode.addListener (this);
    refresh();
}

TrackHeader::~TrackHeader()
{
    trackNode.removeListener (this);
}

void TrackHeader::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == trackNode)
        refresh();
}

void TrackHeader::refresh()
{
    nameLabel.setText (track::name (trackNode), juce::dontSendNotification);
    recordButton.setToggleState (track::recordArm (trackNode), juce::dontSendNotification);
    soloButton.setToggleState (track::solo (trackNode), juce::dontSendNotification);
    muteButton.setToggleState (track::mute (trackNode), juce::dontSendNotification);

    const bool aux = track::isAux (trackNode), instrument = track::isInstrument (trackNode);
    const int input = track::inputChannel (trackNode);
    const auto inputText = aux ? juce::String ("Aux Input")
                           : track::isDrumKit (trackNode) ? juce::String ("Drums (built-in drum machine)")
                           : instrument ? juce::String ("Instrument (built-in synth)")
                           : input < 0 ? juce::String ("no input")
                           : track::channels (trackNode) == 2 ? "In " + juce::String (input + 1) + "-" + juce::String (input + 2)
                                                              : "In " + juce::String (input + 1);
    infoLabel.setText ("vol " + formatDb (track::volumeDb (trackNode)) + "   " + inputText
                           + (aux || instrument ? juce::String() : track::channels (trackNode) == 2 ? "   stereo" : "   mono"),
                       juce::dontSendNotification);
    recordButton.setVisible (! track::isAux (trackNode)); // instruments record MIDI from the keyboard; buses record nothing

    meter.setNumChannels (track::channels (trackNode));
    repaint();
}

void TrackHeader::toggleProperty (const juce::Identifier& property, const juce::String& undoName)
{
    if (auto* um = app.getUndoManager())
    {
        um->beginNewTransaction (undoName);
        trackNode.setProperty (property, ! (bool) trackNode.getProperty (property, false), um);
    }
}

void TrackHeader::updateMeter()
{
    if (auto state = app.getBridge().getTrackState (track::id (trackNode)))
    {
        const auto [left, right] = state->collectPeaks();
        meter.pushPeaks (left, right);
    }
}

void TrackHeader::paint (juce::Graphics& g)
{
    const bool selected = editState.selectedTrackIds.contains (track::id (trackNode));
    g.fillAll (selected ? theme::panelRaised : theme::panel);

    g.setColour (track::colour (trackNode));
    g.fillRect (0, 0, 5, getHeight());

    g.setColour (theme::divider);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

void TrackHeader::resized()
{
    auto area = getLocalBounds().withTrimmedLeft (8).withTrimmedBottom (1).withTrimmedRight (1).reduced (4, 4);

    meter.setBounds (area.removeFromRight (12));
    area.removeFromRight (4);

    nameLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);

    auto keys = area.removeFromTop (22);
    for (auto* b : { &recordButton, &soloButton, &muteButton })
    {
        b->setBounds (keys.removeFromLeft (28));
        keys.removeFromLeft (4);
    }

    area.removeFromTop (4);
    infoLabel.setBounds (area.removeFromTop (18));
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }

    editState.setTimeSelection (editState.selectionStart, editState.selectionEnd, { track::id (trackNode) });
}

void TrackHeader::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, track::channels (trackNode) == 1 ? "Make Stereo" : "Make Mono", track::isAudio (trackNode));
    menu.addItem (4, "Clear Volume Automation", automation::volumeLane (trackNode).isValid());
    menu.addSeparator();
    const auto* session = app.getSession();
    const int index = session != nullptr ? session->indexOfTrack (trackNode) : -1;
    menu.addItem (5, "Duplicate Track");
    menu.addItem (6, "Move Up", index > 0);
    menu.addItem (7, "Move Down", session != nullptr && index >= 0 && index < session->getNumTracks() - 1);
    menu.addSeparator();
    menu.addItem (3, "Delete Track");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe = juce::Component::SafePointer<TrackHeader> (this)] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            auto& self = *safe;
                            auto* um = self.app.getUndoManager();
                            if (result == 1)
                                self.nameLabel.showEditor();
                            else if (result == 2 && um != nullptr)
                            {
                                um->beginNewTransaction ("Change Track Format");
                                self.trackNode.setProperty (ids::channels, track::channels (self.trackNode) == 1 ? 2 : 1, um);
                            }
                            else if (result == 4 && um != nullptr)
                            {
                                um->beginNewTransaction ("Clear Automation");
                                automation::clear (self.trackNode, um);
                            }
                            else if (result == 5)
                                self.app.duplicateTrack (self.trackNode);
                            else if ((result == 6 || result == 7) && self.app.getSession() != nullptr)
                            {
                                const int at = self.app.getSession()->indexOfTrack (self.trackNode);
                                self.app.moveTrack (self.trackNode, at + (result == 6 ? -1 : 1)); // rebuilds the headers
                            }
                            else if (result == 3)
                                self.app.deleteTrack (self.trackNode); // destroys this header: nothing after this
                        });
}

} // namespace meridian
