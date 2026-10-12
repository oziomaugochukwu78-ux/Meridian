#include "LevelMeter.h"

#include "Theme.h"

#include <dsp/MeterScale.h>

namespace meridian
{
namespace
{
constexpr double fallDbPerSecond = 24.0;
constexpr double peakHoldMs = 1500.0;
constexpr int numSegments = 40;

juce::Colour colourForZone (dsp::MeterScale::Zone zone)
{
    switch (zone)
    {
        case dsp::MeterScale::Zone::red:    return theme::meterRed;
        case dsp::MeterScale::Zone::orange: return theme::meterOrange;
        case dsp::MeterScale::Zone::yellow: return theme::meterYellow;
        case dsp::MeterScale::Zone::green:  break;
    }
    return theme::meterGreen;
}

/** The dB value at the top edge of segment i (0 = bottom), inverting MeterScale. */
double segmentTopDb (int i)
{
    const double position = (double) (i + 1) / numSegments;
    if (position <= 0.4)
        return dsp::MeterScale::floorDb + position / 0.4 * (-20.0 - dsp::MeterScale::floorDb);
    return -20.0 + (position - 0.4) / 0.6 * 20.0;
}
} // namespace

LevelMeter::LevelMeter (int numChannels)
{
    setNumChannels (numChannels);
    setTooltip ("Peak meter. Click to clear the clip indicator.");
}

void LevelMeter::setNumChannels (int numChannels)
{
    channels.assign ((size_t) juce::jlimit (1, 2, numChannels), {});
    repaint();
}

void LevelMeter::update (Channel& c, float peak, double nowMs, double elapsedMs)
{
    const double db = dsp::MeterScale::dbForGain (peak);
    const double fallen = c.levelDb - fallDbPerSecond * elapsedMs / 1000.0;
    c.levelDb = juce::jmax (db, fallen);

    if (db >= c.heldDb || nowMs - c.heldAtMs > peakHoldMs)
    {
        c.heldDb = db;
        c.heldAtMs = nowMs;
    }
    if (peak >= 1.0f)
        c.clipped = true;
}

void LevelMeter::pushPeaks (float left, float right)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double elapsed = lastUpdateMs > 0.0 ? juce::jmin (200.0, now - lastUpdateMs) : 0.0;
    lastUpdateMs = now;

    update (channels[0], left, now, elapsed);
    if (channels.size() > 1)
        update (channels[1], right, now, elapsed);
    repaint();
}

void LevelMeter::mouseDown (const juce::MouseEvent&)
{
    for (auto& c : channels)
        c.clipped = false;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    g.setColour (theme::divider);
    g.fillRect (area);
    area.reduce (1, 1);

    const int clipLedHeight = 4;
    auto clipRow = area.removeFromTop (clipLedHeight);
    area.removeFromTop (1);

    const int gap = 1;
    const int numCh = (int) channels.size();
    const int columnWidth = (area.getWidth() - gap * (numCh - 1)) / numCh;
    const float segmentHeight = (float) area.getHeight() / numSegments;

    for (int ch = 0; ch < numCh; ++ch)
    {
        const auto& c = channels[(size_t) ch];
        const int x = area.getX() + ch * (columnWidth + gap);

        g.setColour (c.clipped ? theme::meterRed : theme::meterOff);
        g.fillRect (x, clipRow.getY(), columnWidth, clipLedHeight);

        for (int i = 0; i < numSegments; ++i)
        {
            const double topDb = segmentTopDb (i);
            const double bottomDb = i == 0 ? -1000.0 : segmentTopDb (i - 1);
            const bool lit = c.levelDb > bottomDb;
            const bool isHeld = c.heldDb > bottomDb && c.heldDb <= topDb && c.heldDb > dsp::MeterScale::floorDb;

            const float yTop = (float) area.getBottom() - (float) (i + 1) * segmentHeight;
            const auto colour = colourForZone (dsp::MeterScale::zoneForDb (topDb - 0.01));
            g.setColour (lit || isHeld ? colour : theme::meterOff);
            g.fillRect (juce::Rectangle<float> ((float) x, yTop + 0.5f, (float) columnWidth, segmentHeight - 1.0f));
        }
    }
}

} // namespace meridian
