#pragma once

#include "EditState.h"
#include "EditToolbar.h"
#include "TimelineRuler.h"
#include "TrackHeader.h"
#include "TrackLane.h"

#include "app/AppController.h"

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

    // Edit commands (invoked from menus and shortcuts)
    void separateAtSelection();
    void deleteSelection();
    void openElasticDialog (juce::ValueTree clip);
    void zoomBy (double factor);

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

    AppController& app;
    EditState state;

    EditToolbar toolbar { state };
    TimelineRuler ruler { state };
    juce::Viewport viewport;
    juce::Component trackContainer;
    juce::ScrollBar horizontalScroll { false };
    juce::Label emptyHint;

    juce::OwnedArray<TrackHeader> headers;
    juce::OwnedArray<TrackLane> lanes;
    juce::ValueTree tracksNode;
    std::unique_ptr<juce::FileFilter> audioFileFilter;
};

} // namespace meridian
