#include "TrackHeader.h"

#include "AutomationView.h"
#include "model/Automation.h"
#include "model/Playlists.h"
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

    playlistButton.setTooltip ("Playlist: switch takes, start a new or duplicate playlist, rename, delete, "
                               "or comp the selected range from another playlist");
    playlistButton.onClick = [this] { showPlaylistMenu(); };
    addAndMakeVisible (playlistButton);

    viewButton.setTooltip ("Automation view: which lane this track shows and the Pencil draws");
    viewButton.onClick = [this]
    {
        buildViewMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton),
                                       [safe = juce::Component::SafePointer<TrackHeader> (this)] (int result)
                                       {
                                           if (safe != nullptr && result != 0)
                                               safe->runViewItem (result);
                                       });
    };
    addAndMakeVisible (viewButton);

    addAndMakeVisible (meter);

    trackNode.addListener (this);
    editState.addChangeListener (this);
    refresh();
}

TrackHeader::~TrackHeader()
{
    editState.removeChangeListener (this);
    trackNode.removeListener (this);
}

void TrackHeader::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == trackNode)
        refresh();
}

void TrackHeader::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent.hasType (ids::SENDS) || parent.hasType (ids::INSERTS))
        updateViewButton(); // a removed send's or plug-in's lane falls back to volume
}

void TrackHeader::updateViewButton()
{
    const auto shown = automation::shownParameter (trackNode, editState.automationViewFor (track::id (trackNode)));
    const auto text = automationview::laneName (app, trackNode, shown);
    if (viewButton.getButtonText() != text)
        viewButton.setButtonText (text);
}

juce::PopupMenu TrackHeader::buildViewMenu() const
{
    const auto shown = automation::shownParameter (trackNode, editState.automationViewFor (track::id (trackNode)));
    juce::PopupMenu menu;
    menu.addSectionHeader ("Automation lane");
    menu.addItem (1, "Volume", true, shown == "volume");
    menu.addItem (2, "Pan", true, shown == "pan");
    menu.addItem (3, "Mute", true, shown == "mute");
    for (const auto& s : track::sends (trackNode))
    {
        const int slot = send::slot (s);
        if (! juce::isPositiveAndBelow (slot, Session::maxSends))
            continue;
        const auto parameter = automation::sendParameter (slot);
        menu.addItem (10 + slot, "Send " + juce::String::charToString ((juce::juce_wchar) ('A' + slot)) + " level", true, shown == parameter);
    }

    // Each plug-in's automatable controls, in a submenu per insert.
    for (int slot = 0; slot < Session::maxInserts; ++slot)
    {
        const auto insert = app.getBridge().getInsert (track::id (trackNode), slot);
        if (insert == nullptr || insert->plugin == nullptr)
            continue;
        juce::PopupMenu controls;
        const auto& all = insert->plugin->getParameters();
        for (int i = 0; i < juce::jmin (all.size(), pluginItemsPerSlot); ++i)
            if (all[i]->isAutomatable())
            {
                const auto parameter = automation::pluginParameter (slot, i);
                controls.addItem (pluginItemBase + slot * pluginItemsPerSlot + i, all[i]->getName (64), true, shown == parameter);
            }
        if (controls.getNumItems() > 0)
            menu.addSubMenu ("Insert " + juce::String::charToString ((juce::juce_wchar) ('A' + slot)) + ": " + insert->plugin->getName(),
                             controls);
    }
    return menu;
}

void TrackHeader::runViewItem (int item)
{
    const auto parameter = item >= pluginItemBase ? automation::pluginParameter ((item - pluginItemBase) / pluginItemsPerSlot,
                                                                                 (item - pluginItemBase) % pluginItemsPerSlot)
                         : item == 2 ? juce::String ("pan")
                         : item == 3 ? juce::String ("mute")
                         : item >= 10 ? automation::sendParameter (item - 10)
                                      : juce::String ("volume");
    editState.setAutomationView (track::id (trackNode), automation::shownParameter (trackNode, parameter));
    updateViewButton(); // now, not when the (asynchronous) change message arrives
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

    // Buses have no clips, so no playlists.
    playlistButton.setVisible (! aux);
    playlistButton.setButtonText (aux ? juce::String() : playlists::activeName (trackNode));
    updateViewButton();

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
    playlistButton.setBounds (keys);

    area.removeFromTop (4);
    auto info = area.removeFromTop (18);
    viewButton.setBounds (info.removeFromRight (58));
    info.removeFromRight (4);
    infoLabel.setBounds (info);
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
    menu.addItem (8, "Coalesce Trim (write the fader's offset into the volume lane)",
                  track::automationMode (trackNode) == "trim" && automation::volumeLane (trackNode).isValid()
                      && ! juce::exactlyEqual (track::volumeDb (trackNode), 0.0));
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
                            else if (result == 8 && um != nullptr)
                            {
                                um->beginNewTransaction ("Coalesce Trim");
                                automation::coalesceTrim (self.trackNode, um);
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

//==============================================================================
// Playlists

juce::PopupMenu TrackHeader::buildPlaylistMenu() const
{
    juce::PopupMenu menu;
    if (track::isAux (trackNode))
        return menu;

    // Item ids encode an index into playlists::names, so stop below the next block.
    const auto all = playlists::names (trackNode);
    const auto active = playlists::activeName (trackNode);
    const int count = juce::jmin (all.size(), compFrom - deleteFrom);

    menu.addSectionHeader ("Playlists");
    for (int i = 0; i < count; ++i)
        menu.addItem (switchTo + i, all[i], true, all[i] == active);

    menu.addSeparator();
    menu.addItem (newPlaylist, "New Playlist");
    menu.addItem (duplicatePlaylist, "Duplicate Playlist");
    menu.addItem (renamePlaylist, "Rename Playlist...");

    // Comping copies the Edit selection's range, so it needs a range on this track.
    const bool hasRange = editState.hasTimeRange() && editState.selectedTrackIds.contains (track::id (trackNode));
    juce::PopupMenu comp, remove;
    for (int i = 0; i < count; ++i)
    {
        if (all[i] == active)
            continue;
        comp.addItem (compFrom + i, all[i], hasRange);
        remove.addItem (deleteFrom + i, all[i]);
    }
    menu.addSeparator();
    menu.addSubMenu (hasRange ? "Comp Selection From" : "Comp Selection From (select a range on this track)", comp,
                     hasRange && comp.getNumItems() > 0);
    menu.addSubMenu ("Delete Playlist", remove, remove.getNumItems() > 0);
    return menu;
}

void TrackHeader::showPlaylistMenu()
{
    buildPlaylistMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&playlistButton),
                                       [safe = juce::Component::SafePointer<TrackHeader> (this)] (int result)
                                       {
                                           if (safe != nullptr && result != 0)
                                               safe->runPlaylistItem (result);
                                       });
}

void TrackHeader::runPlaylistItem (int item)
{
    auto* session = app.getSession();
    auto* um = app.getUndoManager();
    if (session == nullptr || um == nullptr || track::isAux (trackNode))
        return;

    const auto all = playlists::names (trackNode);
    auto nameAt = [&all] (int index) { return juce::isPositiveAndBelow (index, all.size()) ? all[index] : juce::String(); };

    // Undo names match the AI tool's, so the history reads the same whoever made the edit.
    if (item == newPlaylist || item == duplicatePlaylist)
    {
        um->beginNewTransaction (item == newPlaylist ? "New Playlist" : "Duplicate Playlist");
        playlists::create (*session, trackNode, item == duplicatePlaylist, um);
    }
    else if (item == renamePlaylist)
    {
        showRenamePlaylist();
    }
    else if (item >= compFrom)
    {
        const auto name = nameAt (item - compFrom);
        if (name.isEmpty() || ! editState.hasTimeRange())
            return;
        um->beginNewTransaction ("Comp");
        playlists::promote (*session, trackNode, name, editState.selectionStart, editState.selectionEnd, um);
    }
    else if (item >= deleteFrom)
    {
        if (const auto name = nameAt (item - deleteFrom); playlists::find (trackNode, name).isValid())
        {
            um->beginNewTransaction ("Delete Playlist");
            playlists::remove (trackNode, name, um);
        }
    }
    else if (item >= switchTo)
    {
        if (const auto name = nameAt (item - switchTo); playlists::find (trackNode, name).isValid())
        {
            um->beginNewTransaction ("Switch Playlist");
            playlists::activate (trackNode, name, um);
        }
    }
}

void TrackHeader::showRenamePlaylist()
{
    const auto oldName = playlists::activeName (trackNode);
    auto* window = new juce::AlertWindow ("Rename Playlist", {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", oldName);
    window->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<TrackHeader> (this), oldName, window] (int result)
        {
            const auto newName = window->getTextEditorContents ("name").trim();
            if (safe == nullptr || result != 1 || newName.isEmpty() || newName == oldName)
                return;
            auto& self = *safe;
            auto* um = self.app.getUndoManager();
            if (um == nullptr)
                return;
            um->beginNewTransaction ("Rename Playlist");
            if (! playlists::rename (self.trackNode, oldName, newName, um) && self.app.showProblem)
                self.app.showProblem ("Rename Playlist", "This track already has a playlist named \"" + newName + "\".");
        }), true);
}

} // namespace meridian
