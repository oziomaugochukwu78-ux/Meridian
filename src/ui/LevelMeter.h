#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Multi-segment LED-style peak meter (one or two channels).

    The owner feeds raw block peaks at its refresh rate via pushPeaks();
    the meter applies console ballistics itself: instant attack, a steady
    fall of 24 dB/s, a held peak line for 1.5 s, and a latching clip LED that
    is cleared by clicking the meter. */
class LevelMeter final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    explicit LevelMeter (int numChannels = 2);

    void setNumChannels (int numChannels);

    /** Linear peaks since the last call (UI thread). */
    void pushPeaks (float left, float right);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Channel
    {
        double levelDb = -100.0;
        double heldDb = -100.0;
        double heldAtMs = 0.0;
        bool clipped = false;
    };

    void update (Channel&, float peak, double nowMs, double elapsedMs);

    std::vector<Channel> channels;
    double lastUpdateMs = 0.0;
};

} // namespace meridian
