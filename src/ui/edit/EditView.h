#pragma once

#include "EditState.h"
#include "EditToolbar.h"
#include "PianoRoll.h"
#include "TimelineRuler.h"
#include "TrackHeader.h"
#include "TrackLane.h"

#include "app/AppController.h"
#include "model/EditClipboard.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The Edit window: toolbar, rulers, and a vertically scrolling stack of
    tracks (header on the left, timeline lane on the right).

    The horizontal timeline is virtual: lanes draw the range EditState
    describes, and the bottom scrollbar moves that range. During playback the
    view pages forward when the cursor reaches the right edge. Audio files
    dropped from the desktop are imported at the drop position. */
class EditView final : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       private juce::Timer,
                       private juce::ChangeListener,
                       private juce::ValueTree::Listener,
                       private juce::ScrollBar::Listener
{
public:
    explicit EditView (AppController&);
    ~EditView() override;

    EditState& getEditState() noexcept { return state; }
    TrackLane* getLane (int index) const noexcept { return lanes[index]; }
    TrackHeader* getHeader (int index) const noexcept { return headers[index]; }
    TimelineRuler& getRuler() noexcept { return ruler; }

    // Edit commands (invoked from menus and shortcuts)
    void separateAtSelection();
    /** actionName: the undo step's name ("Cut"); default "Delete Clip" or "Clear Selection". */
    void deleteSelection (const juce::String& actionName = {});
    /** Cut, Copy, Paste: the selected clip, or the selected time range on the
        selected tracks. Paste goes to the selection start (or the cursor) on the
        selected tracks, replacing what is there (Shuffle: inserting), and
        selects what it pasted. */
    void cutSelection();
    void copySelection();
    void pasteClipboard();
    /** Duplicate: a copy of the selection right after it, then selected, so
        repeating it lays copies end to end. Leaves the clipboard alone. */
    void duplicateSelection();
    bool hasSelectionToCopy() const;
    /** Fade/Crossfade (Ctrl+F) over the time selection on the selected tracks. */
    void fadeSelection();
    const clipboard::Contents& getClipboard() const noexcept { return clipboardContents; }
    void openElasticDialog (juce::ValueTree clip);
    void zoomBy (double factor);

    /** Opens (or brings forward) the MIDI editor for a MIDI clip. */
    void openPianoRoll (juce::ValueTree clip);
    PianoRollComponent* getPianoRoll() noexcept { return pianoRoll != nullptr ? &pianoRoll->getEditor() : nullptr; }
    void closePianoRoll() { pianoRoll.reset(); }

    void resized() override;
    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    void bindSession();
    void rebuildTracks();
    void layoutTracks();
    void updateScrollBar();
    juce::int64 sessionLength() const;
    juce::ValueTree trackAtScreenPosition (juce::Point<int> screenPosition) const;
    juce::Array<juce::ValueTree> tracksForEdit() const;
    void splitTracksAtSelection(); // within the caller's undo transaction
    void pasteContents (const clipboard::Contents&, juce::Array<juce::ValueTree> targets, juce::int64 at, const juce::String& actionName);

    AppController& app;
    EditState state;
    clipboard::Contents clipboardContents;

    EditToolbar toolbar { state };
    TimelineRuler ruler { state };
    juce::Viewport viewport;
    juce::Component trackContainer;
    juce::ScrollBar horizontalScroll { false };
    juce::Label emptyHint;

    juce::OwnedArray<TrackHeader> headers;
    juce::OwnedArray<TrackLane> lanes;
    juce::ValueTree tracksNode;
    juce::uint64 mapsSeenAt = ~juce::uint64 (0); // session change count when the tempo/meter maps were last read
    std::unique_ptr<juce::FileFilter> audioFileFilter;
    std::unique_ptr<PianoRollWindow> pianoRoll;
};

} // namespace meridian
