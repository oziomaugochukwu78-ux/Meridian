#pragma once

#include "AppController.h"
#include "MainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** End-to-end self test, run with:  Meridian --self-test <output folder>

    Drives the real application (session model, engine, offline bounce, media
    jobs, remote command layer, UI) through scripted scenarios and verifies
    the audio numerically: every expectation is an exact level, frequency or
    length with a stated tolerance. Writes report.txt / report.json and PNG
    screenshots of every window and dialog to the output folder.

    Phase 1 runs synchronously (offline renders need no audio device).
    Phase 2 is timed: it plays through the real device, if one is running,
    to check live disk streaming, then captures the screenshots once
    waveforms have loaded. */
/** Collects JUCE assertion failures from any thread. Needs JUCE_LOG_ASSERTIONS,
    which routes them through juce::Logger; installed before the main window
    is built, so assertions during start-up are caught as well. */
class AssertionLog final : public juce::Logger
{
public:
    void logMessage (const juce::String& message) override
    {
        if (message.startsWith ("JUCE Assertion failure"))
        {
            const juce::ScopedLock sl (lock);
            failures.addIfNotAlreadyThere (message);
        }
    }

    juce::StringArray getFailures() const
    {
        const juce::ScopedLock sl (lock);
        return failures;
    }

private:
    juce::CriticalSection lock;
    juce::StringArray failures;
};

class SelfTest final : private juce::Timer
{
public:
    /** includeRecording: also record ~1 s from the default input (opt-in, since it
        uses the microphone). The recording is deleted after it is measured. */
    SelfTest (AppController&, MainComponent&, juce::File outputFolder, bool includeRecording,
              std::function<void (int failures)> onFinished);

    void start();

    /** Rewrite the reference renders instead of comparing against them. */
    void setUpdateGoldenFiles (bool shouldUpdate) noexcept { updateGolden = shouldUpdate; }

    /** On machines without a sound card (CI servers) the live-device checks
        are reported as SKIP instead of FAIL. */
    void setAllowNoAudioDevice (bool shouldAllow) noexcept { allowNoAudioDevice = shouldAllow; }

    /** Every assertion it collected becomes a failure in the report. */
    void setAssertionLog (const AssertionLog* log) noexcept { assertionLog = log; }

private:
    struct Check
    {
        juce::String group, name;
        bool passed = false;
        juce::String detail;
        bool skipped = false;
    };

    void timerCallback() override;

    void expect (const juce::String& name, bool passed, const juce::String& detail);
    void expectNear (const juce::String& name, double actual, double expected, double tolerance, const juce::String& unit);
    void skip (const juce::String& name, const juce::String& reason);
    void trace (const juce::String& message);
    void beginGroup (const juce::String& name) { group = name; }

    // Phase 1
    bool createSession();
    void testLevelsAndPanning();
    void testMuteSoloAndSum();
    void testFadesAndAutomation();
    void testEditingAndUndo();
    void testConsoleSaturation();
    void testSessionRoundTrip();
    void testImportAndResampling();
    void testElasticAudio();
    void testPluginHosting();
    void testBusesAndSends();
    void testMcpTools();
    void testRemoteCommands();
    void testGoldenRenders();
    void testInstruments();
    void testStress();
    void testRobustness();
    void testDamagedSessions (const juce::File& originalSession);
    void testRemoteFuzz();

    // Phase 2
    bool setUpDelayCompensationTest();
    void checkDelayCompensation();
    void buildShowcaseSession();
    void startLivePlayback();
    void finishLivePlayback();
    void checkStopReturned();
    bool startRecordingTest();
    void finishRecordingTest();
    void testUserInterface();
    void testPianoRoll();
    void captureScreens();
    void finish();

    // Simulated mouse input on a real component.
    static juce::MouseEvent mouseEvent (juce::Component&, juce::Point<float> position, juce::Point<float> downPosition,
                                        bool dragged, juce::ModifierKeys = {});
    static void drag (juce::Component&, juce::Point<float> from, juce::Point<float> delta, juce::ModifierKeys = {});

    // Helpers
    juce::File writeTone (const juce::String& name, double sampleRate, double seconds, double frequency,
                          double amplitudeDb, int channels);
    juce::File writeNoise (const juce::String& name, double seconds, double amplitudeDb, juce::uint32 seed);
    void compareWithReference (const juce::String& name, const BounceResult&);
    juce::ValueTree addToneTrack (const juce::String& name, int channels, const juce::File& file, juce::int64 start);
    BounceResult bounce (const juce::String& name, juce::int64 start, juce::int64 end);
    juce::AudioBuffer<float> readAudio (const juce::File&, double* sampleRate = nullptr);
    void clearTracks();
    void setConsole (bool enabled, double driveDb = 6.0, double character = 0.35);
    void snapshot (juce::Component&, const juce::String& fileName);

    AppController& app;
    MainComponent& main;
    juce::File output, media;
    std::function<void (int)> onFinished;

    std::vector<Check> checks;
    const AssertionLog* assertionLog = nullptr;
    juce::String group;
    int phase = 0;
    int ticks = 0;
    bool includeRecording = false;
    bool updateGolden = false;
    bool allowNoAudioDevice = false;
    juce::ValueTree recordTrack;
    juce::int64 recordStartPosition = 0;
    juce::File recordingToDelete;
    std::unique_ptr<juce::PluginDescription> testPlugin; // found by testPluginHosting()
    juce::ValueTree pdcLatentTrack, pdcDryTrack;
    juce::int64 livePositionAtStart = 0;
    double sampleRate = 48000.0;
};

} // namespace meridian
