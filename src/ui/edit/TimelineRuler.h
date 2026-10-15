#pragma once

#include "EditState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace meridian
{

class Session;

/** The rulers above the tracks: Markers, Tempo, Bars|Beats and Min:Secs, with
    the loop range shown as a strip. Clicking a ruler moves the playback cursor
    there; clicking a marker goes to it, right-click renames or deletes it.

    The Tempo ruler shows the starting tempo and time signature and every
    change. Double-click it to add a change at the start of that bar;
    right-click a change to edit it, make it ramp to the next one, or delete
    it. Each edit is one undo step. */
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
    /** The open session (or nullptr), for tempo and meter edits. */
    std::function<Session*()> getSession;

    /** Sets the tempo (and, if it differs, the time signature) from the start of
        `bar` (0-based), as one undo step. Used by the Tempo ruler's dialog; public
        for the self test. False without a session. */
    bool setTempoAtBar (juce::int64 bar, double bpm, int numerator, int denominator);
    /** Makes the tempo at `tick` glide to the next change's (or hold), as one undo step. */
    bool setRampAt (juce::int64 tick, bool ramp);
    /** Deletes the tempo change at `tick` and the time signature at `bar` (either may be absent). */
    bool deleteTempoChange (juce::int64 tick, juce::int64 bar);
    /** The Tempo ruler's labels, left to right (for the self test). */
    juce::StringArray getTempoLabels() const;

    /** The Timecode row's label for a timeline sample, at the session's rate and start. */
    juce::String timecodeAt (juce::int64 sample) const;

    void setPlayheadPosition (juce::int64 sample);
    /** The SESSION tree, for markers and the loop range (invalid when none is open). */
    void setSessionState (juce::ValueTree sessionState);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int rowHeight = 20;
    static constexpr int numRows = 5;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { repaint(); }
    void paintMarkers (juce::Graphics&, juce::Rectangle<int> row);

    /** One label on the Tempo ruler: a tempo change, a time signature, or both at one place. */
    struct TempoMark
    {
        juce::int64 tick = 0;
        juce::int64 bar = 0;
        bool hasTempo = false, hasMeter = false;
        bool ramp = false; ///< the tempo glides to the next change's
        juce::String label;
    };
    std::vector<TempoMark> tempoMarks() const;
    std::optional<TempoMark> tempoMarkAt (int x) const;
    void paintTempo (juce::Graphics&, juce::Rectangle<int> row);
    void showTempoDialog (juce::int64 bar);
    void showTempoMenu (const TempoMark&);
    void paintBarsBeats (juce::Graphics&, juce::Rectangle<int> row);
    void paintMinSecs (juce::Graphics&, juce::Rectangle<int> row);
    void paintTimecode (juce::Graphics&, juce::Rectangle<int> row);
    void showTimecodeMenu();
    void locateFromMouse (const juce::MouseEvent&);
    juce::ValueTree markerAt (int x) const;
    void showMarkerMenu (juce::ValueTree marker);

    EditState& state;
    juce::ValueTree session;
    juce::int64 playhead = 0;
};

} // namespace meridian
