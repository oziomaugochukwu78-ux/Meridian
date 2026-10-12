#pragma once

#include "AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Long-running stability test, run with:  Meridian --soak <minutes> <output folder>

    Builds a demanding session and plays it in a loop through the real audio
    device for the given time:
      - 24 tracks with moving volume automation and panning
      - sends into 4 buses, two of them carrying a latent plug-in (so delay
        compensation is in play)
      - the oversampled console stage on
    The master fader is fully down, so the machine stays silent while the
    engine does all of its work.

    Every 10 seconds it records the playhead, disk underruns, device dropouts
    (xruns), CPU load and process memory to soak.csv. At the end it checks,
    after a warm-up period:
      - no disk underruns and no dropouts
      - the playhead never stalled
      - memory stayed flat
    It writes soak-report.txt and exits with 0 only if everything held. */
class SoakTest final : private juce::Timer
{
public:
    SoakTest (AppController&, juce::File outputFolder, double minutes, std::function<void (int failures)> onFinished);

    void start();

private:
    struct Sample
    {
        double elapsedSeconds = 0.0;
        juce::int64 position = 0;
        bool rolling = false;
        int diskUnderruns = 0;  // since the previous sample
        int xruns = -1;         // device total; -1 = not reported by the driver
        double cpu = 0.0;
        juce::int64 memoryBytes = -1;
        double worstCallbackLoad = 0.0; // longest callback / buffer period, this interval
        int callbackOverruns = 0;       // callbacks over budget, this interval
    };

    void timerCallback() override;
    bool buildSession (juce::String& error);
    juce::File writeTone (const juce::String& name, double frequency, double seconds);
    void takeSample();
    void finish (const juce::String& abortReason = {});

    /** Private (committed) memory of this process, or -1 where unsupported. */
    static juce::int64 processMemoryBytes();

    AppController& app;
    juce::File output;
    double minutes;
    std::function<void (int)> onFinished;

    std::vector<Sample> samples;
    double startTime = 0.0, lastSampleTime = 0.0;
    juce::int64 loopEnd = 0;
    int loops = 0;
    bool finished = false;
};

} // namespace meridian
