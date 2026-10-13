#pragma once

#include "EditState.h"

#include "app/AppController.h"
#include "ui/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The left-hand header of an Edit-window track: colour, name, record/solo/
    mute keys, fader readout, a meter and (tracks with clips) the playlist
    selector. Right-click for track options.

    The playlist selector shows the active playlist; its menu switches
    playlists, makes a new or duplicate one, renames or deletes them, and
    comps the Edit selection from another playlist onto the active one. */
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
    juce::TextButton playlistButton;
    LevelMeter meter;
};

} // namespace meridian
