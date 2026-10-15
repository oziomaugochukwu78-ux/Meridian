#pragma once

#include "EditState.h"

#include "app/AppController.h"
#include "ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The left-hand header of an Edit-window track: colour, name, record/solo/
    mute keys, fader readout, a meter, the automation view selector (which
    lane the Pencil draws: volume, pan, mute or a send) and (tracks with clips)
    the playlist selector. Right-click for track options.

    The playlist selector shows the active playlist; its menu switches
    playlists, makes a new or duplicate one, renames or deletes them, and
    comps the Edit selection from another playlist onto the active one. */
class TrackHeader final : public juce::Component,
                          private juce::ValueTree::Listener,
                          private juce::ChangeListener
{
public:
    TrackHeader (AppController&, EditState&, juce::ValueTree track);
    /** Shows the groups the track is in, if that changed. */
    void refreshGroups();
    ~TrackHeader() override;

    const juce::ValueTree& getTrack() const noexcept { return trackNode; }

    /** Pulls the latest peaks from the engine (Edit window timer). */
    void updateMeter();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    /** Playlist menu items (public for the self test, which has no mouse for menus). */
    enum PlaylistItem
    {
        newPlaylist = 1, duplicatePlaylist, renamePlaylist,
        switchTo = 100,   ///< + index into playlists::names
        deleteFrom = 200, ///< + index into playlists::names
        compFrom = 300    ///< + index into playlists::names
    };
    void runPlaylistItem (int item);
    juce::PopupMenu buildPlaylistMenu() const;
    juce::String getPlaylistButtonText() const { return playlistButton.getButtonText(); }

    /** The automation view selector: which lane the track shows and the Pencil draws.
        buildViewMenu's item ids: 1 volume, 2 pan, 3 mute, 10 + slot for send A..E, and
        pluginItemBase + slot * pluginItemsPerSlot + index for an insert's control. */
    static constexpr int pluginItemBase = 1000, pluginItemsPerSlot = 500;
    juce::PopupMenu buildViewMenu() const;
    void runViewItem (int item);
    juce::String getViewButtonText() const { return viewButton.getButtonText(); }

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateViewButton(); } // EditState changed
    void refresh();
    void updateViewButton();
    void toggleProperty (const juce::Identifier&, const juce::String& undoName);
    void showMenu();
    void showPlaylistMenu();
    void showRenamePlaylist();

    AppController& app;
    EditState& editState;
    juce::ValueTree trackNode;
    juce::String shownGroups;

    juce::Label nameLabel;
    juce::TextButton recordButton { "R" }, soloButton { "S" }, muteButton { "M" };
    juce::Label infoLabel;
    juce::TextButton playlistButton;
    juce::TextButton viewButton;
    LevelMeter meter;
};

} // namespace meridian
