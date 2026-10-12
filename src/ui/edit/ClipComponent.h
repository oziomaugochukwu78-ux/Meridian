#pragma once

#include "EditState.h"

#include "app/AppController.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>

namespace meridian
{

/** One clip on a track lane: a waveform (audio) or its notes (MIDI), the
    name bar, fades (audio only), and the Edit-tool behaviours.

    SMART TOOL zones (when the Smart Tool is active):

        +--fade-in----------------------------fade-out--+   top corners: fades
        |                 SELECTOR (upper half)         |
        |trim                                       trim|   edges: trim
        |                 GRABBER  (lower half)         |   (Alt + right edge:
        +-----------------------------------------------+    time-stretch, TCE)

    The other tools force one behaviour everywhere: Selector selects, Grabber
    moves, Trim trims the nearer edge. */
class ClipComponent final : public juce::Component,
                            private juce::ValueTree::Listener,
                            private juce::ChangeListener
{
public:
    ClipComponent (AppController&, EditState&, juce::ValueTree clip, juce::Colour trackColour);
    ~ClipComponent() override;

    const juce::ValueTree& getClip() const noexcept { return clipNode; }

    /** Positions the component inside its lane for the current view. */
    void updateLayout (int laneWidth, int laneHeight);

    /** Wired by the Edit window. */
    std::function<juce::ValueTree (juce::Point<int> screenPosition)> trackAtScreenPosition;
    std::function<void (juce::ValueTree clip)> openElasticDialog;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class Action { none, select, move, trimStart, trimEnd, stretchEnd, fadeIn, fadeOut };

    Action actionAt (const juce::MouseEvent&) const;
    static juce::MouseCursor cursorFor (Action);
    void forwardToLane (const juce::MouseEvent&, int phase);
    void showMenu();
    void loadThumbnail();

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void paintNotes (juce::Graphics&, juce::Rectangle<int> area);

    AppController& app;
    EditState& editState;
    juce::ValueTree clipNode;
    juce::Colour colour;
    juce::AudioThumbnail thumbnail;

    double logicalLeft = 0.0;   // x of the clip's start, in this component's coordinates
    double logicalRight = 0.0;  // x of the clip's end

    // Drag state
    Action dragAction = Action::none;
    int downScreenX = 0;
    juce::int64 originalStart = 0, originalLength = 0, originalOffset = 0, originalFadeIn = 0, originalFadeOut = 0;
};

} // namespace meridian
