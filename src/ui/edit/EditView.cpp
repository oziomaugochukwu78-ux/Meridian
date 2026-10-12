#include "EditView.h"

#include "ui/Theme.h"
#include "ui/dialogs/TimeStretchComponent.h"

namespace meridian
{
namespace
{
constexpr int toolbarHeight = 36;
constexpr int scrollBarHeight = 14;
} // namespace

EditView::EditView (AppController& a) : app (a)
{
    addAndMakeVisible (toolbar);
    addAndMakeVisible (ruler);

    viewport.setViewedComponent (&trackContainer, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (scrollBarHeight);
    addAndMakeVisible (viewport);

    horizontalScroll.setAutoHide (false);
    horizontalScroll.addListener (this);
    addAndMakeVisible (horizontalScroll);

    emptyHint.setText ("No tracks yet.  Track > New Track (Ctrl+Shift+N), or drop audio files here.",
                       juce::dontSendNotification);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setColour (juce::Label::textColourId, theme::textDim);
    emptyHint.setInterceptsMouseClicks (false, false);
    addChildComponent (emptyHint);

    ruler.onLocate = [this] (juce::int64 sample) { app.locate (sample); };

    state.addChangeListener (this);
    app.addChangeListener (this);
    bindSession();
}

EditView::~EditView()
{
    if (tracksNode.isValid())
        tracksNode.removeListener (this);
    app.removeChangeListener (this);
    state.removeChangeListener (this);
    horizontalScroll.removeListener (this);
}

//==============================================================================
void EditView::bindSession()
{
    pianoRoll.reset(); // its clip belonged to the previous session

    if (tracksNode.isValid())
        tracksNode.removeListener (this);

    auto* session = app.getSession();
    tracksNode = session != nullptr ? session->getTracksNode() : juce::ValueTree();
    if (tracksNode.isValid())
        tracksNode.addListener (this);

    state.sampleRate = session != nullptr ? session->getSampleRate() : 48000.0;
    state.tempo = session != nullptr ? session->getTempo() : timeformat::tempoBpm;
    state.clearSelection();
    state.setView (0, state.samplesPerPixel);
    rebuildTracks();
}

void EditView::rebuildTracks()
{
    headers.clear();
    lanes.clear();

    for (const auto& t : tracksNode)
    {
        auto* header = headers.add (new TrackHeader (app, state, t));
        trackContainer.addAndMakeVisible (header);

        auto* lane = lanes.add (new TrackLane (app, state, t));
        lane->trackAtScreenPosition = [this] (juce::Point<int> p) { return trackAtScreenPosition (p); };
        lane->openElasticDialog = [this] (juce::ValueTree c) { openElasticDialog (c); };
        lane->openPianoRoll = [this] (juce::ValueTree c) { openPianoRoll (c); };
        lane->onModifiedWheel = [this] (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
        {
            if (e.mods.isCommandDown())
                state.zoom (w.deltaY > 0 ? 0.8 : 1.25, e.position.x); // e is in lane coordinates
            else
                state.setView (state.viewStart - (juce::int64) (w.deltaY * 400.0 * state.samplesPerPixel), state.samplesPerPixel);
        };
        trackContainer.addAndMakeVisible (lane);
    }

    emptyHint.setVisible (headers.isEmpty() && app.getSession() != nullptr);
    layoutTracks();
}

void EditView::layoutTracks()
{
    const int width = viewport.getMaximumVisibleWidth();
    const int height = state.trackHeight;
    trackContainer.setSize (width, juce::jmax (1, headers.size() * height));

    for (int i = 0; i < headers.size(); ++i)
    {
        headers[i]->setBounds (0, i * height, trackHeaderWidth, height);
        lanes[i]->setBounds (trackHeaderWidth, i * height, juce::jmax (1, width - trackHeaderWidth), height);
    }
    updateScrollBar();
}

juce::int64 EditView::sessionLength() const
{
    juce::int64 end = 0;
    for (const auto& t : tracksNode)
        for (const auto& c : track::clips (t))
            end = juce::jmax (end, clip::end (c));

    const auto padding = (juce::int64) (30.0 * state.sampleRate);
    const auto minimum = (juce::int64) (300.0 * state.sampleRate);
    return juce::jmax (minimum, end + padding, app.getEngine().getTransport().getPosition() + padding);
}

void EditView::updateScrollBar()
{
    const double visible = state.pixelsToSamples (juce::jmax (1, viewport.getMaximumVisibleWidth() - trackHeaderWidth));
    horizontalScroll.setRangeLimits (0.0, (double) sessionLength(), juce::dontSendNotification);
    horizontalScroll.setCurrentRange ((double) state.viewStart, visible, juce::dontSendNotification);
}

void EditView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    state.setView ((juce::int64) newRangeStart, state.samplesPerPixel);
}

juce::ValueTree EditView::trackAtScreenPosition (juce::Point<int> screenPosition) const
{
    for (auto* lane : lanes)
        if (lane->getScreenBounds().contains (screenPosition))
            return lane->getTrack();
    return {};
}

//==============================================================================
void EditView::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &app)
    {
        bindSession(); // a different session was opened
        return;
    }
    updateScrollBar();
    ruler.repaint();
    for (auto* h : headers)
        h->repaint();
}

void EditView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == tracksNode)
        rebuildTracks();
}

void EditView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == tracksNode)
        rebuildTracks();
}

void EditView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == tracksNode)
        rebuildTracks();
}

void EditView::visibilityChanged()
{
    if (isVisible())
        startTimerHz (30);
    else
        stopTimer();
}

void EditView::timerCallback()
{
    // Follow tempo changes (from the AI tools or undo): grid, ruler and clips are drawn in beats.
    if (auto* session = app.getSession(); session != nullptr && ! juce::exactlyEqual (session->getTempo(), state.tempo))
    {
        state.tempo = session->getTempo();
        state.sendChangeMessage();
        repaint();
    }

    const auto& transport = app.getEngine().getTransport();
    const auto position = transport.getPosition();

    // Page-scroll: when the cursor leaves the visible range during playback,
    // jump so it sits near the left edge.
    if (transport.isRolling())
    {
        const double x = state.sampleToX (position);
        const double visibleWidth = viewport.getMaximumVisibleWidth() - trackHeaderWidth;
        if (x > visibleWidth - 10.0 || x < 0.0)
            state.setView (position - (juce::int64) state.pixelsToSamples (20.0), state.samplesPerPixel);
    }

    ruler.setPlayheadPosition (position);
    for (auto* lane : lanes)
        lane->setPlayhead (position);
    for (auto* header : headers)
        header->updateMeter();
}

//==============================================================================
void EditView::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
}

void EditView::resized()
{
    auto area = getLocalBounds();
    toolbar.setBounds (area.removeFromTop (toolbarHeight));
    ruler.setBounds (area.removeFromTop (TimelineRuler::rowHeight * 2));

    auto bottom = area.removeFromBottom (scrollBarHeight);
    horizontalScroll.setBounds (bottom.withTrimmedLeft (trackHeaderWidth).withTrimmedRight (scrollBarHeight));

    viewport.setBounds (area);
    emptyHint.setBounds (area);
    layoutTracks();
}

void EditView::zoomBy (double factor)
{
    // Zoom around the cursor if it is on screen, otherwise around the left edge.
    const double x = state.sampleToX (app.getEngine().getTransport().getPosition());
    const double width = viewport.getMaximumVisibleWidth() - trackHeaderWidth;
    state.zoom (factor, x >= 0.0 && x <= width ? x : 0.0);
}

//==============================================================================
// Editing

juce::Array<juce::ValueTree> EditView::tracksForEdit() const
{
    juce::Array<juce::ValueTree> result;
    if (auto* session = app.getSession())
    {
        for (const int id : state.selectedTrackIds)
            if (auto t = session->getTrackById (id); t.isValid())
                result.add (t);
        if (result.isEmpty() && state.selectedClip.isValid())
            result.add (clip::owningTrack (state.selectedClip));
    }
    return result;
}

void EditView::separateAtSelection()
{
    if (auto* session = app.getSession())
    {
        session->getUndoManager().beginNewTransaction ("Separate Clip");
        splitTracksAtSelection();
    }
}

void EditView::splitTracksAtSelection()
{
    auto* session = app.getSession();
    auto& um = session->getUndoManager();

    juce::Array<juce::int64> cuts;
    if (state.hasTimeRange())
        cuts = { state.selectionStart, state.selectionEnd };
    else
        cuts = { app.getEngine().getTransport().getPosition() };

    for (auto t : tracksForEdit())
    {
        for (const auto cut : cuts)
        {
            // Copy the list first: splitting inserts siblings.
            juce::Array<juce::ValueTree> clipsHere;
            for (const auto& c : track::clips (t))
                clipsHere.add (c);
            for (auto& c : clipsHere)
                session->splitClip (c, cut, &um);
        }
    }
}

void EditView::deleteSelection()
{
    auto* session = app.getSession();
    if (session == nullptr)
        return;

    auto& um = session->getUndoManager();

    if (state.selectedClip.isValid() && state.selectedClip.getParent().isValid())
    {
        um.beginNewTransaction ("Delete Clip");
        auto c = state.selectedClip;
        state.selectClip ({});
        c.getParent().removeChild (c, &um);
        return;
    }

    if (! state.hasTimeRange())
        return;

    // One undo step: split at the selection edges, then remove what lies inside.
    um.beginNewTransaction ("Clear Selection");
    splitTracksAtSelection();
    for (auto t : tracksForEdit())
    {
        juce::Array<juce::ValueTree> doomed;
        for (const auto& c : track::clips (t))
            if (clip::start (c) >= state.selectionStart && clip::end (c) <= state.selectionEnd)
                doomed.add (c);
        for (auto& c : doomed)
            track::clips (t).removeChild (c, &um);
    }
}

void EditView::openElasticDialog (juce::ValueTree c)
{
    if (! c.isValid())
        c = state.selectedClip;
    if (! c.isValid())
    {
        if (app.showProblem)
            app.showProblem ("Elastic Audio", "Select a clip first (click it with the Grabber, or the lower half with the Smart Tool).");
        return;
    }
    TimeStretchComponent::show (app, c);
}

void EditView::openPianoRoll (juce::ValueTree c)
{
    if (! c.isValid() || ! clip::isMidi (c))
        return;
    if (pianoRoll != nullptr && pianoRoll->getEditor().getClip() == c)
    {
        pianoRoll->toFront (true);
        return;
    }
    pianoRoll = std::make_unique<PianoRollWindow> (app, c, [safe = juce::Component::SafePointer<EditView> (this)]
    {
        if (safe != nullptr)
            safe->pianoRoll.reset();
    });
}

//==============================================================================
bool EditView::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (app.getSession() == nullptr)
        return false;
    if (audioFileFilter == nullptr)
        audioFileFilter = std::make_unique<juce::WildcardFileFilter> (app.getFormatManager().getWildcardForAllFormats(), "", "Audio");
    for (const auto& f : files)
        if (audioFileFilter->isFileSuitable (juce::File (f)))
            return true;
    return false;
}

void EditView::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::Array<juce::File> audioFiles;
    for (const auto& f : files)
        if (audioFileFilter != nullptr && audioFileFilter->isFileSuitable (juce::File (f)))
            audioFiles.add (juce::File (f));
    if (audioFiles.isEmpty())
        return;

    const auto position = state.snap (std::max<juce::int64> (0, state.xToSample (x - trackHeaderWidth)));
    const auto target = trackAtScreenPosition (localPointToGlobal (juce::Point<int> (x, y)));
    app.importAudioFiles (audioFiles, position, target);
}

} // namespace meridian
