#include "SoakTest.h"

#include "media/AudioImporter.h"
#include "model/Automation.h"
#include "model/Routing.h"

#include <dsp/FaderTaper.h>

#include <algorithm>
#include <array>
#include <cmath>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
#elif JUCE_LINUX
 #include <unistd.h>
#endif

namespace meridian
{
namespace
{
constexpr double sampleRate = 48000.0;
constexpr double clipSeconds = 8.0;
constexpr int numTracks = 24, numBuses = 4;
constexpr double sampleIntervalSeconds = 10.0;
constexpr juce::int64 memoryGrowthLimitBytes = 4 * 1024 * 1024;
} // namespace

SoakTest::SoakTest (AppController& a, juce::File folder, double m, std::function<void (int)> done)
    : app (a), output (std::move (folder)), minutes (juce::jmax (0.5, m)), onFinished (std::move (done))
{
}

void SoakTest::start()
{
    output.deleteRecursively();
    output.createDirectory();
    app.setRememberRecentSessions (false);
    app.showProblem = [this] (const juce::String& title, const juce::String& message)
    {
        output.getChildFile ("soak-problems.txt").appendText (title + ": " + message + "\n");
    };

    juce::String error;
    if (! buildSession (error))
    {
        finish ("could not build the soak session: " + error);
        return;
    }
    if (! app.getEngine().isDeviceRunning())
    {
        finish ("no audio device is running; the soak test needs one");
        return;
    }

    app.locate (0);
    if (const auto played = app.transportPlay(); played.failed())
    {
        finish ("transport did not start: " + played.getErrorMessage());
        return;
    }

    startTime = lastSampleTime = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    app.getEngine().collectDiskUnderruns(); // reset
    output.getChildFile ("soak.csv").replaceWithText ("seconds,position,rolling,disk_underruns,xruns,cpu_percent,memory_mb,"
                                                      "worst_callback_percent_of_budget,callback_overruns\n");
    app.getEngine().collectWorstCallbackLoad(); // reset
    app.getEngine().collectCallbackOverruns();
    startTimer (250);
}

juce::File SoakTest::writeTone (const juce::String& name, double frequency, double seconds)
{
    const auto file = app.getSession()->getAudioFolder().getChildFile (name + ".wav");
    juce::String error;
    auto writer = createSessionWriter (file, sampleRate, 1, 24, false, error);
    if (writer == nullptr)
        return {};

    const int length = (int) (seconds * sampleRate);
    juce::AudioBuffer<float> buffer (1, length);
    for (int i = 0; i < length; ++i)
        buffer.setSample (0, i, (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * frequency * i / sampleRate)));
    writer->writeFromAudioSampleBuffer (buffer, 0, length);
    return file;
}

bool SoakTest::buildSession (juce::String& error)
{
    Session::Settings settings;
    settings.name = "Soak";
    settings.sampleRate = sampleRate;
    if (const auto created = app.createSession (output, settings, nullptr); created.failed())
    {
        error = created.getErrorMessage();
        return false;
    }

    auto* session = app.getSession();
    auto state = session->getState();
    state.setProperty (ids::masterVolumeDb, dsp::FaderTaper::minDb, nullptr); // silent: the work still happens
    state.setProperty (ids::consoleEnabled, true, nullptr);
    state.setProperty (ids::consoleDrive, 6.0, nullptr);

    // The test plug-in, when this build has it, adds real plug-in processing and latency.
    std::unique_ptr<juce::PluginDescription> plugin;
   #if defined (MERIDIAN_TEST_PLUGIN_BINARY)
    auto bundle = juce::File (MERIDIAN_TEST_PLUGIN_BINARY).getParentDirectory();
    while (bundle != bundle.getParentDirectory() && ! bundle.getFileName().endsWithIgnoreCase (".vst3"))
        bundle = bundle.getParentDirectory();
    for (auto* format : app.getPluginHost().getFormatManager().getFormats())
        if (format->getName() == "VST3")
        {
            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, bundle.getFullPathName());
            if (! found.isEmpty())
                plugin = std::make_unique<juce::PluginDescription> (*found[0]);
        }
   #endif

    juce::Array<int> buses;
    for (int b = 0; b < numBuses; ++b)
    {
        auto bus = session->addTrack ("Bus " + juce::String (b + 1), 2, nullptr, Session::TrackKind::aux);
        buses.add (track::id (bus));
        if (plugin != nullptr && b < 2)
        {
            juce::String pluginError;
            if (app.getBridge().insertPlugin (bus, 0, *plugin, pluginError))
                if (auto slot = app.getBridge().getInsert (track::id (bus), 0); slot != nullptr && slot->plugin != nullptr)
                    slot->plugin->getParameters()[1]->setValueNotifyingHost ((float) (256 * (b + 1)) / 8192.0f); // latency
        }
    }

    const auto length = (juce::int64) (clipSeconds * sampleRate);
    for (int i = 0; i < numTracks; ++i)
    {
        const auto file = writeTone ("Tone " + juce::String (i + 1), 110.0 * std::pow (2.0, i / 7.0), clipSeconds);
        if (! file.existsAsFile())
        {
            error = "could not write test audio";
            return false;
        }
        auto t = session->addTrack ("Track " + juce::String (i + 1), 1, nullptr);
        auto c = session->addClip (t, file, 0, 0, length, length, "Tone", nullptr);
        c.setProperty (ids::fadeIn, 2400, nullptr);
        c.setProperty (ids::fadeOut, 2400, nullptr);
        t.setProperty (ids::pan, std::sin (i * 0.7), nullptr);

        // Moving automation: a level wave with a breakpoint every half second.
        auto lane = automation::getOrCreateVolumeLane (t, nullptr);
        for (int p = 0; p <= (int) (clipSeconds * 2); ++p)
            automation::addPoint (lane, (juce::int64) (p * 0.5 * sampleRate), -12.0 + 9.0 * std::sin (p * 0.9 + i), nullptr);

        routing::setSend (*session, t, 0, buses[i % numBuses], nullptr);
        routing::findSend (t, 0).setProperty (ids::levelDb, -10.0, nullptr);
    }

    loopEnd = length;
    return true;
}

void SoakTest::timerCallback()
{
    if (finished)
        return;

    // Loop the session: back to the start once the playhead passes the end.
    if (app.getEngine().getTransport().getPosition() >= loopEnd)
    {
        app.locate (0);
        ++loops;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (now - lastSampleTime >= sampleIntervalSeconds)
    {
        lastSampleTime = now;
        takeSample();
    }

    if (now - startTime >= minutes * 60.0)
        finish();
}

void SoakTest::takeSample()
{
    Sample s;
    s.elapsedSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0 - startTime;
    s.position = app.getEngine().getTransport().getPosition();
    s.rolling = app.isRolling();
    s.diskUnderruns = app.getEngine().collectDiskUnderruns();
    if (auto* device = app.getDeviceManager().getCurrentAudioDevice())
        s.xruns = device->getXRunCount();
    s.cpu = app.getDeviceManager().getCpuUsage() * 100.0;
    s.memoryBytes = processMemoryBytes();
    s.worstCallbackLoad = app.getEngine().collectWorstCallbackLoad();
    s.callbackOverruns = app.getEngine().collectCallbackOverruns();
    samples.push_back (s);

    output.getChildFile ("soak.csv").appendText (juce::String (s.elapsedSeconds, 1) + "," + juce::String (s.position) + ","
                                                 + (s.rolling ? "1" : "0") + "," + juce::String (s.diskUnderruns) + ","
                                                 + juce::String (s.xruns) + "," + juce::String (s.cpu, 1) + ","
                                                 + (s.memoryBytes >= 0 ? juce::String ((double) s.memoryBytes / (1024.0 * 1024.0), 2) : juce::String ("n/a"))
                                                 + "," + juce::String (s.worstCallbackLoad * 100.0, 1) + "," + juce::String (s.callbackOverruns)
                                                 + "\n");
}

void SoakTest::finish (const juce::String& abortReason)
{
    finished = true;
    stopTimer();
    app.transportStop();

    juce::StringArray lines;
    int failures = 0;
    auto check = [&] (const juce::String& name, bool ok, const juce::String& detail)
    {
        lines.add ((ok ? "[PASS] " : "[FAIL] ") + name + "\n       " + detail);
        failures += ok ? 0 : 1;
    };

    if (abortReason.isNotEmpty())
    {
        check ("soak test ran", false, abortReason);
    }
    else if (samples.size() < 3)
    {
        check ("enough samples collected", false, juce::String ((int) samples.size()) + " samples");
    }
    else
    {
        // Warm-up: the first minute (or the first fifth of a short run) is excluded
        // from the memory check, while caches and buffers settle.
        const double warmUp = juce::jmin (60.0, minutes * 60.0 / 5.0);
        size_t baseline = 0;
        while (baseline + 1 < samples.size() && samples[baseline].elapsedSeconds < warmUp)
            ++baseline;

        int underruns = 0, overruns = 0;
        bool alwaysRolling = true, alwaysMoving = true;
        double cpuSum = 0.0, cpuMax = 0.0, worstLoad = 0.0;
        for (size_t i = 0; i < samples.size(); ++i)
        {
            underruns += samples[i].diskUnderruns;
            overruns += samples[i].callbackOverruns;
            worstLoad = juce::jmax (worstLoad, samples[i].worstCallbackLoad);
            alwaysRolling = alwaysRolling && samples[i].rolling;
            if (i > 0)
                alwaysMoving = alwaysMoving && samples[i].position != samples[i - 1].position;
            cpuSum += samples[i].cpu;
            cpuMax = juce::jmax (cpuMax, samples[i].cpu);
        }

        const auto& first = samples[baseline];
        const auto& last = samples.back();
        const auto played = last.elapsedSeconds / 60.0;

        check ("played for " + juce::String (played, 1) + " minutes (" + juce::String (loops) + " loops of the session)",
               played >= minutes * 0.95, {});
        check ("transport kept rolling and the playhead never stalled", alwaysRolling && alwaysMoving, {});
        check ("no disk underruns (streaming kept up)", underruns == 0, juce::String (underruns) + " blocks with missing data");

        // Meridian's own share: every callback must finish well inside the time the buffer allows.
        check ("Meridian's processing never overran its time budget", overruns == 0,
               juce::String (overruns) + " callbacks over budget");
        check ("worst callback used under 80 % of its time budget (real-time headroom)", worstLoad < 0.8,
               "worst " + juce::String (worstLoad * 100.0, 1) + " % of the buffer period");

        // Dropouts the driver reports: Meridian's fault only if a callback overran.
        if (first.xruns >= 0 && last.xruns >= 0)
        {
            const int dropouts = last.xruns - samples.front().xruns;
            if (dropouts == 0)
                check ("no audio dropouts (device xruns)", true, "0 dropouts reported by the driver");
            else if (overruns > 0)
                check ("no audio dropouts (device xruns)", false,
                       juce::String (dropouts) + " dropouts, and Meridian overran its budget " + juce::String (overruns) + " times");
            else
                lines.add ("[WARN] " + juce::String (dropouts) + " dropouts reported by the driver, not caused by Meridian\n"
                           "       Meridian's worst callback used " + juce::String (worstLoad * 100.0, 1)
                           + " % of its budget, so the audio thread was delayed by the OS or driver (other software, power"
                             " management, Wi-Fi/USB drivers). A larger buffer or an ASIO driver avoids this.");
        }
        else
        {
            lines.add ("[SKIP] audio dropouts\n       this driver does not report xruns");
        }

        if (first.memoryBytes >= 0 && last.memoryBytes >= 0)
        {
            // A leak grows steadily; one-off caches (waveform overviews, driver
            // buffers) step once and stay flat. So the check compares the start
            // and end of the second half of the run, each the median of three
            // samples to ignore momentary noise.
            auto medianOf3 = [this] (size_t from)
            {
                std::array<juce::int64, 3> m { samples[from].memoryBytes, samples[from + 1].memoryBytes, samples[from + 2].memoryBytes };
                std::sort (m.begin(), m.end());
                return m[1];
            };
            const auto mb = [] (juce::int64 bytes) { return juce::String ((double) bytes / 1048576.0, 2) + " MB"; };
            const size_t half = samples.size() / 2;
            if (samples.size() >= 8)
            {
                const auto early = medianOf3 (half), late = medianOf3 (samples.size() - 3);
                const auto settledSeconds = samples.back().elapsedSeconds - samples[half + 1].elapsedSeconds;
                check ("no memory leak: flat over the second half of the run (within 4 MB)", late - early <= memoryGrowthLimitBytes,
                       mb (early) + " -> " + mb (late) + " over " + juce::String (settledSeconds / 60.0, 1) + " min; whole run "
                           + mb (first.memoryBytes) + " -> " + mb (last.memoryBytes));
            }
            else
            {
                lines.add ("[SKIP] memory leak check\n       run too short (needs at least 8 samples, 80 s)");
            }
        }
        else
        {
            lines.add ("[SKIP] memory growth\n       process memory is not measured on this platform");
        }

        lines.add ("[INFO] CPU load: average " + juce::String (cpuSum / (double) samples.size(), 1) + " %, peak "
                   + juce::String (cpuMax, 1) + " %");
        if (auto* device = app.getDeviceManager().getCurrentAudioDevice())
            lines.add ("[INFO] device: " + device->getTypeName() + " / " + device->getName() + ", "
                       + juce::String (device->getCurrentBufferSizeSamples()) + " samples at "
                       + juce::String (device->getCurrentSampleRate() / 1000.0, 1) + " kHz ("
                       + juce::String (1000.0 * device->getCurrentBufferSizeSamples() / device->getCurrentSampleRate(), 1)
                       + " ms per buffer)");
    }

    const auto text = "Meridian soak test (" + juce::String (minutes, 1) + " min): "
                      + (failures == 0 ? juce::String ("PASSED") : juce::String (failures) + " FAILED") + "\n\n"
                      + lines.joinIntoString ("\n") + "\n";
    output.getChildFile ("soak-report.txt").replaceWithText (text);

    app.showProblem = nullptr;
    if (onFinished)
        onFinished (failures);
}

juce::int64 SoakTest::processMemoryBytes()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX counters {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&counters), sizeof (counters)))
        return (juce::int64) counters.PrivateUsage;
    return -1;
   #elif JUCE_LINUX
    // /proc/self/statm: size resident shared text lib data dt (in pages); "data" is the private heap + stacks.
    const auto fields = juce::StringArray::fromTokens (juce::File ("/proc/self/statm").loadFileAsString(), true);
    if (fields.size() >= 6)
        return fields[5].getLargeIntValue() * (juce::int64) sysconf (_SC_PAGESIZE);
    return -1;
   #else
    return -1;
   #endif
}

} // namespace meridian
