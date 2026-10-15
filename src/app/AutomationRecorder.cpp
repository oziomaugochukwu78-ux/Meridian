#include "AutomationRecorder.h"

#include "AppController.h"
#include "model/Automation.h"
#include "model/Routing.h"

#include <algorithm>

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
    if (automation::pluginTarget (parameter).first >= 0) return 0.001; // normalised 0..1
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
    else if (auto* p = findPluginParameter (trackId, parameter))
        app.getBridge().setParameterTouched (trackId, p, isTouched);
}

juce::AudioProcessorParameter* AutomationRecorder::findPluginParameter (int trackId, const juce::String& parameter) const
{
    const auto [slot, index] = automation::pluginTarget (parameter);
    if (slot < 0)
        return nullptr;
    const auto insert = app.getBridge().getInsert (trackId, slot);
    return insert != nullptr && insert->plugin != nullptr ? insert->plugin->getParameters()[index] : nullptr;
}

AutomationRecorder::PluginWatch::PluginWatch (AutomationRecorder& o, int track, int insertSlot, std::shared_ptr<InsertSlot> s)
    : owner (o), trackId (track), slot (insertSlot), insert (std::move (s))
{
    insert->plugin->addListener (this);
}

AutomationRecorder::PluginWatch::~PluginWatch()
{
    insert->plugin->removeListener (this);
}

void AutomationRecorder::PluginWatch::gesture (int index, bool starting)
{
    const auto parameter = automation::pluginParameter (slot, index);
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        owner.pluginGesture (trackId, parameter, starting);
        return;
    }
    // A plug-in's own thread: hand it to the message thread.
    juce::MessageManager::callAsync ([recorder = juce::WeakReference<AutomationRecorder> (&owner), id = trackId, parameter, starting]
    {
        if (auto* r = recorder.get())
            r->pluginGesture (id, parameter, starting);
    });
}

void AutomationRecorder::pluginGesture (int trackId, const juce::String& parameter, bool starting)
{
    if (! starting && held.count ({ trackId, parameter }) != 0)
        setTouched (trackId, parameter, false);
    auto* session = app.getSession();
    if (! passActive || session == nullptr)
        return;
    const auto t = session->getTrackById (trackId);
    if (! t.isValid())
        return;

    auto existing = std::find_if (controls.begin(), controls.end(),
                                  [&] (const Control& c) { return c.trackId == trackId && c.parameter == parameter; });
    Control* c = existing != controls.end() ? &*existing : addControl (t, parameter, modeOf (t));
    if (c == nullptr)
        return;
    if (starting)
        setTouched (trackId, parameter, true);
    else
        c->changedAt = lastPosition; // the end of a move counts as a change, like a fader's last step
}

AutomationRecorder::Control* AutomationRecorder::addControl (const juce::ValueTree& t, const juce::String& parameter, dsp::AutomationMode mode)
{
    const auto value = currentValue (t, parameter);
    if (! value.has_value())
        return nullptr;
    std::vector<dsp::AutomationPoint> existing;
    for (const auto& p : automation::points (automation::lane (t, parameter)))
        existing.push_back ({ p.time, p.value });
    const auto glide = (juce::int64) (glideSeconds * app.getSession()->getSampleRate());
    Control c;
    c.trackId = track::id (t);
    c.parameter = parameter;
    c.writer = std::make_unique<dsp::AutomationWriter> (mode, std::move (existing), *value, toleranceFor (parameter), glide);
    c.lastValue = *value;
    c.watchValue = automation::pluginTarget (parameter).first < 0;
    controls.push_back (std::move (c));
    return &controls.back();
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
    if (const auto* p = findPluginParameter (track::id (t), parameter))
        return (double) p->getValue();
    return std::nullopt;
}

void AutomationRecorder::beginPass (juce::int64 position)
{
    controls.clear();
    passActive = true;
    auto* session = app.getSession();
    if (session == nullptr)
        return;

    for (const auto& t : session->getTracksNode())
    {
        const auto mode = modeOf (t);
        if (! records (mode))
            continue;

        // Plug-in controls only where they are already automated (or once touched,
        // through the watches): Write would otherwise lane every knob of every plug-in.
        juce::StringArray parameters { "volume", "pan", "mute" };
        for (const auto& s : track::sends (t))
            parameters.add (automation::sendParameter (send::slot (s)));
        for (const auto& lane : track::automation (t))
            if (const auto parameter = lane[ids::parameter].toString(); automation::pluginTarget (parameter).first >= 0)
                parameters.addIfNotAlreadyThere (parameter);
        for (const auto& [heldTrack, heldParameter] : held)
            if (heldTrack == track::id (t) && automation::pluginTarget (heldParameter).first >= 0)
                parameters.addIfNotAlreadyThere (heldParameter);

        for (const auto& parameter : parameters)
            addControl (t, parameter, mode);

        for (const auto& insert : track::inserts (t))
            if (auto s = app.getBridge().getInsert (track::id (t), (int) insert[ids::slot]); s != nullptr && s->plugin != nullptr)
                pluginWatches.push_back (std::make_unique<PluginWatch> (*this, track::id (t), (int) insert[ids::slot], std::move (s)));
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
        if (c.watchValue && ! juce::exactlyEqual (*value, c.lastValue))
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
    // The watches go, so their gesture ends would never arrive: let go of held plug-in controls.
    pluginWatches.clear();
    for (const auto& [trackId, parameter] : std::set<std::pair<int, juce::String>> (held))
        if (automation::pluginTarget (parameter).first >= 0)
            setTouched (trackId, parameter, false);
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
