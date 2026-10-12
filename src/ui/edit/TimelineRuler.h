#pragma once

#include "EditState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace meridian
{

/** Two timebase rulers above the tracks: Bars|Beats and Min:Secs.
    Clicking a ruler moves the playback cursor there. */
class TimelineRuler final : public juce::Component,
                            private juce::ChangeListener
{
public:
    explicit TimelineRuler (EditState&);
    ~TimelineRuler() override;

    std::function<void (juce::int64)> onLocate;

    void setPlayheadPosition (juce::int64 sample);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    static constexpr int rowHeight = 20;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void paintBarsBeats (juce::Graphics&, juce::Rectangle<int> row);
    void paintMinSecs (juce::Graphics&, juce::Rectangle<int> row);
    void locateFromMouse (const juce::MouseEvent&);

    EditState& state;
    juce::int64 playhead = 0;
};

} // namespace meridian
