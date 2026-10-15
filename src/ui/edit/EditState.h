#pragma once

#include "ui/TimeFormat.h"

#include <midi/MeterMap.h>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <map>

namespace meridian
{

enum class EditTool { zoomer, trim, selector, grabber, smart, pencil };
enum class EditMode { shuffle, slip, spot, grid };

/** Width of the channel-header column on the left of the Edit window. */
constexpr int trackHeaderWidth = 216;

/** View and selection state of the Edit window, shared by its toolbar, ruler,
    track headers, lanes and clips. Broadcasts a change whenever the view,
    tool or selection changes. */
class EditState final : public juce::ChangeBroadcaster
{
public:
    // View -------------------------------------------------------------------------------
    juce::int64 viewStart = 0;         ///< sample at the left edge of the timeline
    double samplesPerPixel = 400.0;
    double sampleRate = 48000.0;
    double tempo = timeformat::tempoBpm;   ///< session tempo (BPM): grid, ruler and bar counts follow it
    int trackHeight = 92;

    static constexpr double minSamplesPerPixel = 1.0;
    static constexpr double maxSamplesPerPixel = 48000.0;

    double sampleToX (juce::int64 sample) const noexcept { return (double) (sample - viewStart) / samplesPerPixel; }
    juce::int64 xToSample (double x) const noexcept      { return viewStart + (juce::int64) std::llround (x * samplesPerPixel); }
    double pixelsToSamples (double px) const noexcept    { return px * samplesPerPixel; }

    void setView (juce::int64 newStart, double newSamplesPerPixel)
    {
        newSamplesPerPixel = juce::jlimit (minSamplesPerPixel, maxSamplesPerPixel, newSamplesPerPixel);
        newStart = std::max<juce::int64> (0, newStart);
        if (newStart != viewStart || ! juce::exactlyEqual (newSamplesPerPixel, samplesPerPixel))
        {
            viewStart = newStart;
            samplesPerPixel = newSamplesPerPixel;
            sendChangeMessage();
        }
    }

    /** Zooms by `factor` (< 1 = in) keeping the sample under anchorX fixed. */
    void zoom (double factor, double anchorX)
    {
        const auto anchorSample = xToSample (anchorX);
        const double newSpp = juce::jlimit (minSamplesPerPixel, maxSamplesPerPixel, samplesPerPixel * factor);
        setView (anchorSample - (juce::int64) std::llround (anchorX * newSpp), newSpp);
    }

    // Tools ------------------------------------------------------------------------------
    EditTool tool = EditTool::smart;
    EditMode mode = EditMode::slip;

    void setTool (EditTool t) { tool = t; sendChangeMessage(); }
    void setMode (EditMode m) { mode = m; sendChangeMessage(); }

    /** The session's tempo and meter maps (the grid, ruler and bar counts follow them). */
    midi::TempoMap tempoMap { timeformat::tempoBpm };
    midi::MeterMap meterMap;

    /** The timeline sample of a musical position (ticks), and back. */
    juce::int64 ticksToSample (juce::int64 tick) const noexcept   { return tempoMap.ticksToSamples (tick, sampleRate); }
    juce::int64 sampleToTicks (juce::int64 sample) const noexcept { return tempoMap.samplesToTicks (sample, sampleRate); }

    /** In Grid mode: the nearest beat (beats follow the tempo and meter maps). */
    juce::int64 snap (juce::int64 sample) const noexcept
    {
        if (mode != EditMode::grid)
            return sample;
        const auto tick = sampleToTicks (sample);
        const auto p = meterMap.positionOf (tick);
        const auto beatTick = meterMap.barToTicks (p.bar) + p.beat * midi::MeterMap::ticksPerBeat (meterMap.meterAt (p.bar));
        const auto before = ticksToSample (beatTick);
        const auto after = ticksToSample (meterMap.beatAtOrAfter (beatTick + 1).tick);
        return sample - before <= after - sample ? before : after;
    }

    // Selection --------------------------------------------------------------------------
    juce::int64 selectionStart = 0;
    juce::int64 selectionEnd = 0;
    juce::Array<int> selectedTrackIds;
    juce::ValueTree selectedClip;

    bool hasTimeRange() const noexcept { return selectionEnd > selectionStart; }

    void setTimeSelection (juce::int64 a, juce::int64 b, juce::Array<int> trackIds)
    {
        selectionStart = juce::jmin (a, b);
        selectionEnd = juce::jmax (a, b);
        selectedTrackIds = std::move (trackIds);
        selectedClip = {};
        sendChangeMessage();
    }

    void selectClip (juce::ValueTree clip)
    {
        selectedClip = std::move (clip);
        sendChangeMessage();
    }

    // Automation view -------------------------------------------------------------------
    /** The automation lane a track shows, and the Pencil draws ("volume", "pan",
        "mute", "send:<slot>"), by track id. View state only: not saved, not undoable. */
    juce::String automationViewFor (int trackId) const
    {
        const auto it = automationViews.find (trackId);
        return it != automationViews.end() ? it->second : juce::String ("volume");
    }

    void setAutomationView (int trackId, const juce::String& parameter)
    {
        automationViews[trackId] = parameter;
        sendChangeMessage();
    }

    std::map<int, juce::String> automationViews;

    void clearSelection()
    {
        selectionStart = selectionEnd = 0;
        selectedTrackIds.clear();
        selectedClip = {};
        sendChangeMessage();
    }
};

} // namespace meridian
