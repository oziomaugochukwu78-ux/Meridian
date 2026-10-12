#pragma once

#include "model/Session.h"

#include <dsp/AutomationWriter.h>

#include <juce_events/juce_events.h>

#include <map>
#include <set>

namespace meridian
{
class AppController;

/** Records automation passes for tracks in Touch, Latch or Write mode.

    While the transport rolls it samples every recordable control of those
    tracks (volume, pan, mute, send levels) every 20 ms, with whether the user
    is holding it; a change of value (a mute click, an AI tool moving a fader)
    counts as a touch for a moment. When the transport stops, each pass is
    merged into its lane (dsp::AutomationWriter) as one undo step,
    "Write Automation".

    Message thread only. process() is public so a test can drive a pass with
    simulated playhead positions. */
class AutomationRecorder final : private juce::Timer
{
public:
    explicit AutomationRecorder (AppController&);
    ~AutomationRecorder() override;

    /** The UI holds (or lets go of) a control. parameter: "volume", "pan",
        "mute" or "send:<slot>". While held in Touch or Latch the engine
        follows the control instead of the lane. */
    void setTouched (int trackId, const juce::String& parameter, bool isTouched);

    /** One step: rolling = the transport is playing or recording; position =
        the playhead. Starts a pass on the first rolling step and finishes it
        on the first step that is not rolling. */
    void process (bool rolling, juce::int64 position);

    bool isPassActive() const noexcept { return passActive; }

    /** Touch-mode glide back to the existing automation after release. */
    static constexpr double glideSeconds = 0.25;
    /** A change of value counts as a touch for this long. */
    static constexpr double changeHoldSeconds = 0.3;

private:
    struct Control
    {
        int trackId = 0;
        juce::String parameter;
        std::unique_ptr<dsp::AutomationWriter> writer;
        double lastValue = 0.0;
        juce::int64 changedAt = -1;
    };

    void timerCallback() override;
    void beginPass (juce::int64 position);
    void finishPass();
    std::optional<double> currentValue (const juce::ValueTree& track, const juce::String& parameter) const;

    AppController& app;
    std::vector<Control> controls;
    std::set<std::pair<int, juce::String>> held;
    bool passActive = false;
    juce::int64 lastPosition = 0;
};

} // namespace meridian
