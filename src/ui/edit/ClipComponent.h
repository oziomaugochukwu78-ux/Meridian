#pragma once

#include "EditState.h"

#include "app/AppController.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <utility>
#include <vector>

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
    moves, Trim trims the nearer edge.

    CLIP GAIN (audio clips, any of those tools): the readout at the bottom left
    is a handle. Drag it up or down (0.25 dB per pixel; Shift: 0.05 dB) to set
    the clip's gain, -60 to +24 dB; double-click it for 0 dB. The waveform is
    drawn at the clip's gain, and a line marks it (0 dB in the middle).

    EDIT MODES: in Shuffle, trimming moves every later clip on the track by the
    same amount (automation follows), and a moved clip drops in at the nearest
    clip boundary, closing the gap it left. In Spot, clicking a clip with the
    Grabber asks for its exact start time instead of dragging. */
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
    std::function<void (juce::ValueTree clip)> openPianoRoll;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override; // MIDI clips: open the MIDI editor

    /** The clip-gain handle, in this component's coordinates (empty for MIDI clips
        or when the clip is too small to show it). Public for the self test. */
    juce::Rectangle<float> gainHandleBounds() const;

    /** Spot: moves the clip to a typed start time (m:ss.mmm, seconds, or samples
        as "48000 smp"), as one undo step. False if the text is not a time.
        Public for the self test, which cannot type into the dialog. */
    bool spotTo (const juce::String& timeText);

    static constexpr double gainDbPerPixel = 0.25, fineGainDbPerPixel = 0.05;
    static constexpr double minGainDb = -60.0, maxGainDb = 24.0;

private:
    enum class Action { none, select, move, trimStart, trimEnd, stretchEnd, fadeIn, fadeOut, clipGain };

    Action actionAt (const juce::MouseEvent&) const;
    static juce::MouseCursor cursorFor (Action);
    void forwardToLane (const juce::MouseEvent&, int phase);
    void showMenu();
    void showSpotDialog();
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
    int downScreenX = 0, downScreenY = 0;
    juce::int64 originalStart = 0, originalLength = 0, originalOffset = 0, originalFadeIn = 0, originalFadeOut = 0;
    double originalGainDb = 0.0;
    std::vector<std::pair<juce::ValueTree, juce::int64>> laterClips; // Shuffle trims: later clips and their starts
};

} // namespace meridian
