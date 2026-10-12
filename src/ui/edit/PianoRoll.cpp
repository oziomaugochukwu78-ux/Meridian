#include "PianoRoll.h"

#include "model/MidiModel.h"
#include "ui/Theme.h"
#include "ui/TimeFormat.h"

#include <algorithm>
#include <cmath>

namespace meridian
{
namespace
{
constexpr float edgeZone = 6.0f;
constexpr int64_t beatTicks = midi::ticksPerQuarter;
constexpr int64_t barTicks = beatTicks * timeformat::beatsPerBar;

struct GridChoice { const char* name; int64_t ticks; };
const GridChoice gridChoices[] = {
    { "Grid: 1 bar", barTicks }, { "Grid: 1/4", beatTicks }, { "Grid: 1/8", beatTicks / 2 },
    { "Grid: 1/8 triplet", beatTicks / 3 }, { "Grid: 1/16", beatTicks / 4 }, { "Grid: 1/32", beatTicks / 8 },
    { "Grid: off", 1 },
};

// Tick values in the document. Through juce::int64 because on Linux int64_t is
// long, which juce::var neither converts to nor from without ambiguity.
int64_t ticksOf (const juce::var& v) { return (int64_t) (juce::int64) v; }
juce::var tickVar (int64_t ticks) { return juce::var ((juce::int64) ticks); }

bool isBlackKey (int pitch) noexcept
{
    const int pc = pitch % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

juce::String noteName (int pitch)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[pitch % 12]) + juce::String (pitch / 12 - 1); // 60 = C4
}
} // namespace

//==============================================================================
PianoRollComponent::PianoRollComponent (AppController& a, juce::ValueTree c) : app (a), clipNode (std::move (c))
{
    drumKit = track::isDrumKit (clip::owningTrack (clipNode));
    keyboardWidth = drumKit ? 104 : 64;
    clipNode.addListener (this);
    setWantsKeyboardFocus (true);

    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        gridBox.addItem (gridChoices[i].name, i + 1);
    gridBox.setSelectedId (5, juce::dontSendNotification); // 1/16
    gridBox.onChange = [this] { repaint(); };
    gridBox.setTooltip ("Where new and moved notes snap to");
    addAndMakeVisible (gridBox);

    quantizeButton.setTooltip ("Snap the selected notes (or all notes) to the grid");
    quantizeButton.onClick = [this] { quantizeSelection(); };
    octaveDown.onClick = [this] { transposeSelection (-12); };
    semitoneDown.onClick = [this] { transposeSelection (-1); };
    semitoneUp.onClick = [this] { transposeSelection (1); };
    octaveUp.onClick = [this] { transposeSelection (12); };
    for (auto* b : { &octaveDown, &semitoneDown, &semitoneUp, &octaveUp })
        b->setTooltip ("Transpose the selected notes (or all notes)");
    for (auto* b : { &quantizeButton, &octaveDown, &semitoneDown, &semitoneUp, &octaveUp })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    velocityLabel.setText ("Velocity", juce::dontSendNotification);
    velocityLabel.setFont (theme::font (12.0f));
    velocityLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (velocityLabel);
    velocity.setSliderStyle (juce::Slider::LinearHorizontal);
    velocity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 20);
    velocity.setRange (1.0, 127.0, 1.0);
    velocity.setValue (100.0, juce::dontSendNotification);
    velocity.setTooltip ("Loudness of new notes (1-127); also sets the selected notes");
    velocity.onValueChange = [this]
    {
        if (selection.empty())
            return;
        if (auto* um = undoManager())
        {
            um->beginNewTransaction ("Note Velocity");
            for (auto& n : selection)
                n.setProperty (ids::velocity, velocity.getValue() / 127.0, um);
        }
    };
    addAndMakeVisible (velocity);

    hint.setText ("Click: add   Drag: move   Drag right edge: length   Double-click: delete", juce::dontSendNotification);
    hint.setFont (theme::font (11.0f));
    hint.setColour (juce::Label::textColourId, theme::textDim);
    hint.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (hint);

    scrollToNotes();
}

PianoRollComponent::~PianoRollComponent()
{
    clipNode.removeListener (this);
}

juce::UndoManager* PianoRollComponent::undoManager() const
{
    return app.getUndoManager();
}

//==============================================================================
// Geometry

juce::Rectangle<int> PianoRollComponent::gridArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarHeight + rulerHeight).withTrimmedLeft (keyboardWidth);
}

float PianoRollComponent::xForTick (int64_t tick) const noexcept
{
    return (float) keyboardWidth + (float) ((double) (tick - viewStartTick) * pixelsPerTick);
}

float PianoRollComponent::yForPitch (int pitch) const noexcept
{
    return (float) (toolbarHeight + rulerHeight) + (float) ((topPitch - pitch) * noteRowHeight);
}

int64_t PianoRollComponent::tickAtX (float x) const noexcept
{
    return viewStartTick + (int64_t) std::floor ((double) (x - (float) keyboardWidth) / pixelsPerTick);
}

int PianoRollComponent::pitchAtY (float y) const noexcept
{
    return juce::jlimit (0, 127, topPitch - (int) std::floor ((y - (float) (toolbarHeight + rulerHeight)) / (float) noteRowHeight));
}

juce::Point<float> PianoRollComponent::positionFor (int64_t tick, int pitch) const
{
    return { xForTick (tick), yForPitch (pitch) };
}

juce::Rectangle<float> PianoRollComponent::noteRect (const juce::ValueTree& n) const
{
    const auto start = ticksOf (n[ids::start]);
    const auto length = std::max<int64_t> (1, ticksOf (n[ids::length]));
    const float x0 = xForTick (start), x1 = xForTick (start + length);
    return { x0, yForPitch ((int) n[ids::pitch]) + 1.0f, std::max (3.0f, x1 - x0), (float) noteRowHeight - 2.0f };
}

int64_t PianoRollComponent::getGridTicks() const noexcept
{
    const int index = juce::jlimit (0, (int) std::size (gridChoices) - 1, gridBox.getSelectedId() - 1);
    return gridChoices[index].ticks;
}

void PianoRollComponent::setGridTicks (int64_t ticks)
{
    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        if (gridChoices[i].ticks == ticks)
            gridBox.setSelectedId (i + 1, juce::dontSendNotification);
}

int64_t PianoRollComponent::snap (int64_t tick) const noexcept
{
    const auto grid = getGridTicks();
    return grid <= 1 ? tick : (int64_t) std::llround ((double) tick / (double) grid) * grid;
}

void PianoRollComponent::clipRangeTicks (int64_t& start, int64_t& end) const
{
    // The part of the content that plays: from the clip's offset to its end.
    start = end = 0;
    if (auto* session = app.getSession())
    {
        const auto tempo = midimodel::tempoMap (*session);
        const double rate = session->getSampleRate();
        start = tempo.samplesToTicks (clip::offset (clipNode), rate);
        end = tempo.samplesToTicks (clip::offset (clipNode) + clip::length (clipNode), rate);
    }
}

void PianoRollComponent::scrollToNotes()
{
    int64_t start = 0, end = 0;
    clipRangeTicks (start, end);
    viewStartTick = std::max<int64_t> (0, start - beatTicks / 2);

    // Centre the notes vertically (or middle C if there are none).
    int low = 127, high = 0;
    for (const auto& n : clip::notes (clipNode))
    {
        low = std::min (low, (int) n[ids::pitch]);
        high = std::max (high, (int) n[ids::pitch]);
    }
    const int centre = high >= low ? (low + high) / 2 : drumKit ? 44 : 60; // drums: kick to cymbals
    const int visibleRows = std::max (12, (getHeight() > 0 ? gridArea().getHeight() : 420) / noteRowHeight);
    topPitch = drumKit ? 60 // the whole General MIDI kit (35-59) from the top
                       : juce::jlimit (visibleRows - 1, 127, centre + visibleRows / 2);
}

PianoRollComponent::Hit PianoRollComponent::noteAt (juce::Point<float> p) const
{
    // Topmost (last drawn) first.
    const auto notes = clip::notes (clipNode);
    for (int i = notes.getNumChildren(); --i >= 0;)
    {
        const auto n = notes.getChild (i);
        const auto r = noteRect (n);
        if (r.expanded (0.0f, 1.0f).contains (p))
            return { n, p.x >= r.getRight() - edgeZone && r.getWidth() > edgeZone * 1.5f };
    }
    return {};
}

bool PianoRollComponent::isSelected (const juce::ValueTree& n) const
{
    return std::find (selection.begin(), selection.end(), n) != selection.end();
}

std::vector<juce::ValueTree> PianoRollComponent::targets() const
{
    if (! selection.empty())
        return selection;
    std::vector<juce::ValueTree> all;
    for (const auto& n : clip::notes (clipNode))
        all.push_back (n);
    return all;
}

//==============================================================================
// Painting

void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::well);
    const auto grid = gridArea();

    // Toolbar and ruler backgrounds.
    g.setColour (theme::panel);
    g.fillRect (0, 0, getWidth(), toolbarHeight);
    g.setColour (theme::panelRaised);
    g.fillRect (0, toolbarHeight, getWidth(), rulerHeight);

    // Pitch rows: black-key rows darker, C rows marked.
    for (int pitch = topPitch; pitch >= 0; --pitch)
    {
        const float y = yForPitch (pitch);
        if (y > (float) getHeight())
            break;
        g.setColour (isBlackKey (pitch) ? theme::well.darker (0.25f) : theme::window);
        g.fillRect ((float) grid.getX(), y, (float) grid.getWidth(), (float) noteRowHeight);
        if (pitch % 12 == 0)
        {
            g.setColour (theme::gridBar);
            g.drawHorizontalLine ((int) (y + noteRowHeight) - 1, (float) grid.getX(), (float) grid.getRight());
        }
    }

    // Outside the clip's playing range: shaded.
    int64_t rangeStart = 0, rangeEnd = 0;
    clipRangeTicks (rangeStart, rangeEnd);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    const float rangeLeft = xForTick (rangeStart), rangeRight = xForTick (rangeEnd);
    if (rangeLeft > (float) grid.getX())
        g.fillRect ((float) grid.getX(), (float) grid.getY(), rangeLeft - (float) grid.getX(), (float) grid.getHeight());
    if (rangeRight < (float) grid.getRight())
        g.fillRect (rangeRight, (float) grid.getY(), (float) grid.getRight() - rangeRight, (float) grid.getHeight());

    // Vertical grid: bars, beats and the snap grid; bar numbers in the ruler.
    const auto snapTicks = getGridTicks();
    const int64_t lineStep = snapTicks > 1 && (double) snapTicks * pixelsPerTick >= 6.0 ? snapTicks : beatTicks;
    const int64_t lastTick = tickAtX ((float) getWidth());
    for (int64_t t = (viewStartTick / lineStep) * lineStep; t <= lastTick; t += lineStep)
    {
        if (t < viewStartTick)
            continue;
        const float x = xForTick (t);
        const bool bar = t % barTicks == 0, beat = t % beatTicks == 0;
        g.setColour (bar ? theme::gridBar.brighter (0.3f) : beat ? theme::gridBar : theme::gridLine);
        g.drawVerticalLine ((int) x, (float) grid.getY(), (float) grid.getBottom());
        if (bar)
        {
            g.setColour (theme::text);
            g.setFont (theme::font (11.0f));
            g.drawText (juce::String (t / barTicks + 1), (int) x + 3, toolbarHeight, 40, rulerHeight, juce::Justification::centredLeft);
        }
    }

    // Notes.
    for (const auto& n : clip::notes (clipNode))
    {
        const auto r = noteRect (n);
        if (r.getRight() < (float) grid.getX() || r.getX() > (float) grid.getRight())
            continue;
        const auto start = ticksOf (n[ids::start]);
        const bool plays = start >= rangeStart && start < rangeEnd;
        const float vel = (float) (double) n.getProperty (ids::velocity, 0.8);
        auto colour = juce::Colour (0xff3fbf9f).withMultipliedBrightness (0.55f + 0.45f * vel);
        if (! plays)
            colour = colour.withAlpha (0.35f);
        g.setColour (colour);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (isSelected (n) ? theme::selectionEdge : colour.darker (0.6f));
        g.drawRoundedRectangle (r, 2.0f, isSelected (n) ? 2.0f : 1.0f);
    }

    // Keyboard: piano keys, or for drum tracks the drum each row plays.
    for (int pitch = topPitch; pitch >= 0; --pitch)
    {
        const float y = yForPitch (pitch);
        if (y > (float) getHeight())
            break;
        if (drumKit)
        {
            const char* drum = synth::DrumSynth::nameForNote (pitch);
            g.setColour (drum != nullptr ? theme::panelRaised : theme::well);
            g.fillRect (0.0f, y, (float) keyboardWidth - 1.0f, (float) noteRowHeight - 1.0f);
            g.setColour (drum != nullptr ? theme::text : theme::textDim.withAlpha (0.4f));
            g.setFont (theme::font (10.0f, drum != nullptr));
            g.drawText (drum != nullptr ? juce::String (drum) : juce::String (pitch), 4, (int) y, keyboardWidth - 8, noteRowHeight,
                        juce::Justification::centredLeft);
            continue;
        }
        const bool black = isBlackKey (pitch);
        g.setColour (black ? juce::Colour (0xff202020) : juce::Colour (0xffe9e9e9));
        g.fillRect (0.0f, y, (float) keyboardWidth - (black ? 20.0f : 1.0f), (float) noteRowHeight - 1.0f);
        if (pitch % 12 == 0)
        {
            g.setColour (theme::textOnLight);
            g.setFont (theme::font (10.0f, true));
            g.drawText (noteName (pitch), 4, (int) y, keyboardWidth - 8, noteRowHeight, juce::Justification::centredRight);
        }
    }
    g.setColour (theme::divider);
    g.drawVerticalLine (keyboardWidth - 1, (float) toolbarHeight, (float) getHeight());
}

void PianoRollComponent::resized()
{
    auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (6, 5);
    gridBox.setBounds (bar.removeFromLeft (150));
    bar.removeFromLeft (6);
    quantizeButton.setBounds (bar.removeFromLeft (80));
    bar.removeFromLeft (12);
    for (auto* b : { &octaveDown, &semitoneDown, &semitoneUp, &octaveUp })
        b->setBounds (bar.removeFromLeft (38).reduced (1, 0));
    bar.removeFromLeft (12);
    velocityLabel.setBounds (bar.removeFromLeft (60));
    velocity.setBounds (bar.removeFromLeft (170));
    hint.setBounds (bar);
}

//==============================================================================
// Editing

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    if (! gridArea().contains (e.getPosition()))
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    const auto hit = noteAt (e.position);
    setMouseCursor (! hit.note.isValid() ? juce::MouseCursor::CrosshairCursor
                    : hit.onRightEdge   ? juce::MouseCursor::RightEdgeResizeCursor
                                        : juce::MouseCursor::DraggingHandCursor);
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = Drag::none;
    if (! gridArea().contains (e.getPosition()))
        return;

    auto* um = undoManager();
    if (um == nullptr)
        return;

    auto hit = noteAt (e.position);
    if (! hit.note.isValid())
    {
        // Empty space: create a note one grid step long, then drag sets its length.
        um->beginNewTransaction ("Add Note");
        const auto grid = std::max<int64_t> (getGridTicks(), beatTicks / 8);
        const auto start = std::max<int64_t> (0, getGridTicks() > 1 ? (tickAtX (e.position.x) / grid) * grid : tickAtX (e.position.x));
        hit.note = midimodel::addNote (clipNode, { pitchAtY (e.position.y), (float) (velocity.getValue() / 127.0), start, grid }, um);
        hit.onRightEdge = true;
        selection = { hit.note };
    }
    else if (e.mods.isShiftDown())
    {
        if (isSelected (hit.note))
            selection.erase (std::find (selection.begin(), selection.end(), hit.note));
        else
            selection.push_back (hit.note);
        repaint();
        return;
    }
    else
    {
        if (! isSelected (hit.note))
            selection = { hit.note };
        um->beginNewTransaction (hit.onRightEdge ? "Note Length" : "Move Notes");
        velocity.setValue (std::round ((double) hit.note.getProperty (ids::velocity, 0.8) * 127.0), juce::dontSendNotification);
    }

    drag = hit.onRightEdge ? Drag::resize : Drag::move;
    dragNote = hit.note;
    dragStart = e.position;
    originals.clear();
    for (const auto& n : (drag == Drag::resize ? std::vector<juce::ValueTree> { hit.note } : selection))
        originals.push_back ({ n, ticksOf (n[ids::start]), ticksOf (n[ids::length]), (int) n[ids::pitch] });
    repaint();
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto* um = undoManager();
    if (drag == Drag::none || um == nullptr || originals.empty())
        return;

    const auto deltaTicks = (int64_t) std::llround ((double) (e.position.x - dragStart.x) / pixelsPerTick);
    const auto grid = getGridTicks();

    if (drag == Drag::resize)
    {
        auto& o = originals.front();
        const auto minimum = std::max<int64_t> (1, grid > 1 ? grid : beatTicks / 16);
        const auto newEnd = std::max (o.start + minimum, snap (o.start + o.length + deltaTicks));
        o.note.setProperty (ids::length, tickVar (newEnd - o.start), um);
        return;
    }

    // Move: snap the grabbed note's new start, move everything by the same amount.
    const auto& grabbed = *std::find_if (originals.begin(), originals.end(), [this] (const Original& o) { return o.note == dragNote; });
    const auto shift = snap (grabbed.start + deltaTicks) - grabbed.start;
    const int semis = (int) std::lround ((dragStart.y - e.position.y) / (float) noteRowHeight);
    for (auto& o : originals)
    {
        o.note.setProperty (ids::start, tickVar (std::max<int64_t> (0, o.start + shift)), um);
        o.note.setProperty (ids::pitch, juce::jlimit (0, 127, o.pitch + semis), um);
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent& e)
{
    // A plain click (no drag) on a note in a group selects just that note;
    // dragging it moves the whole group.
    if (drag == Drag::move && e.getDistanceFromDragStart() < 2 && ! e.mods.isShiftDown() && dragNote.isValid())
    {
        selection = { dragNote };
        repaint();
    }
    drag = Drag::none;
    originals.clear();
}

void PianoRollComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = noteAt (e.position);
    if (! hit.note.isValid())
        return;
    if (auto* um = undoManager())
    {
        um->beginNewTransaction ("Delete Note");
        selection.erase (std::remove (selection.begin(), selection.end(), hit.note), selection.end());
        clip::notes (clipNode).removeChild (hit.note, um);
    }
}

void PianoRollComponent::deleteSelection()
{
    auto* um = undoManager();
    if (um == nullptr || selection.empty())
        return;
    um->beginNewTransaction ("Delete Notes");
    auto notes = clip::notes (clipNode);
    for (const auto& n : selection)
        notes.removeChild (n, um);
    selection.clear();
}

void PianoRollComponent::quantizeSelection()
{
    auto* um = undoManager();
    if (um == nullptr || getGridTicks() <= 1)
        return;
    um->beginNewTransaction ("Quantize Notes");
    for (auto& n : targets())
        n.setProperty (ids::start, tickVar (std::max<int64_t> (0, snap (ticksOf (n[ids::start])))), um);
}

void PianoRollComponent::transposeSelection (int semitones)
{
    auto* um = undoManager();
    if (um == nullptr)
        return;
    um->beginNewTransaction ("Transpose Notes");
    for (auto& n : targets())
        n.setProperty (ids::pitch, juce::jlimit (0, 127, (int) n[ids::pitch] + semitones), um);
}

bool PianoRollComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        app.togglePlay();
        return true;
    }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
    {
        selection = targets();
        repaint();
        return true;
    }
    return false;
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
    {
        // Zoom time around the mouse.
        const auto anchor = tickAtX (e.position.x);
        pixelsPerTick = juce::jlimit (0.005, 1.0, pixelsPerTick * (wheel.deltaY > 0 ? 1.25 : 0.8));
        viewStartTick = std::max<int64_t> (0, anchor - (int64_t) ((double) (e.position.x - (float) keyboardWidth) / pixelsPerTick));
    }
    else if (e.mods.isShiftDown())
    {
        viewStartTick = std::max<int64_t> (0, viewStartTick - (int64_t) (wheel.deltaY * 600.0 / pixelsPerTick * 0.1));
    }
    else
    {
        const int visibleRows = gridArea().getHeight() / noteRowHeight;
        topPitch = juce::jlimit (visibleRows - 1, 127, topPitch + (wheel.deltaY > 0 ? 3 : -3));
    }
    repaint();
}

void PianoRollComponent::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    selection.erase (std::remove (selection.begin(), selection.end(), child), selection.end());
    repaint();
}

void PianoRollComponent::valueTreeParentChanged (juce::ValueTree& tree)
{
    if (tree == clipNode && ! clipNode.getParent().isValid() && onClipGone)
        onClipGone();
}

//==============================================================================
PianoRollWindow::PianoRollWindow (AppController& app, juce::ValueTree clipNode, std::function<void()> closed)
    : juce::DocumentWindow ("MIDI Editor - " + track::name (clip::owningTrack (clipNode)) + ": " + clip::name (clipNode),
                            theme::panel, juce::DocumentWindow::closeButton),
      onClose (std::move (closed))
{
    editor = new PianoRollComponent (app, clipNode);
    editor->onClipGone = [this] { closeButtonPressed(); };
    setUsingNativeTitleBar (true);
    setContentOwned (editor, false);
    setResizable (true, true);
    setResizeLimits (520, 260, 4000, 3000);
    centreWithSize (980, 560);
    setVisible (true);
}

void PianoRollWindow::closeButtonPressed()
{
    // The owner deletes the window; do it after this call returns.
    juce::MessageManager::callAsync ([cb = onClose] { if (cb) cb(); });
}

} // namespace meridian
