#include "TimelineRuler.h"

#include "model/Session.h"
#include "ui/Theme.h"

namespace meridian
{

TimelineRuler::TimelineRuler (EditState& s) : state (s)
{
    state.addChangeListener (this);
}

TimelineRuler::~TimelineRuler()
{
    state.removeChangeListener (this);
    session.removeListener (this);
}

void TimelineRuler::setSessionState (juce::ValueTree s)
{
    session.removeListener (this);
    session = std::move (s);
    session.addListener (this); // the whole session: markers and loop settings live in it
    repaint();
}

juce::ValueTree TimelineRuler::markerAt (int x) const
{
    for (const auto& m : session.getChildWithName (ids::MARKERS))
    {
        const auto mx = trackHeaderWidth + (int) state.sampleToX ((juce::int64) m[ids::start]);
        const auto labelWidth = 14 + juce::GlyphArrangement::getStringWidthInt (theme::font (11.0f), m[ids::name].toString());
        if (x >= mx - 4 && x <= mx + labelWidth)
            return m;
    }
    return {};
}

void TimelineRuler::paintMarkers (juce::Graphics& g, juce::Rectangle<int> row)
{
    g.setFont (theme::font (11.0f));
    for (const auto& m : session.getChildWithName (ids::MARKERS))
    {
        const auto x = (float) (row.getX() + state.sampleToX ((juce::int64) m[ids::start]));
        juce::Path flag;
        flag.addTriangle (x - 4.0f, (float) row.getY() + 4.0f, x + 4.0f, (float) row.getY() + 4.0f, x, (float) row.getBottom() - 4.0f);
        g.setColour (theme::marker);
        g.fillPath (flag);
        g.drawVerticalLine ((int) x, (float) row.getBottom() - 4.0f, (float) row.getBottom());
        g.setColour (theme::text);
        g.drawText (m[ids::name].toString(), juce::Rectangle<float> (x + 7.0f, (float) row.getY(), 200.0f, (float) row.getHeight()),
                    juce::Justification::centredLeft);
    }
}

void TimelineRuler::showMarkerMenu (juce::ValueTree marker)
{
    juce::PopupMenu menu;
    menu.addItem ("Rename Marker...", [this, marker]
    {
        auto* window = new juce::AlertWindow ("Rename Marker", {}, juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("name", marker[ids::name].toString());
        window->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([this, marker, window] (int result) mutable
        {
            const auto name = window->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty())
            {
                auto* um = undoManager ? undoManager() : nullptr;
                if (um != nullptr)
                    um->beginNewTransaction ("Rename Marker");
                marker.setProperty (ids::name, name, um);
            }
        }), true);
    });
    menu.addItem ("Delete Marker", [this, marker]
    {
        auto* um = undoManager ? undoManager() : nullptr;
        if (um != nullptr)
            um->beginNewTransaction ("Delete Marker");
        auto parent = marker.getParent();
        parent.removeChild (marker, um);
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void TimelineRuler::setPlayheadPosition (juce::int64 sample)
{
    if (sample == playhead)
        return;
    const auto oldX = trackHeaderWidth + (int) juce::jlimit (-1.0e6, 1.0e6, state.sampleToX (playhead));
    playhead = sample;
    const auto newX = trackHeaderWidth + (int) juce::jlimit (-1.0e6, 1.0e6, state.sampleToX (playhead));
    repaint (oldX - 6, 0, 12, getHeight());
    repaint (newX - 6, 0, 12, getHeight());
}

void TimelineRuler::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    auto area = getLocalBounds();
    auto labels = area.removeFromLeft (trackHeaderWidth);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("Markers", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);
    g.drawText ("Bars|Beats", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);
    g.drawText ("Min:Secs", labels.removeFromTop (rowHeight).reduced (10, 0), juce::Justification::centredRight);

    g.reduceClipRegion (area);

    // Time selection, mirrored on the ruler as in the tracks.
    if (state.hasTimeRange())
    {
        const auto x1 = (float) (trackHeaderWidth + state.sampleToX (state.selectionStart));
        const auto x2 = (float) (trackHeaderWidth + state.sampleToX (state.selectionEnd));
        g.setColour (theme::selection);
        g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, (float) getHeight()));
    }

    // Loop range: a strip under the marker row, bright when loop playback is on.
    if (const juce::int64 loopEnd = session.getProperty (ids::loopEnd, 0), loopStart = session.getProperty (ids::loopStart, 0);
        loopEnd > loopStart)
    {
        const auto x1 = (float) (trackHeaderWidth + state.sampleToX (loopStart));
        const auto x2 = (float) (trackHeaderWidth + state.sampleToX (loopEnd));
        const bool on = session.getProperty (ids::loopEnabled, false);
        g.setColour (theme::loop.withAlpha (on ? 0.9f : 0.3f));
        g.fillRect (juce::Rectangle<float> (x1, (float) rowHeight - 4.0f, x2 - x1, 4.0f));
    }

    // Punch range: a red strip just above the loop strip.
    if (const juce::int64 punchOut = session.getProperty (ids::punchOut, 0), punchIn = session.getProperty (ids::punchIn, 0);
        punchOut > punchIn)
    {
        const auto x1 = (float) (trackHeaderWidth + state.sampleToX (punchIn));
        const auto x2 = (float) (trackHeaderWidth + state.sampleToX (punchOut));
        const bool on = session.getProperty (ids::punchEnabled, false);
        g.setColour (theme::record.withAlpha (on ? 0.9f : 0.3f));
        g.fillRect (juce::Rectangle<float> (x1, (float) rowHeight - 8.0f, x2 - x1, 3.0f));
    }

    paintMarkers (g, area.removeFromTop (rowHeight));
    paintBarsBeats (g, area.removeFromTop (rowHeight));
    paintMinSecs (g, area.removeFromTop (rowHeight));

    g.setColour (theme::divider);
    g.drawHorizontalLine (rowHeight, (float) trackHeaderWidth, (float) getWidth());
    g.drawHorizontalLine (rowHeight * 2, (float) trackHeaderWidth, (float) getWidth());
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (trackHeaderWidth - 1, 0.0f, (float) getHeight());

    // Playhead marker
    const auto px = (float) (trackHeaderWidth + state.sampleToX (playhead));
    juce::Path marker;
    marker.addTriangle (px - 5.0f, (float) getHeight() - 9.0f, px + 5.0f, (float) getHeight() - 9.0f, px, (float) getHeight() - 1.0f);
    g.setColour (theme::playhead);
    g.fillPath (marker);
}

void TimelineRuler::paintBarsBeats (juce::Graphics& g, juce::Rectangle<int> row)
{
    // Bars and beats follow the tempo and meter maps: bar lengths change with both.
    const auto& meter = state.meterMap;
    const auto firstBar = meter.positionOf (state.sampleToTicks (state.viewStart)).bar;
    const auto lastBar = meter.positionOf (state.sampleToTicks (state.xToSample (row.getWidth()))).bar + 1;

    // Label every 2^n bars so labels stay ~60 px apart (judged on the first visible bar).
    const double firstBarPixels = (double) (state.ticksToSample (meter.barToTicks (firstBar + 1))
                                            - state.ticksToSample (meter.barToTicks (firstBar))) / state.samplesPerPixel;
    int barStep = 1;
    while (firstBarPixels * barStep < 60.0 && barStep < 1024)
        barStep *= 2;

    g.setFont (theme::font (11.0f));
    for (auto bar = std::max<juce::int64> (0, firstBar - (firstBar % barStep)); bar <= lastBar; ++bar)
    {
        const auto& m = meter.meterAt (bar);
        const auto barStart = meter.barToTicks (bar);
        const auto x = (float) (row.getX() + state.sampleToX (state.ticksToSample (barStart)));
        const bool labelled = bar % barStep == 0;
        const auto beatPixels = (double) (state.ticksToSample (barStart + midi::MeterMap::ticksPerBeat (m)) - state.ticksToSample (barStart))
                                / state.samplesPerPixel;

        if (labelled || beatPixels >= 12.0)
        {
            g.setColour (labelled ? theme::text : theme::textDim);
            g.drawVerticalLine ((int) x, (float) row.getY() + (labelled ? 4.0f : 12.0f), (float) row.getBottom());
        }
        if (labelled)
        {
            // A new time signature is shown on the bar where it starts.
            const bool meterChanges = bar > 0 && std::any_of (meter.getChanges().begin(), meter.getChanges().end(),
                                                              [bar] (const midi::MeterMap::Change& c) { return c.bar == bar; });
            g.drawText (juce::String (bar + 1) + (meterChanges ? "  " + juce::String (m.numerator) + "/" + juce::String (m.denominator) : juce::String()),
                        juce::Rectangle<float> (x + 3.0f, (float) row.getY(), 90.0f, (float) row.getHeight()),
                        juce::Justification::centredLeft);
        }

        if (beatPixels >= 12.0)
        {
            g.setColour (theme::textDim);
            for (int beat = 1; beat < m.numerator; ++beat)
            {
                const auto bx = (float) (row.getX() + state.sampleToX (state.ticksToSample (barStart + beat * midi::MeterMap::ticksPerBeat (m))));
                g.drawVerticalLine ((int) bx, (float) row.getBottom() - 5.0f, (float) row.getBottom());
            }
        }
    }
}

void TimelineRuler::paintMinSecs (juce::Graphics& g, juce::Rectangle<int> row)
{
    static constexpr double steps[] = { 0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
    const double pixelsPerSecond = state.sampleRate / state.samplesPerPixel;

    double step = steps[std::size (steps) - 1];
    for (const double s : steps)
    {
        if (s * pixelsPerSecond >= 90.0)
        {
            step = s;
            break;
        }
    }

    const double startSeconds = (double) state.viewStart / state.sampleRate;
    const double endSeconds = (double) state.xToSample (row.getWidth()) / state.sampleRate;

    g.setFont (theme::font (11.0f));
    for (double t = std::floor (startSeconds / step) * step; t <= endSeconds; t += step)
    {
        const auto x = (float) (row.getX() + state.sampleToX ((juce::int64) std::llround (t * state.sampleRate)));
        g.setColour (theme::text);
        g.drawVerticalLine ((int) x, (float) row.getY() + 4.0f, (float) row.getBottom());

        juce::String label = timeformat::minSecs ((juce::int64) std::llround (t * state.sampleRate), state.sampleRate);
        if (step >= 1.0)
            label = label.upToLastOccurrenceOf (".", false, false);
        g.drawText (label, juce::Rectangle<float> (x + 3.0f, (float) row.getY(), 90.0f, (float) row.getHeight()),
                    juce::Justification::centredLeft);
    }
}

void TimelineRuler::locateFromMouse (const juce::MouseEvent& e)
{
    if (e.x < trackHeaderWidth || ! onLocate)
        return;
    onLocate (state.snap (std::max<juce::int64> (0, state.xToSample (e.x - trackHeaderWidth))));
}

void TimelineRuler::mouseDown (const juce::MouseEvent& e)
{
    if (e.y < rowHeight)
    {
        if (const auto m = markerAt (e.x); m.isValid())
        {
            if (e.mods.isPopupMenu())
                showMarkerMenu (m);
            else if (onLocate)
                onLocate ((juce::int64) m[ids::start]);
            return;
        }
    }
    locateFromMouse (e);
}
void TimelineRuler::mouseDrag (const juce::MouseEvent& e) { locateFromMouse (e); }

} // namespace meridian
