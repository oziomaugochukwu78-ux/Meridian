#include "ClipComponent.h"

#include "media/TimeStretchEngine.h"
#include "model/MidiModel.h"
#include "model/Shuffle.h"
#include "ui/Theme.h"
#include "ui/TimeFormat.h"

namespace meridian
{
namespace
{
constexpr int nameBarHeight = 15;
constexpr double edgeZonePixels = 6.0;
constexpr double fadeZonePixels = 16.0;
constexpr juce::int64 minimumClipLength = 64;
constexpr float gainHandleWidth = 58.0f, gainHandleHeight = 14.0f, gainHandleInset = 8.0f;
constexpr double gainLineRangeDb = 24.0; // the gain line reaches the waveform's edge at +/-24 dB

juce::String gainText (double db)
{
    const bool zero = std::abs (db) < 0.005;
    return (zero || db < 0.0 ? "" : "+") + juce::String (zero ? 0.0 : db, 2) + " dB";
}
} // namespace

juce::Rectangle<float> ClipComponent::gainHandleBounds() const
{
    if (clip::isMidi (clipNode) || getHeight() < 40)
        return {};
    const float left = (float) std::max (logicalLeft, 0.0) + gainHandleInset;
    if ((float) logicalRight - left < gainHandleWidth + gainHandleInset)
        return {};
    return { left, (float) getHeight() - gainHandleHeight - 4.0f, gainHandleWidth, gainHandleHeight };
}

ClipComponent::ClipComponent (AppController& a, EditState& s, juce::ValueTree c, juce::Colour trackColour)
    : app (a), editState (s), clipNode (std::move (c)), colour (trackColour),
      thumbnail (512, a.getFormatManager(), a.getThumbnailCache())
{
    thumbnail.addChangeListener (this);
    clipNode.addListener (this);
    loadThumbnail();
}

ClipComponent::~ClipComponent()
{
    clipNode.removeListener (this);
    thumbnail.removeChangeListener (this);
}

void ClipComponent::loadThumbnail()
{
    if (clip::isMidi (clipNode))
        return; // MIDI clips draw their notes, not a waveform
    if (auto* session = app.getSession())
        thumbnail.setSource (new juce::FileInputSource (session->resolveStoredPath (clipNode[ids::file].toString())));
}

void ClipComponent::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == clipNode)
    {
        if (property == ids::file)
            loadThumbnail();
        repaint();
    }
    else if (tree.hasType (ids::NOTE))
    {
        repaint(); // a note was edited
    }
}

void ClipComponent::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (ids::NOTE) || child.hasType (ids::NOTES))
        repaint();
}

void ClipComponent::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (ids::NOTE) || child.hasType (ids::NOTES))
        repaint();
}

void ClipComponent::paintNotes (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto* session = app.getSession();
    if (session == nullptr)
        return;

    const auto notes = midimodel::readNotes (clipNode);
    if (notes.empty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText ("empty MIDI clip", area, juce::Justification::centred);
        return;
    }

    // Fit the clip's pitch range to the clip height (at least an octave tall).
    int low = 127, high = 0;
    for (const auto& n : notes.getNotes())
    {
        low = juce::jmin (low, n.pitch);
        high = juce::jmax (high, n.pitch);
    }
    const int span = juce::jmax (12, high - low + 1);
    const int bottomPitch = juce::jmax (0, (low + high) / 2 - span / 2);
    const float rowHeight = (float) area.getHeight() / (float) span;

    const auto tempo = midimodel::tempoMap (*session);
    const double rate = session->getSampleRate();
    const auto origin = clip::contentOrigin (clipNode);
    const auto clipStart = clip::start (clipNode), clipEnd = clip::end (clipNode);

    g.saveState();
    g.reduceClipRegion (area);
    for (const auto& n : notes.getNotes())
    {
        const auto on = (juce::int64) tempo.samplesFromOrigin (origin, n.start, rate); // int64_t is long on Linux
        if (on < clipStart || on >= clipEnd)
            continue; // not played: outside the clip
        const auto off = juce::jmin (clipEnd, (juce::int64) tempo.samplesFromOrigin (origin, n.end(), rate));
        const float x0 = (float) (logicalLeft + (double) (on - clipStart) / editState.samplesPerPixel);
        const float x1 = (float) (logicalLeft + (double) (off - clipStart) / editState.samplesPerPixel);
        const float y = (float) area.getBottom() - (float) (n.pitch - bottomPitch + 1) * rowHeight;
        g.setColour (colour.brighter (0.4f).withAlpha (0.45f + 0.55f * n.velocity));
        g.fillRect (juce::Rectangle<float> (x0, y, juce::jmax (1.5f, x1 - x0 - 0.5f), juce::jmax (1.5f, rowHeight - 0.5f)));
    }
    g.restoreState();
}

void ClipComponent::updateLayout (int laneWidth, int laneHeight)
{
    const double startX = editState.sampleToX (clip::start (clipNode));
    const double endX = editState.sampleToX (clip::end (clipNode));

    // Only the visible part becomes the component, so extreme zoom levels
    // never produce gigantic component bounds.
    const double visibleLeft = juce::jmax (startX, -8.0);
    const double visibleRight = juce::jmin (endX, (double) laneWidth + 8.0);

    if (visibleRight <= visibleLeft)
    {
        setVisible (false);
        return;
    }

    const int left = (int) std::floor (visibleLeft);
    const int right = (int) std::ceil (visibleRight);
    logicalLeft = startX - left;
    logicalRight = endX - left;
    setBounds (left, 1, juce::jmax (1, right - left), laneHeight - 2);
    setVisible (true);
}

//==============================================================================
void ClipComponent::paint (juce::Graphics& g)
{
    const bool selected = editState.selectedClip == clipNode;
    const auto bounds = getLocalBounds().toFloat();
    const auto clipRect = juce::Rectangle<float> ((float) logicalLeft, 0.0f,
                                                  (float) (logicalRight - logicalLeft), bounds.getHeight());

    g.setColour (colour.darker (0.75f).withSaturation (0.35f));
    g.fillRect (clipRect);

    auto nameBar = clipRect.withHeight ((float) nameBarHeight);
    g.setColour (colour.darker (0.15f));
    g.fillRect (nameBar);

    // Waveform for the visible slice of the clip.
    const auto waveArea = getLocalBounds().withTrimmedTop (nameBarHeight + 1).reduced (0, 2);
    const double sampleRate = editState.sampleRate;
    const auto sourceAt = [&] (double x)
    {
        return (double) clip::offset (clipNode) + (x - logicalLeft) * editState.samplesPerPixel;
    };
    if (clip::isMidi (clipNode))
    {
        paintNotes (g, waveArea);
    }
    else if (thumbnail.getTotalLength() > 0.0)
    {
        // Drawn at the clip's gain, so turning it up or down is visible in the waveform.
        const auto gain = (float) juce::jlimit (0.0, 8.0, juce::Decibels::decibelsToGain (clip::gainDb (clipNode), -144.0));
        g.setColour (theme::waveform.withAlpha (0.9f));
        thumbnail.drawChannels (g, waveArea, sourceAt (0.0) / sampleRate, sourceAt ((double) getWidth()) / sampleRate, gain);
    }
    else
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText ("offline", waveArea, juce::Justification::centred);
    }

    // Fades: shade the region outside the fade curve.
    const auto drawFade = [&] (juce::int64 length, bool isFadeIn)
    {
        if (length <= 0)
            return;
        const float w = (float) ((double) length / editState.samplesPerPixel);
        const float x0 = isFadeIn ? clipRect.getX() : clipRect.getRight() - w;
        const float top = (float) nameBarHeight, bottom = clipRect.getBottom();

        juce::Path shade;
        if (isFadeIn)
        {
            shade.startNewSubPath (x0, top);
            shade.lineTo (x0 + w, top);
            shade.quadraticTo (x0 + w * 0.1f, top + (bottom - top) * 0.1f, x0, bottom);
        }
        else
        {
            shade.startNewSubPath (x0 + w, top);
            shade.lineTo (x0, top);
            shade.quadraticTo (x0 + w * 0.9f, top + (bottom - top) * 0.1f, x0 + w, bottom);
        }
        shade.closeSubPath();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillPath (shade);
        g.setColour (theme::text.withAlpha (0.6f));
        g.strokePath (shade, juce::PathStrokeType (1.0f));
    };
    drawFade (clip::fadeIn (clipNode), true);
    drawFade (clip::fadeOut (clipNode), false);

    // Clip gain: a line where it is set (when not 0 dB), and the handle with its value.
    if (! clip::isMidi (clipNode))
    {
        const double gainDb = clip::gainDb (clipNode);
        const auto gainColour = juce::Colour (0xffe8a33d);
        if (std::abs (gainDb) >= 0.005)
        {
            const float y = (float) waveArea.getCentreY()
                            - (float) (juce::jlimit (-1.0, 1.0, gainDb / gainLineRangeDb) * waveArea.getHeight() * 0.5);
            g.setColour (gainColour.withAlpha (0.85f));
            g.drawHorizontalLine ((int) y, clipRect.getX(), clipRect.getRight());
        }
        if (const auto handle = gainHandleBounds(); ! handle.isEmpty())
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (handle, 3.0f);
            g.setColour (std::abs (gainDb) >= 0.005 ? gainColour : theme::text.withAlpha (0.75f));
            g.setFont (theme::font (10.0f));
            g.drawText (gainText (gainDb), handle, juce::Justification::centred, false);
        }
    }

    g.setColour (theme::textOnLight);
    g.setFont (theme::font (11.0f, true));
    g.drawText (clip::name (clipNode), nameBar.reduced (4.0f, 0.0f).withLeft (juce::jmax (nameBar.getX(), 0.0f) + 4.0f),
                juce::Justification::centredLeft, true);

    g.setColour (selected ? theme::selectionEdge : theme::divider);
    g.drawRect (clipRect, selected ? 2.0f : 1.0f);
}

//==============================================================================
ClipComponent::Action ClipComponent::actionAt (const juce::MouseEvent& e) const
{
    const double x = e.position.x;
    const double y = e.position.y;
    const bool startVisible = logicalLeft >= -1.0;
    const bool endVisible = logicalRight <= getWidth() + 1.0;
    const bool nearStart = startVisible && std::abs (x - logicalLeft) <= edgeZonePixels;
    const bool nearEnd = endVisible && std::abs (x - logicalRight) <= edgeZonePixels;

    // Fades and time-stretching are audio operations; MIDI clips just trim.
    const bool midi = clip::isMidi (clipNode);
    const bool stretch = e.mods.isAltDown() && ! midi;

    const bool clipTool = editState.tool == EditTool::smart || editState.tool == EditTool::selector
                          || editState.tool == EditTool::grabber || editState.tool == EditTool::trim;
    if (clipTool && gainHandleBounds().contains (e.position))
        return Action::clipGain;

    switch (editState.tool)
    {
        case EditTool::selector: return Action::select;
        case EditTool::grabber:  return Action::move;
        case EditTool::trim:
            if (x < (logicalLeft + logicalRight) * 0.5)
                return Action::trimStart;
            return stretch ? Action::stretchEnd : Action::trimEnd;

        case EditTool::smart:
            if (! midi && y < nameBarHeight && startVisible && x - logicalLeft < fadeZonePixels && ! nearStart)
                return Action::fadeIn;
            if (! midi && y < nameBarHeight && endVisible && logicalRight - x < fadeZonePixels && ! nearEnd)
                return Action::fadeOut;
            if (nearStart)
                return Action::trimStart;
            if (nearEnd)
                return stretch ? Action::stretchEnd : Action::trimEnd;
            return y < getHeight() * 0.5 ? Action::select : Action::move;

        case EditTool::zoomer:
        case EditTool::pencil:
            break;
    }
    return Action::none;
}

juce::MouseCursor ClipComponent::cursorFor (Action a)
{
    switch (a)
    {
        case Action::select:     return juce::MouseCursor::IBeamCursor;
        case Action::move:       return juce::MouseCursor::DraggingHandCursor;
        case Action::trimStart:  return juce::MouseCursor::LeftEdgeResizeCursor;
        case Action::trimEnd:    return juce::MouseCursor::RightEdgeResizeCursor;
        case Action::stretchEnd: return juce::MouseCursor::LeftRightResizeCursor;
        case Action::fadeIn:
        case Action::fadeOut:    return juce::MouseCursor::CrosshairCursor;
        case Action::clipGain:   return juce::MouseCursor::UpDownResizeCursor;
        case Action::none:       break;
    }
    return juce::MouseCursor::NormalCursor;
}

void ClipComponent::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (cursorFor (actionAt (e)));
}

void ClipComponent::forwardToLane (const juce::MouseEvent& e, int phase)
{
    if (auto* lane = getParentComponent())
    {
        const auto relative = e.getEventRelativeTo (lane);
        if (phase == 0)      lane->mouseDown (relative);
        else if (phase == 1) lane->mouseDrag (relative);
        else                 lane->mouseUp (relative);
    }
}

void ClipComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        editState.selectClip (clipNode);
        showMenu();
        return;
    }

    dragAction = actionAt (e);
    if (dragAction == Action::select)
    {
        forwardToLane (e, 0);
        return;
    }
    if (dragAction == Action::none)
        return;

    downScreenX = e.getScreenX();
    downScreenY = e.getScreenY();
    originalGainDb = clip::gainDb (clipNode);
    originalStart = clip::start (clipNode);
    originalLength = clip::length (clipNode);
    originalOffset = clip::offset (clipNode);
    originalFadeIn = clip::fadeIn (clipNode);
    originalFadeOut = clip::fadeOut (clipNode);

    laterClips.clear();
    if (editState.mode == EditMode::shuffle && (dragAction == Action::trimStart || dragAction == Action::trimEnd))
        for (const auto& c : clipNode.getParent())
            if (c != clipNode && clip::start (c) >= originalStart + originalLength)
                laterClips.emplace_back (c, clip::start (c));

    editState.selectClip (clipNode);

    if (auto* um = app.getUndoManager())
    {
        static const char* names[] = { "", "", "Move Clip", "Trim Clip", "Trim Clip", "Elastic Audio", "Fade In", "Fade Out", "Clip Gain" };
        um->beginNewTransaction (names[(int) dragAction]);
    }
}

void ClipComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragAction == Action::select)
    {
        forwardToLane (e, 1);
        return;
    }

    auto* um = app.getUndoManager();
    if (um == nullptr || dragAction == Action::none)
        return;

    const auto delta = (juce::int64) std::llround (editState.pixelsToSamples (e.getScreenX() - downScreenX));
    const auto originalEnd = originalStart + originalLength;
    const auto sourceLength = clip::sourceLength (clipNode);
    const bool shuffling = editState.mode == EditMode::shuffle;

    // Shuffle: later clips follow this clip's end, from where they were when the drag began.
    auto shiftLaterClips = [this, um] (juce::int64 by)
    {
        for (auto& [c, start] : laterClips)
            c.setProperty (ids::start, std::max<juce::int64> (0, start + by), um);
    };

    switch (dragAction)
    {
        case Action::move:
            if (editState.mode != EditMode::spot) // Spot asks for a time on mouse-up instead
                clipNode.setProperty (ids::start, std::max<juce::int64> (0, editState.snap (originalStart + delta)), um);
            break;

        case Action::trimStart:
        {
            const auto earliest = std::max<juce::int64> (0, originalStart - originalOffset); // cannot reveal before the file starts
            const auto newStart = juce::jlimit (earliest, originalEnd - minimumClipLength, editState.snap (originalStart + delta));
            const auto trimmed = newStart - originalStart;
            if (shuffling)
            {
                // The clip's start stays put; its audio and everything after it slide instead.
                clipNode.setProperty (ids::offset, originalOffset + trimmed, um);
                clipNode.setProperty (ids::length, originalLength - trimmed, um);
                shiftLaterClips (-trimmed);
                break;
            }
            clipNode.setProperty (ids::start, newStart, um);
            clipNode.setProperty (ids::offset, originalOffset + trimmed, um);
            clipNode.setProperty (ids::length, originalEnd - newStart, um);
            break;
        }

        case Action::trimEnd:
        {
            const auto latest = sourceLength > 0 ? originalStart + (sourceLength - originalOffset) : originalEnd + (1LL << 40);
            const auto newEnd = juce::jlimit (originalStart + minimumClipLength, latest, editState.snap (originalEnd + delta));
            clipNode.setProperty (ids::length, newEnd - originalStart, um);
            if (shuffling)
                shiftLaterClips (newEnd - originalEnd);
            break;
        }

        case Action::stretchEnd:
        {
            // Preview only (not undoable): the real edit happens when the render finishes.
            const auto newLength = juce::jlimit (originalLength / 8, originalLength * 8, originalLength + delta);
            clipNode.setProperty (ids::length, newLength, nullptr);
            break;
        }

        case Action::fadeIn:
            clipNode.setProperty (ids::fadeIn, juce::jlimit<juce::int64> (0, originalLength - clip::fadeOut (clipNode), originalFadeIn + delta), um);
            break;

        case Action::fadeOut:
            clipNode.setProperty (ids::fadeOut, juce::jlimit<juce::int64> (0, originalLength - clip::fadeIn (clipNode), originalFadeOut - delta), um);
            break;

        case Action::clipGain:
        {
            const double step = e.mods.isShiftDown() ? fineGainDbPerPixel : gainDbPerPixel;
            const double gainDb = juce::jlimit (minGainDb, maxGainDb, originalGainDb - (double) (e.getScreenY() - downScreenY) * step);
            clipNode.setProperty (ids::gainDb, gainDb, um);
            break;
        }

        case Action::select:
        case Action::none:
            break;
    }
}

void ClipComponent::mouseUp (const juce::MouseEvent& e)
{
    const auto action = dragAction;
    dragAction = Action::none;

    if (action == Action::select)
    {
        forwardToLane (e, 2);
        return;
    }

    if (action == Action::stretchEnd)
    {
        const auto newLength = clip::length (clipNode);
        clipNode.setProperty (ids::length, originalLength, nullptr); // undo the preview
        if (newLength != originalLength && originalLength > 0)
            app.renderElasticAudio (clipNode, (double) newLength / (double) originalLength, 0.0, false);
        return;
    }

    if (action == Action::move && editState.mode == EditMode::spot)
    {
        showSpotDialog();
        return;
    }

    auto* um = app.getUndoManager();
    const bool shuffling = editState.mode == EditMode::shuffle;

    if (shuffling && um != nullptr && (action == Action::trimStart || action == Action::trimEnd))
    {
        // The track's automation follows the audio, once, by the drag's final amount.
        auto track = clip::owningTrack (clipNode);
        if (action == Action::trimEnd)
        {
            shuffle::shiftAutomation (track, originalStart + originalLength, clip::end (clipNode) - (originalStart + originalLength), um);
        }
        else if (const auto trimmed = clip::offset (clipNode) - originalOffset; trimmed > 0)
        {
            shuffle::shiftAutomation (track, originalStart + trimmed, -trimmed, um);   // [start, start + trimmed) is gone
        }
        else if (trimmed < 0)
        {
            shuffle::shiftAutomation (track, originalStart, -trimmed, um);             // audio revealed at the start
        }
        laterClips.clear();
        return;
    }

    if (action == Action::move && trackAtScreenPosition)
    {
        // Dropped on another track: move the clip there. Deferred, because
        // re-parenting the clip deletes this component.
        auto target = trackAtScreenPosition (e.getScreenPosition());
        auto source = clip::owningTrack (clipNode);
        const bool fits = target.isValid() && (clip::isMidi (clipNode) ? track::isInstrument (target) : track::isAudio (target));
        if (fits && target != source)
        {
            juce::MessageManager::callAsync ([clipCopy = clipNode, source, target, um]() mutable
            {
                if (um == nullptr || ! clipCopy.getParent().isValid())
                    return;
                track::clips (source).removeChild (clipCopy, um);
                track::clips (target).appendChild (clipCopy, um);
            });
            return;
        }
    }

    if (action == Action::move && shuffling && um != nullptr && clip::start (clipNode) != originalStart)
    {
        // Shuffle: the drag only showed where; now take the clip out of the
        // sequence and drop it in at the nearest clip boundary.
        const auto dropped = clip::start (clipNode);
        clipNode.setProperty (ids::start, originalStart, um);
        shuffle::moveClip (clipNode, dropped, um);
    }
}

bool ClipComponent::spotTo (const juce::String& timeText)
{
    const auto start = timeformat::parseTime (timeText, editState.sampleRate);
    auto* um = app.getUndoManager();
    if (! start.has_value() || um == nullptr)
        return false;
    um->beginNewTransaction ("Spot Clip");
    clipNode.setProperty (ids::start, *start, um);
    return true;
}

void ClipComponent::showSpotDialog()
{
    auto* window = new juce::AlertWindow ("Spot Clip", "Start time: m:ss.mmm, seconds, or samples (for example 48000 smp)",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("start", timeformat::minSecs (clip::start (clipNode), editState.sampleRate));
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<ClipComponent> (this), window] (int result)
        {
            const auto text = window->getTextEditorContents ("start").trim();
            if (safe == nullptr || result != 1)
                return;
            if (! safe->spotTo (text) && safe->app.showProblem)
                safe->app.showProblem ("Spot Clip", "\"" + text + "\" is not a time. Type m:ss.mmm, seconds, or samples (48000 smp).");
        }), true);
}

void ClipComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (clip::isMidi (clipNode) && openPianoRoll)
    {
        openPianoRoll (clipNode);
    }
    else if (gainHandleBounds().contains (e.position))
    {
        if (auto* um = app.getUndoManager())
        {
            um->beginNewTransaction ("Clip Gain");
            clipNode.setProperty (ids::gainDb, 0.0, um);
        }
    }
}

void ClipComponent::showMenu()
{
    juce::PopupMenu menu;
    const bool midi = clip::isMidi (clipNode);
    if (midi)
    {
        menu.addItem (4, "Edit Notes (MIDI Editor)...");
        menu.addSeparator();
    }
    menu.addItem (1, "Elastic Audio: Time / Pitch...", isTimeStretchAvailable() && ! midi);
    menu.addSeparator();
    menu.addItem (2, "Remove Fades", ! midi && (clip::fadeIn (clipNode) > 0 || clip::fadeOut (clipNode) > 0));
    menu.addItem (3, "Delete Clip");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe = juce::Component::SafePointer<ClipComponent> (this)] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            auto clipCopy = safe->clipNode;
                            auto* um = safe->app.getUndoManager();
                            if (um == nullptr)
                                return;

                            if (result == 1 && safe->openElasticDialog)
                            {
                                safe->openElasticDialog (clipCopy);
                            }
                            else if (result == 4 && safe->openPianoRoll)
                            {
                                safe->openPianoRoll (clipCopy);
                            }
                            else if (result == 2)
                            {
                                um->beginNewTransaction ("Remove Fades");
                                clipCopy.setProperty (ids::fadeIn, 0, um);
                                clipCopy.setProperty (ids::fadeOut, 0, um);
                            }
                            else if (result == 3)
                            {
                                um->beginNewTransaction ("Delete Clip");
                                clipCopy.getParent().removeChild (clipCopy, um); // deletes this component
                            }
                        });
}

} // namespace meridian
