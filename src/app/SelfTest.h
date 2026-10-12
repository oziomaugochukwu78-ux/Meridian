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
class SelfTest final : private juce::Timer
{
public:
    /** includeRecording: also record ~1 s from the default input (opt-in, since it
        uses the microphone). The recording is deleted after it is measured. */
    SelfTest (AppController&, MainComponent&, juce::File outputFolder, bool includeRecording,
              std::function<void (int failures)> onFinished);

    void start();

private:
    struct Check
    {
        juce::String group, name;
        bool passed = false;
        juce::String detail;
    };

    void timerCallback() override;

    void expect (const juce::String& name, bool passed, const juce::String& detail);
    void expectNear (const juce::String& name, double actual, double expected, double tolerance, const juce::String& unit);
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
    void testRemoteCommands();

    // Phase 2
    void buildShowcaseSession();
    void startLivePlayback();
    void finishLivePlayback();
    void checkStopReturned();
    bool startRecordingTest();
    void finishRecordingTest();
    void testUserInterface();
    void captureScreens();
    void finish();

    // Simulated mouse input on a real component.
    static juce::MouseEvent mouseEvent (juce::Component&, juce::Point<float> position, juce::Point<float> downPosition,
                                        bool dragged, juce::ModifierKeys = {});
    static void drag (juce::Component&, juce::Point<float> from, juce::Point<float> delta, juce::ModifierKeys = {});

    // Helpers
    juce::File writeTone (const juce::String& name, double sampleRate, double seconds, double frequency,
                          double amplitudeDb, int channels);
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
    juce::String group;
    int phase = 0;
    int ticks = 0;
    bool includeRecording = false;
    juce::ValueTree recordTrack;
    juce::int64 recordStartPosition = 0;
    juce::File recordingToDelete;
    juce::int64 livePositionAtStart = 0;
    double sampleRate = 48000.0;
};

} // namespace meridian
