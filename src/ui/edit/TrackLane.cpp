#include "TrackLane.h"

#include "AutomationView.h"
#include "model/Automation.h"
#include "model/MidiModel.h"
#include "ui/Theme.h"

#include <dsp/FaderTaper.h>

namespace meridian
{

TrackLane::TrackLane (AppController& a, EditState& s, juce::ValueTree t)
    : app (a), editState (s), trackNode (std::move (t))
{
    trackNode.addListener (this);
    editState.addChangeListener (this);
    rebuildClips();
}

TrackLane::~TrackLane()
{
    editState.removeChangeListener (this);
    trackNode.removeListener (this);
}

void TrackLane::rebuildClips()
{
    clips.clear();
    for (const auto& c : track::clips (trackNode))
    {
        auto* comp = clips.add (new ClipComponent (app, editState, c, track::colour (trackNode)));
        comp->trackAtScreenPosition = [this] (juce::Point<int> p) { return trackAtScreenPosition ? trackAtScreenPosition (p) : juce::ValueTree(); };
        comp->openElasticDialog = [this] (juce::ValueTree clipNode) { if (openElasticDialog) openElasticDialog (clipNode); };
        comp->openPianoRoll = [this] (juce::ValueTree clipNode) { if (openPianoRoll) openPianoRoll (clipNode); };
        addAndMakeVisible (comp);
    }
    layoutClips();
}

void TrackLane::layoutClips()
{
    // With the Pencil, clicks go to the lane (automation), not to the clips.
    const bool clipsClickable = editState.tool != EditTool::pencil;
    for (auto* c : clips)
    {
        c->updateLayout (getWidth(), getHeight());
        c->setInterceptsMouseClicks (clipsClickable, clipsClickable);
    }
    repaint();
}

void TrackLane::resized()
{
    layoutClips();
}

int TrackLane::playheadX() const
{
    return (int) juce::jlimit (-1.0e6, 1.0e6, editState.sampleToX (playhead));
}

void TrackLane::setPlayhead (juce::int64 sample)
{
    if (sample == playhead)
        return;
    const int oldX = playheadX();
    playhead = sample;
    const int newX = playheadX();
    repaint (oldX - 1, 0, 3, getHeight());
    repaint (newX - 1, 0, 3, getHeight());
}

//==============================================================================
void TrackLane::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (ids::CLIP) && (property == ids::start || property == ids::length || property == ids::offset))
        layoutClips();
    else if (tree == trackNode && property == ids::colour)
        rebuildClips();
    else if (tree.hasType (ids::POINT) || tree.hasType (ids::SEND)
             || (tree == trackNode && (property == ids::automationMode || property == ids::volumeDb || property == ids::pan || property == ids::mute)))
        repaint(); // automation line, or the guide showing the control's own setting
}

void TrackLane::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent.hasType (ids::CLIPS))
        rebuildClips();
    else if (parent.hasType (ids::LANE) || parent.hasType (ids::AUTOMATION))
        repaint();
}

void TrackLane::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent.hasType (ids::CLIPS))
    {
        if (editState.selectedClip == child)
            editState.selectClip ({});
        rebuildClips();
    }
    else if (parent.hasType (ids::LANE) || parent.hasType (ids::AUTOMATION))
    {
        repaint();
    }
}

void TrackLane::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent.hasType (ids::CLIPS))
        rebuildClips();
}

void TrackLane::changeListenerCallback (juce::ChangeBroadcaster*)
{
    layoutClips(); // view, tool or selection changed
}

//==============================================================================
void TrackLane::paint (juce::Graphics& g)
{
    g.fillAll (theme::well);

    // Bar lines, with beat lines when zoomed in far enough (tempo and meter maps).
    {
        const auto& meter = editState.meterMap;
        const auto firstBar = std::max<juce::int64> (0, meter.positionOf (editState.sampleToTicks (editState.viewStart)).bar);
        const auto lastBar = meter.positionOf (editState.sampleToTicks (editState.xToSample (getWidth()))).bar + 1;
        for (auto bar = firstBar; bar <= lastBar; ++bar)
        {
            const auto& m = meter.meterAt (bar);
            const auto barStart = meter.barToTicks (bar);
            const auto barX = editState.sampleToX (editState.ticksToSample (barStart));
            const auto nextBarX = editState.sampleToX (editState.ticksToSample (meter.barToTicks (bar + 1)));
            if (nextBarX - barX < 6.0)
                continue; // too dense to draw
            g.setColour (theme::gridBar);
            g.drawVerticalLine ((int) barX, 0.0f, (float) getHeight());
            if ((nextBarX - barX) / m.numerator >= 12.0)
            {
                g.setColour (theme::gridLine);
                for (int beat = 1; beat < m.numerator; ++beat)
                    g.drawVerticalLine ((int) editState.sampleToX (editState.ticksToSample (barStart + beat * midi::MeterMap::ticksPerBeat (m))),
                                        0.0f, (float) getHeight());
            }
        }
    }

    if (track::isAux (trackNode))
    {
        g.setColour (theme::textDim.withAlpha (0.5f));
        g.setFont (theme::font (12.0f));
        g.drawText ("Aux Input: plays what is sent or routed to it (see the Mix window)",
                    getLocalBounds().reduced (12, 0), juce::Justification::centredLeft);
    }
    else if (track::isInstrument (trackNode) && track::clips (trackNode).getNumChildren() == 0)
    {
        g.setColour (theme::textDim.withAlpha (0.5f));
        g.setFont (theme::font (12.0f));
        g.drawText ("Instrument: double-click to create a MIDI clip",
                    getLocalBounds().reduced (12, 0), juce::Justification::centredLeft);
    }

    g.setColour (theme::divider);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void TrackLane::paintOverChildren (juce::Graphics& g)
{
    // Time selection on top of the clips, as in a console DAW.
    if (editState.hasTimeRange() && editState.selectedTrackIds.contains (track::id (trackNode)))
    {
        const auto x1 = (float) juce::jlimit (-10.0, getWidth() + 10.0, editState.sampleToX (editState.selectionStart));
        const auto x2 = (float) juce::jlimit (-10.0, getWidth() + 10.0, editState.sampleToX (editState.selectionEnd));
        g.setColour (theme::selection);
        g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, (float) getHeight()));
        g.setColour (theme::selectionEdge);
        g.drawVerticalLine ((int) x1, 0.0f, (float) getHeight());
        g.drawVerticalLine ((int) x2, 0.0f, (float) getHeight());
    }

    paintAutomation (g);

    g.setColour (theme::playhead);
    g.drawVerticalLine (playheadX(), 0.0f, (float) getHeight());
}

//==============================================================================
// Automation. Each lane has its own vertical scale:
//   volume, sends   the mixer fader's taper (0 dB at 75 % of the height)
//   pan             left at the top, right at the bottom, centre in the middle
//   mute            muted at the top, unmuted at the bottom; drawn as steps
//   plug-in         the control's normalised range, 1 at the top

juce::String TrackLane::shownParameter() const
{
    return automation::shownParameter (trackNode, editState.automationViewFor (track::id (trackNode)));
}

float TrackLane::valueToY (double value) const
{
    const auto parameter = shownParameter();
    const bool linear = parameter == "mute" || automation::pluginTarget (parameter).first >= 0;
    const double position = parameter == "pan" ? (1.0 - value) * 0.5
                          : linear             ? juce::jlimit (0.0, 1.0, value)
                                               : dsp::FaderTaper::dbToPosition (value);
    const float usable = (float) getHeight() - 8.0f;
    return 4.0f + usable * (float) (1.0 - position);
}

double TrackLane::yToValue (float y) const
{
    const auto parameter = shownParameter();
    const float usable = juce::jmax (1.0f, (float) getHeight() - 8.0f);
    const double position = juce::jlimit (0.0, 1.0, 1.0 - (double) (y - 4.0f) / usable);
    if (parameter == "pan")
        return 1.0 - 2.0 * position;
    if (parameter == "mute")
        return position >= 0.5 ? 1.0 : 0.0;
    if (automation::pluginTarget (parameter).first >= 0)
        return position;
    return dsp::FaderTaper::positionToDb (position);
}

void TrackLane::paintAutomation (juce::Graphics& g)
{
    const bool pencil = editState.tool == EditTool::pencil;
    const auto parameter = shownParameter();
    const auto pts = automation::points (automation::lane (trackNode, parameter));
    if (pts.empty() && ! pencil)
        return;

    const bool active = track::automationRead (trackNode);
    const auto colour = (active ? theme::automation : theme::textDim).withAlpha (pts.empty() ? 0.5f : 0.95f);

    if (pencil)
    {
        g.setColour (colour);
        g.setFont (theme::font (11.0f));
        g.drawText (automationview::laneName (app, trackNode, parameter), 6, 2, 200, 14, juce::Justification::centredLeft);
    }

    if (pts.empty())
    {
        // Pencil over an unautomated control: show where it is set, as a guide.
        const float y = valueToY (automationview::staticValue (app, trackNode, parameter));
        const float dashes[] = { 4.0f, 4.0f };
        g.setColour (colour);
        g.drawDashedLine (juce::Line<float> (0.0f, y, (float) getWidth(), y), dashes, 2, 1.0f);
        return;
    }

    // Mute changes in steps (the engine never ramps between its breakpoints).
    const bool stepped = parameter == "mute";
    juce::Path line;
    float lastY = valueToY (automation::valueAt (pts, editState.xToSample (0.0)));
    line.startNewSubPath (0.0f, lastY);
    for (const auto& p : pts)
    {
        const auto x = (float) juce::jlimit (-10.0, getWidth() + 10.0, editState.sampleToX (p.time));
        const auto y = valueToY (p.value);
        if (stepped)
            line.lineTo (x, lastY);
        line.lineTo (x, y);
        lastY = y;
    }
    line.lineTo ((float) getWidth(), lastY);

    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (1.6f));

    for (const auto& p : pts)
    {
        const auto x = (float) editState.sampleToX (p.time);
        if (x >= -4.0f && x <= getWidth() + 4.0f)
            g.fillRect (juce::Rectangle<float> (x - 2.5f, valueToY (p.value) - 2.5f, 5.0f, 5.0f));
    }
}

void TrackLane::pencilDown (const juce::MouseEvent& e)
{
    auto* um = app.getUndoManager();
    if (um == nullptr)
        return;

    um->beginNewTransaction ("Draw Automation");
    auto lane = automation::getOrCreateLane (trackNode, shownParameter(), um);
    const auto time = std::max<juce::int64> (0, editState.xToSample (e.position.x));
    const auto tolerance = (juce::int64) (6.0 * editState.samplesPerPixel);

    // Near an existing point: Alt-click deletes it, a drag moves it.
    juce::ValueTree nearest;
    juce::int64 nearestDistance = tolerance + 1;
    for (const auto& p : lane)
    {
        const auto distance = std::abs ((juce::int64) p[ids::time] - time);
        if (distance < nearestDistance && std::abs (valueToY ((double) p[ids::value]) - e.position.y) < 8.0f)
        {
            nearestDistance = distance;
            nearest = p;
        }
    }

    if (nearest.isValid())
    {
        if (e.mods.isAltDown())
        {
            lane.removeChild (nearest, um);
            pencilAction = PencilAction::none;
        }
        else
        {
            draggedPoint = nearest;
            pencilAction = PencilAction::movePoint;
        }
        return;
    }

    automation::addPoint (lane, time, yToValue (e.position.y), um);
    lastDrawnTime = time;
    lastDrawnX = e.position.x;
    pencilAction = PencilAction::draw;
}

void TrackLane::pencilDrag (const juce::MouseEvent& e)
{
    auto* um = app.getUndoManager();
    if (um == nullptr)
        return;

    const auto time = std::max<juce::int64> (0, editState.xToSample (e.position.x));

    if (pencilAction == PencilAction::movePoint && draggedPoint.isValid())
    {
        draggedPoint.setProperty (ids::time, time, um);
        draggedPoint.setProperty (ids::value, yToValue (e.position.y), um);
        return;
    }

    if (pencilAction != PencilAction::draw || std::abs (e.position.x - lastDrawnX) < 3.0f)
        return; // thin freehand strokes to a point every few pixels

    // Freehand: what the stroke passes over is replaced.
    auto lane = automation::lane (trackNode, shownParameter());
    if (time > lastDrawnTime)
        automation::removeRange (lane, lastDrawnTime + 1, time, um);
    else
        automation::removeRange (lane, time, lastDrawnTime - 1, um);
    automation::addPoint (lane, time, yToValue (e.position.y), um);
    lastDrawnTime = time;
    lastDrawnX = e.position.x;
}

void TrackLane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if ((e.mods.isCommandDown() || e.mods.isShiftDown()) && onModifiedWheel)
        onModifiedWheel (e, wheel);
    else
        juce::Component::mouseWheelMove (e, wheel); // passes up to the viewport
}

void TrackLane::mouseDown (const juce::MouseEvent& e)
{
    if (editState.tool == EditTool::pencil)
    {
        pencilDown (e);
        return;
    }

    draggingSelection = false;
    selectionAnchor = editState.snap (std::max<juce::int64> (0, editState.xToSample (e.position.x)));
    editState.setTimeSelection (selectionAnchor, selectionAnchor, { track::id (trackNode) });
}

void TrackLane::mouseDrag (const juce::MouseEvent& e)
{
    if (editState.tool == EditTool::pencil)
    {
        pencilDrag (e);
        return;
    }

    if (e.getDistanceFromDragStart() < 3 && ! draggingSelection)
        return;
    draggingSelection = true;

    const auto here = editState.snap (std::max<juce::int64> (0, editState.xToSample (e.position.x)));
    editState.setTimeSelection (selectionAnchor, here, { track::id (trackNode) });
}

void TrackLane::mouseUp (const juce::MouseEvent&)
{
    if (editState.tool == EditTool::pencil)
    {
        pencilAction = PencilAction::none;
        draggedPoint = {};
        return;
    }

    // A click without a drag places the playback cursor (the insertion point).
    if (! draggingSelection)
        app.locate (selectionAnchor);
}

void TrackLane::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto* session = app.getSession();
    if (session == nullptr || ! track::isInstrument (trackNode) || editState.tool == EditTool::pencil)
        return;

    // A four-bar MIDI clip (in the session's meter), starting at the beginning of the bar that was clicked.
    const auto tempo = midimodel::tempoMap (*session);
    const auto meter = midimodel::meterMap (*session);
    const double rate = session->getSampleRate();
    const auto tick = tempo.samplesToTicks (std::max<juce::int64> (0, editState.xToSample (e.position.x)), rate);
    const auto bar = std::max<int64_t> (0, meter.positionOf (tick).bar);
    const auto start = tempo.ticksToSamples (meter.barToTicks (bar), rate);
    const auto end = tempo.ticksToSamples (meter.barToTicks (bar + 4), rate);

    auto& um = session->getUndoManager();
    um.beginNewTransaction ("New MIDI Clip");
    editState.selectClip (session->addMidiClip (trackNode, start, end - start, "MIDI", &um));
}

} // namespace meridian
