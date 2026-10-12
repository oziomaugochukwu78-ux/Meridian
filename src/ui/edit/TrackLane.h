#pragma once

#include "ClipComponent.h"
#include "EditState.h"

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The timeline row of one track: grid, time selection, playback cursor, and
    the track's clips. Clicking empty space places the cursor; dragging makes
    a time selection (the Selector behaviour). */
class TrackLane final : public juce::Component,
                        private juce::ValueTree::Listener,
                        private juce::ChangeListener
{
public:
    TrackLane (AppController&, EditState&, juce::ValueTree track);
    ~TrackLane() override;

    const juce::ValueTree& getTrack() const noexcept { return trackNode; }
    ClipComponent* getClipComponent (int index) const noexcept { return clips[index]; }

    void setPlayhead (juce::int64 sample);

    std::function<juce::ValueTree (juce::Point<int>)> trackAtScreenPosition;
    std::function<void (juce::ValueTree)> openElasticDialog;
    /** Ctrl+wheel (zoom) and Shift+wheel (horizontal scroll); plain wheel scrolls tracks. */
    std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onModifiedWheel;

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void rebuildClips();
    void layoutClips();
    int playheadX() const;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    AppController& app;
    EditState& editState;
    juce::ValueTree trackNode;
    juce::OwnedArray<ClipComponent> clips;

    juce::int64 playhead = 0;
    juce::int64 selectionAnchor = 0;
    bool draggingSelection = false;
};

} // namespace meridian
