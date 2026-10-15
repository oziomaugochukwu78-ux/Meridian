#include "EditView.h"
#include "model/Fades.h"
#include "model/Groups.h"
#include "model/MidiModel.h"
#include "model/Shuffle.h"

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
    ruler.undoManager = [this] { return app.getUndoManager(); };
    ruler.getSession = [this] { return app.getSession(); };
    state.linkTracks = [this] (const juce::Array<int>& trackIds)
    {
        auto* session = app.getSession();
        return session != nullptr ? groups::expandForEdit (*session, trackIds) : trackIds;
    };

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

    ruler.setSessionState (session != nullptr ? session->getState() : juce::ValueTree());
    state.sampleRate = session != nullptr ? session->getSampleRate() : 48000.0;
    state.tempo = session != nullptr ? session->getTempo() : timeformat::tempoBpm;
    state.tempoMap = session != nullptr ? midimodel::tempoMap (*session) : midi::TempoMap (timeformat::tempoBpm);
    state.meterMap = session != nullptr ? midimodel::meterMap (*session) : midi::MeterMap();
    mapsSeenAt = session != nullptr ? session->getChangeCount() : ~juce::uint64 (0);
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
    // Follow tempo and meter changes (from the AI tools, menus or undo): grid, ruler and
    // MIDI clips are drawn in beats. Rebuilt only when the session changed at all.
    if (auto* session = app.getSession(); session != nullptr && session->getChangeCount() != mapsSeenAt)
    {
        mapsSeenAt = session->getChangeCount();
        for (auto* h : headers)
            h->refreshGroups();
        auto tempo = midimodel::tempoMap (*session);
        auto meter = midimodel::meterMap (*session);
        const auto& t1 = tempo.getChanges();
        const auto& t2 = state.tempoMap.getChanges();
        const auto& m1 = meter.getChanges();
        const auto& m2 = state.meterMap.getChanges();
        bool same = t1.size() == t2.size() && m1.size() == m2.size();
        for (size_t i = 0; same && i < t1.size(); ++i)
            same = t1[i].tick == t2[i].tick && juce::exactlyEqual (t1[i].bpm, t2[i].bpm);
        for (size_t i = 0; same && i < m1.size(); ++i)
            same = m1[i].bar == m2[i].bar && m1[i].numerator == m2[i].numerator && m1[i].denominator == m2[i].denominator;
        if (! same)
        {
            state.tempoMap = std::move (tempo);
            state.meterMap = std::move (meter);
            state.tempo = session->getTempo();
            state.sendChangeMessage();
            repaint();
        }
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
    ruler.setBounds (area.removeFromTop (TimelineRuler::rowHeight * TimelineRuler::numRows));

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

void EditView::deleteSelection (const juce::String& actionName)
{
    auto* session = app.getSession();
    if (session == nullptr)
        return;

    auto& um = session->getUndoManager();

    const bool shuffling = state.mode == EditMode::shuffle;

    if (state.selectedClip.isValid() && state.selectedClip.getParent().isValid())
    {
        um.beginNewTransaction (actionName.isNotEmpty() ? actionName : juce::String ("Delete Clip"));
        auto c = state.selectedClip;
        state.selectClip ({});
        if (shuffling)
            shuffle::deleteClip (c, &um);
        else
            c.getParent().removeChild (c, &um);
        return;
    }

    if (! state.hasTimeRange())
        return;

    // One undo step: split at the selection edges, then remove what lies inside.
    um.beginNewTransaction (actionName.isNotEmpty() ? actionName : juce::String ("Clear Selection"));
    splitTracksAtSelection();
    for (auto t : tracksForEdit())
    {
        juce::Array<juce::ValueTree> doomed;
        for (const auto& c : track::clips (t))
            if (clip::start (c) >= state.selectionStart && clip::end (c) <= state.selectionEnd)
                doomed.add (c);
        for (auto& c : doomed)
            track::clips (t).removeChild (c, &um);
        if (shuffling)
            shuffle::closeRange (t, state.selectionStart, state.selectionEnd, &um);
    }
}

stripsilence::Result EditView::stripSilence (const stripsilence::Params& params)
{
    stripsilence::Result total;
    auto* session = app.getSession();
    if (session == nullptr || ! state.hasTimeRange())
        return total;
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Strip Silence");
    for (auto t : tracksForEdit())
    {
        if (! track::isAudio (t))
            continue;
        const auto r = stripsilence::apply (*session, app.getFormatManager(), t, state.selectionStart, state.selectionEnd, params, &um);
        total.clipsAnalysed += r.clipsAnalysed;
        total.clipsMade += r.clipsMade;
        total.samplesRemoved += r.samplesRemoved;
        total.unreadable.addArray (r.unreadable);
    }
    return total;
}

void EditView::fadeSelection()
{
    auto* session = app.getSession();
    if (session == nullptr || ! state.hasTimeRange())
        return;
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Fade/Crossfade");
    const auto shape = app.getSettings().getBoolValue ("crossfadeEqualGain", false) ? fades::Shape::equalGain
                                                                                     : fades::Shape::equalPower;
    const auto batch = (juce::int64) std::llround (fades::defaultBatchSeconds * session->getSampleRate());
    int impossible = 0;
    for (auto t : tracksForEdit())
        impossible += fades::apply (t, state.selectionStart, state.selectionEnd, shape, batch, &um).impossible;
    if (impossible > 0 && app.showProblem)
        app.showProblem ("Fade/Crossfade", juce::String (impossible) + (impossible == 1 ? " join" : " joins")
                                           + " could not be crossfaded: neither clip has recorded audio past the join to fade over. "
                                             "Trim the clips back a little, then try again.");
}

int EditView::getNudgeMilliseconds() const
{
    const int ms = app.getSettings().getIntValue ("nudgeMilliseconds", 10);
    return ms == 1 || ms == 10 || ms == 100 || ms == 1000 ? ms : 10;
}

void EditView::setNudgeMilliseconds (int ms)
{
    app.getSettings().setValue ("nudgeMilliseconds", ms);
}

void EditView::nudge (int direction)
{
    auto* session = app.getSession();
    if (session == nullptr || direction == 0 || state.mode == EditMode::shuffle)
        return;
    auto delta = (juce::int64) std::llround (getNudgeMilliseconds() * 0.001 * session->getSampleRate()) * (direction > 0 ? 1 : -1);

    juce::Array<juce::ValueTree> moving;
    const bool clipSelected = state.selectedClip.isValid() && state.selectedClip.getParent().isValid();
    if (clipSelected)
        moving.add (state.selectedClip);
    else if (state.hasTimeRange())
        for (const auto& t : tracksForEdit())
            for (const auto& c : track::clips (t))
                if (clip::start (c) >= state.selectionStart && clip::end (c) <= state.selectionEnd)
                    moving.add (c);
    if (moving.isEmpty())
        return;

    // Everything moves by the same amount, so nothing goes before the session start.
    for (const auto& c : moving)
        delta = std::max (delta, -clip::start (c));
    if (delta == 0)
        return;

    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Nudge");
    for (auto& c : moving)
        c.setProperty (ids::start, clip::start (c) + delta, &um);
    if (! clipSelected)
        state.setTimeSelection (std::max<juce::int64> (0, state.selectionStart + delta), state.selectionEnd + delta, state.selectedTrackIds);
}

bool EditView::hasSelectionToCopy() const
{
    if (state.selectedClip.isValid() && state.selectedClip.getParent().isValid())
        return true;
    return state.hasTimeRange() && ! tracksForEdit().isEmpty();
}

void EditView::copySelection()
{
    if (app.getSession() == nullptr)
        return;
    if (state.selectedClip.isValid() && state.selectedClip.getParent().isValid())
        clipboardContents = clipboard::copyClip (state.selectedClip);
    else if (state.hasTimeRange())
        clipboardContents = clipboard::copyRange (tracksForEdit(), state.selectionStart, state.selectionEnd);
}

void EditView::cutSelection()
{
    if (! hasSelectionToCopy())
        return;
    copySelection();
    deleteSelection ("Cut");
}

void EditView::pasteClipboard()
{
    auto* session = app.getSession();
    if (session == nullptr || clipboardContents.isEmpty())
        return;

    juce::int64 at = app.getEngine().getTransport().getPosition();
    juce::Array<juce::ValueTree> targets;
    if (state.selectedClip.isValid() && state.selectedClip.getParent().isValid())
    {
        at = clip::start (state.selectedClip);
        targets.add (clip::owningTrack (state.selectedClip));
    }
    else
    {
        // The insertion point: where the user clicked (a selection, even of no length).
        if (state.hasTimeRange() || ! state.selectedTrackIds.isEmpty())
            at = state.selectionStart;
        targets = tracksForEdit();
    }
    // Nothing selected: back onto the tracks it was copied from.
    if (targets.isEmpty())
        for (const auto& lane : clipboardContents.lanes)
            if (auto t = session->getTrackById (lane.sourceTrackId); t.isValid())
                targets.add (t);
    pasteContents (clipboardContents, targets, at, "Paste");
}

void EditView::duplicateSelection()
{
    if (app.getSession() == nullptr || ! hasSelectionToCopy())
        return;
    if (state.selectedClip.isValid() && state.selectedClip.getParent().isValid())
        pasteContents (clipboard::copyClip (state.selectedClip), { clip::owningTrack (state.selectedClip) },
                       clip::end (state.selectedClip), "Duplicate");
    else
        pasteContents (clipboard::copyRange (tracksForEdit(), state.selectionStart, state.selectionEnd), tracksForEdit(),
                       state.selectionEnd, "Duplicate");
}

void EditView::pasteContents (const clipboard::Contents& contents, juce::Array<juce::ValueTree> targets, juce::int64 at,
                              const juce::String& actionName)
{
    auto* session = app.getSession();
    if (session == nullptr || contents.isEmpty() || targets.isEmpty())
        return;
    auto& um = session->getUndoManager();
    um.beginNewTransaction (actionName);
    const bool wasClip = state.selectedClip.isValid();
    const auto result = clipboard::paste (*session, contents, targets, at, state.mode == EditMode::shuffle, &um);

    if (result.clipsPasted == 0 && result.lanesSkipped > 0)
    {
        if (app.showProblem)
            app.showProblem (actionName, "Audio clips go on audio tracks and MIDI clips on instrument tracks. "
                                         "Select a track of the right kind and paste again.");
        return;
    }

    // Select what was pasted, so a repeat lays the next copy after it.
    state.setTimeSelection (at, at + contents.length, result.trackIds);
    if (wasClip && result.trackIds.size() == 1)
        if (auto t = session->getTrackById (result.trackIds[0]); t.isValid())
            for (const auto& c : track::clips (t))
                if (clip::start (c) == at && clip::length (c) == contents.length)
                    state.selectClip (c);
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
