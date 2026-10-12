#include "ClipComponent.h"

#include "media/TimeStretchEngine.h"
#include "ui/Theme.h"

namespace meridian
{
namespace
{
constexpr int nameBarHeight = 15;
constexpr double edgeZonePixels = 6.0;
constexpr double fadeZonePixels = 16.0;
constexpr juce::int64 minimumClipLength = 64;
} // namespace

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
    if (auto* session = app.getSession())
        thumbnail.setSource (new juce::FileInputSource (session->resolveStoredPath (clipNode[ids::file].toString())));
}

void ClipComponent::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree != clipNode)
        return;
    if (property == ids::file)
        loadThumbnail();
    repaint();
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
    if (thumbnail.getTotalLength() > 0.0)
    {
        g.setColour (theme::waveform.withAlpha (0.9f));
        thumbnail.drawChannels (g, waveArea, sourceAt (0.0) / sampleRate, sourceAt ((double) getWidth()) / sampleRate, 1.0f);
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

    switch (editState.tool)
    {
        case EditTool::selector: return Action::select;
        case EditTool::grabber:  return Action::move;
        case EditTool::trim:
            if (x < (logicalLeft + logicalRight) * 0.5)
                return Action::trimStart;
            return e.mods.isAltDown() ? Action::stretchEnd : Action::trimEnd;

        case EditTool::smart:
            if (y < nameBarHeight && startVisible && x - logicalLeft < fadeZonePixels && ! nearStart)
                return Action::fadeIn;
            if (y < nameBarHeight && endVisible && logicalRight - x < fadeZonePixels && ! nearEnd)
                return Action::fadeOut;
            if (nearStart)
                return Action::trimStart;
            if (nearEnd)
                return e.mods.isAltDown() ? Action::stretchEnd : Action::trimEnd;
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
    originalStart = clip::start (clipNode);
    originalLength = clip::length (clipNode);
    originalOffset = clip::offset (clipNode);
    originalFadeIn = clip::fadeIn (clipNode);
    originalFadeOut = clip::fadeOut (clipNode);

    editState.selectClip (clipNode);

    if (auto* um = app.getUndoManager())
    {
        static const char* names[] = { "", "", "Move Clip", "Trim Clip", "Trim Clip", "Elastic Audio", "Fade In", "Fade Out" };
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

    switch (dragAction)
    {
        case Action::move:
            clipNode.setProperty (ids::start, juce::jmax<juce::int64> (0, editState.snap (originalStart + delta)), um);
            break;

        case Action::trimStart:
        {
            const auto earliest = juce::jmax<juce::int64> (0, originalStart - originalOffset); // cannot reveal before the file starts
            const auto newStart = juce::jlimit (earliest, originalEnd - minimumClipLength, editState.snap (originalStart + delta));
            clipNode.setProperty (ids::start, newStart, um);
            clipNode.setProperty (ids::offset, originalOffset + (newStart - originalStart), um);
            clipNode.setProperty (ids::length, originalEnd - newStart, um);
            break;
        }

        case Action::trimEnd:
        {
            const auto latest = sourceLength > 0 ? originalStart + (sourceLength - originalOffset) : originalEnd + (1LL << 40);
            const auto newEnd = juce::jlimit (originalStart + minimumClipLength, latest, editState.snap (originalEnd + delta));
            clipNode.setProperty (ids::length, newEnd - originalStart, um);
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

    if (action == Action::move && trackAtScreenPosition)
    {
        // Dropped on another track: move the clip there. Deferred, because
        // re-parenting the clip deletes this component.
        auto target = trackAtScreenPosition (e.getScreenPosition());
        auto source = clip::owningTrack (clipNode);
        if (target.isValid() && target != source)
        {
            juce::MessageManager::callAsync ([clipCopy = clipNode, source, target, um = app.getUndoManager()]() mutable
            {
                if (um == nullptr || ! clipCopy.getParent().isValid())
                    return;
                track::clips (source).removeChild (clipCopy, um);
                track::clips (target).appendChild (clipCopy, um);
            });
        }
    }
}

void ClipComponent::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Elastic Audio: Time / Pitch...", isTimeStretchAvailable());
    menu.addSeparator();
    menu.addItem (2, "Remove Fades", clip::fadeIn (clipNode) > 0 || clip::fadeOut (clipNode) > 0);
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
