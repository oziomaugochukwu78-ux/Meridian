#include "TimelineRuler.h"

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

    paintBarsBeats (g, area.removeFromTop (rowHeight));
    paintMinSecs (g, area.removeFromTop (rowHeight));

    g.setColour (theme::divider);
    g.drawHorizontalLine (rowHeight, (float) trackHeaderWidth, (float) getWidth());
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
    const double samplesPerBeat = timeformat::samplesPerBeat (state.sampleRate);
    const double samplesPerBar = samplesPerBeat * timeformat::beatsPerBar;
    const double barPixels = samplesPerBar / state.samplesPerPixel;

    int barStep = 1;
    while (barPixels * barStep < 60.0 && barStep < 1024)
        barStep *= 2;
    const bool drawBeats = samplesPerBeat / state.samplesPerPixel >= 12.0;

    const auto firstBar = (juce::int64) std::floor ((double) state.viewStart / samplesPerBar);
    const auto lastBar = (juce::int64) std::ceil ((double) state.xToSample (row.getWidth()) / samplesPerBar);

    g.setFont (theme::font (11.0f));
    for (auto bar = firstBar - (firstBar % barStep); bar <= lastBar; bar += (drawBeats ? 1 : barStep))
    {
        const auto x = (float) (row.getX() + state.sampleToX ((juce::int64) std::llround ((double) bar * samplesPerBar)));
        const bool labelled = bar % barStep == 0;

        g.setColour (labelled ? theme::text : theme::textDim);
        g.drawVerticalLine ((int) x, (float) row.getY() + (labelled ? 4.0f : 12.0f), (float) row.getBottom());
        if (labelled)
            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 3.0f, (float) row.getY(), 60.0f, (float) row.getHeight()),
                        juce::Justification::centredLeft);

        if (drawBeats)
        {
            g.setColour (theme::textDim);
            for (int beat = 1; beat < timeformat::beatsPerBar; ++beat)
            {
                const auto bx = (float) (row.getX() + state.sampleToX ((juce::int64) std::llround ((double) bar * samplesPerBar + beat * samplesPerBeat)));
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
    onLocate (state.snap (juce::jmax<juce::int64> (0, state.xToSample (e.x - trackHeaderWidth))));
}

void TimelineRuler::mouseDown (const juce::MouseEvent& e) { locateFromMouse (e); }
void TimelineRuler::mouseDrag (const juce::MouseEvent& e) { locateFromMouse (e); }

} // namespace meridian
