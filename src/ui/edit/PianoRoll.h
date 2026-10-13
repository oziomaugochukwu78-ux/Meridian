#pragma once

#include "app/AppController.h"

#include <midi/MeterMap.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace meridian
{

/** The MIDI editor for one clip: a piano keyboard down the left, a bar/beat
    grid, and the clip's notes.

        click empty space        add a note (snapped to the grid); drag to set its length
        drag a note              move it in time and pitch (all selected notes move together)
        drag a note's right edge change its length
        shift-click              add to / remove from the selection
        double-click a note      delete it
        Delete / Backspace       delete the selected notes
        mouse wheel              scroll pitch;  shift+wheel: scroll time;  ctrl+wheel: zoom time
        Space                    play / stop

    Every gesture is one undo step. The time axis is in ticks from the clip's
    content origin; notes outside the clip's playing range are shown dimmed. */
class PianoRollComponent final : public juce::Component,
                                 private juce::ValueTree::Listener
{
public:
    PianoRollComponent (AppController&, juce::ValueTree clip);
    ~PianoRollComponent() override;

    const juce::ValueTree& getClip() const noexcept { return clipNode; }

    /** Called when the clip disappears from the session (deleted, undone). */
    std::function<void()> onClipGone;

    // Geometry, public for the self test (simulated mouse input).
    juce::Point<float> positionFor (int64_t tick, int pitch) const;  ///< top-left of a note cell
    float rowHeight() const noexcept { return (float) noteRowHeight; }
    int64_t getGridTicks() const noexcept;
    void setGridTicks (int64_t ticks);
    std::vector<juce::ValueTree> getSelection() const { return selection; }
    void quantizeSelection();
    void transposeSelection (int semitones);
    void deleteSelection();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Drag { none, move, resize };

    struct Hit
    {
        juce::ValueTree note;
        bool onRightEdge = false;
    };

    juce::Rectangle<int> gridArea() const;
    int64_t tickAtX (float x) const noexcept;
    int pitchAtY (float y) const noexcept;
    float xForTick (int64_t tick) const noexcept;
    float yForPitch (int pitch) const noexcept;
    juce::Rectangle<float> noteRect (const juce::ValueTree& note) const;
    Hit noteAt (juce::Point<float>) const;
    /** The nearest grid position (or, with `down`, the grid cell's start), counted from bar lines. */
    int64_t snap (int64_t tick, bool down = false) const;
    int64_t originTick() const;        ///< the session tick of the clip's content origin (note tick 0)
    midi::MeterMap meterMap() const;   ///< the session's time signatures
    bool isSelected (const juce::ValueTree&) const;
    std::vector<juce::ValueTree> targets() const; ///< the selection, or every note if none
    juce::UndoManager* undoManager() const;
    void clipRangeTicks (int64_t& start, int64_t& end) const;
    void scrollToNotes();

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override;

    AppController& app;
    juce::ValueTree clipNode;

    // View
    static constexpr int toolbarHeight = 36, rulerHeight = 20, noteRowHeight = 13;
    bool drumKit = false;          // rows are drums (General MIDI names) instead of piano keys
    int keyboardWidth = 64;        // wider on drum tracks, for the drum names
    double pixelsPerTick = 0.08;   // 76.8 px per beat
    int64_t viewStartTick = 0;
    int topPitch = 84;             // pitch of the top row

    // Editing
    std::vector<juce::ValueTree> selection;
    Drag drag = Drag::none;
    juce::ValueTree dragNote;
    struct Original { juce::ValueTree note; int64_t start; int64_t length; int pitch; };
    std::vector<Original> originals;
    juce::Point<float> dragStart;

    juce::ComboBox gridBox;
    juce::TextButton quantizeButton { "Quantize" }, octaveDown { "-12" }, semitoneDown { "-1" },
                     semitoneUp { "+1" }, octaveUp { "+12" };
    juce::Slider velocity;
    juce::Label velocityLabel, hint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};

/** A non-modal window holding a PianoRollComponent, so the editor can stay
    open while the session plays. */
class PianoRollWindow final : public juce::DocumentWindow
{
public:
    PianoRollWindow (AppController&, juce::ValueTree clip, std::function<void()> onClose);
    PianoRollComponent& getEditor() noexcept { return *editor; }
    void closeButtonPressed() override;

private:
    PianoRollComponent* editor = nullptr; // owned by the window (setContentOwned)
    std::function<void()> onClose;
};

} // namespace meridian
