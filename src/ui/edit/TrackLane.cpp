#include "TrackLane.h"

#include "ui/Theme.h"

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
        addAndMakeVisible (comp);
    }
    layoutClips();
}

void TrackLane::layoutClips()
{
    for (auto* c : clips)
        c->updateLayout (getWidth(), getHeight());
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
}

void TrackLane::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent.hasType (ids::CLIPS))
        rebuildClips();
}

void TrackLane::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent.hasType (ids::CLIPS))
    {
        if (editState.selectedClip == child)
            editState.selectClip ({});
        rebuildClips();
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

    // Bar lines, with beat lines when zoomed in far enough.
    const double samplesPerBeat = timeformat::samplesPerBeat (editState.sampleRate);
    const double samplesPerBar = samplesPerBeat * timeformat::beatsPerBar;
    const bool drawBeats = samplesPerBeat / editState.samplesPerPixel >= 12.0;
    const double step = drawBeats ? samplesPerBeat : samplesPerBar;
    if (step / editState.samplesPerPixel >= 6.0)
    {
        const auto first = (juce::int64) std::floor ((double) editState.viewStart / step);
        const auto last = (juce::int64) std::ceil ((double) editState.xToSample (getWidth()) / step);
        for (auto i = first; i <= last; ++i)
        {
            const auto sample = (juce::int64) std::llround ((double) i * step);
            const bool isBar = ! drawBeats || i % timeformat::beatsPerBar == 0;
            g.setColour (isBar ? theme::gridBar : theme::gridLine);
            g.drawVerticalLine ((int) editState.sampleToX (sample), 0.0f, (float) getHeight());
        }
    }

    if (track::isAux (trackNode))
    {
        g.setColour (theme::textDim.withAlpha (0.5f));
        g.setFont (theme::font (12.0f));
        g.drawText ("Aux Input: plays what is sent or routed to it (see the Mix window)",
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

    g.setColour (theme::playhead);
    g.drawVerticalLine (playheadX(), 0.0f, (float) getHeight());
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
    draggingSelection = false;
    selectionAnchor = editState.snap (juce::jmax<juce::int64> (0, editState.xToSample (e.position.x)));
    editState.setTimeSelection (selectionAnchor, selectionAnchor, { track::id (trackNode) });
}

void TrackLane::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 3 && ! draggingSelection)
        return;
    draggingSelection = true;

    const auto here = editState.snap (juce::jmax<juce::int64> (0, editState.xToSample (e.position.x)));
    editState.setTimeSelection (selectionAnchor, here, { track::id (trackNode) });
}

void TrackLane::mouseUp (const juce::MouseEvent&)
{
    // A click without a drag places the playback cursor (the insertion point).
    if (! draggingSelection)
        app.locate (selectionAnchor);
}

} // namespace meridian
