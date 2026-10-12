#pragma once

#include "EditState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace meridian
{

/** The rulers above the tracks: Markers, Bars|Beats and Min:Secs, with the
    loop range shown as a strip. Clicking a ruler moves the playback cursor
    there; clicking a marker goes to it, right-click renames or deletes it. */
class TimelineRuler final : public juce::Component,
                            private juce::ChangeListener,
                            private juce::ValueTree::Listener
{
public:
    explicit TimelineRuler (EditState&);
    ~TimelineRuler() override;

    std::function<void (juce::int64)> onLocate;
    /** The session's undo manager, for marker renames and deletions. */
    std::function<juce::UndoManager*()> undoManager;

    void setPlayheadPosition (juce::int64 sample);
    /** The SESSION tree, for markers and the loop range (invalid when none is open). */
    void setSessionState (juce::ValueTree sessionState);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    static constexpr int rowHeight = 20;
    static constexpr int numRows = 3;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { repaint(); }
    void paintMarkers (juce::Graphics&, juce::Rectangle<int> row);
    void paintBarsBeats (juce::Graphics&, juce::Rectangle<int> row);
    void paintMinSecs (juce::Graphics&, juce::Rectangle<int> row);
    void locateFromMouse (const juce::MouseEvent&);
    juce::ValueTree markerAt (int x) const;
    void showMarkerMenu (juce::ValueTree marker);

    EditState& state;
    juce::ValueTree session;
    juce::int64 playhead = 0;
};

} // namespace meridian
