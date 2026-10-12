#include "AutomationRecorder.h"

#include "AppController.h"
#include "model/Automation.h"
#include "model/Routing.h"

namespace meridian
{
namespace
{
dsp::AutomationMode modeOf (const juce::ValueTree& t)
{
    const auto mode = track::automationMode (t);
    return mode == "touch" ? dsp::AutomationMode::touch
         : mode == "latch" ? dsp::AutomationMode::latch
         : mode == "write" ? dsp::AutomationMode::write
         : mode == "read"  ? dsp::AutomationMode::read
                           : dsp::AutomationMode::off;
}

bool records (dsp::AutomationMode m)
{
    return m == dsp::AutomationMode::touch || m == dsp::AutomationMode::latch || m == dsp::AutomationMode::write;
}

/** Finest change worth a breakpoint, per parameter. */
double toleranceFor (const juce::String& parameter)
{
    if (parameter == "pan")  return 0.002;
    if (parameter == "mute") return 0.0;
    return 0.05; // dB: volume and sends
}

uint32_t touchBitFor (const juce::String& parameter)
{
    if (parameter == "volume") return TrackRealtimeState::touchVolume;
    if (parameter == "pan")    return TrackRealtimeState::touchPan;
    if (parameter == "mute")   return TrackRealtimeState::touchMute;
    if (const int slot = automation::sendTarget (parameter); slot >= 0)
        return TrackRealtimeState::touchSend (slot);
    return 0;
}
} // namespace

AutomationRecorder::AutomationRecorder (AppController& a) : app (a)
{
    startTimer (20);
}

AutomationRecorder::~AutomationRecorder()
{
    stopTimer();
}

void AutomationRecorder::timerCallback()
{
    const auto& transport = app.getEngine().getTransport();
    // Not during a count-in: nothing plays yet.
    const bool rolling = transport.isRolling() && transport.getCountInRemaining() == 0;
    process (rolling, transport.getPosition());
}

void AutomationRecorder::setTouched (int trackId, const juce::String& parameter, bool isTouched)
{
    if (isTouched)
        held.insert ({ trackId, parameter });
    else
        held.erase ({ trackId, parameter });
    if (const auto bit = touchBitFor (parameter); bit != 0)
        app.getBridge().setControlTouched (trackId, bit, isTouched);
}

std::optional<double> AutomationRecorder::currentValue (const juce::ValueTree& t, const juce::String& parameter) const
{
    if (parameter == "volume")
        return juce::jmax (-144.0, track::volumeDb (t));
    if (parameter == "pan")
        return track::pan (t);
    if (parameter == "mute")
        return track::mute (t) ? 1.0 : 0.0;
    if (const int slot = automation::sendTarget (parameter); slot >= 0)
        if (const auto s = routing::findSend (t, slot); s.isValid())
            return juce::jmax (-144.0, send::levelDb (s));
    return std::nullopt;
}

void AutomationRecorder::beginPass (juce::int64 position)
{
    controls.clear();
    passActive = true;
    auto* session = app.getSession();
    if (session == nullptr)
        return;

    const auto glide = (juce::int64) (glideSeconds * session->getSampleRate());
    for (const auto& t : session->getTracksNode())
    {
        const auto mode = modeOf (t);
        if (! records (mode))
            continue;

        juce::StringArray parameters { "volume", "pan", "mute" };
        for (const auto& s : track::sends (t))
            parameters.add (automation::sendParameter (send::slot (s)));

        for (const auto& parameter : parameters)
        {
            const auto value = currentValue (t, parameter);
            if (! value.has_value())
                continue;
            std::vector<dsp::AutomationPoint> existing;
            for (const auto& p : automation::points (automation::lane (t, parameter)))
                existing.push_back ({ p.time, p.value });
            Control c;
            c.trackId = track::id (t);
            c.parameter = parameter;
            c.writer = std::make_unique<dsp::AutomationWriter> (mode, std::move (existing), *value, toleranceFor (parameter), glide);
            c.lastValue = *value;
            controls.push_back (std::move (c));
        }
    }
    lastPosition = position;
}

void AutomationRecorder::process (bool rolling, juce::int64 position)
{
    if (! rolling)
    {
        if (passActive)
            finishPass();
        return;
    }
    if (! passActive)
        beginPass (position);

    auto* session = app.getSession();
    if (session == nullptr)
        return;
    const auto holdSamples = (juce::int64) (changeHoldSeconds * session->getSampleRate());

    for (auto& c : controls)
    {
        const auto t = session->getTrackById (c.trackId);
        const auto value = t.isValid() ? currentValue (t, c.parameter) : std::nullopt;
        if (! value.has_value())
            continue;
        if (! juce::exactlyEqual (*value, c.lastValue))
        {
            c.changedAt = position; // a change counts as a touch for a moment
            c.lastValue = *value;
        }
        const bool touched = held.count ({ c.trackId, c.parameter }) != 0
                             || (c.changedAt >= 0 && position >= c.changedAt && position - c.changedAt < holdSamples);
        c.writer->update (position, *value, touched);
    }
    lastPosition = position;
}

void AutomationRecorder::finishPass()
{
    passActive = false;
    auto* session = app.getSession();
    if (session == nullptr)
    {
        controls.clear();
        return;
    }

    auto& um = session->getUndoManager();
    bool began = false;
    for (auto& c : controls)
    {
        if (! c.writer->hasRecorded())
            continue;
        auto t = session->getTrackById (c.trackId);
        if (! t.isValid())
            continue;
        const auto lane = c.writer->finish (lastPosition);
        if (! began)
            um.beginNewTransaction ("Write Automation");
        began = true;
        std::vector<automation::Point> points;
        for (const auto& p : lane)
            points.push_back ({ p.time, p.value });
        automation::setPoints (automation::getOrCreateLane (t, c.parameter, &um), points, &um);
    }
    controls.clear();
}

} // namespace meridian
