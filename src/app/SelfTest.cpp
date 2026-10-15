#include "SelfTest.h"

#include "CommandIds.h"
#include "media/AudioImporter.h"
#include "model/Automation.h"
#include "model/ConsolePresets.h"
#include "model/MidiModel.h"
#include "model/Playlists.h"
#include "model/Routing.h"
#include "model/SessionRepair.h"
#include "media/TimeStretchEngine.h"
#include "engine/ClipStream.h"
#include "model/SessionBackups.h"
#include "plugins/BuiltinEffects.h"

#include <dsp/Biquad.h>
#include <dsp/Dither.h>
#include <dsp/Loudness.h>
#include "ui/dialogs/DashboardComponent.h"
#include "ui/dialogs/NewTracksComponent.h"
#include "ui/dialogs/PlaybackEngineComponent.h"
#include "ui/dialogs/TimeStretchComponent.h"

#include <dsp/FaderTaper.h>
#include <dsp/SignalAnalysis.h>
#include <remote/CommandDispatcher.h>
#include <remote/JsonWriter.h>

#include <cmath>
#include <limits>

namespace meridian
{
namespace
{
constexpr double toneDb = -6.020599913279624;   // 20*log10(0.5): test tones peak at exactly 0.5
constexpr double panCentreDb = -3.010299956639812; // constant-power centre, per side
constexpr juce::int64 clipStart = 24000;          // 0.5 s in, so every ramp settles before audio starts
constexpr double silentDb = -140.0;

double db (double gain) { return dsp::gainToDb (gain); }

juce::String fmt (double v, int decimals = 3) { return juce::String (v, decimals); }
} // namespace

SelfTest::SelfTest (AppController& a, MainComponent& m, juce::File folder, bool recording, std::function<void (int)> done)
    : app (a), main (m), output (std::move (folder)), onFinished (std::move (done)), includeRecording (recording)
{
}

void SelfTest::start()
{
    output.deleteRecursively();
    output.createDirectory();
    trace ("Meridian self-test started");
    app.setRememberRecentSessions (false); // keep test sessions out of the Dashboard's Recent list
    media = output.getChildFile ("media");
    media.createDirectory();

    // Problems are recorded as failures instead of popping up dialogs.
    app.showProblem = [this] (const juce::String& title, const juce::String& message)
    {
        expect ("no unexpected problem dialogs", false, title + ": " + message);
    };

    if (createSession())
    {
        testLevelsAndPanning();
        testMuteSoloAndSum();
        testFadesAndAutomation();
        testEditingAndUndo();
        testConsoleSaturation();
        testConsolePresets();
        testSessionRoundTrip();
        testImportAndResampling();
        testElasticAudio();
        testPluginHosting();
        testBusesAndSends();
        testGoldenRenders();
        testInstruments();
        testBuiltinEffects();
        testLoopClickMarkers();
        testMidiInput();
        testAutomation();
        testLoudnessAndExport();
        testTrackEditing();
        testPlaylists();
        testPunchRecording();
        testTempoAndMeter();
        testPluginInstrument();
        testSustainPedal();
        testPitchAndModWheels();
        testStress();
        testRobustness(); // before the remote tests: their BOUNCE runs in the background
        testMcpTools();
        testRemoteCommands();
    }

    phase = 1;
    startTimer (100);
}

//==============================================================================
void SelfTest::expect (const juce::String& name, bool passed, const juce::String& detail)
{
    checks.push_back ({ group, name, passed, detail });
    trace ((passed ? "[PASS] " : "[FAIL] ") + name);
}

void SelfTest::trace (const juce::String& message)
{
    // Flushed line by line, so if the process dies the last line shows where.
    output.getChildFile ("progress.log").appendText (message + "\n");
}

void SelfTest::skip (const juce::String& name, const juce::String& reason)
{
    checks.push_back ({ group, name, true, reason, true });
}

void SelfTest::expectNear (const juce::String& name, double actual, double expected, double tolerance, const juce::String& unit)
{
    const bool ok = std::abs (actual - expected) <= tolerance;
    expect (name, ok, "measured " + fmt (actual) + " " + unit + ", expected " + fmt (expected) + " +/- " + fmt (tolerance) + " " + unit);
}

//==============================================================================
// Helpers

juce::File SelfTest::writeTone (const juce::String& name, double rate, double seconds, double frequency,
                                double amplitudeDb, int channels)
{
    const auto file = media.getChildFile (name + ".wav");
    juce::String error;
    auto writer = createSessionWriter (file, rate, channels, 32, false, error);
    if (writer == nullptr)
    {
        expect ("write test tone " + name, false, error);
        return file;
    }

    const int length = (int) std::llround (seconds * rate);
    const double amplitude = std::pow (10.0, amplitudeDb / 20.0);
    juce::AudioBuffer<float> buffer (channels, length);
    for (int i = 0; i < length; ++i)
    {
        const auto value = (float) (amplitude * std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate));
        for (int ch = 0; ch < channels; ++ch)
            buffer.setSample (ch, i, value);
    }
    writer->writeFromAudioSampleBuffer (buffer, 0, length);
    return file;
}

juce::File SelfTest::writeNoise (const juce::String& name, double seconds, double amplitudeDb, juce::uint32 seed)
{
    const auto file = media.getChildFile (name + ".wav");
    juce::String error;
    auto writer = createSessionWriter (file, sampleRate, 1, 32, false, error);
    if (writer == nullptr)
    {
        expect ("write test noise " + name, false, error);
        return file;
    }

    // xorshift32: the same sequence on every platform and compiler, unlike
    // std:: distributions, so reference renders stay comparable everywhere.
    const int length = (int) std::llround (seconds * sampleRate);
    const double amplitude = std::pow (10.0, amplitudeDb / 20.0);
    juce::AudioBuffer<float> buffer (1, length);
    auto state = seed != 0 ? seed : 1u;
    for (int i = 0; i < length; ++i)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        buffer.setSample (0, i, (float) (amplitude * ((double) state / 2147483648.0 - 1.0)));
    }
    writer->writeFromAudioSampleBuffer (buffer, 0, length);
    return file;
}

juce::ValueTree SelfTest::addToneTrack (const juce::String& name, int channels, const juce::File& file, juce::int64 start)
{
    auto* session = app.getSession();
    std::unique_ptr<juce::AudioFormatReader> reader (app.getFormatManager().createReaderFor (file));
    const auto length = reader != nullptr ? reader->lengthInSamples : 0;

    auto t = session->addTrack (name, channels, nullptr);
    session->addClip (t, file, start, 0, length, length, name, nullptr);
    return t;
}

BounceResult SelfTest::bounce (const juce::String& name, juce::int64 start, juce::int64 end)
{
    auto request = app.makeSessionBounceRequest();
    request.start = start;
    request.end = end;
    request.bitDepth = 32;
    request.aiff = false;
    request.format = "wav";
    request.dither = false;
    request.destination = output.getChildFile ("bounces").getChildFile (name + ".wav");
    request.destination.getParentDirectory().createDirectory();

    auto result = app.bounceNow (request);
    if (! result.ok)
        expect ("bounce " + name, false, result.error);
    return result;
}

juce::AudioBuffer<float> SelfTest::readAudio (const juce::File& file, double* rate)
{
    std::unique_ptr<juce::AudioFormatReader> reader (app.getFormatManager().createReaderFor (file));
    if (reader == nullptr)
        return {};
    if (rate != nullptr)
        *rate = reader->sampleRate;
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), 0, buffer.getNumSamples());
    return buffer;
}

void SelfTest::clearTracks()
{
    auto* session = app.getSession();
    while (session->getNumTracks() > 0)
        session->removeTrack (session->getTrack (0), nullptr);
}

void SelfTest::setConsole (bool enabled, double driveDb, double character)
{
    auto state = app.getSession()->getState();
    state.setProperty (ids::consoleEnabled, enabled, nullptr);
    state.setProperty (ids::consoleDrive, driveDb, nullptr);
    state.setProperty (ids::consoleCharacter, character, nullptr);
    state.setProperty (ids::consoleCurve, 0, nullptr);
}

//==============================================================================
// Phase 1: offline, numerical

bool SelfTest::createSession()
{
    beginGroup ("Session");
    Session::Settings settings;
    settings.name = "SelfTestSession";
    settings.sampleRate = sampleRate;
    settings.bitDepth = 24;

    const auto result = app.createSession (output, settings, nullptr);
    expect ("create a 48 kHz session", result.wasOk(), result.getErrorMessage());
    if (result.failed())
        return false;

    setConsole (false); // exact-level checks need the clean bus; the console is tested on its own
    return true;
}

void SelfTest::testLevelsAndPanning()
{
    beginGroup ("Levels and panning");
    const auto tone = writeTone ("tone1k_mono", sampleRate, 2.0, 1000.0, toneDb, 1);
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;

    auto t = addToneTrack ("Mono", 1, tone, clipStart);
    auto r = bounce ("mono_centre", 0, end);
    expectNear ("mono track, centre pan: left peak (-6 dB tone, -3 dB pan law)", db (r.left.peak), toneDb + panCentreDb, 0.02, "dB");
    expectNear ("mono track, centre pan: right peak", db (r.right.peak), toneDb + panCentreDb, 0.02, "dB");
    expectNear ("bounce length", (double) r.length, (double) end, 0.0, "samples");

    t.setProperty (ids::volumeDb, -6.0, nullptr);
    r = bounce ("fader_minus6", 0, end);
    expectNear ("fader at -6 dB lowers the output by exactly 6 dB", db (r.left.peak), toneDb + panCentreDb - 6.0, 0.02, "dB");

    t.setProperty (ids::volumeDb, 0.0, nullptr);
    t.setProperty (ids::pan, -1.0, nullptr);
    r = bounce ("pan_hard_left", 0, end);
    expectNear ("hard-left pan: left at full level", db (r.left.peak), toneDb, 0.02, "dB");
    expect ("hard-left pan: right is silent", db (r.right.peak) < silentDb, "right peak " + fmt (db (r.right.peak)) + " dB");

    t.setProperty (ids::pan, 1.0, nullptr);
    r = bounce ("pan_hard_right", 0, end);
    expectNear ("hard-right pan: right at full level", db (r.right.peak), toneDb, 0.02, "dB");
    expect ("hard-right pan: left is silent", db (r.left.peak) < silentDb, "left peak " + fmt (db (r.left.peak)) + " dB");

    clearTracks();
    const auto stereoTone = writeTone ("tone1k_stereo", sampleRate, 2.0, 1000.0, toneDb, 2);
    addToneTrack ("Stereo", 2, stereoTone, clipStart);
    r = bounce ("stereo_centre", 0, end);
    expectNear ("stereo track at centre passes through at unity (left)", db (r.left.peak), toneDb, 0.02, "dB");
    expectNear ("stereo track at centre passes through at unity (right)", db (r.right.peak), toneDb, 0.02, "dB");
    clearTracks();
}

void SelfTest::testMuteSoloAndSum()
{
    beginGroup ("Mute, solo and summing");
    const auto tone1k = media.getChildFile ("tone1k_mono.wav");
    const auto tone500 = writeTone ("tone500_mono", sampleRate, 2.0, 500.0, toneDb, 1);
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const int from = (int) clipStart + 4800, count = (int) sampleRate; // 1 s: whole cycles of 500 Hz and 1 kHz
    const double expectedAmp = 0.5 * std::sqrt (0.5);

    auto a = addToneTrack ("A 1kHz", 1, tone1k, clipStart);
    auto b = addToneTrack ("B 500Hz", 1, tone500, clipStart);

    a.setProperty (ids::mute, true, nullptr);
    auto r = bounce ("mute_a", 0, end);
    auto audio = readAudio (r.file);
    const float* left = audio.getReadPointer (0, from);
    expect ("muted track contributes nothing", db (dsp::toneAmplitude (left, count, 1000.0, sampleRate)) < silentDb,
            "1 kHz at " + fmt (db (dsp::toneAmplitude (left, count, 1000.0, sampleRate))) + " dB");
    expectNear ("other track unaffected by the mute", dsp::toneAmplitude (left, count, 500.0, sampleRate), expectedAmp, 0.002, "");

    a.setProperty (ids::mute, false, nullptr);
    b.setProperty (ids::solo, true, nullptr);
    r = bounce ("solo_b", 0, end);
    audio = readAudio (r.file);
    left = audio.getReadPointer (0, from);
    expect ("solo silences the other track", db (dsp::toneAmplitude (left, count, 1000.0, sampleRate)) < silentDb,
            "1 kHz at " + fmt (db (dsp::toneAmplitude (left, count, 1000.0, sampleRate))) + " dB");
    expectNear ("soloed track plays", dsp::toneAmplitude (left, count, 500.0, sampleRate), expectedAmp, 0.002, "");

    b.setProperty (ids::solo, false, nullptr);
    clearTracks();
    addToneTrack ("Sum A", 1, tone1k, clipStart);
    addToneTrack ("Sum B", 1, tone1k, clipStart);
    r = bounce ("sum_identical", 0, end);
    expectNear ("two identical tracks sum to +6.02 dB", db (r.left.peak), toneDb + panCentreDb + 6.0206, 0.02, "dB");
    clearTracks();
}

void SelfTest::testFadesAndAutomation()
{
    beginGroup ("Fades and automation");
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const int window = 480; // 10 ms

    auto t = addToneTrack ("Fade", 1, tone, clipStart);
    auto c = track::clips (t).getChild (0);
    c.setProperty (ids::fadeIn, 4800, nullptr); // 100 ms
    auto r = bounce ("fade_in", 0, end);
    auto audio = readAudio (r.file);

    bool rising = true;
    double previous = -1.0;
    juce::String envelope;
    for (int w = 0; w < 10; ++w)
    {
        const auto stats = dsp::measureLevels (audio.getReadPointer (0, (int) clipStart + w * window), window);
        rising = rising && stats.rms > previous;
        previous = stats.rms;
        envelope << fmt (stats.rms, 3) << " ";
    }
    const auto first = dsp::measureLevels (audio.getReadPointer (0, (int) clipStart), window);
    const auto steady = dsp::measureLevels (audio.getReadPointer (0, (int) clipStart + 9600), window);
    expect ("100 ms fade-in rises steadily", rising, "RMS per 10 ms: " + envelope);
    expect ("fade-in starts near silence", first.rms < 0.2 * steady.rms,
            "first 10 ms RMS " + fmt (first.rms) + " vs steady " + fmt (steady.rms));
    expectNear ("after the fade the level is unchanged", steady.rms, 0.5 * std::sqrt (0.5) / std::sqrt (2.0), 0.002, "RMS");

    c.setProperty (ids::fadeIn, 0, nullptr);
    auto lane = juce::ValueTree (ids::LANE);
    lane.setProperty (ids::parameter, "volume", nullptr);
    for (const auto& [time, value] : { std::pair<juce::int64, double> { clipStart, -60.0 }, { clipStart + 48000, 0.0 } })
    {
        juce::ValueTree point (ids::POINT);
        point.setProperty (ids::time, time, nullptr);
        point.setProperty (ids::value, value, nullptr);
        lane.appendChild (point, nullptr);
    }
    track::automation (t).appendChild (lane, nullptr);

    r = bounce ("volume_automation", 0, end);
    audio = readAudio (r.file);
    bool increasing = true;
    previous = -1.0;
    envelope.clear();
    for (int step = 0; step < 10; ++step)
    {
        const auto stats = dsp::measureLevels (audio.getReadPointer (0, (int) clipStart + step * 4800), 480);
        increasing = increasing && stats.rms > previous;
        previous = stats.rms;
        envelope << fmt (stats.rms, 3) << " ";
    }
    expect ("volume automation ramp (-60 dB to 0 dB over 1 s) is followed", increasing, "RMS every 100 ms: " + envelope);
    const auto settled = dsp::measureLevels (audio.getReadPointer (0, (int) clipStart + 60000), 4800);
    expectNear ("level after the last breakpoint holds at 0 dB", settled.rms, 0.5 * std::sqrt (0.5) / std::sqrt (2.0), 0.002, "RMS");
    clearTracks();
}

void SelfTest::testEditingAndUndo()
{
    beginGroup ("Editing and undo");
    auto* session = app.getSession();
    auto& um = session->getUndoManager();
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const auto splitPoint = clipStart + 48000;

    auto t = addToneTrack ("Edit", 1, tone, clipStart);
    um.clearUndoHistory();
    um.beginNewTransaction ("Separate");
    auto right = session->splitClip (track::clips (t).getChild (0), splitPoint, &um);
    expect ("separate clip creates two clips", right.isValid() && track::clips (t).getNumChildren() == 2,
            juce::String (track::clips (t).getNumChildren()) + " clips");

    um.beginNewTransaction ("Delete");
    track::clips (t).removeChild (right, &um);

    auto r = bounce ("split_delete", 0, end);
    auto audio = readAudio (r.file);
    const auto before = dsp::measureLevels (audio.getReadPointer (0, (int) clipStart + 4800), 24000);
    const auto after = dsp::measureLevels (audio.getReadPointer (0, (int) splitPoint + 2400), 24000);
    expect ("audio before the split point still plays", before.rms > 0.2, "RMS " + fmt (before.rms));
    expect ("deleted half is silent", db (after.peak) < silentDb, "peak " + fmt (db (after.peak)) + " dB");

    um.undo();
    r = bounce ("undo_delete", 0, end);
    audio = readAudio (r.file);
    const auto restored = dsp::measureLevels (audio.getReadPointer (0, (int) splitPoint + 2400), 24000);
    expect ("undo brings the deleted audio back", restored.rms > 0.2, "RMS " + fmt (restored.rms));

    // Seamless split: the two halves must join without a click.
    const float* l = audio.getReadPointer (0);
    double worstJump = 0.0;
    for (int i = (int) splitPoint - 8; i < (int) splitPoint + 8; ++i)
        worstJump = juce::jmax (worstJump, (double) std::abs (l[i + 1] - l[i]));
    const double maxSineStep = 0.5 * std::sqrt (0.5) * juce::MathConstants<double>::twoPi * 1000.0 / sampleRate;
    expect ("split point is seamless (no click)", worstJump <= maxSineStep * 1.01,
            "largest sample step " + fmt (worstJump, 5) + ", a clean sine allows " + fmt (maxSineStep, 5));

    um.undo();
    expect ("undo of separate restores one clip", track::clips (t).getNumChildren() == 1,
            juce::String (track::clips (t).getNumChildren()) + " clips");
    clearTracks();
}

void SelfTest::testConsoleSaturation()
{
    beginGroup ("Analog console (master bus)");
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const int from = (int) clipStart + 9600, count = (int) sampleRate;
    addToneTrack ("Console", 1, tone, clipStart);

    auto harmonics = [&] (const juce::String& name, double& h1, double& h2, double& h3, double& dc)
    {
        const auto r = bounce (name, 0, end);
        const auto audio = readAudio (r.file);
        const float* x = audio.getReadPointer (0, from);
        h1 = dsp::toneAmplitude (x, count, 1000.0, sampleRate);
        h2 = dsp::toneAmplitude (x, count, 2000.0, sampleRate);
        h3 = dsp::toneAmplitude (x, count, 3000.0, sampleRate);
        double sum = 0.0;
        for (int i = 0; i < count; ++i)
            sum += x[i];
        dc = sum / count;
    };

    double h1 = 0, h2 = 0, h3 = 0, dc = 0;
    setConsole (false);
    harmonics ("console_off", h1, h2, h3, dc);
    expect ("console off: no harmonics added (clean 64-bit path)", db (h2 / h1) < -100.0 && db (h3 / h1) < -100.0,
            "H2 " + fmt (db (h2 / h1), 1) + " dB, H3 " + fmt (db (h3 / h1), 1) + " dB relative to the fundamental");

    setConsole (true, 12.0, 0.5);
    harmonics ("console_drive12", h1, h2, h3, dc);
    expect ("console on: 2nd harmonic (warmth, from asymmetry) is generated", db (h2 / h1) > -60.0,
            "H2 " + fmt (db (h2 / h1), 1) + " dB");
    expect ("console on: 3rd harmonic (saturation) is generated", db (h3 / h1) > -60.0, "H3 " + fmt (db (h3 / h1), 1) + " dB");
    expect ("console on: no DC offset reaches the output", std::abs (dc) < 1.0e-3, "mean " + fmt (dc, 6));

    // Aliasing: a 15 kHz tone's 2nd and 3rd harmonics (30 kHz, 45 kHz) would fold
    // back to 18 kHz and 3 kHz without oversampling.
    clearTracks();
    const auto high = writeTone ("tone15k_mono", sampleRate, 2.0, 15000.0, toneDb, 1);
    addToneTrack ("Console HF", 1, high, clipStart);
    const auto r = bounce ("console_alias", 0, end);
    const auto audio = readAudio (r.file);
    const float* x = audio.getReadPointer (0, from);
    const double fundamental = dsp::toneAmplitude (x, count, 15000.0, sampleRate);
    const double alias3k = dsp::toneAmplitude (x, count, 3000.0, sampleRate);
    const double alias18k = dsp::toneAmplitude (x, count, 18000.0, sampleRate);
    expect ("oversampling suppresses aliasing (folded 3rd harmonic at 3 kHz)", db (alias3k / fundamental) < -60.0,
            fmt (db (alias3k / fundamental), 1) + " dB relative to the fundamental");
    expect ("oversampling suppresses aliasing (folded 2nd harmonic at 18 kHz)", db (alias18k / fundamental) < -60.0,
            fmt (db (alias18k / fundamental), 1) + " dB relative to the fundamental");

    setConsole (false);
    clearTracks();
}

//==============================================================================
// Console presets: each one's measured character, through a real bounce of a
// 1 kHz sine at -6 dBFS (ROADMAP.md: "defined by measurable targets").

void SelfTest::testConsolePresets()
{
    beginGroup ("Console presets (measured character)");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    auto state = session->getState();
    const juce::var before[] = { state[ids::consoleEnabled], state[ids::consoleDrive], state[ids::consoleCharacter], state[ids::consoleCurve] };

    clearTracks();
    addToneTrack ("Preset Tone", 2, media.getChildFile ("tone1k_stereo.wav"), clipStart); // -6.02 dBFS per channel
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;

    for (const auto& preset : consolepresets::all())
    {
        const juce::String name (preset.name);
        auto r = call ("set_master", "{\"preset\":\"" + name + "\"}");
        const auto audio = readAudio (bounce ("preset_" + name.removeCharacters (" "), 0, end).file);

        // One second, from 0.2 s into the tone: whole cycles of every harmonic of 1 kHz.
        const float* p = audio.getReadPointer (0, (int) clipStart + 9600);
        const int n = (int) sampleRate;
        double harmonics[11] = {};
        for (int k = 1; k <= 10; ++k)
            harmonics[k] = dsp::toneAmplitude (p, n, 1000.0 * k, sampleRate);
        double distortion = 0.0;
        for (int k = 2; k <= 10; ++k)
            distortion += harmonics[k] * harmonics[k];
        const double thd = 100.0 * std::sqrt (distortion) / harmonics[1];
        const bool balanced = preset.dominantHarmonic == 2 ? harmonics[2] >= harmonics[3]
                            : preset.dominantHarmonic == 3 ? harmonics[3] >= harmonics[2]
                                                           : true;
        expect (name + ": THD within " + fmt (preset.minThdPercent, 1) + "-" + fmt (preset.maxThdPercent, 3) + " %"
                    + (preset.dominantHarmonic != 0 ? ", H" + juce::String (preset.dominantHarmonic) + " the larger of H2 and H3" : juce::String()),
                r.ok && thd >= preset.minThdPercent && thd <= preset.maxThdPercent && balanced
                    && consolepresets::current (state) == name,
                r.error + fmt (thd, 4) + " % THD, H2/H3 " + fmt (db (harmonics[2]) - db (harmonics[3]), 1) + " dB");
    }

    auto r = call ("set_master", R"({"preset":"Warm Tape"})");
    auto tweaked = call ("set_master", R"({"drive_db":0})");
    const auto status = call ("get_status", "{}");
    expect ("changing a control after a preset reports \"Custom\"; one undo goes back to the preset",
            r.ok && tweaked.ok && consolepresets::current (state) == "Custom"
                && status.data["master"]["preset"].toString() == "Custom",
            consolepresets::current (state));
    session->getUndoManager().undo();
    expect ("undo restores Warm Tape", consolepresets::current (state) == "Warm Tape", consolepresets::current (state));
    expect ("an unknown preset is refused", ! call ("set_master", R"({"preset":"Loud"})").ok, {});

    clearTracks();
    state.setProperty (ids::consoleEnabled, before[0], nullptr);
    state.setProperty (ids::consoleDrive, before[1], nullptr);
    state.setProperty (ids::consoleCharacter, before[2], nullptr);
    state.setProperty (ids::consoleCurve, before[3], nullptr);
}

void SelfTest::testSessionRoundTrip()
{
    beginGroup ("Session files");
    auto* session = app.getSession();
    auto t = addToneTrack ("Saved Track", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    t.setProperty (ids::volumeDb, -3.5, nullptr);
    t.setProperty (ids::pan, -0.25, nullptr);
    track::clips (t).getChild (0).setProperty (ids::fadeOut, 1234, nullptr);

    const auto saved = app.saveSession();
    expect ("save session", saved.wasOk(), saved.getErrorMessage());

    juce::String error;
    const auto loaded = Session::load (session->getSessionFile(), error);
    expect ("reload session", loaded != nullptr, error);
    if (loaded != nullptr)
    {
        const auto lt = loaded->getTrack (0);
        expect ("reloaded track name, volume and pan match",
                track::name (lt) == "Saved Track" && juce::exactlyEqual (track::volumeDb (lt), -3.5)
                    && juce::exactlyEqual (track::pan (lt), -0.25),
                track::name (lt) + ", " + fmt (track::volumeDb (lt)) + " dB, pan " + fmt (track::pan (lt)));
        const auto lc = track::clips (lt).getChild (0);
        expect ("reloaded clip position and fade match",
                clip::start (lc) == clipStart && clip::fadeOut (lc) == 1234
                    && loaded->resolveStoredPath (lc[ids::file].toString()).existsAsFile(),
                "start " + juce::String (clip::start (lc)) + ", fade " + juce::String (clip::fadeOut (lc)));
    }
    clearTracks();
}

void SelfTest::testImportAndResampling()
{
    beginGroup ("Import and sample-rate conversion");
    auto* session = app.getSession();

    const auto source = writeTone ("tone1k_44k1", 44100.0, 1.0, 1000.0, toneDb, 1);
    const auto destination = session->createUniqueAudioFile ("imported_44k1");
    const auto result = importAudioFile (source, destination, sampleRate, 32, false, app.getFormatManager(), {});
    expect ("import a 44.1 kHz file into a 48 kHz session", result.ok, result.error);
    if (result.ok)
    {
        double rate = 0.0;
        const auto audio = readAudio (destination, &rate);
        expectNear ("converted file is 48 kHz", rate, 48000.0, 0.0, "Hz");
        expectNear ("converted length (1 s)", (double) audio.getNumSamples(), 48000.0, 2.0, "samples");
        const float* x = audio.getReadPointer (0, 4800);
        expectNear ("pitch preserved through conversion", dsp::estimateFrequency (x, 38400, rate), 1000.0, 0.5, "Hz");
        expectNear ("level preserved through conversion", db (dsp::toneAmplitude (x, 38400, 1000.0, rate)), toneDb, 0.1, "dB");
    }

    // Downsampling must filter first: a 36 kHz tone at 96 kHz would alias to 12 kHz.
    const auto ultrasonic = writeTone ("tone36k_96k", 96000.0, 1.0, 36000.0, toneDb, 1);
    const auto downsampled = session->createUniqueAudioFile ("imported_96k");
    const auto r2 = importAudioFile (ultrasonic, downsampled, sampleRate, 32, false, app.getFormatManager(), {});
    if (r2.ok)
    {
        const auto audio = readAudio (downsampled);
        const auto stats = dsp::measureLevels (audio.getReadPointer (0, 4800), 38400);
        expect ("96 kHz to 48 kHz conversion removes inaudible content instead of aliasing it",
                db (stats.peak) - toneDb < -60.0, "residue " + fmt (db (stats.peak) - toneDb, 1) + " dB relative to the tone");
    }
    else
    {
        expect ("import a 96 kHz file", false, r2.error);
    }
}

void SelfTest::testElasticAudio()
{
    beginGroup ("Elastic Audio (Rubber Band)");
    if (! isTimeStretchAvailable())
    {
        expect ("time-stretch engine available", false, timeStretchEngineName());
        return;
    }

    TimeStretchRequest request;
    request.source = media.getChildFile ("tone1k_mono.wav");
    request.sourceStart = 0;
    request.sourceLength = (juce::int64) (2.0 * sampleRate);
    request.sampleRate = sampleRate;
    request.bitDepth = 32;

    request.destination = media.getChildFile ("stretch_2x.wav");
    request.timeRatio = 2.0;
    auto r = renderTimeStretch (request, app.getFormatManager(), {});
    expect ("time-stretch to 200 % renders", r.ok, r.error);
    if (r.ok)
    {
        const auto audio = readAudio (request.destination);
        expectNear ("stretched length is 2x", (double) audio.getNumSamples() / (double) request.sourceLength, 2.0, 0.01, "x");
        const float* x = audio.getReadPointer (0, 48000);
        expectNear ("time-stretch keeps the pitch", dsp::estimateFrequency (x, 96000, sampleRate), 1000.0, 2.0, "Hz");
        expectNear ("time-stretch keeps the level", db (dsp::toneAmplitude (x, 96000, 1000.0, sampleRate)), toneDb, 1.5, "dB");
    }

    request.destination = media.getChildFile ("pitch_up12.wav");
    request.timeRatio = 1.0;
    request.pitchSemitones = 12.0;
    r = renderTimeStretch (request, app.getFormatManager(), {});
    expect ("pitch shift +12 semitones renders", r.ok, r.error);
    if (r.ok)
    {
        const auto audio = readAudio (request.destination);
        expectNear ("pitch shift keeps the length", (double) audio.getNumSamples() / (double) request.sourceLength, 1.0, 0.01, "x");
        const float* x = audio.getReadPointer (0, 24000);
        expectNear ("+12 semitones doubles the frequency", dsp::estimateFrequency (x, 48000, sampleRate), 2000.0, 4.0, "Hz");
    }
}

void SelfTest::testPluginHosting()
{
    beginGroup ("Plug-in hosting (VST3)");
#if ! defined(MERIDIAN_TEST_PLUGIN_BINARY)
    expect ("test plug-in available", false, "this build has no test plug-in (MERIDIAN_BUILD_TESTS=OFF)");
#else
    // The build gives the DLL inside the bundle; hosts load the .vst3 bundle folder.
    auto bundle = juce::File (MERIDIAN_TEST_PLUGIN_BINARY).getParentDirectory();
    while (bundle != bundle.getParentDirectory() && ! bundle.getFileName().endsWithIgnoreCase (".vst3"))
        bundle = bundle.getParentDirectory();

    juce::AudioPluginFormat* vst3 = nullptr;
    for (auto* format : app.getPluginHost().getFormatManager().getFormats())
        if (format->getName() == "VST3")
            vst3 = format;

    juce::OwnedArray<juce::PluginDescription> found;
    if (vst3 != nullptr)
        vst3->findAllTypesForFile (found, bundle.getFullPathName());
    expect ("scan finds the test VST3", found.size() == 1, bundle.getFullPathName());
    if (found.isEmpty())
        return;

    const auto description = *found[0];
    testPlugin = std::make_unique<juce::PluginDescription> (description);
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const double dry = toneDb + panCentreDb;

    clearTracks();
    auto t = addToneTrack ("Plug-in", 1, tone, clipStart);
    juce::String error;
    expect ("insert the plug-in on a mono track", app.getBridge().insertPlugin (t, 0, description, error), error);
    auto slot = app.getBridge().getInsert (track::id (t), 0);
    if (slot == nullptr || slot->plugin == nullptr)
    {
        expect ("plug-in instance created", false, "no instance");
        return;
    }

    auto r = bounce ("plugin_default", 0, end);
    expectNear ("insert processes audio (plug-in default -6 dB)", db (r.left.peak), dry - 6.0, 0.02, "dB");

    app.getBridge().setPluginBypassed (t, 0, true);
    r = bounce ("plugin_bypassed", 0, end);
    expectNear ("bypass passes audio through untouched", db (r.left.peak), dry, 0.02, "dB");
    app.getBridge().setPluginBypassed (t, 0, false);

    // Turn the plug-in's gain knob to -12 dB (range -24..+24, so normalised 0.25).
    if (auto* parameter = slot->plugin->getParameters()[0])
        parameter->setValueNotifyingHost (0.25f);
    r = bounce ("plugin_minus12", 0, end);
    expectNear ("plug-in parameter changes are heard", db (r.left.peak), dry - 12.0, 0.02, "dB");

    // Persistence: save, re-open, and the plug-in must come back with its setting.
    const auto sessionFile = app.getSession()->getSessionFile();
    const auto saved = app.saveSession();
    const auto reopened = app.openSession (sessionFile);
    expect ("save and re-open a session with a plug-in", saved.wasOk() && reopened.wasOk(),
            saved.getErrorMessage() + reopened.getErrorMessage());
    t = app.getSession()->getTrack (0);
    r = bounce ("plugin_after_reopen", 0, end);
    expectNear ("plug-in settings are restored from the session file", db (r.left.peak), dry - 12.0, 0.02, "dB");

    app.getBridge().removePlugin (t, 0);
    r = bounce ("plugin_removed", 0, end);
    expectNear ("removing the plug-in restores the dry signal", db (r.left.peak), dry, 0.02, "dB");

    app.getSession()->getUndoManager().undo();
    r = bounce ("plugin_undo_remove", 0, end);
    expectNear ("undo brings the plug-in back with its settings", db (r.left.peak), dry - 12.0, 0.02, "dB");
    clearTracks();
#endif
}

void SelfTest::testBusesAndSends()
{
    beginGroup ("Buses, sends and routing");
    auto* session = app.getSession();
    auto* um = &session->getUndoManager();
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const double dry = toneDb + panCentreDb; // mono track at centre into a stereo bus

    clearTracks();
    auto t = addToneTrack ("Source", 1, tone, clipStart);
    auto bus = session->addTrack ("Bus 1", 2, nullptr, Session::TrackKind::aux);
    const int busId = track::id (bus);

    // Output routing: track -> bus -> master.
    expect ("route a track's output to a bus", routing::setOutput (*session, t, busId, um), {});
    auto r = bounce ("route_to_bus", 0, end);
    expectNear ("signal passes through the bus at unity", db (r.left.peak), dry, 0.02, "dB");

    bus.setProperty (ids::volumeDb, -6.0, nullptr);
    r = bounce ("bus_fader", 0, end);
    expectNear ("the bus fader controls everything routed to it", db (r.left.peak), dry - 6.0, 0.02, "dB");

    bus.setProperty (ids::mute, true, nullptr);
    r = bounce ("bus_muted", 0, end);
    expect ("muting the bus silences its sources", db (r.left.peak) < silentDb, fmt (db (r.left.peak)) + " dB");
    bus.setProperty (ids::mute, false, nullptr);
    bus.setProperty (ids::volumeDb, 0.0, nullptr);

    // Post-fader send: the dry track still goes to the master, a copy goes to the bus.
    routing::setOutput (*session, t, 0, um);
    expect ("create a send to the bus", routing::setSend (*session, t, 0, busId, um), {});
    auto s = routing::findSend (t, 0);
    r = bounce ("send_0db", 0, end);
    expectNear ("0 dB send doubles the signal (+6.02 dB, coherent sum)", db (r.left.peak), dry + 6.0206, 0.02, "dB");

    s.setProperty (ids::levelDb, -6.0, nullptr);
    r = bounce ("send_minus6", 0, end);
    expectNear ("-6 dB send adds exactly its share", db (r.left.peak), dry + db (1.0 + std::pow (10.0, -6.0 / 20.0)), 0.02, "dB");

    s.setProperty (ids::mute, true, nullptr);
    r = bounce ("send_muted", 0, end);
    expectNear ("muted send contributes nothing", db (r.left.peak), dry, 0.02, "dB");
    s.setProperty (ids::mute, false, nullptr);
    s.setProperty (ids::levelDb, 0.0, nullptr);

    t.setProperty (ids::volumeDb, -6.0, nullptr);
    r = bounce ("post_fader_follows", 0, end);
    expectNear ("post-fader send follows the track fader", db (r.left.peak), dry + 6.0206 - 6.0, 0.02, "dB");

    // Pre-fader send: independent of the fader (headphone-mix style).
    s.setProperty (ids::preFader, true, nullptr);
    t.setProperty (ids::volumeDb, -144.0, nullptr);
    r = bounce ("pre_fader_send", 0, end);
    expectNear ("pre-fader send ignores the fader (fader fully down)", db (r.left.peak), dry, 0.02, "dB");

    t.setProperty (ids::mute, true, nullptr);
    r = bounce ("pre_fader_send_muted_track", 0, end);
    expect ("muting the track also mutes its pre-fader sends", db (r.left.peak) < silentDb, fmt (db (r.left.peak)) + " dB");
    t.setProperty (ids::mute, false, nullptr);

    s.setProperty (ids::pan, -1.0, nullptr);
    r = bounce ("send_pan_left", 0, end);
    expectNear ("send pan: hard left reaches the left side at full level", db (r.left.peak), toneDb, 0.02, "dB");
    expect ("send pan: right side silent", db (r.right.peak) < silentDb, fmt (db (r.right.peak)) + " dB");
    s.setProperty (ids::pan, 0.0, nullptr);
    s.setProperty (ids::preFader, false, nullptr);
    t.setProperty (ids::volumeDb, 0.0, nullptr);

    // Implicit solo: soloing the source keeps its bus audible; other tracks go quiet.
    auto other = addToneTrack ("Other", 1, media.getChildFile ("tone500_mono.wav"), clipStart);
    t.setProperty (ids::solo, true, nullptr);
    r = bounce ("solo_keeps_bus", 0, end);
    {
        const auto audio = readAudio (r.file);
        const float* x = audio.getReadPointer (0, (int) clipStart + 4800);
        expectNear ("soloed track is heard directly and through its bus", dsp::toneAmplitude (x, (int) sampleRate, 1000.0, sampleRate),
                    2.0 * 0.5 * std::sqrt (0.5), 0.003, "");
        expect ("unrelated track is silenced by the solo", db (dsp::toneAmplitude (x, (int) sampleRate, 500.0, sampleRate)) < silentDb,
                fmt (db (dsp::toneAmplitude (x, (int) sampleRate, 500.0, sampleRate))) + " dB");
    }
    t.setProperty (ids::solo, false, nullptr);
    session->removeTrack (other, nullptr);
    routing::removeSend (t, 0, nullptr);

    // Bus into bus, and loop protection.
    auto bus2 = session->addTrack ("Bus 2", 2, nullptr, Session::TrackKind::aux);
    routing::setOutput (*session, t, busId, um);
    routing::setOutput (*session, bus, track::id (bus2), um);
    r = bounce ("bus_chain", 0, end);
    expectNear ("track -> bus -> bus -> master arrives intact", db (r.left.peak), dry, 0.02, "dB");
    expect ("a loop (Bus 2 back into Bus 1) is refused",
            ! routing::setOutput (*session, bus2, busId, um) && ! routing::setSend (*session, bus2, 0, busId, um)
                && routing::wouldCreateCycle (*session, bus2, busId),
            "Bus 2 output: " + juce::String (track::outputBus (bus2)));
    expect ("a track cannot send to itself", ! routing::setSend (*session, bus, 0, busId, um), {});

    // Persistence.
    routing::setSend (*session, t, 2, track::id (bus2), um);
    routing::findSend (t, 2).setProperty (ids::levelDb, -9.5, nullptr);
    app.saveSession();
    juce::String error;
    if (const auto loaded = Session::load (session->getSessionFile(), error))
    {
        const auto lt = loaded->getTrack (0);
        const auto ls = routing::findSend (lt, 2);
        expect ("routing and sends are saved with the session",
                track::outputBus (lt) == busId && ls.isValid() && send::target (ls) == track::id (bus2) && juce::exactlyEqual (send::levelDb (ls), -9.5),
                "output " + juce::String (track::outputBus (lt)) + ", send level " + fmt (send::levelDb (ls)));
    }
    else
    {
        expect ("reload routed session", false, error);
    }
    clearTracks();
}

//==============================================================================
// Reference renders ("golden files"): fixed mixes that exercise most of the
// signal path at once (clip gain, fades, pan, automation, pre-fader send,
// aux bus, master console). Each render is null-tested against the copy in
// tests/golden. Any change that alters the sound by more than -120 dBFS
// fails, so nothing can quietly make the audio worse. After a deliberate,
// reviewed change to the sound, rerun with --update-golden and commit the
// new references.

void SelfTest::compareWithReference (const juce::String& name, const BounceResult& r)
{
    if (! r.ok)
        return;

    const auto rendered = readAudio (r.file);
    double peak = 0.0;
    for (int ch = 0; ch < rendered.getNumChannels(); ++ch)
        peak = juce::jmax (peak, (double) rendered.getMagnitude (ch, 0, rendered.getNumSamples()));
    expect (name + ": mix is at a sensible level (not silent, not clipping)", db (peak) > -30.0 && db (peak) < -0.1,
            "peak " + fmt (db (peak), 2) + " dBFS");

#ifdef MERIDIAN_GOLDEN_DIR
    const auto reference = juce::File (MERIDIAN_GOLDEN_DIR).getChildFile (name + ".wav");
#else
    const juce::File reference;
    expect (name + ": reference folder configured", false, "MERIDIAN_GOLDEN_DIR is not defined");
    return;
#endif

    if (updateGolden)
    {
        reference.getParentDirectory().createDirectory();
        expect (name + ": reference render updated", r.file.copyFileTo (reference), reference.getFullPathName());
        return;
    }

    if (! reference.existsAsFile())
    {
        expect (name + ": reference render exists", false,
                reference.getFullPathName() + " is missing; create it with --update-golden");
        return;
    }

    const auto expected = readAudio (reference);
    if (expected.getNumChannels() != rendered.getNumChannels() || expected.getNumSamples() != rendered.getNumSamples())
    {
        expect (name + ": matches the reference render (null test)", false,
                "shape differs: " + juce::String (rendered.getNumChannels()) + " ch x " + juce::String (rendered.getNumSamples())
                    + " vs reference " + juce::String (expected.getNumChannels()) + " ch x " + juce::String (expected.getNumSamples()));
        return;
    }

    double residualPeak = 0.0, residualEnergy = 0.0, referenceEnergy = 0.0;
    int differing = 0;
    for (int ch = 0; ch < rendered.getNumChannels(); ++ch)
    {
        const float* a = rendered.getReadPointer (ch);
        const float* b = expected.getReadPointer (ch);
        for (int i = 0; i < rendered.getNumSamples(); ++i)
        {
            const double d = (double) a[i] - (double) b[i];
            residualPeak = juce::jmax (residualPeak, std::abs (d));
            residualEnergy += d * d;
            referenceEnergy += (double) b[i] * (double) b[i];
            differing += juce::exactlyEqual (d, 0.0) ? 0 : 1;
        }
    }

    const auto nullDepth = residualEnergy > 0.0 ? 10.0 * std::log10 (residualEnergy / referenceEnergy) : -400.0;
    expect (name + ": matches the reference render (null test, residual below -120 dBFS)", db (residualPeak) < -120.0,
            "residual peak " + fmt (db (residualPeak), 1) + " dBFS, null depth " + fmt (nullDepth, 1) + " dB, "
                + juce::String (differing) + " samples differ");
}

void SelfTest::testGoldenRenders()
{
    beginGroup ("Reference renders (golden-file null tests)");
    auto* session = app.getSession();
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    clearTracks();
    setConsole (false);

    // The reference mix. Every value here is part of the reference: changing
    // one requires --update-golden.
    const auto low = writeTone ("golden_tone220_stereo", sampleRate, 2.0, 220.0, -12.0, 2);
    const auto noise = writeNoise ("golden_noise", 2.0, -18.0, 0x4d455249u);
    const auto mid = media.getChildFile ("tone1k_mono.wav");

    auto pad = addToneTrack ("Pad", 2, low, clipStart);
    pad.setProperty (ids::volumeDb, -2.0, nullptr);
    auto padClip = track::clips (pad).getChild (0);
    padClip.setProperty (ids::gainDb, -1.5, nullptr);
    padClip.setProperty (ids::fadeIn, 2400, nullptr);
    padClip.setProperty (ids::fadeOut, 4800, nullptr);

    auto hiss = addToneTrack ("Noise", 1, noise, clipStart);
    hiss.setProperty (ids::pan, -0.6, nullptr);
    auto lane = automation::getOrCreateVolumeLane (hiss, nullptr);
    automation::addPoint (lane, clipStart, -20.0, nullptr);
    automation::addPoint (lane, clipStart + 48000, -3.0, nullptr);
    automation::addPoint (lane, clipStart + 96000, -10.0, nullptr);

    auto lead = addToneTrack ("Lead", 1, mid, clipStart);
    lead.setProperty (ids::pan, 0.5, nullptr);
    lead.setProperty (ids::volumeDb, -8.0, nullptr);

    auto verb = session->addTrack ("Verb", 2, nullptr, Session::TrackKind::aux);
    verb.setProperty (ids::volumeDb, -3.0, nullptr);
    verb.setProperty (ids::pan, 0.2, nullptr);
    expect ("reference mix: pre-fader send to the aux", routing::setSend (*session, lead, 0, track::id (verb), nullptr), {});
    auto send = routing::findSend (lead, 0);
    send.setProperty (ids::levelDb, -4.0, nullptr);
    send.setProperty (ids::preFader, true, nullptr);

    const auto clean = bounce ("golden_mix_clean", 0, end);
    compareWithReference ("golden_mix_clean", clean);

    // Sensitivity: a 0.001 dB level change, far below audibility, must already
    // show up well above the -120 dBFS threshold.
    if (clean.ok)
    {
        auto altered = readAudio (clean.file);
        const auto original = readAudio (clean.file);
        altered.applyGain ((float) std::pow (10.0, 0.001 / 20.0));
        double residual = 0.0;
        for (int ch = 0; ch < altered.getNumChannels(); ++ch)
            for (int i = 0; i < altered.getNumSamples(); ++i)
                residual = juce::jmax (residual, std::abs ((double) altered.getSample (ch, i) - original.getSample (ch, i)));
        expect ("null test detects a 0.001 dB level change (proves it can fail)", db (residual) > -120.0,
                "residual peak " + fmt (db (residual), 1) + " dBFS");
    }

    setConsole (true, 9.0, 0.4);
    compareWithReference ("golden_mix_console_tanh", bounce ("golden_mix_console_tanh", 0, end));

    setConsole (true, 6.0, 0.0);
    session->getState().setProperty (ids::consoleCurve, 1, nullptr);
    compareWithReference ("golden_mix_console_cubic", bounce ("golden_mix_console_cubic", 0, end));

    setConsole (false);
    clearTracks();
}

//==============================================================================
// Instrument tracks: MIDI notes played by the built-in synth, measured.

namespace
{
/** Delay (in samples) of a sinusoid at `hz` in x[from, from + count) relative
    to a sine that started at sample `onset` with zero phase. Measured from the
    phase of a correlation over the window (use whole cycles). */
double sineDelay (const juce::AudioBuffer<float>& audio, int from, int count, double hz, double rate, int onset)
{
    const double w = juce::MathConstants<double>::twoPi * hz / rate;
    double i = 0.0, q = 0.0;
    for (int n = from; n < from + count; ++n)
    {
        i += audio.getSample (0, n) * std::sin (w * (n - onset));
        q += audio.getSample (0, n) * std::cos (w * (n - onset));
    }
    return -std::atan2 (q, i) / w;
}
} // namespace

void SelfTest::testInstruments()
{
    beginGroup ("Instrument tracks (MIDI + built-in synth)");
    auto* session = app.getSession();
    clearTracks();
    setConsole (false);
    session->getState().setProperty (ids::tempo, 120.0, nullptr); // a beat = 0.5 s = 24000 samples

    // A measurement instrument: a pure sine at -6.02 dB, filter wide open, fast attack.
    auto keys = session->addTrack ("Keys", 2, nullptr, Session::TrackKind::instrument);
    auto synthNode = track::synth (keys);
    synthNode.setProperty (ids::waveform, "sine", nullptr);
    synthNode.setProperty (ids::detune, 0.0, nullptr);
    synthNode.setProperty (ids::attack, 0.001, nullptr);
    synthNode.setProperty (ids::sustain, 1.0, nullptr);
    synthNode.setProperty (ids::release, 0.05, nullptr);
    synthNode.setProperty (ids::cutoff, 20000.0, nullptr);
    synthNode.setProperty (ids::filterEnv, 0.0, nullptr);
    synthNode.setProperty (ids::resonance, 0.0, nullptr);
    synthNode.setProperty (ids::gainDb, toneDb, nullptr);
    expect ("an instrument track is a valid part of the session", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));

    // One A4 (440 Hz) for two beats (1 s), starting where the clip starts.
    auto clipNode = session->addMidiClip (keys, clipStart, 4 * 24000, "A4", nullptr);
    midimodel::addNote (clipNode, { 69, 1.0f, 0, 2 * midi::ticksPerQuarter }, nullptr);
    const auto end = clipStart + 2 * 48000 + 24000;
    auto r = bounce ("instrument_a4", 0, end);
    auto audio = readAudio (r.file);
    const int noteStart = (int) clipStart, noteEnd = (int) clipStart + 48000;

    if (audio.getNumSamples() >= (int) end)
    {
        const float* x = audio.getReadPointer (0);
        expectNear ("MIDI note 69 plays at 440 Hz", dsp::estimateFrequency (x + noteStart + 4800, 38400, sampleRate), 440.0, 0.05, "Hz");
        expectNear ("note level matches the synth's output setting (-6.02 dB)",
                    db (dsp::toneAmplitude (x + noteStart + 4800, 36000, 440.0, sampleRate)), toneDb, 0.05, "dB");

        // The synth's 16-sample filter delay is compensated: the note sits exactly on the timeline.
        const double delay = sineDelay (audio, noteStart + 4800, 36000, 440.0, sampleRate, noteStart);
        expect ("note starts exactly at its timeline position (synth latency compensated)", std::abs (delay) < 0.3,
                "measured offset " + fmt (delay, 3) + " samples");

        const auto before = dsp::measureLevels (x, noteStart - 40);
        expect ("silence before the note", db (before.peak) < silentDb, "peak " + fmt (db (before.peak), 1) + " dB");
        const auto after = dsp::measureLevels (x + noteEnd + 4800, 9600); // 100-300 ms after note-off, release 50 ms
        expect ("the note stops at its end (released to silence)", db (after.peak) < -80.0, "peak " + fmt (db (after.peak), 1) + " dB");
    }

    // A chord: C major, three equal notes.
    midi::NoteSequence chord;
    for (const int p : { 60, 64, 67 })
        chord.add ({ p, 1.0f, 0, 2 * midi::ticksPerQuarter });
    midimodel::writeNotes (clipNode, chord, nullptr);
    r = bounce ("instrument_chord", 0, end);
    audio = readAudio (r.file);
    if (audio.getNumSamples() >= (int) end)
    {
        const float* x = audio.getReadPointer (0) + noteStart + 4800;
        juce::String levels;
        double lowest = 1.0e9, highest = -1.0e9;
        for (const int p : { 60, 64, 67 })
        {
            const double level = db (dsp::toneAmplitude (x, 38400, 440.0 * std::pow (2.0, (p - 69) / 12.0), sampleRate));
            lowest = juce::jmin (lowest, level);
            highest = juce::jmax (highest, level);
            levels << fmt (level, 2) << " ";
        }
        expect ("a three-note chord plays all three notes at equal level", highest - lowest < 0.2 && lowest > toneDb - 0.3,
                "C, E, G at " + levels + "dB");
    }

    // Alignment with audio: an audio track playing the same 440 Hz tone from the
    // same sample must add up in phase (+6.02 dB). 16 samples off would cost 0.9 dB.
    midimodel::writeNotes (clipNode, {}, nullptr);
    midimodel::addNote (clipNode, { 69, 1.0f, 0, 2 * midi::ticksPerQuarter }, nullptr);
    const auto tone440 = writeTone ("tone440_stereo", sampleRate, 1.0, 440.0, toneDb, 2);
    addToneTrack ("Audio 440", 2, tone440, clipStart);
    r = bounce ("instrument_with_audio", 0, end);
    audio = readAudio (r.file);
    if (audio.getNumSamples() >= (int) end)
        expectNear ("instrument and audio stay sample-aligned (in-phase sum +6.02 dB)",
                    db (dsp::toneAmplitude (audio.getReadPointer (0) + noteStart + 4800, 36000, 440.0, sampleRate)),
                    toneDb + 6.0206, 0.02, "dB");

    // Tempo: notes are placed in beats, so doubling the tempo halves their length.
    clearTracks();
    keys = session->addTrack ("Keys", 2, nullptr, Session::TrackKind::instrument);
    track::synth (keys).copyPropertiesFrom (synthNode, nullptr);
    clipNode = session->addMidiClip (keys, clipStart, 4 * 24000, "beat", nullptr);
    midimodel::addNote (clipNode, { 69, 1.0f, midi::ticksPerQuarter, midi::ticksPerQuarter }, nullptr); // beat 2, one beat long
    session->getState().setProperty (ids::tempo, 240.0, nullptr);
    r = bounce ("instrument_tempo240", 0, end);
    audio = readAudio (r.file);
    if (audio.getNumSamples() >= (int) end)
    {
        // At 240 BPM a beat is 12000 samples: the note sounds from clipStart + 12000 to clipStart + 24000.
        const float* x = audio.getReadPointer (0);
        const auto inside = dsp::measureLevels (x + clipStart + 12000 + 1200, 9600);
        const auto earlier = dsp::measureLevels (x + clipStart, 11900);
        const auto later = dsp::measureLevels (x + clipStart + 24000 + 4800, 9600);
        expect ("at 240 BPM the note sits on beat 2 and lasts one beat (notes follow the tempo)",
                db (inside.peak) > toneDb - 0.5 && db (earlier.peak) < silentDb && db (later.peak) < -80.0,
                "before " + fmt (db (earlier.peak), 1) + " dB, during " + fmt (db (inside.peak), 1) + " dB, after "
                    + fmt (db (later.peak), 1) + " dB");
    }
    session->getState().setProperty (ids::tempo, 120.0, nullptr);

    // An AI writes a melody through the tools: C, D, E on beats 1-3.
    clearTracks();
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const char* json)
    {
        int status = 0;
        const auto body = juce::String (R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":)") + json + "}";
        return juce::JSON::parse (juce::String (tools.handleMcpRequest (body.toStdString(), status)));
    };
    auto ok = [] (const juce::var& reply) { return reply["result"].isObject() && ! (bool) reply["result"]["isError"]; };
    const auto created = call (R"({"name":"create_track","arguments":{"kind":"instrument","name":"Melody"}})");
    const auto shaped = call (R"({"name":"set_synth","arguments":{"track":"Melody","waveform":"sine","detune_cents":0,"attack":0.002,"sustain":1,"release":0.03,"cutoff_hz":20000,"filter_env_octaves":0,"gain_db":-6}})");
    const auto clipReply = call (R"({"name":"add_midi_clip","arguments":{"track":"Melody","start_beat":1,"length_beats":4}})");
    const auto notesReply = call (R"({"name":"add_notes","arguments":{"track":"Melody","clip":1,"notes":[{"pitch":60,"start_beats":0,"length_beats":1},{"pitch":62,"start_beats":1,"length_beats":1},{"pitch":64,"start_beats":2,"length_beats":1}]}})");
    const auto listed = call (R"({"name":"list_notes","arguments":{"track":"Melody","clip":1}})");
    expect ("AI tools: create an instrument, shape its synth, add a clip and three notes",
            ok (created) && ok (shaped) && ok (clipReply) && ok (notesReply) && ok (listed),
            juce::JSON::toString (notesReply, true).substring (0, 200));
    const auto wrongKind = call (R"({"name":"add_notes","arguments":{"track":"Melody","clip":1,"notes":[{"pitch":200,"start_beats":0,"length_beats":1}]}})");
    expect ("AI tools refuse an impossible note (pitch 200) with an explanation",
            (bool) wrongKind["result"]["isError"], juce::JSON::toString (wrongKind, true).substring (0, 160));

    r = bounce ("instrument_ai_melody", 0, 24000 * 5);
    audio = readAudio (r.file);
    if (audio.getNumSamples() >= 24000 * 5)
    {
        // The clip starts at beat 1 (24000); each note fills one beat.
        juce::String heard;
        bool allRight = true;
        const double expected[] = { 261.6256, 293.6648, 329.6276 };
        for (int k = 0; k < 3; ++k)
        {
            const double f = dsp::estimateFrequency (audio.getReadPointer (0, 24000 * (k + 1) + 2400), 19200, sampleRate);
            heard << fmt (f, 2) << " ";
            allRight = allRight && std::abs (f - expected[k]) < 0.1;
        }
        expect ("the AI's melody plays C4, D4, E4 in order, in tune", allRight, "heard " + heard + "Hz");
    }

    expect ("session with instruments stays valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));

    // Drum tracks: the built-in drum machine on the General MIDI map.
    clearTracks();
    auto beat = session->addTrack ("Beat", 2, nullptr, Session::TrackKind::drums);
    auto beatClip = session->addMidiClip (beat, clipStart, 4 * 24000, "Beat", nullptr);
    for (const auto& n : std::initializer_list<midi::Note> { { 36, 1.0f, 0, 480 }, { 38, 1.0f, 960, 480 }, { 42, 1.0f, 1920, 240 } })
        midimodel::addNote (beatClip, n, nullptr);
    r = bounce ("drums_basic", 0, clipStart + 4 * 24000);
    audio = readAudio (r.file);
    if (audio.getNumSamples() >= (int) (clipStart + 4 * 24000))
    {
        const float* x = audio.getReadPointer (0);
        int onset = -1;
        for (int i = 0; i < audio.getNumSamples() && onset < 0; ++i)
            if (std::abs (x[i]) > 1.0e-3f)
                onset = i;
        expect ("drum hits land exactly on their timeline position (kick on the clip start)",
                onset >= (int) clipStart && onset <= (int) clipStart + 2,
                "first sound at sample " + juce::String (onset) + ", clip starts at " + juce::String (clipStart));
        expectNear ("note 36 is a kick: tuned to 50 Hz after its pitch drop",
                    dsp::estimateFrequency (x + clipStart + 7200, 9600, sampleRate), 50.0, 3.0, "Hz");

        auto crossingsPerSecond = [&] (int from, int count)
        {
            int n = 0;
            for (int i = from + 1; i < from + count; ++i)
                n += (x[i - 1] < 0.0f) != (x[i] < 0.0f) ? 1 : 0;
            return n * sampleRate / count;
        };
        const double snareZcr = crossingsPerSecond ((int) clipStart + 24000 + 240, 2160);
        const double hatZcr = crossingsPerSecond ((int) clipStart + 48000 + 48, 1200);
        expect ("note 38 is a snare and note 42 a hi-hat (noise content rises: snare > 2 k, hat > 8 k crossings/s)",
                snareZcr > 2000.0 && hatZcr > 8000.0, "snare " + fmt (snareZcr, 0) + ", hat " + fmt (hatZcr, 0) + " crossings/s");
    }

    // The AI builds a beat through the tools.
    clearTracks();
    const auto drumTrack = call (R"({"name":"create_track","arguments":{"kind":"drums","name":"Groove"}})");
    const auto drumClip = call (R"({"name":"add_midi_clip","arguments":{"track":"Groove","start_beat":0,"length_beats":4}})");
    const auto drumNotes = call (R"({"name":"add_notes","arguments":{"track":"Groove","clip":1,"notes":[{"pitch":36,"start_beats":0,"length_beats":0.5},{"pitch":42,"start_beats":0.5,"length_beats":0.25},{"pitch":38,"start_beats":1,"length_beats":0.5},{"pitch":46,"start_beats":1.5,"length_beats":0.5},{"pitch":42,"start_beats":2,"length_beats":0.25}]}})");
    const auto tuned = call (R"({"name":"set_synth","arguments":{"track":"Groove","tune_semitones":-2,"decay_scale":1.5,"tone":0.7}})");
    expect ("AI tools: create a drum track, write a beat, tune the kit",
            ok (drumTrack) && ok (drumClip) && ok (drumNotes) && ok (tuned)
                && juce::JSON::toString (tuned, true).contains ("\"drums\""),
            juce::JSON::toString (tuned, true).substring (0, 200));
    r = bounce ("drums_ai_groove", 0, 4 * 24000);
    expect ("the AI's beat is audible", r.ok && db (juce::jmax (r.left.peak, r.right.peak)) > -30.0,
            "peak " + fmt (db (juce::jmax (r.left.peak, r.right.peak)), 1) + " dBFS");
    expect ("session with drum tracks stays valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));

    // Reference render: a two-bar groove on every drum of the kit.
    clearTracks();
    auto kit = session->addTrack ("Kit", 2, nullptr, Session::TrackKind::drums);
    auto groove = session->addMidiClip (kit, 0, 8 * 24000, "groove", nullptr);
    for (int step = 0; step < 16; ++step)
    {
        const int64_t at = step * 480;
        midimodel::addNote (groove, { step % 4 == 2 ? 46 : 42, step % 2 == 0 ? 0.8f : 0.5f, at, 240 }, nullptr);
        if (step % 8 == 0 || step == 11) midimodel::addNote (groove, { 36, 1.0f, at, 240 }, nullptr);
        if (step % 8 == 4)               midimodel::addNote (groove, { 38, 0.9f, at, 240 }, nullptr);
    }
    for (const auto& n : std::initializer_list<midi::Note> { { 39, 0.8f, 7680, 240 }, { 37, 0.7f, 7920, 240 }, { 41, 0.9f, 8640, 240 },
                                                             { 45, 0.9f, 9120, 240 }, { 48, 0.9f, 9600, 240 }, { 49, 0.9f, 11520, 240 },
                                                             { 51, 0.7f, 12480, 240 }, { 56, 0.6f, 13440, 240 }, { 44, 0.6f, 14400, 240 } })
        midimodel::addNote (groove, n, nullptr);
    compareWithReference ("golden_mix_drums", bounce ("golden_mix_drums", 0, 8 * 24000 + 24000));

    // Reference render: the default instrument sound (detuned saws, filter
    // envelope, resonance) playing two chords and a bass note. Any change to
    // the synth's sound fails this null test.
    clearTracks();
    auto pad = session->addTrack ("Pad", 2, nullptr, Session::TrackKind::instrument);
    auto padClip = session->addMidiClip (pad, 0, 4 * 24000, "chords", nullptr);
    for (const auto& n : std::initializer_list<midi::Note> { { 57, 0.8f, 0, 1920 }, { 60, 0.7f, 0, 1920 }, { 64, 0.7f, 0, 1920 },
                                                             { 53, 0.8f, 1920, 1920 }, { 57, 0.7f, 1920, 1920 }, { 60, 0.7f, 1920, 1920 },
                                                             { 33, 1.0f, 0, 3600 } })
        midimodel::addNote (padClip, n, nullptr);
    compareWithReference ("golden_mix_instrument", bounce ("golden_mix_instrument", 0, 4 * 24000 + 24000));
    clearTracks();
}

//==============================================================================
// Built-in effects: Meridian EQ and Meridian Reverb, through the plug-in path.

void SelfTest::testBuiltinEffects()
{
    beginGroup ("Built-in effects (Meridian EQ, Meridian Reverb)");
    auto* session = app.getSession();
    clearTracks();
    setConsole (false);

    const auto descriptions = BuiltinEffectFormat::getDescriptions();
    auto find = [&descriptions] (const juce::String& name) -> const juce::PluginDescription*
    {
        for (const auto& d : descriptions)
            if (d.name == name)
                return &d;
        return nullptr;
    };
    const auto* eqDesc = find ("Meridian EQ");
    const auto* compressorDesc = find ("Meridian Compressor");
    const auto* limiterDesc = find ("Meridian Limiter");
    const auto* reverbDesc = find ("Meridian Reverb");
    bool listed = true;
    for (const auto& d : descriptions)
        listed = listed && app.getPluginHost().getKnownPlugins().getTypeForIdentifierString (d.createIdentifierString()) != nullptr;
    expect ("built-in effects are in the plug-in list without a scan",
            listed && eqDesc != nullptr && compressorDesc != nullptr && limiterDesc != nullptr && reverbDesc != nullptr,
            juce::String (descriptions.size()) + " built-in effects");
    if (eqDesc == nullptr || compressorDesc == nullptr || limiterDesc == nullptr || reverbDesc == nullptr)
        return;

    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments)
    {
        return tools.call (tool, juce::JSON::parse (arguments));
    };

    // EQ: band 2 (1 kHz) up 6 dB through the AI tool, measured on a 1 kHz tone.
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    auto t = addToneTrack ("EQ Test", 1, tone, clipStart);
    juce::String error;
    expect ("insert Meridian EQ", app.getBridge().insertPlugin (t, 0, *eqDesc, error), error);
    const auto setGain = call ("set_plugin_parameter", R"({"track":"EQ Test","slot":"A","parameter":"p2Gain","value":6})");
    const auto listedParams = call ("list_plugin_parameters", R"({"track":"EQ Test","slot":"A"})");
    expect ("AI tools read and set a plug-in control (Band 2 Gain = 6 dB)",
            setGain.ok && listedParams.ok && juce::JSON::toString (listedParams.data).contains ("6.00 dB"),
            setGain.ok ? juce::JSON::toString (setGain.data).substring (0, 160) : setGain.error);
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    auto r = bounce ("eq_plus6_at_1k", 0, end);
    expectNear ("EQ band boosts its frequency by exactly its gain (1 kHz +6 dB)", db (r.left.peak), toneDb + panCentreDb + 6.0, 0.05, "dB");

    // Settings survive save and re-open, and are heard again.
    const auto sessionFile = session->getSessionFile();
    const auto saved = app.saveSession();
    const auto reopened = app.openSession (sessionFile);
    session = app.getSession();
    const auto reread = call ("list_plugin_parameters", R"({"track":"EQ Test","slot":"A"})");
    r = bounce ("eq_after_reopen", 0, end);
    expect ("EQ settings are saved in the session and restored",
            saved.wasOk() && reopened.wasOk() && reread.ok && juce::JSON::toString (reread.data).contains ("6.00 dB")
                && std::abs (db (r.left.peak) - (toneDb + panCentreDb + 6.0)) < 0.05,
            saved.getErrorMessage() + reopened.getErrorMessage() + fmt (db (r.left.peak)) + " dB");

    // Compressor: -18 dB threshold, 4:1, hard knee. The -6.02 dB tone is 11.98 dB
    // over, so it must come out 3.00 dB over: -15.00 dB, before the pan law.
    clearTracks();
    t = addToneTrack ("Comp Test", 1, tone, clipStart);
    expect ("insert Meridian Compressor", app.getBridge().insertPlugin (t, 0, *compressorDesc, error), error);
    call ("set_plugin_parameter", R"({"track":"Comp Test","slot":"A","parameter":"threshold","value":-18})");
    call ("set_plugin_parameter", R"({"track":"Comp Test","slot":"A","parameter":"ratio","value":4})");
    call ("set_plugin_parameter", R"({"track":"Comp Test","slot":"A","parameter":"knee","value":0})");
    r = bounce ("compressor_4to1", 0, end);
    {
        // Steady state, from 0.5 s into the tone (the onset passes before the 10 ms attack acts).
        const auto audio = readAudio (r.file);
        const auto from = (int) clipStart + 24000;
        const auto steady = audio.getNumSamples() > from + 24000 ? dsp::measureLevels (audio.getReadPointer (0, from), 24000).peak : 0.0f;
        expectNear ("compressor follows its static curve (4:1, 12 dB over -> 3 dB over)", db (steady),
                    -18.0 + (toneDb + 18.0) / 4.0 + panCentreDb, 0.1, "dB");
    }

    // Limiter: the tone driven +12 dB into a -1 dB ceiling comes out at -1 dB.
    clearTracks();
    t = addToneTrack ("Limit Test", 1, tone, clipStart);
    expect ("insert Meridian Limiter", app.getBridge().insertPlugin (t, 0, *limiterDesc, error), error);
    call ("set_plugin_parameter", R"({"track":"Limit Test","slot":"A","parameter":"input","value":12})");
    call ("set_plugin_parameter", R"({"track":"Limit Test","slot":"A","parameter":"ceiling","value":-1})");
    r = bounce ("limiter_ceiling", 0, end);
    expectNear ("limiter holds the ceiling (+12 dB drive into -1 dB)", db (r.left.peak), -1.0 + panCentreDb, 0.03, "dB");

    // Its look-ahead latency is compensated: an unlimited copy of the same tone
    // on another track must still add up coherently (+6.02 dB), not comb.
    call ("set_plugin_parameter", R"({"track":"Limit Test","slot":"A","parameter":"input","value":0})");
    call ("set_plugin_parameter", R"({"track":"Limit Test","slot":"A","parameter":"ceiling","value":0})");
    addToneTrack ("Plain Copy", 1, tone, clipStart);
    r = bounce ("limiter_pdc", 0, end);
    expectNear ("limiter latency is compensated (limited + plain copy sum coherently)", db (r.left.peak),
                toneDb + panCentreDb + 6.0206, 0.02, "dB");

    // Reverb on an aux bus, fed by a send: a tail that decays in the set time.
    // Excited with broadband noise, as in ISO 3382's interrupted-noise method
    // (a pure tone leaves both channels as sines, whose correlation is only a phase).
    clearTracks();
    const auto noise = writeNoise ("noise_2s", 2.0, -12.0, 0x5eed1234u);
    auto source = addToneTrack ("Dry", 1, noise, clipStart);
    auto bus = session->addTrack ("Reverb", 2, nullptr, Session::TrackKind::aux);
    routing::setSend (*session, source, 0, track::id (bus), nullptr);
    expect ("insert Meridian Reverb on a bus", app.getBridge().insertPlugin (bus, 0, *reverbDesc, error), error);
    call ("set_plugin_parameter", R"({"track":"Reverb","slot":"A","parameter":"mix","value":1})");
    call ("set_plugin_parameter", R"({"track":"Reverb","slot":"A","parameter":"decay","value":1.5})");
    call ("set_plugin_parameter", R"({"track":"Reverb","slot":"A","parameter":"damping","value":0})");
    call ("set_plugin_parameter", R"({"track":"Reverb","slot":"A","parameter":"predelay","value":0})");
    const auto toneEnd = clipStart + (juce::int64) (2.0 * sampleRate);
    r = bounce ("reverb_tail", 0, toneEnd + (juce::int64) (3.0 * sampleRate));
    auto audio = readAudio (r.file);
    if (audio.getNumSamples() > (int) toneEnd + (int) (2.5 * sampleRate))
    {
        // Decay rate of the tail after the tone stops: RMS in 50 ms windows, slope from -10 to -40 dB.
        const float* x = audio.getReadPointer (0);
        auto windowDb = [&] (juce::int64 at) { return db (dsp::measureLevels (x + at, 2400).rms); };
        const double startDb = windowDb (toneEnd);
        double t10 = -1, t40 = -1;
        for (juce::int64 at = toneEnd; at + 2400 < audio.getNumSamples(); at += 480)
        {
            const double level = windowDb (at) - startDb;
            if (t10 < 0 && level <= -10.0) t10 = (double) (at - toneEnd) / sampleRate;
            if (t40 < 0 && level <= -40.0) { t40 = (double) (at - toneEnd) / sampleRate; break; }
        }
        const double rt60 = t40 > t10 && t10 >= 0 ? 2.0 * (t40 - t10) : 0.0; // 30 dB in (t40 - t10)
        expectNear ("reverb tail decays in the set time (RT60 1.5 s)", rt60, 1.5, 0.2, "s");
        double lr = 0, ll = 0, rr = 0;
        for (int i = (int) toneEnd + 2400; i < (int) toneEnd + 48000; ++i)
        {
            const double a = audio.getSample (0, i), b = audio.getSample (1, i);
            lr += a * b;
            ll += a * a;
            rr += b * b;
        }
        const double correlation = ll > 0.0 && rr > 0.0 ? lr / std::sqrt (ll * rr) : 1.0;
        expect ("the reverb tail is wide stereo (|L/R correlation| < 0.3)", std::abs (correlation) < 0.3,
                "correlation " + fmt (correlation, 3));
    }

    // Delay: 100 ms, no feedback, full wet: the tone (0.5..2.5 s) echoes until 2.6 s at the same level.
    const auto* delayDesc = find ("Meridian Delay");
    const auto* gateDesc = find ("Meridian Gate");
    expect ("the delay and gate are in the plug-in list too", delayDesc != nullptr && gateDesc != nullptr, {});
    if (delayDesc != nullptr && gateDesc != nullptr)
    {
        clearTracks();
        t = addToneTrack ("Echo", 1, tone, clipStart);
        app.getBridge().insertPlugin (t, 0, *delayDesc, error);
        call ("set_plugin_parameter", R"({"track":"Echo","slot":"A","parameter":"time","value":100})");
        call ("set_plugin_parameter", R"({"track":"Echo","slot":"A","parameter":"feedback","value":0})");
        call ("set_plugin_parameter", R"({"track":"Echo","slot":"A","parameter":"mix","value":1})");
        r = bounce ("delay_echo", 0, clipStart + (juce::int64) (2.3 * sampleRate));
        {
            const auto echoed = readAudio (r.file);
            const auto from = (int) (clipStart + (juce::int64) (2.02 * sampleRate)), length = (int) (0.06 * sampleRate);
            const auto echo = echoed.getNumSamples() >= from + length ? dsp::measureLevels (echoed.getReadPointer (0, from), length).peak : 0.0f;
            expectNear ("delay: the echo continues the tone for exactly the delay time, at the same level",
                        db (echo), toneDb + panCentreDb, 0.05, "dB");
        }
        {
            // The first bounce ends with echoes still in the delay line; a bounce must start clean.
            call ("set_plugin_parameter", R"({"track":"Echo","slot":"A","parameter":"feedback","value":0.8})");
            const auto first = readAudio (bounce ("delay_repeat_1", 0, clipStart + (juce::int64) (1.5 * sampleRate)).file);
            const auto second = readAudio (bounce ("delay_repeat_2", 0, clipStart + (juce::int64) (1.5 * sampleRate)).file);
            bool identical = first.getNumSamples() == second.getNumSamples();
            for (int ch = 0; identical && ch < 2; ++ch)
                for (int i = 0; identical && i < first.getNumSamples(); ++i)
                    identical = juce::exactlyEqual (first.getSample (ch, i), second.getSample (ch, i));
            expect ("the same session bounces to the same file twice (plug-ins start clean: no tail from before)", identical, {});
        }

        // Gate: open for a -6 dB tone with a -20 dB threshold; closed (-80 dB) with -3 dB.
        auto levelIn = [this] (const BounceResult& result, double from, double to)
        {
            const auto rendered = readAudio (result.file);
            const auto a = (int) (from * sampleRate), b = std::min ((int) (to * sampleRate), rendered.getNumSamples());
            return b > a ? db (dsp::measureLevels (rendered.getReadPointer (0, a), b - a).peak) : -999.0;
        };
        clearTracks();
        t = addToneTrack ("Gated", 1, tone, clipStart);
        app.getBridge().insertPlugin (t, 0, *gateDesc, error);
        call ("set_plugin_parameter", R"({"track":"Gated","slot":"A","parameter":"threshold","value":-20})");
        r = bounce ("gate_open", 0, end);
        const double openLevel = levelIn (r, 1.0, 2.0);
        call ("set_plugin_parameter", R"({"track":"Gated","slot":"A","parameter":"threshold","value":-3})");
        r = bounce ("gate_closed", 0, end);
        const double closedLevel = levelIn (r, 1.0, 2.0);
        expect ("gate: a tone above the threshold passes untouched; below it, it drops by the 80 dB range",
                std::abs (openLevel - (toneDb + panCentreDb)) < 0.02 && std::abs (closedLevel - (toneDb + panCentreDb - 80.0)) < 0.1,
                "open " + fmt (openLevel) + " dB, closed " + fmt (closedLevel) + " dB");

        // De-esser: the 1 kHz tone is far below a 6 kHz band and passes; with the band moved down to
        // 2 kHz and a -60 dB threshold, it is turned down by the full 12 dB shelf's response at 1 kHz.
        if (const auto* deEsserDesc = find ("Meridian De-Esser"); deEsserDesc != nullptr)
        {
            clearTracks();
            t = addToneTrack ("Esses", 1, tone, clipStart);
            app.getBridge().insertPlugin (t, 0, *deEsserDesc, error);
            r = bounce ("deesser_pass", 0, end);
            const double passLevel = levelIn (r, 1.0, 2.0);
            call ("set_plugin_parameter", R"({"track":"Esses","slot":"A","parameter":"frequency","value":2000})");
            call ("set_plugin_parameter", R"({"track":"Esses","slot":"A","parameter":"threshold","value":-60})");
            r = bounce ("deesser_cut", 0, end);
            const double cutLevel = levelIn (r, 1.0, 2.0);
            const auto shelf = dsp::Biquad::design (dsp::Biquad::Type::highShelf, 2000.0, 1.0, -12.0, sampleRate);
            const double expectedCut = toneDb + panCentreDb + db (dsp::Biquad::magnitudeAt (shelf, 1000.0, sampleRate));
            expect ("de-esser: a tone below the band passes untouched; inside the band it is shelved down by the range",
                    std::abs (passLevel - (toneDb + panCentreDb)) < 0.02 && std::abs (cutLevel - expectedCut) < 0.1,
                    "pass " + fmt (passLevel) + " dB, cut " + fmt (cutLevel) + " dB (expected " + fmt (expectedCut) + ")");
        }
        else
            expect ("the de-esser is in the plug-in list", false, {});

        // The AI tools report a dynamics insert's gain reduction (the live value: only its presence is checked here).
        const auto controls = call ("list_plugin_parameters", R"({"track":"Esses","slot":"A"})");
        expect ("list_plugin_parameters gives a dynamics effect's gain_reduction_db",
                controls.ok && controls.data.hasProperty ("gain_reduction_db") && (double) controls.data["gain_reduction_db"] >= 0.0,
                controls.ok ? juce::JSON::toString (controls.data, true).getLastCharacters (80) : controls.error);
    }

    // Gain-reduction meters: one second of the 1 kHz tone (peak 0.5) through a fresh instance of each
    // dynamics effect; its meter must read the steady reduction its specification predicts, and its
    // window must show that figure. The EQ never reduces gain and has no meter.
    if (const auto* deEsserDesc = find ("Meridian De-Esser"); deEsserDesc != nullptr && gateDesc != nullptr)
    {
        struct Reading
        {
            bool created = false;
            float reduction = -1.0f; ///< -1: the effect has no meter
            juce::String shown;      ///< the meter's figure in the effect's window ("" if none)
        };
        BuiltinEffectFormat format;
        auto readMeter = [&format, this] (const juce::PluginDescription& description,
                                         std::initializer_list<std::pair<const char*, float>> settings)
        {
            Reading reading;
            juce::String createError;
            auto effect = format.createInstanceFromDescription (description, sampleRate, 512, createError);
            if (effect == nullptr)
                return reading;
            reading.created = true;
            for (const auto& setting : settings)
                for (auto* p : effect->getParameters())
                    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p);
                        hosted != nullptr && hosted->getParameterID() == setting.first)
                        p->setValue (p->getValueForText (juce::String (setting.second)));
            effect->prepareToPlay (sampleRate, 512);
            juce::AudioBuffer<float> block (2, 512);
            juce::MidiBuffer noMidi;
            for (int start = 0; start < (int) sampleRate; start += 512)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const auto x = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (start + i) / sampleRate));
                    block.setSample (0, i, x);
                    block.setSample (1, i, x);
                }
                effect->processBlock (block, noMidi);
            }
            if (const auto* meter = dynamic_cast<const GainReductionMeter*> (effect.get()))
                reading.reduction = meter->getGainReductionDb();
            std::unique_ptr<juce::AudioProcessorEditor> editor (effect->createEditorAndMakeActive());
            if (auto* bar = editor != nullptr ? editor->findChildWithID ("gainReduction") : nullptr)
                reading.shown = bar->getDescription();
            if (editor != nullptr && reading.reduction >= 0.0f)
                snapshot (*editor, "plugin_" + description.name.removeCharacters (" -").toLowerCase() + ".png");
            effect->editorBeingDeleted (editor.get()); // as a host does before deleting the window
            editor.reset();
            return reading;
        };

        const auto comp = readMeter (*compressorDesc, { { "threshold", -18.0f }, { "ratio", 4.0f }, { "knee", 0.0f } });
        const auto lim = readMeter (*limiterDesc, { { "input", 12.0f }, { "ceiling", -1.0f } });
        const auto gated = readMeter (*gateDesc, { { "threshold", -3.0f } });
        const auto ess = readMeter (*deEsserDesc, { { "frequency", 2000.0f }, { "threshold", -60.0f } });
        const auto eq = readMeter (*eqDesc, {});
        const double compExpected = (toneDb + 18.0) * (1.0 - 1.0 / 4.0), limExpected = toneDb + 12.0 + 1.0;
        expect ("gain-reduction meters read the steady reduction (compressor 8.98, limiter 6.98, gate 80, de-esser 12 dB)",
                comp.created && lim.created && gated.created && ess.created
                    && std::abs (comp.reduction - compExpected) < 0.15 && std::abs (lim.reduction - limExpected) < 0.1
                    && std::abs (gated.reduction - 80.0) < 0.1 && std::abs (ess.reduction - 12.0) < 0.01,
                "compressor " + fmt (comp.reduction) + ", limiter " + fmt (lim.reduction) + ", gate " + fmt (gated.reduction)
                    + ", de-esser " + fmt (ess.reduction) + " dB");
        auto showsItsFigure = [] (const Reading& reading)
        {
            return reading.shown == "-" + juce::String (reading.reduction, 1) + " dB";
        };
        expect ("each dynamics window shows its meter with that figure; the EQ has none",
                showsItsFigure (comp) && showsItsFigure (lim) && showsItsFigure (gated) && showsItsFigure (ess)
                    && eq.created && eq.reduction < 0.0f && eq.shown.isEmpty(),
                comp.shown + " | " + lim.shown + " | " + gated.shown + " | " + ess.shown + " | EQ \"" + eq.shown + "\"");

        // Tuner: the 1 kHz tone passes untouched and reads B5 +21.3 cents (69 + 12 log2(1000/440) = 83.213),
        // and 7.85 cents lower (+13.5) with the reference at A4 = 442 Hz.
        const auto* tunerDesc = find ("Meridian Tuner");
        juce::String tunerError;
        auto tuner = tunerDesc != nullptr ? format.createInstanceFromDescription (*tunerDesc, sampleRate, 512, tunerError) : nullptr;
        auto* readout = dynamic_cast<PitchReadout*> (tuner.get());
        expect ("the tuner is in the plug-in list and reads pitch", readout != nullptr, tunerError);
        if (readout != nullptr)
        {
            const auto silent = readout->readPitch(); // before any audio
            tuner->prepareToPlay (sampleRate, 512);
            juce::AudioBuffer<float> block (2, 512);
            juce::MidiBuffer noMidi;
            bool untouched = true;
            for (int start = 0; start < (int) sampleRate; start += 512)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const auto x = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (start + i) / sampleRate));
                    block.setSample (0, i, x);
                    block.setSample (1, i, x);
                }
                tuner->processBlock (block, noMidi);
                for (int i = 0; untouched && i < 512; ++i)
                    untouched = juce::exactlyEqual (block.getSample (0, i),
                                                    (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (start + i) / sampleRate)));
            }
            const auto at440 = readout->readPitch();
            for (auto* p : tuner->getParameters())
                if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p); hosted != nullptr && hosted->getParameterID() == "reference")
                    p->setValue (p->getValueForText ("442"));
            const auto at442 = readout->readPitch();
            auto describePitch = [] (const PitchReadout::Reading& reading)
            {
                return reading.found ? reading.note + " " + juce::String (reading.cents, 2) + " cents, " + juce::String (reading.frequencyHz, 2) + " Hz" : juce::String ("no pitch");
            };
            const double expected440 = (69.0 + 12.0 * std::log2 (1000.0 / 440.0) - 83.0) * 100.0;
            const double expected442 = (69.0 + 12.0 * std::log2 (1000.0 / 442.0) - 83.0) * 100.0;
            expect ("tuner: audio passes bit for bit; the 1 kHz tone reads B5 +21.3 cents (A4 = 440), +13.5 (A4 = 442); silence reads nothing",
                    untouched && ! silent.found && at440.found && at440.note == "B5" && std::abs (at440.cents - expected440) < 0.5
                        && std::abs (at440.frequencyHz - 1000.0) < 0.3 && at442.found && std::abs (at442.cents - expected442) < 0.5,
                    "untouched " + juce::String (untouched ? "yes" : "no") + "; " + describePitch (at440) + "; at 442: "
                        + describePitch (at442));

            std::unique_ptr<juce::AudioProcessorEditor> editor (tuner->createEditorAndMakeActive());
            auto* display = editor != nullptr ? editor->findChildWithID ("tunerDisplay") : nullptr;
            const auto shown = display != nullptr ? display->getDescription() : juce::String();
            expect ("the tuner window shows the note, cents and frequency", shown.startsWith ("B5 +") && shown.contains ("1000.0 Hz"), shown);
            if (editor != nullptr)
                snapshot (*editor, "plugin_meridiantuner.png");
            tuner->editorBeingDeleted (editor.get());
            editor.reset();
        }
    }

    // Reference render: a pad through EQ, compressor and limiter, into the reverb bus.
    clearTracks();
    auto pad = session->addTrack ("Pad", 2, nullptr, Session::TrackKind::instrument);
    auto padClip = session->addMidiClip (pad, 0, 4 * 24000, "pad", nullptr);
    for (const auto& n : std::initializer_list<midi::Note> { { 60, 0.8f, 0, 1440 }, { 64, 0.8f, 0, 1440 }, { 67, 0.8f, 0, 1440 } })
        midimodel::addNote (padClip, n, nullptr);
    auto room = session->addTrack ("Room", 2, nullptr, Session::TrackKind::aux);
    routing::setSend (*session, pad, 0, track::id (room), nullptr);
    app.getBridge().insertPlugin (pad, 0, *eqDesc, error);
    app.getBridge().insertPlugin (room, 0, *reverbDesc, error);
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"A","parameter":"hpOn","value":1})");
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"A","parameter":"hpFreq","value":200})");
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"A","parameter":"p3Gain","value":-5})");
    call ("set_plugin_parameter", R"({"track":"Room","slot":"A","parameter":"mix","value":1})");
    app.getBridge().insertPlugin (pad, 1, *compressorDesc, error);
    app.getBridge().insertPlugin (pad, 2, *limiterDesc, error);
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"B","parameter":"threshold","value":-24})");
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"B","parameter":"makeup","value":6})");
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"C","parameter":"input","value":6})");
    call ("set_plugin_parameter", R"({"track":"Pad","slot":"C","parameter":"ceiling","value":-3})");
    compareWithReference ("golden_mix_effects", bounce ("golden_mix_effects", 0, 4 * 24000 + 48000));

    expect ("session with built-in effects stays valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));
    clearTracks();
}

//==============================================================================
// Loop playback, click, count-in and markers.

namespace
{
/** Renders `total` samples from a bare engine (no graph, no device) in blocks of
    `block`, returning the left output. */
std::vector<float> renderEngine (AudioEngine& engine, int total, int block)
{
    std::vector<float> left ((size_t) total), right ((size_t) total);
    for (int done = 0; done < total; done += block)
    {
        const int n = std::min (block, total - done);
        float* outs[2] = { left.data() + done, right.data() + done };
        engine.renderOffline (outs, 2, n);
    }
    return left;
}

/** Sample indices where a click starts. A click begins at zero phase and is
    never exactly zero after that, so a zero followed by sound marks an onset
    (also when a new click cuts off a ringing one). */
std::vector<int> clickOnsets (const std::vector<float>& x)
{
    std::vector<int> onsets;
    for (int i = 0; i + 1 < (int) x.size(); ++i)
        if (juce::exactlyEqual (x[(size_t) i], 0.0f) && ! juce::exactlyEqual (x[(size_t) i + 1], 0.0f))
            onsets.push_back (i);
    return onsets;
}

juce::String listOf (const std::vector<int>& v, size_t max = 8)
{
    juce::StringArray s;
    for (size_t i = 0; i < v.size() && i < max; ++i)
        s.add (juce::String (v[i]));
    return s.joinIntoString (", ") + (v.size() > max ? ", ..." : "");
}
} // namespace

void SelfTest::testLoopClickMarkers()
{
    beginGroup ("Loop playback, click, count-in, markers");

    // A bare engine: its output is only the click, so every timing is visible to the sample.
    {
        AudioEngine engine;
        engine.prepare (48000.0, 512);
        engine.getMasterBus().consoleEnabled.store (false);
        engine.clickEnabled.store (true);
        engine.clickBpm.store (120.0); // 24000 samples per beat
        engine.clickLevelDb.store (-6.0);
        auto& transport = engine.getTransport();

        transport.play();
        auto x = renderEngine (engine, 120000, 480);
        const auto onsets = clickOnsets (x);
        expect ("click: one click on every beat, starting exactly on the beat sample (0, 24000, 48000...)",
                onsets == std::vector<int> { 0, 24000, 48000, 72000, 96000 }, listOf (onsets));
        float accent = 0.0f, other = 0.0f;
        for (int i = 0; i < 1920; ++i)
        {
            accent = std::max (accent, std::abs (x[(size_t) i]));
            other = std::max (other, std::abs (x[(size_t) i + 24000]));
        }
        expectNear ("click: the accent (beat 1) peaks at the set level", db (accent), -6.0, 0.001, "dB");
        expectNear ("click: other beats 4 dB lower", db (other), -10.0, 0.001, "dB");
        transport.stop();

        // Loop [12000, 30000): 18000 samples, wrapping inside blocks of 512.
        transport.locate (12000);
        transport.setLoop (true, 12000, 30000);
        transport.play();
        x = renderEngine (engine, 18000 * 5, 512);
        const auto looped = clickOnsets (x);
        expect ("loop: the click at beat 24000 repeats every 18000 samples, exactly (wraps are sample-accurate)",
                looped == std::vector<int> { 12000, 30000, 48000, 66000, 84000 }, listOf (looped));
        expect ("loop: the playhead is back at the loop start after whole passes", transport.getPosition() == 12000,
                juce::String (transport.getPosition()));
        transport.stop();

        // A loop shorter than a block: several wraps inside every block.
        transport.locate (24000);
        transport.setLoop (true, 24000, 24100);
        transport.play();
        x = renderEngine (engine, 5000, 512);
        const auto tight = clickOnsets (x);
        expect ("loop: a 100-sample loop wraps 50 times in 5000 samples, each on its sample",
                tight.size() == 50 && tight.front() == 0 && tight.back() == 4900 && transport.getPosition() == 24000,
                juce::String ((int) tight.size()) + " onsets, playhead " + juce::String (transport.getPosition()));
        transport.stop();

        // Never while recording.
        transport.locate (12000);
        transport.setLoop (true, 12000, 30000);
        transport.play (Transport::State::recording);
        renderEngine (engine, 48000, 512);
        expect ("loop: recording runs straight through the loop end", transport.getPosition() == 12000 + 48000,
                juce::String (transport.getPosition()));
        transport.stop();
        transport.setLoop (false, 0, 0);

        // Count-in: 2 bars at 120 BPM = 192000 samples of click; the playhead waits, then plays from bar 2.
        engine.clickEnabled.store (false); // the count-in clicks even with the click off
        transport.locate (96000);
        transport.play (Transport::State::playing, 192000);
        x = renderEngine (engine, 192000, 500);
        const auto counted = clickOnsets (x);
        expect ("count-in: 8 clicks on its own beat grid, the playhead held at the start",
                counted.size() == 8 && counted.front() == 0 && counted.back() == 168000 && transport.getPosition() == 96000,
                listOf (counted) + "; playhead " + juce::String (transport.getPosition()));
        x = renderEngine (engine, 24000, 500);
        expect ("count-in: then playback starts exactly at the start (and the click stays off)",
                transport.getPosition() == 96000 + 24000 && clickOnsets (x).empty(), juce::String (transport.getPosition()));
        transport.stop();
    }

    // Seamless loop audio: the pinned loop-start buffer is served at once after a jump.
    {
        const auto file = media.getChildFile ("tone1k_mono.wav");
        auto makeStream = [this, &file]
        {
            return std::make_unique<ClipStream> (std::unique_ptr<juce::AudioFormatReader> (app.getFormatManager().createReaderFor (file)), 4096);
        };
        auto serviceAll = [] (ClipStream& s) { for (int i = 0; i < 10 && s.service(); ++i) {} };
        float buffer[512] {};
        float* dest[1] = { buffer };

        auto pinned = makeStream();
        pinned->cue (60000); // playing near the loop end
        pinned->pin (1000);  // the loop starts at source frame 1000
        serviceAll (*pinned);
        const bool served = pinned->isPinnedReady (1000) && pinned->read (1000, dest, 1, 512);
        float direct[512] {};
        float* directDest[1] = { direct };
        std::unique_ptr<juce::AudioFormatReader> reader (app.getFormatManager().createReaderFor (file));
        reader->read (directDest, 1, 1000, 512);
        expect ("loop audio: after the jump to the loop start, the pinned buffer serves it at once, bit-exact",
                served && std::equal (buffer, buffer + 512, direct), {});

        auto unpinned = makeStream();
        unpinned->cue (60000);
        serviceAll (*unpinned);
        expect ("loop audio: without the pin the same jump would miss (proves the test can fail)",
                ! unpinned->read (1000, dest, 1, 512), {});
    }

    // The application and the AI tools.
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    auto& transport = app.getEngine().getTransport();

    auto r = call ("set_loop", R"({"enabled":true,"start_seconds":1.0,"end_seconds":3.0})");
    expect ("set_loop: the engine loops exactly the given range", r.ok && transport.isLoopEnabled()
            && transport.getLoopStart() == 48000 && transport.getLoopEnd() == 144000, r.error);
    r = call ("set_loop", R"({"enabled":true,"start_seconds":3.0,"end_seconds":1.0})");
    expect ("set_loop: an empty range is refused", ! r.ok, r.error);
    call ("set_loop", R"({"enabled":false})");
    expect ("set_loop: off", ! transport.isLoopEnabled(), {});

    r = call ("set_click", R"({"enabled":true,"level_db":-12,"count_in_bars":2})");
    expect ("set_click: click, level and count-in reach the engine",
            r.ok && app.getEngine().clickEnabled.load() && juce::exactlyEqual (app.getEngine().clickLevelDb.load(), -12.0)
                && app.getCountInSamples() == (juce::int64) std::llround (8 * 60.0 * app.getEngine().getSampleRate() / session->getTempo()),
            r.error + " count-in " + juce::String (app.getCountInSamples()));
    call ("set_click", R"({"enabled":false,"count_in_bars":0})");

    r = call ("add_marker", R"({"name":"Chorus","seconds":1.5})");
    const auto second = call ("add_marker", R"({"name":"Verse","seconds":0.5})");
    const auto listed = call ("list_markers", "{}");
    expect ("markers: added, numbered, and listed in time order",
            r.ok && second.ok && juce::JSON::toString (listed.data, true).contains (R"("name": "Verse")")
                && juce::JSON::toString (listed.data, true).indexOf ("Verse") < juce::JSON::toString (listed.data, true).indexOf ("Chorus"),
            juce::JSON::toString (listed.data, true));
    const int chorus = r.data["marker"];
    call ("transport", R"({"action":"locate","marker":)" + juce::String (chorus) + "}");
    for (int waited = 0; transport.getPosition() != 72000 && waited < 2000; waited += 10)
        juce::Thread::sleep (10); // with a device running, the audio thread applies the move at its next block
    expect ("markers: locate to a marker", transport.getPosition() == 72000, juce::String (transport.getPosition()));
    r = call ("delete_marker", R"({"marker":)" + juce::String (chorus) + "}");
    expect ("markers: delete, then undo brings it back",
            r.ok && ! session->getMarkerByNumber (chorus).isValid()
                && (session->getUndoManager().undo(), session->getMarkerByNumber (chorus).isValid()), r.error);

    const auto saved = app.saveSession();
    juce::String error;
    const auto reloaded = Session::load (session->getSessionFile(), error);
    expect ("markers are saved in the session and pass the validator",
            saved.wasOk() && reloaded != nullptr && reloaded->getMarkers().size() == 2 && findSessionProblem (*reloaded).isEmpty(), error);
    for (const auto& m : session->getMarkers())
        session->removeMarker (m, nullptr);
}

//==============================================================================
// MIDI keyboard: live play on armed instrument tracks, capture while recording.

void SelfTest::testMidiInput()
{
    beginGroup ("MIDI keyboard (live play, recording)");

    // A bare engine with one armed instrument track built by hand: what the
    // keyboard does is the only thing in the output.
    AudioEngine engine;
    engine.prepare (48000.0, 512);
    engine.getMasterBus().consoleEnabled.store (false);
    auto state = std::make_shared<TrackRealtimeState> (7);
    state->recordArmed.store (true);
    {
        auto graph = std::make_unique<RenderGraph>();
        graph->maxBlockSize = 512;
        graph->sampleRate = 48000.0;
        TrackRender t;
        t.state = state;
        t.numChannels = 2;
        t.inputChannel = -1;
        t.instrument = std::make_shared<InstrumentState>();
        t.buffer.setSize (2, 512);
        t.clipScratch.setSize (2, 512);
        graph->tracks.push_back (std::move (t));
        engine.publishGraph (std::move (graph));
    }
    auto firstSound = [] (const std::vector<float>& x)
    {
        for (int i = 0; i < (int) x.size(); ++i)
            if (! juce::exactlyEqual (x[(size_t) i], 0.0f))
                return i;
        return -1;
    };

    auto x = renderEngine (engine, 2048, 512);
    expect ("armed instrument track is silent until a key is played", firstSound (x) < 0, {});

    engine.pushLiveMidi (69, 1.0f, true, 0.0); // arrived long ago: plays at the start of the next block
    x = renderEngine (engine, 512, 512);
    const int atBlockStart = firstSound (x); // the synth's own latency (2x oversampling filter) after sample 0
    expect ("a key played while stopped sounds at the start of the next block (plus the synth's latency)",
            atBlockStart >= 0 && atBlockStart <= synth::PolySynth::getLatencySamples() + 2,
            "first sound at sample " + juce::String (atBlockStart));
    engine.pushLiveMidi (69, 0.0f, false, 0.0);
    renderEngine (engine, 48000, 512); // let the release finish

    // Timing within a block: a note that arrived halfway through the previous
    // block's time lands halfway through this one (constant one-block latency).
    const double blockSeconds = 512.0 / 48000.0;
    // Keys played a quarter, a half and three quarters of a block apart must sound
    // exactly that far apart (128 samples each): constant latency, no jitter.
    juce::StringArray placed;
    std::vector<int> onsets;
    for (const double fraction : { 0.25, 0.5, 0.75 })
    {
        engine.pushLiveMidi (72, 1.0f, true, juce::Time::getMillisecondCounterHiRes() * 0.001 - blockSeconds * (1.0 - fraction));
        x = renderEngine (engine, 512, 512);
        onsets.push_back (firstSound (x));
        placed.add (juce::String (onsets.back()));
        engine.pushLiveMidi (72, 0.0f, false, 0.0);
        renderEngine (engine, 48000, 512);
    }
    expect ("keys keep their spacing within a block (no jitter): 1/4, 1/2, 3/4 of a block -> 128 samples apart",
            onsets[0] > 0 && std::abs (onsets[1] - onsets[0] - 128) <= 3 && std::abs (onsets[2] - onsets[1] - 128) <= 3,
            "first sound at samples " + placed.joinIntoString (", "));

    state->recordArmed.store (false);
    engine.pushLiveMidi (60, 1.0f, true, 0.0);
    x = renderEngine (engine, 1024, 512);
    expect ("a track that is not armed ignores the keyboard", firstSound (x) < 0, {});
    engine.pushLiveMidi (60, 0.0f, false, 0.0);
    renderEngine (engine, 512, 512); // consumed here, not by the recording below
    state->recordArmed.store (true);

    // Recording: notes are captured at the timeline sample they were played into.
    auto& transport = engine.getTransport();
    transport.locate (10000);
    transport.play (Transport::State::recording);
    renderEngine (engine, 1024, 512);                  // playhead now 11024
    engine.pushLiveMidi (64, 0.75f, true, 0.0);
    renderEngine (engine, 512, 512);                    // captured at 11024
    renderEngine (engine, 1024, 512);
    engine.pushLiveMidi (64, 0.0f, false, 0.0);
    renderEngine (engine, 512, 512);                    // captured at 12560
    transport.stop();

    std::vector<AudioEngine::RecordedMidi> captured;
    AudioEngine::RecordedMidi m;
    while (engine.popRecordedMidi (m))
        captured.push_back (m);
    expect ("recording captures each key at its exact timeline sample, for the right track",
            captured.size() == 2 && captured[0].trackId == 7 && captured[0].time == 11024 && captured[0].on && captured[0].pitch == 64
                && std::abs (captured[0].velocity - 0.75f) < 1.0e-6f && captured[1].time == 12560 && ! captured[1].on,
            juce::String ((int) captured.size()) + " events" + (captured.empty() ? juce::String() : ", first at " + juce::String (captured[0].time)));

    engine.pushLiveMidi (65, 1.0f, true, 0.0);
    renderEngine (engine, 512, 512);
    expect ("nothing is captured while not recording", ! engine.popRecordedMidi (m), {});
    engine.pushLiveMidi (65, 0.0f, false, 0.0);

    // End to end in the application: arm, record, play keys, stop -> a MIDI clip.
    if (! app.getEngine().isDeviceRunning())
    {
        skip ("record from the keyboard into a MIDI clip (whole application)", "needs a running audio device");
        return;
    }
    clearTracks();
    auto* session = app.getSession();
    auto keys = session->addTrack ("Keys", 2, nullptr, Session::TrackKind::instrument);
    keys.setProperty (ids::recordArm, true, nullptr);
    app.locate (0);
    juce::Thread::sleep (50);
    const auto started = app.transportRecord();
    juce::Thread::sleep (200);
    for (const int pitch : { 60, 64, 67 })
    {
        app.getEngine().pushLiveMidi (pitch, 0.8f, true, juce::Time::getMillisecondCounterHiRes() * 0.001);
        juce::Thread::sleep (150);
        app.getEngine().pushLiveMidi (pitch, 0.0f, false, juce::Time::getMillisecondCounterHiRes() * 0.001);
        juce::Thread::sleep (50);
    }
    app.transportStop();
    const auto clips = track::clips (keys);
    const auto notes = clips.getNumChildren() == 1 ? midimodel::readNotes (clips.getChild (0)) : midi::NoteSequence();
    juce::String detail;
    for (const auto& n : notes.getNotes())
        detail << n.pitch << "@" << n.start << "+" << n.length << " ";
    expect ("recording an armed instrument track makes a MIDI clip with the notes played, in order",
            started.wasOk() && notes.size() == 3 && notes.getNotes()[0].pitch == 60 && notes.getNotes()[1].pitch == 64
                && notes.getNotes()[2].pitch == 67,
            started.getErrorMessage() + detail);
    // Each key was held 150 ms and keys were 200 ms apart (at 120 BPM, 960 ticks = 500 ms).
    // Wall-clock sleeps are not exact, so these are checked to within 30 ms (58 ticks).
    bool spacing = notes.size() == 3;
    for (size_t i = 0; spacing && i < 3; ++i)
    {
        spacing = spacing && std::abs ((double) notes.getNotes()[i].length - 288.0) < 58.0;
        if (i > 0)
            spacing = spacing && std::abs ((double) (notes.getNotes()[i].start - notes.getNotes()[i - 1].start) - 384.0) < 58.0;
    }
    expect ("recorded notes keep their lengths (150 ms) and spacing (200 ms)", spacing, detail);
    expect ("the take is one undo step", session->getUndoManager().getUndoDescription() == "Record MIDI",
            session->getUndoManager().getUndoDescription());
    clearTracks();
}

//==============================================================================
// Automation of pan, mute, sends and plug-in controls; Touch, Latch and Write.

void SelfTest::testAutomation()
{
    beginGroup ("Automation (pan, mute, sends, plug-ins; Touch, Latch, Write)");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    auto* session = app.getSession();
    auto at = [this] (double seconds) { return (juce::int64) (seconds * sampleRate); };

    // Peak (dB) of a channel of the last bounce over [from, to) seconds.
    auto levelIn = [this, at] (const BounceResult& r, int channel, double from, double to)
    {
        const auto audio = readAudio (r.file);
        const auto a = (int) at (from), b = (int) std::min<juce::int64> (at (to), audio.getNumSamples());
        return b > a && channel < audio.getNumChannels() ? db (dsp::measureLevels (audio.getReadPointer (channel, a), b - a).peak) : -999.0;
    };

    // Pan: hard left until 1.0 s, hard right from 1.5 s.
    clearTracks();
    auto t = addToneTrack ("Pan Auto", 1, tone, clipStart);
    auto r = call ("set_automation", R"({"track":"Pan Auto","parameter":"pan","points":[{"seconds":0,"value":-1},{"seconds":1.0,"value":-1},{"seconds":1.5,"value":1}]})");
    auto b = bounce ("automation_pan", 0, end);
    expect ("pan automation: hard left, then hard right (sample-accurate pan law)",
            r.ok && std::abs (levelIn (b, 0, 0.6, 0.95) - toneDb) < 0.02 && levelIn (b, 1, 0.6, 0.95) < -100.0
                && std::abs (levelIn (b, 1, 1.6, 2.4) - toneDb) < 0.02 && levelIn (b, 0, 1.6, 2.4) < -100.0,
            r.error + " L " + fmt (levelIn (b, 0, 0.6, 0.95)) + " / R " + fmt (levelIn (b, 1, 1.6, 2.4)) + " dB");

    // Mute: muted between 1.0 and 1.5 s.
    clearTracks();
    t = addToneTrack ("Mute Auto", 1, tone, clipStart);
    r = call ("set_automation", R"({"track":"Mute Auto","parameter":"mute","points":[{"seconds":0,"value":0},{"seconds":1.0,"value":1},{"seconds":1.5,"value":0}]})");
    b = bounce ("automation_mute", 0, end);
    expect ("mute automation: silent between its points, at full level either side",
            r.ok && levelIn (b, 0, 1.05, 1.45) < -100.0 && std::abs (levelIn (b, 0, 0.6, 0.95) - (toneDb + panCentreDb)) < 0.02
                && std::abs (levelIn (b, 0, 1.6, 2.4) - (toneDb + panCentreDb)) < 0.02,
            r.error + " muted part " + fmt (levelIn (b, 0, 1.05, 1.45)) + " dB");

    // Send level: a pre-fader send with the fader off, so only the send is heard; -12 dB, then 0 dB.
    clearTracks();
    t = addToneTrack ("Send Auto", 1, tone, clipStart);
    auto bus = session->addTrack ("FX Bus", 2, nullptr, Session::TrackKind::aux);
    routing::setSend (*session, t, 0, track::id (bus), nullptr);
    routing::findSend (t, 0).setProperty (ids::preFader, true, nullptr);
    t.setProperty (ids::volumeDb, -144.0, nullptr);
    r = call ("set_automation", R"({"track":"Send Auto","parameter":"send A","points":[{"seconds":0,"value":-12},{"seconds":1.2,"value":-12},{"seconds":1.3,"value":0}]})");
    b = bounce ("automation_send", 0, end);
    const double sendStep = levelIn (b, 0, 1.4, 2.4) - levelIn (b, 0, 0.6, 1.1);
    expectNear ("send level automation: the send rises by exactly the drawn 12 dB", sendStep, 12.0, 0.02, "dB");

    // Plug-in control: Meridian EQ's Output from 0 dB to -12 dB.
    clearTracks();
    t = addToneTrack ("Plugin Auto", 1, tone, clipStart);
    juce::PluginDescription eq;
    for (const auto& d : BuiltinEffectFormat::getDescriptions())
        if (d.name == "Meridian EQ")
            eq = d;
    juce::String error;
    app.getBridge().insertPlugin (t, 0, eq, error);
    r = call ("set_automation", R"({"track":"Plugin Auto","parameter":"insert A: Output","points":[{"seconds":0,"value":0},{"seconds":1.2,"value":0},{"seconds":1.21,"value":-12}]})");
    b = bounce ("automation_plugin", 0, end);
    const double pluginStep = levelIn (b, 0, 1.4, 2.4) - levelIn (b, 0, 0.6, 1.1);
    expectNear ("plug-in control automation: the EQ's output steps down by the drawn 12 dB", pluginStep, -12.0, 0.05,
                "dB" + (r.ok ? juce::String() : " (" + r.error + ")"));
    r = call ("set_automation", R"({"track":"Plugin Auto","parameter":"insert A: No Such Knob","points":[{"seconds":0,"value":1}]})");
    expect ("set_automation refuses a control the plug-in does not have", ! r.ok && r.error.contains ("list_plugin_parameters"), r.error);

    // The recorder: passes driven with simulated playhead positions, 20 ms apart.
    auto& recorder = app.getAutomationRecorder();
    const auto step = at (0.02);
    auto runPass = [&] (double fromSeconds, double toSeconds, const std::function<void (double)>& atTime)
    {
        for (auto p = at (fromSeconds); p <= at (toSeconds); p += step)
        {
            atTime ((double) p / sampleRate);
            recorder.process (true, p);
        }
        recorder.process (false, 0);
    };
    auto laneValue = [&] (const juce::ValueTree& track, const char* parameter, double seconds)
    {
        return automation::valueAt (automation::points (automation::lane (track, parameter)), at (seconds));
    };

    // Write: the whole pass is recorded, a fader ramp from -20 to -10 dB.
    clearTracks();
    t = addToneTrack ("Write", 1, tone, clipStart);
    call ("set_track", R"({"track":"Write","automation_mode":"write"})");
    runPass (0.5, 2.5, [&] (double s) { t.setProperty (ids::volumeDb, -20.0 + 10.0 * (s - 0.5) / 2.0, nullptr); });
    expect ("Write: the pass is recorded as one undo step and follows the fader",
            session->getUndoManager().getUndoDescription() == "Write Automation"
                && std::abs (laneValue (t, "volume", 1.5) - (-15.0)) < 0.06 && std::abs (laneValue (t, "volume", 0.5) - (-20.0)) < 0.06,
            session->getUndoManager().getUndoDescription() + ", " + fmt (laneValue (t, "volume", 1.5)) + " dB at 1.5 s");
    expect ("Write: thinned to a few points (a straight ramp needs only its ends)",
            automation::lane (t, "volume").getNumChildren() <= 6, juce::String (automation::lane (t, "volume").getNumChildren()) + " points");
    call ("set_track", R"({"track":"Write","automation_mode":"read","volume_db":0})");
    b = bounce ("automation_written", 0, end);
    expectNear ("Write: playing it back in Read gives the recorded level (-15 dB at 1.5 s, before the pan law)",
                levelIn (b, 0, 1.49, 1.51), toneDb - 15.0 + panCentreDb, 0.1, "dB");

    // Touch: an existing flat -6 dB lane; the fader is held at -18 dB from 1.0 to 1.5 s.
    clearTracks();
    t = addToneTrack ("Touch", 1, tone, clipStart);
    call ("set_automation", R"({"track":"Touch","parameter":"volume","points":[{"seconds":0,"value":-6},{"seconds":3,"value":-6}]})");
    call ("set_track", R"({"track":"Touch","automation_mode":"touch"})");
    const int touchId = track::id (t);
    runPass (0.5, 2.5, [&] (double s)
    {
        const bool holding = s >= 1.0 && s < 1.5;
        recorder.setTouched (touchId, "volume", holding);
        if (holding)
            t.setProperty (ids::volumeDb, -18.0, nullptr);
    });
    recorder.setTouched (touchId, "volume", false);
    expect ("Touch: recorded only while held, then glides back to the existing lane",
            std::abs (laneValue (t, "volume", 0.8) + 6.0) < 0.01 && std::abs (laneValue (t, "volume", 1.25) + 18.0) < 0.01
                && std::abs (laneValue (t, "volume", 1.5 + AutomationRecorder::glideSeconds + 0.05) + 6.0) < 0.01,
            fmt (laneValue (t, "volume", 0.8)) + " / " + fmt (laneValue (t, "volume", 1.25)) + " / "
                + fmt (laneValue (t, "volume", 1.85)) + " dB");

    // Latch: touched briefly at 1.0 s; the value is held from there to the stop.
    clearTracks();
    t = addToneTrack ("Latch", 1, tone, clipStart);
    call ("set_automation", R"({"track":"Latch","parameter":"volume","points":[{"seconds":0,"value":-6}]})");
    call ("set_track", R"({"track":"Latch","automation_mode":"latch"})");
    const int latchId = track::id (t);
    runPass (0.5, 2.5, [&] (double s)
    {
        const bool holding = s >= 1.0 && s < 1.1;
        recorder.setTouched (latchId, "volume", holding);
        if (holding)
            t.setProperty (ids::volumeDb, -24.0, nullptr);
    });
    expect ("Latch: recorded from the first touch until the stop",
            std::abs (laneValue (t, "volume", 0.8) + 6.0) < 0.01 && std::abs (laneValue (t, "volume", 2.0) + 24.0) < 0.01,
            fmt (laneValue (t, "volume", 0.8)) + " / " + fmt (laneValue (t, "volume", 2.0)) + " dB");

    // While a control is held in Touch, the engine follows the control, not the lane.
    call ("set_track", R"({"track":"Latch","automation_mode":"touch","volume_db":0})");
    call ("set_automation", R"({"track":"Latch","parameter":"volume","points":[{"seconds":0,"value":-20}]})");
    b = bounce ("automation_lane", 0, end);
    const double laneLevel = levelIn (b, 0, 1.0, 2.0);
    recorder.setTouched (latchId, "volume", true);
    b = bounce ("automation_held", 0, end);
    const double heldLevel = levelIn (b, 0, 1.0, 2.0);
    recorder.setTouched (latchId, "volume", false);
    expect ("holding a control overrides its lane (Touch): -20 dB lane, 0 dB fader",
            std::abs (laneLevel - (toneDb + panCentreDb - 20.0)) < 0.02 && std::abs (heldLevel - (toneDb + panCentreDb)) < 0.02,
            "lane " + fmt (laneLevel) + " dB, held " + fmt (heldLevel) + " dB");

    // Damaged lanes are repaired: an unknown parameter, a pan beyond hard right, a mute of 0.4.
    auto automationNode = track::automation (t);
    juce::ValueTree bogus (ids::LANE);
    bogus.setProperty (ids::parameter, "warp drive", nullptr);
    automationNode.appendChild (bogus, nullptr);
    juce::ValueTree panLane (ids::LANE);
    panLane.setProperty (ids::parameter, "pan", nullptr);
    juce::ValueTree wide (ids::POINT);
    wide.setProperty (ids::time, 0, nullptr);
    wide.setProperty (ids::value, 7.0, nullptr);
    panLane.appendChild (wide, nullptr);
    automationNode.appendChild (panLane, nullptr);
    const auto problemBefore = findSessionProblem (*session);
    const auto repairs = repairSession (*session);
    expect ("damaged automation is found by the validator and repaired (unknown lane removed, pan clamped)",
            problemBefore.isNotEmpty() && findSessionProblem (*session).isEmpty() && ! automation::lane (t, "warp drive").isValid()
                && juce::exactlyEqual ((double) automation::lane (t, "pan").getChild (0)[ids::value], 1.0),
            problemBefore + " | " + repairs.joinIntoString ("; ").substring (0, 200));
    clearTracks();
}

//==============================================================================
// Plug-in instruments (VST3) on instrument tracks.

void SelfTest::testPluginInstrument()
{
    beginGroup ("Plug-in instruments (VST3)");
#ifndef MERIDIAN_TEST_SYNTH_BINARY
    skip ("VST3 instrument hosting", "the test instrument was not built");
#else
    auto bundle = juce::File (MERIDIAN_TEST_SYNTH_BINARY).getParentDirectory();
    while (bundle != bundle.getParentDirectory() && ! bundle.getFileName().endsWithIgnoreCase (".vst3"))
        bundle = bundle.getParentDirectory();
    juce::AudioPluginFormat* vst3 = nullptr;
    for (auto* format : app.getPluginHost().getFormatManager().getFormats())
        if (format->getName() == "VST3")
            vst3 = format;
    juce::OwnedArray<juce::PluginDescription> found;
    if (vst3 != nullptr)
        vst3->findAllTypesForFile (found, bundle.getFullPathName());
    expect ("scan finds the test instrument, marked as an instrument", found.size() == 1 && found[0]->isInstrument, bundle.getFullPathName());
    if (found.isEmpty())
        return;
    const auto synthDesc = *found[0];

    auto* session = app.getSession();
    clearTracks();
    auto audio = addToneTrack ("Not An Instrument Track", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    auto keys = session->addTrack ("VST Keys", 2, nullptr, Session::TrackKind::instrument);
    juce::String error;
    const bool refusedOnAudio = ! app.getBridge().insertPlugin (audio, 0, synthDesc, error);
    const bool refusedInB = ! app.getBridge().insertPlugin (keys, 1, synthDesc, error);
    const bool inserted = app.getBridge().insertPlugin (keys, 0, synthDesc, error);
    expect ("an instrument plug-in goes in insert A of an instrument track (refused elsewhere)",
            refusedOnAudio && refusedInB && inserted, error);
    session->removeTrack (audio, nullptr);

    // A4 at exactly 1 s (beat 2 at 120 BPM), for half a second.
    auto clip = session->addMidiClip (keys, 0, (juce::int64) (3.0 * sampleRate), "notes", nullptr);
    midimodel::addNote (clip, { 69, 1.0f, 2 * midi::ticksPerQuarter, midi::ticksPerQuarter }, nullptr);
    const auto audioOut = readAudio (bounce ("vst_instrument", 0, (juce::int64) (3.0 * sampleRate)).file);
    const int on = (int) sampleRate, off = (int) (1.5 * sampleRate);
    int first = -1, last = -1;
    for (int i = 0; i < audioOut.getNumSamples(); ++i)
        if (! juce::exactlyEqual (audioOut.getSample (0, i), 0.0f))
        {
            if (first < 0)
                first = i;
            last = i;
        }
    expect ("the plug-in plays the clip's note from exactly its sample to exactly its note-off",
            first == on + 1 && last == off - 1, // the sine starts at zero phase; the note-off stops it dead
            "sound from " + juce::String (first) + " to " + juce::String (last) + ", note " + juce::String (on) + "-" + juce::String (off));

    // The sustain pedal reaches the plug-in as CC 64 on its exact samples: held from 1 s to 2 s
    // (beats 2-4), the note released at 1.5 s rings until exactly 2 s.
    {
        auto& pedalTools = app.getRemoteTools();
        const auto set = pedalTools.call ("add_notes", juce::JSON::parse (
            R"({"track":"VST Keys","clip":1,"notes":[],"sustain":[{"start_beats":2,"length_beats":2}]})"));
        const auto held = readAudio (bounce ("vst_instrument_sustain", 0, (juce::int64) (3.0 * sampleRate)).file);
        int heldLast = -1;
        for (int i = 0; i < held.getNumSamples(); ++i)
            if (! juce::exactlyEqual (held.getSample (0, i), 0.0f))
                heldLast = i;
        expect ("the pedal (CC 64) holds the plug-in's note past its note-off until exactly the pedal lifts",
                set.ok && heldLast == (int) (2.0 * sampleRate) - 1,
                set.ok ? "sound until " + juce::String (heldLast) + ", pedal up at " + juce::String ((int) (2.0 * sampleRate)) : set.error);
        midimodel::writeSustain (clip, {}, nullptr);
    }
    const float* x = audioOut.getReadPointer (0, on + 2400);
    expectNear ("it is the plug-in's 440 Hz at velocity 1 (amplitude 0.5), centre-panned stereo at unity",
                db (dsp::toneAmplitude (x, 19200, 440.0, sampleRate)), -6.0206, 0.02, "dB");
    const auto level = dsp::measureLevels (x, 19200);
    expect ("and only the plug-in: the built-in synth is silent (the output is a pure sine)",
            std::abs (level.rms * std::sqrt (2.0) / 0.5 - 1.0) < 0.002, "RMS " + juce::String (level.rms, 5));

    auto& tools = app.getRemoteTools();
    app.getPluginHost().getKnownPlugins().addType (synthDesc); // what a scan in the Plug-in Manager does
    const auto listed = juce::JSON::toString (tools.call ("list_plugins", juce::JSON::parse (R"({"filter":"Test Synth"})")).data);
    expect ("list_plugins shows the instrument as one", listed.contains ("Meridian Test Synth") && listed.contains (R"("instrument": true)"), listed);
    clearTracks();
#endif
}

//==============================================================================
// The sustain pedal: in clips, from the keyboard, recorded, and through the AI tools.

//==============================================================================
// The pitch wheel and the mod wheel in MIDI clips, through the AI tools, the
// engine and the built-in synth (a pure sine, so frequencies are exact).

void SelfTest::testPitchAndModWheels()
{
    beginGroup ("Pitch wheel and mod wheel");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    clearTracks();
    call ("set_tempo", R"({"bpm":120,"clear_changes":true})");
    call ("create_track", R"({"kind":"instrument","name":"Wheels"})");
    call ("set_synth", R"({"track":"Wheels","waveform":"sine","detune_cents":0,"attack":0.001,"sustain":1,"release":0.01,)"
                       R"("cutoff_hz":20000,"filter_env_octaves":0,"resonance":0})");

    // Clip 1 (beats 0-4): A4 throughout, the wheel up from beat 2. Clip 2 (beats 4-6): A4, no wheel.
    call ("add_midi_clip", R"({"track":"Wheels","start_beat":0,"length_beats":4,"name":"Bend"})");
    call ("add_midi_clip", R"({"track":"Wheels","start_beat":4,"length_beats":2,"name":"After"})");
    const auto added = call ("add_notes", R"({"track":"Wheels","clip":"Bend","notes":[{"pitch":69,"start_beats":0,"length_beats":4,"velocity":1}],)"
                                          R"("pitch_bend":[{"start_beats":0,"value":0},{"start_beats":2,"value":1}]})");
    call ("add_notes", R"({"track":"Wheels","clip":"After","notes":[{"pitch":69,"start_beats":0,"length_beats":2,"velocity":1}]})");
    const auto listed = call ("list_notes", R"({"track":"Wheels","clip":"Bend"})");
    const auto* bendList = listed.data["pitch_bend"].getArray();
    expect ("add_notes sets the pitch wheel and list_notes reports it",
            added.ok && bendList != nullptr && bendList->size() == 2 && juce::exactlyEqual ((double) (*bendList)[1]["start_beats"], 2.0)
                && juce::exactlyEqual ((double) (*bendList)[1]["value"], 1.0),
            added.error + juce::JSON::toString (listed.data["pitch_bend"], true));

    const auto audio = readAudio (bounce ("wheels_bend", 0, (juce::int64) (3.0 * sampleRate)).file);
    auto hzBetween = [&] (const juce::AudioBuffer<float>& a, double from, double to)
    {
        const auto start = (int) (from * sampleRate);
        return dsp::estimateFrequency (a.getReadPointer (0, start), (int) ((to - from) * sampleRate), sampleRate);
    };
    const double before = hzBetween (audio, 0.2, 0.9), bent = hzBetween (audio, 1.2, 1.9), after = hzBetween (audio, 2.2, 2.9);
    const double target = 440.0 * std::exp2 (synth::PolySynth::bendRangeSemitones / 12.0);
    expect ("the wheel at +1 from beat 2 raises A4 by exactly two semitones (440 -> 493.88 Hz)",
            std::abs (before - 440.0) < 0.1 && std::abs (bent - target) < 0.1,
            fmt (before, 2) + " Hz, then " + fmt (bent, 2) + " Hz");
    expect ("at the clip's end the wheel returns to rest: the next clip's A4 is 440 Hz again",
            std::abs (after - 440.0) < 0.1, fmt (after, 2) + " Hz");

    // Mod wheel fully up: vibrato of exactly half a semitone either way.
    call ("add_notes", R"({"track":"Wheels","clip":"Bend","notes":[],"pitch_bend":[],"mod_wheel":[{"start_beats":0,"value":1}]})");
    const auto vibrato = readAudio (bounce ("wheels_mod", 0, (juce::int64) (2.0 * sampleRate)).file);
    const float* x = vibrato.getReadPointer (0);
    double previousCrossing = -1.0, low = 1.0e9, high = 0.0;
    for (int i = (int) (0.2 * sampleRate); i < (int) (1.9 * sampleRate); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double crossing = (double) (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (previousCrossing >= 0.0)
            {
                low = std::min (low, sampleRate / (crossing - previousCrossing));
                high = std::max (high, sampleRate / (crossing - previousCrossing));
            }
            previousCrossing = crossing;
        }
    const double depth = synth::PolySynth::vibratoDepthSemitones;
    expect ("the mod wheel at 1 gives vibrato of exactly half a semitone either way (427.47 to 452.89 Hz)",
            std::abs (high - 440.0 * std::exp2 (depth / 12.0)) < 0.3 && std::abs (low - 440.0 * std::exp2 (-depth / 12.0)) < 0.3,
            fmt (low, 2) + " to " + fmt (high, 2) + " Hz");

    // Saved and reloaded; damaged values repaired.
    auto bendClip = track::clips (session->getTrack (0)).getChild (0);
    midimodel::writeControl (bendClip, midi::pitchBend, { { 0, -0.5f }, { 960, 0.25f } }, nullptr);
    const auto saved = app.saveSession();
    juce::String error;
    const auto loaded = saved.wasOk() ? Session::load (session->getSessionFile(), error) : nullptr;
    const auto reloaded = loaded != nullptr ? midimodel::readControl (track::clips (loaded->getTrack (0)).getChild (0), midi::pitchBend)
                                            : std::vector<midi::ControlPoint>();
    expect ("save and reload keep the wheel moves",
            reloaded.size() == 2 && reloaded[0].value == -0.5f && reloaded[1].tick == 960 && reloaded[1].value == 0.25f,
            saved.getErrorMessage() + error);

    auto list = bendClip.getChildWithName (ids::PITCHBEND);
    juce::ValueTree wild (ids::CC);
    wild.setProperty (ids::start, 480, nullptr);
    wild.setProperty (ids::value, 5.0, nullptr);
    list.appendChild (wild, nullptr); // out of range, and out of order
    const auto problem = findSessionProblem (*session);
    repairSession (*session);
    const auto repaired = midimodel::readControl (bendClip, midi::pitchBend);
    expect ("a damaged pitch-wheel list is found, then sorted and clamped by repair",
            problem.isNotEmpty() && findSessionProblem (*session).isEmpty() && repaired.size() == 3 && repaired[1].tick == 480
                && repaired[1].value == 1.0f,
            problem);
    clearTracks();
}

void SelfTest::testSustainPedal()
{
    beginGroup ("Sustain pedal");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    clearTracks();
    call ("set_tempo", R"({"bpm":120,"clear_changes":true})");

    // A4 for half a beat (0.25 s); the pedal down for the first four beats (0-2 s).
    call ("create_track", R"({"kind":"instrument","name":"Piano"})");
    call ("add_midi_clip", R"({"track":"Piano","start_beat":0,"length_beats":8})");
    const auto added = call ("add_notes", R"({"track":"Piano","clip":1,"notes":[{"pitch":69,"start_beats":0,"length_beats":0.5,"velocity":1}],)"
                                          R"("sustain":[{"start_beats":0,"length_beats":4}]})");
    const auto listed = call ("list_notes", R"({"track":"Piano","clip":1})");
    const auto* pedalList = listed.data["sustain"].getArray();
    expect ("add_notes sets the pedal and list_notes reports it (beats 0-4)",
            added.ok && pedalList != nullptr && pedalList->size() == 1
                && juce::exactlyEqual ((double) (*pedalList)[0]["start_beats"], 0.0)
                && juce::exactlyEqual ((double) (*pedalList)[0]["length_beats"], 4.0),
            juce::JSON::toString (listed.data["sustain"], true));

    auto peakDb = [this] (const juce::AudioBuffer<float>& audio, double from, double to)
    {
        const auto a = (int) (from * sampleRate), b = std::min ((int) (to * sampleRate), audio.getNumSamples());
        return b > a ? db (dsp::measureLevels (audio.getReadPointer (0, a), b - a).peak) : -999.0;
    };
    const auto pedalled = readAudio (bounce ("sustain_pedal", 0, (juce::int64) (4.0 * sampleRate)).file);
    const double whileHeld = peakDb (pedalled, 1.0, 1.5), afterLift = peakDb (pedalled, 2.6, 3.0);
    call ("add_notes", R"({"track":"Piano","clip":1,"notes":[],"sustain":[]})");
    const auto dry = readAudio (bounce ("sustain_none", 0, (juce::int64) (4.0 * sampleRate)).file);
    const double withoutPedal = peakDb (dry, 1.0, 1.5);
    expect ("the built-in synth holds a released note while the pedal is down, and releases it when it lifts",
            whileHeld > -30.0 && afterLift < -100.0 && withoutPedal < -100.0,
            "held " + fmt (whileHeld) + " dB at 1.0-1.5 s; " + fmt (afterLift) + " dB after the lift; without the pedal "
                + fmt (withoutPedal) + " dB");

    // A pedal range that began before the clip's (trimmed) start goes down at the clip start:
    // with the front trimmed by 0.1 s, a note at 0.25 s still rings while the pedal is held.
    auto clipNode = track::clips (session->getTrack (0)).getChild (0);
    midimodel::writeSustain (clipNode, { { 0, 4 * midi::ticksPerQuarter } }, nullptr);
    midi::NoteSequence oneNote;
    oneNote.add ({ 69, 1.0f, midi::ticksPerQuarter / 2, midi::ticksPerQuarter / 4 });
    midimodel::writeNotes (clipNode, oneNote, nullptr);
    const auto trim = (juce::int64) (0.1 * sampleRate);
    clipNode.setProperty (ids::start, clip::start (clipNode) + trim, nullptr);
    clipNode.setProperty (ids::offset, clip::offset (clipNode) + trim, nullptr);
    clipNode.setProperty (ids::length, clip::length (clipNode) - trim, nullptr);
    const auto chased = readAudio (bounce ("sustain_chased", 0, (juce::int64) (4.0 * sampleRate)).file);
    expect ("a pedal held from before a trimmed clip's start still holds its notes",
            peakDb (chased, 1.0, 1.5) > -30.0 && peakDb (chased, 2.6, 3.0) < -100.0 && findSessionProblem (*session).isEmpty(),
            fmt (peakDb (chased, 1.0, 1.5)) + " dB while held, " + fmt (peakDb (chased, 2.6, 3.0)) + " dB after");

    // Damaged pedal data is found and repaired.
    auto list = clipNode.getChildWithName (ids::SUSTAIN);
    juce::ValueTree overlapping (ids::PEDAL);
    overlapping.setProperty (ids::start, 100, nullptr);
    overlapping.setProperty (ids::length, 5000, nullptr);
    list.appendChild (overlapping, nullptr);
    const auto problem = findSessionProblem (*session);
    repairSession (*session);
    const auto repaired = midimodel::readSustain (clipNode);
    expect ("overlapping pedal ranges are found and merged by repair",
            problem.isNotEmpty() && findSessionProblem (*session).isEmpty() && repaired.size() == 1 && repaired[0].start == 0
                && repaired[0].length == 5100,
            problem);

    // From the keyboard: the pedal is captured while recording, timed like the notes.
    {
        AudioEngine engine;
        engine.prepare (sampleRate, 512);
        engine.getMasterBus().consoleEnabled.store (false);
        auto graph = std::make_unique<RenderGraph>();
        graph->maxBlockSize = 512;
        graph->sampleRate = sampleRate;
        TrackRender keys;
        keys.state = std::make_shared<TrackRealtimeState> (7);
        keys.state->recordArmed.store (true);
        keys.numChannels = 2;
        keys.inputChannel = -1;
        keys.instrument = std::make_shared<InstrumentState>();
        keys.buffer.setSize (2, 512);
        keys.clipScratch.setSize (2, 512);
        graph->tracks.push_back (std::move (keys));
        engine.publishGraph (std::move (graph));

        auto& transport = engine.getTransport();
        transport.play (Transport::State::recording);
        renderEngine (engine, 1024, 512);
        engine.pushLiveSustain (true, 0.0);
        engine.pushLiveMidi (60, 1.0f, true, 0.0);
        renderEngine (engine, 512, 512);                 // both at 1024
        engine.pushLiveMidi (60, 0.0f, false, 0.0);
        renderEngine (engine, 512, 512);                 // key up at 1536: the pedal holds it
        const auto ringing = renderEngine (engine, 4096, 512);
        engine.pushLiveSustain (false, 0.0);
        renderEngine (engine, 512, 512);                 // pedal up at 6144
        renderEngine (engine, 48000, 512);
        const auto after = renderEngine (engine, 4800, 512);
        transport.stop();

        std::vector<AudioEngine::RecordedMidi> captured;
        AudioEngine::RecordedMidi m;
        while (engine.popRecordedMidi (m))
            captured.push_back (m);
        juce::String detail;
        bool pedalDown = false, pedalUp = false;
        for (const auto& e : captured)
        {
            detail << (e.pitch == midi::sustainPedal ? juce::String ("pedal") : juce::String (e.pitch)) << (e.on ? " on@" : " off@") << e.time << " ";
            pedalDown = pedalDown || (e.pitch == midi::sustainPedal && e.on && e.time == 1024);
            pedalUp = pedalUp || (e.pitch == midi::sustainPedal && ! e.on && e.time == 6144);
        }
        float ringingPeak = 0.0f, afterPeak = 0.0f;
        for (const auto s : ringing) ringingPeak = std::max (ringingPeak, std::abs (s));
        for (const auto s : after) afterPeak = std::max (afterPeak, std::abs (s));
        expect ("from the keyboard the pedal holds a released key, and recording captures it on its samples",
                pedalDown && pedalUp && db (ringingPeak) > -30.0 && afterPeak < 1.0e-6f, detail + "; ringing " + fmt (db (ringingPeak)) + " dB");

        // And the take has the pedal as a range, from where it went down to where it came up.
        std::vector<midi::RecordedEvent> events;
        for (const auto& e : captured)
            events.push_back ({ e.time, e.pitch, e.velocity, e.on });
        const auto take = midi::buildTake (std::move (events), 0, 60000, 0, midi::TempoMap (120.0), sampleRate);
        expect ("the recorded take holds the pedal as one range (1024 to 6144 samples = 40.96 to 245.76 ticks)",
                take.sustain.size() == 1 && take.sustain[0].start == 41 && take.sustain[0].length == 205,
                take.sustain.empty() ? juce::String ("none")
                                     : juce::String (take.sustain[0].start) + " + " + juce::String (take.sustain[0].length));
    }
    clearTracks();
}

//==============================================================================
// Tempo changes and time signatures.

void SelfTest::testTempoAndMeter()
{
    beginGroup ("Tempo map and time signatures");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    clearTracks();

    // 4/4 for bars 1-2, 3/4 for bars 2-3... : 4/4 (bar 1), 3/4 (bars 2-3), 6/8 (from bar 4); 120 BPM, then 90 from bar 3.
    auto r = call ("set_tempo", R"({"bpm":120,"clear_changes":true})");
    call ("set_meter", R"({"numerator":3,"denominator":4,"at_bar":2})");
    call ("set_meter", R"({"numerator":6,"denominator":8,"at_bar":4})");
    r = call ("set_tempo", R"({"bpm":90,"at_bar":3})");
    const auto mapText = juce::JSON::toString (r.data, true);
    expect ("the tools set tempo changes and time signatures, and report them",
            r.ok && mapText.contains (R"("time_signature": "6/8")") && mapText.contains (R"("bpm": 90)"), mapText.substring (0, 200));
    session->getUndoManager().undo();
    const bool undone = midimodel::tempoMap (*session).getChanges().size() == 1;
    session->getUndoManager().redo();
    expect ("a tempo change is one undo step (undo removes it, redo restores it)",
            undone && midimodel::tempoMap (*session).getChanges().size() == 2, {});

    const auto tempo = midimodel::tempoMap (*session);
    const auto meter = midimodel::meterMap (*session);
    // Bar 5 starts at tick 3840 (4/4) + 2 x 2880 (3/4) + 2880 (6/8) = 12480 = 13 quarter notes.
    // Seconds: 120 BPM until bar 3 (tick 6720 = 7 quarters = 3.5 s), then 6 quarters at 90 BPM = 4 s: 7.5 s.
    expect ("bar positions follow the meters, times follow the tempo changes",
            meter.barToTicks (4) == 12480 && tempo.ticksToSamples (12480, sampleRate) == (juce::int64) (7.5 * sampleRate),
            juce::String (meter.barToTicks (4)) + " ticks, " + juce::String (tempo.ticksToSamples (12480, sampleRate)) + " samples");

    // A synth note written on bar 5 sounds at 7.5 s exactly (plus the synth's own latency).
    call ("create_track", R"({"kind":"instrument","name":"Keys"})");
    call ("add_midi_clip", R"({"track":"Keys","start_beat":0,"length_beats":16})");
    call ("add_notes", R"({"track":"Keys","clip":1,"notes":[{"pitch":69,"start_beats":13,"length_beats":1,"velocity":1}]})");
    {
        const auto audio = readAudio (bounce ("tempo_map_note", 0, (juce::int64) (8.0 * sampleRate)).file);
        int onset = -1;
        for (int i = 0; i < audio.getNumSamples() && onset < 0; ++i)
            if (! juce::exactlyEqual (audio.getSample (0, i), 0.0f))
                onset = i;
        const int expected = (int) (7.5 * sampleRate);
        // The synth's 16-sample latency is compensated, and its linear-phase oversampling
        // filter rings ahead of the attack: the first non-zero sample is up to 16 early.
        expect ("a note on bar 5 sounds at exactly 7.500 s (after a 3/4 and a 6/8 bar and a tempo change)",
                onset >= expected - synth::PolySynth::getLatencySamples() && onset <= expected,
                "first sound at " + juce::String (onset) + ", bar 5 at " + juce::String (expected));
    }
    clearTracks();

    // A clip that starts after the tempo change keeps musical time: bar 4 starts at 5.5 s, and a
    // note 3 beats (one 6/8 bar) into the clip is on bar 5, at 7.5 s. Timed with the session's
    // first tempo (120 BPM) instead, it would sound at 7.0 s.
    call ("create_track", R"({"kind":"instrument","name":"Keys"})");
    call ("add_midi_clip", R"({"track":"Keys","start_beat":4,"length_beats":6})");  // bars 2-3 (3/4)
    call ("add_midi_clip", R"({"track":"Keys","start_beat":10,"length_beats":8})"); // bars 4-6 (6/8)
    call ("add_notes", R"({"track":"Keys","clip":2,"notes":[{"pitch":69,"start_beats":3,"length_beats":1,"velocity":1}]})");
    {
        const auto listedNotes = call ("list_notes", R"({"track":"Keys","clip":2})");
        const auto audio = readAudio (bounce ("tempo_map_clip_after_change", 0, (juce::int64) (8.0 * sampleRate)).file);
        int onset = -1;
        for (int i = 0; i < audio.getNumSamples() && onset < 0; ++i)
            if (! juce::exactlyEqual (audio.getSample (0, i), 0.0f))
                onset = i;
        const int expected = (int) (7.5 * sampleRate);
        expect ("a clip after a tempo change plays in musical time (its note on bar 5 sounds at 7.500 s)",
                onset >= expected - synth::PolySynth::getLatencySamples() && onset <= expected
                    && juce::JSON::toString (listedNotes.data).contains (R"("start_beats": 3.0)"),
                "first sound at " + juce::String (onset) + ", bar 5 at " + juce::String (expected) + "; "
                    + juce::JSON::toString (listedNotes.data).substring (0, 120));
    }

    // The MIDI editor uses the meter too: with the 1-bar grid, a click in the second 3/4 bar of
    // the first clip (bars 2-3) adds a note on that bar line, one 3/4 bar long.
    {
        auto keysTrack = session->getTrack (0); // the only track
        auto firstClip = keysTrack.isValid() && track::name (keysTrack) == "Keys" ? track::clips (keysTrack).getChild (0) : juce::ValueTree();
        auto& editView = main.getEditView();
        main.showWindow (WindowKind::edit);
        if (firstClip.isValid())
            editView.openPianoRoll (firstClip);
        if (auto* pr = editView.getPianoRoll(); pr != nullptr && firstClip.isValid())
        {
            pr->setGridTicks (3840);
            const auto p = pr->positionFor (2880 + 400, 67) + juce::Point<float> (2.0f, pr->rowHeight() * 0.5f);
            pr->mouseDown (mouseEvent (*pr, p, p, false, {}));
            pr->mouseUp (mouseEvent (*pr, p, p, false, {}));
            const auto added = clip::notes (firstClip).getChild (0);
            expect ("piano roll: the 1-bar grid snaps to the 3/4 bar line, and a bar is three beats",
                    added.isValid() && (juce::int64) added[ids::start] == 2880 && (juce::int64) added[ids::length] == 2880,
                    added.isValid() ? "start " + added[ids::start].toString() + ", length " + added[ids::length].toString()
                                    : juce::String ("no note"));
            snapshot (*pr, "piano_roll_meter.png");
            editView.closePianoRoll();
        }
        else
            expect ("the MIDI editor opens for the clip", false, {});
    }
    clearTracks();

    // The click through the real graph: every beat on its exact sample, accents on bar starts.
    {
        AudioEngine engine;
        engine.prepare (sampleRate, 512);
        engine.getMasterBus().consoleEnabled.store (false);
        engine.publishGraph (app.getBridge().buildOfflineGraph (sampleRate, 512));
        engine.clickEnabled.store (true);
        engine.getTransport().play();
        const int total = (int) (10.0 * sampleRate);
        const auto x = renderEngine (engine, total, 512);
        const auto onsets = clickOnsets (x);

        std::vector<int> expected;
        std::vector<bool> accents;
        for (auto beat = meter.beatAtOrAfter (0);; beat = meter.beatAtOrAfter (beat.tick + 1))
        {
            const auto s = tempo.ticksToSamples (beat.tick, sampleRate);
            if (s + 2 >= total)
                break;
            expected.push_back ((int) s);
            accents.push_back (beat.barStart);
        }
        bool accentsRight = onsets == expected;
        for (size_t i = 0; accentsRight && i < expected.size(); ++i)
        {
            float peak = 0.0f;
            for (int k = 0; k < 1700; ++k)
                peak = std::max (peak, std::abs (x[(size_t) expected[i] + (size_t) k]));
            accentsRight = (db (peak) > -8.0) == accents[i];
        }
        expect ("the click follows the tempo and meter maps to the sample, accenting each bar's first beat",
                onsets == expected && accentsRight,
                juce::String ((int) onsets.size()) + " clicks, " + juce::String ((int) expected.size()) + " expected");
    }

    // A count-in in 6/8 at 90 BPM: six eighths per bar at 180 eighths per minute.
    app.locate (tempo.ticksToSamples (meter.barToTicks (3), sampleRate));
    for (int waited = 0; app.getEngine().getTransport().getPosition() != tempo.ticksToSamples (meter.barToTicks (3), sampleRate) && waited < 2000; waited += 10)
        juce::Thread::sleep (10);
    const auto [countBpm, countBeats] = app.countInGrid();
    expect ("a count-in in a 6/8 bar at 90 BPM clicks six eighths per bar (180 per minute)",
            countBeats == 6 && std::abs (countBpm - 180.0) < 1.0e-9, juce::String (countBpm) + " / " + juce::String (countBeats));
    app.locate (0);

    // The editing grid snaps to the beats of the maps: in 6/8 at 90 BPM an eighth is 16000 samples.
    {
        EditState grid;
        grid.sampleRate = sampleRate;
        grid.tempoMap = tempo;
        grid.meterMap = meter;
        grid.setMode (EditMode::grid);
        const auto bar4 = tempo.ticksToSamples (meter.barToTicks (3), sampleRate);
        expect ("Grid mode snaps to the beats of the tempo and meter maps",
                grid.snap (bar4 + 7000) == bar4 && grid.snap (bar4 + 9000) == bar4 + 16000, juce::String (grid.snap (bar4 + 9000) - bar4));
    }

    // Damaged maps.
    auto bad = juce::ValueTree (ids::TEMPO);
    bad.setProperty (ids::tick, 960, nullptr);
    bad.setProperty (ids::bpm, std::numeric_limits<double>::quiet_NaN(), nullptr);
    session->getState().getOrCreateChildWithName (ids::TEMPO_MAP, nullptr).appendChild (bad, nullptr);
    auto badMeter = juce::ValueTree (ids::METER);
    badMeter.setProperty (ids::bar, 2, nullptr);
    badMeter.setProperty (ids::numerator, 5, nullptr);
    badMeter.setProperty (ids::denominator, 3, nullptr);
    session->getState().getOrCreateChildWithName (ids::METER_MAP, nullptr).appendChild (badMeter, nullptr);
    const auto problem = findSessionProblem (*session);
    repairSession (*session);
    expect ("an invalid tempo or time signature is found and removed by repair",
            problem.isNotEmpty() && findSessionProblem (*session).isEmpty(), problem);
    call ("set_tempo", R"({"bpm":120,"clear_changes":true})");
}

//==============================================================================
// Punch recording: takes written only between punch-in and punch-out.

void SelfTest::testPunchRecording()
{
    beginGroup ("Punch recording");
    juce::TimeSliceThread writerThread ("punch test writer");
    writerThread.startThread();
    const auto takeFile = output.getChildFile ("punch-take.wav");

    {
        AudioEngine engine;
        engine.prepare (sampleRate, 500);
        engine.getMasterBus().consoleEnabled.store (false);

        auto take = std::make_shared<RecordTake>();
        juce::String error;
        auto writer = createSessionWriter (takeFile, sampleRate, 1, 24, false, error);
        take->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 48000);
        take->trackId = 1;
        take->numChannels = 1;
        take->inputChannel = 0;

        auto graph = std::make_unique<RenderGraph>();
        graph->maxBlockSize = 500;
        graph->sampleRate = sampleRate;
        TrackRender audio;
        audio.state = std::make_shared<TrackRealtimeState> (1);
        audio.state->recordArmed.store (true);
        audio.numChannels = 1;
        audio.inputChannel = 0;
        audio.take = take;
        audio.buffer.setSize (1, 500);
        audio.clipScratch.setSize (2, 500);
        graph->tracks.push_back (std::move (audio));
        TrackRender keys;
        keys.state = std::make_shared<TrackRealtimeState> (2);
        keys.state->recordArmed.store (true);
        keys.numChannels = 2;
        keys.inputChannel = -1;
        keys.instrument = std::make_shared<InstrumentState>();
        keys.buffer.setSize (2, 500);
        keys.clipScratch.setSize (2, 500);
        graph->tracks.push_back (std::move (keys));
        engine.publishGraph (std::move (graph));

        // Punch 0.5 s .. 1.0 s; record from 0 (a half-second pre-roll), in blocks that do not
        // line up with the punch points; a key before, inside and after the range.
        auto& transport = engine.getTransport();
        transport.setPunch (true, 24000, 48000);
        transport.play (Transport::State::recording);
        std::vector<float> l (500), r (500);
        float* outs[2] = { l.data(), r.data() };
        for (int done = 0; done < 72000; done += 500)
        {
            if (done == 10000 || done == 30000 || done == 60000)
                engine.pushLiveMidi (60 + done / 10000, 0.8f, true, 0.0);
            engine.renderOffline (outs, 2, 500);
        }
        transport.stop();

        expect ("punch: the take starts exactly at punch-in and holds exactly the punch range (not the pre/post-roll)",
                take->startSample.load() == 24000 && take->samplesWritten.load() == 24000,
                "start " + juce::String (take->startSample.load()) + ", " + juce::String (take->samplesWritten.load()) + " samples");
        std::vector<int> captured;
        AudioEngine::RecordedMidi m;
        while (engine.popRecordedMidi (m))
            captured.push_back ((int) m.time);
        expect ("punch: MIDI is captured only inside the range (the key at 0.625 s, not 0.21 s or 1.25 s)",
                captured == std::vector<int> { 30000 }, juce::String ((int) captured.size()) + " captured");
        take->writer.reset();
    }
    writerThread.stopThread (2000);
    takeFile.deleteFile();

    auto& tools = app.getRemoteTools();
    auto r = tools.call ("set_punch", juce::JSON::parse (R"({"enabled":true,"start_seconds":2,"end_seconds":3})"));
    const auto& transport = app.getEngine().getTransport();
    expect ("set_punch sets the engine's punch range; an empty range is refused",
            r.ok && transport.isPunchEnabled() && transport.getPunchIn() == 96000 && transport.getPunchOut() == 144000
                && ! tools.call ("set_punch", juce::JSON::parse (R"({"enabled":true,"start_seconds":3,"end_seconds":2})")).ok,
            r.error);
    tools.call ("set_punch", juce::JSON::parse (R"({"enabled":false})"));
}

//==============================================================================
// Track duplicate and reorder.

void SelfTest::testTrackEditing()
{
    beginGroup ("Tracks: duplicate and reorder");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;

    clearTracks();
    auto vocal = addToneTrack ("Vocal", 1, tone, clipStart);
    juce::PluginDescription eq;
    for (const auto& d : BuiltinEffectFormat::getDescriptions())
        if (d.name == "Meridian EQ")
            eq = d;
    juce::String error;
    app.getBridge().insertPlugin (vocal, 0, eq, error);
    call ("set_plugin_parameter", R"({"track":"Vocal","slot":"A","parameter":"p2Gain","value":6})");
    const double single = db (bounce ("dup_single", 0, end).left.peak);

    auto copy = app.duplicateTrack (vocal);
    const auto copyPlugin = app.getBridge().getInsert (track::id (copy), 0);
    juce::String copiedGain;
    if (copyPlugin != nullptr && copyPlugin->plugin != nullptr)
        for (auto* p : copyPlugin->plugin->getParameters())
            if (p->getName (64) == "Band 2 Gain")
                copiedGain = p->getCurrentValueAsText();
    expect ("Duplicate makes Vocal.dup1 right after the original, with its own plug-in at the same setting",
            track::name (copy) == "Vocal.dup1" && session->indexOfTrack (copy) == session->indexOfTrack (vocal) + 1
                && copiedGain == "6.00 dB" && track::id (copy) != track::id (vocal),
            track::name (copy) + ", EQ " + copiedGain);
    const auto clipIdsDiffer = clip::id (track::clips (copy).getChild (0)) != clip::id (track::clips (vocal).getChild (0));
    expect ("the copy has fresh track and clip ids, and the session stays valid",
            clipIdsDiffer && findSessionProblem (*session).isEmpty(), findSessionProblem (*session));
    expectNear ("the copy plays the same audio: the two sum to exactly +6.02 dB", db (bounce ("dup_pair", 0, end).left.peak),
                single + 6.0206, 0.02, "dB");

    auto second = call ("duplicate_track", R"({"track":"Vocal.dup1"})");
    expect ("duplicating a copy numbers on from the original's name (Vocal.dup2)",
            second.ok && session->getTrack (2).isValid() && track::name (session->getTrack (2)) == "Vocal.dup2", second.error);

    // Reordering changes nothing in the mix.
    const auto before = readAudio (bounce ("order_before", 0, end).file);
    app.moveTrack (vocal, 2);
    const bool moved = session->indexOfTrack (vocal) == 2;
    const auto after = readAudio (bounce ("order_after", 0, end).file);
    double difference = 0.0;
    for (int ch = 0; ch < 2 && before.getNumSamples() == after.getNumSamples(); ++ch)
        for (int i = 0; i < before.getNumSamples(); ++i)
            difference = std::max (difference, (double) std::abs (before.getSample (ch, i) - after.getSample (ch, i)));
    expect ("Move: the track moves, and the mix is unchanged to the bit", moved && before.getNumSamples() == after.getNumSamples()
                && juce::exactlyEqual (difference, 0.0), "largest difference " + juce::String (difference));
    session->getUndoManager().undo();
    expect ("undo puts it back", session->indexOfTrack (vocal) == 0, juce::String (session->indexOfTrack (vocal)));

    auto r = call ("move_track", R"({"track":"Vocal","position":3})");
    expect ("move_track moves by number (1 = first) and refuses positions that do not exist",
            r.ok && session->indexOfTrack (vocal) == 2 && ! call ("move_track", R"({"track":"Vocal","position":9})").ok, r.error);
    clearTracks();
}

//==============================================================================
// Playlists: alternate takes on one track, and comping one performance from
// parts of several. Take 1 is a 1 kHz tone and take 2 a 500 Hz tone, so every
// stretch of a render shows which take it came from.

void SelfTest::testPlaylists()
{
    beginGroup ("Playlists and comping");
    auto& tools = app.getRemoteTools();
    auto call = [&tools] (const juce::String& tool, const juce::String& arguments) { return tools.call (tool, juce::JSON::parse (arguments)); };
    auto* session = app.getSession();
    auto& um = session->getUndoManager();
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const double expectedDb = toneDb + panCentreDb; // a mono test tone through a centred track

    // Bit-exact comparisons need a memoryless master: the console's filters would carry
    // take 1's audio into the comped range. Restored at the end.
    auto state = session->getState();
    const juce::var consoleBefore[] = { state[ids::consoleEnabled], state[ids::consoleDrive], state[ids::consoleCharacter], state[ids::consoleCurve] };
    setConsole (false);

    // Which take a stretch of the render holds: its 1 kHz and 500 Hz levels in dB.
    // 12000 samples is a whole number of cycles of both, so neither leaks into the other.
    auto takeLevels = [this] (const juce::AudioBuffer<float>& audio, juce::int64 from)
    {
        const auto* p = audio.getReadPointer (0, (int) from);
        return std::pair { db (dsp::toneAmplitude (p, 12000, 1000.0, sampleRate)), db (dsp::toneAmplitude (p, 12000, 500.0, sampleRate)) };
    };
    auto largestDifference = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, juce::int64 from, juce::int64 to)
    {
        if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels())
            return 1.0;
        double worst = 0.0;
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (auto i = (int) from; i < (int) to; ++i)
                worst = std::max (worst, (double) std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        return worst;
    };

    clearTracks();
    auto t = addToneTrack ("Vocal", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    um.clearUndoHistory();
    expect ("a track starts with one playlist, named after the track (Vocal.01)",
            playlists::names (t) == juce::StringArray { "Vocal.01" } && playlists::activeName (t) == "Vocal.01",
            playlists::names (t).joinIntoString (", "));

    const auto take1 = readAudio (bounce ("playlist_take1", 0, end).file);

    // New: an empty playlist becomes active; take 1 is kept, and silent.
    auto r = call ("playlist", R"({"track":"Vocal","action":"new"})");
    const auto empty = bounce ("playlist_new", 0, end);
    expect ("New Playlist: Vocal.02 becomes active and empty, so the track is silent; Vocal.01 is kept",
            r.ok && playlists::activeName (t) == "Vocal.02" && track::clips (t).getNumChildren() == 0
                && playlists::find (t, "Vocal.01").isValid() && db (empty.left.peak) < silentDb,
            r.error + playlists::names (t).joinIntoString (", ") + ", peak " + fmt (db (empty.left.peak)) + " dB");

    // Take 2, recorded onto the new playlist.
    const auto tone500 = media.getChildFile ("tone500_mono.wav");
    std::unique_ptr<juce::AudioFormatReader> reader (app.getFormatManager().createReaderFor (tone500));
    const auto takeLength = reader != nullptr ? reader->lengthInSamples : 0;
    session->addClip (t, tone500, clipStart, 0, takeLength, takeLength, "Take 2", nullptr);
    const auto take2 = readAudio (bounce ("playlist_take2", 0, end).file);
    const auto [take2At1k, take2At500] = takeLevels (take2, clipStart + 12000);
    expect ("the new playlist plays only its own take (500 Hz at the expected level, no 1 kHz)",
            std::abs (take2At500 - expectedDb) < 0.05 && take2At1k < -100.0,
            "500 Hz " + fmt (take2At500) + " dB, 1 kHz " + fmt (take2At1k) + " dB");

    // Switch back: take 1 plays again, to the bit; take 2 waits in Vocal.02.
    r = call ("playlist", R"({"track":"Vocal","action":"switch","name":"Vocal.01"})");
    const auto switched = readAudio (bounce ("playlist_switch", 0, end).file);
    const auto stored = playlists::find (t, "Vocal.02").getChildWithName (ids::CLIPS);
    expect ("Switch: Vocal.01 plays take 1 again, identical to the bit; Vocal.02 keeps take 2",
            r.ok && playlists::activeName (t) == "Vocal.01" && juce::exactlyEqual (largestDifference (switched, take1, 0, end), 0.0)
                && stored.getNumChildren() == 1,
            r.error + "largest difference " + juce::String (largestDifference (switched, take1, 0, end)));

    // Comp 1.0 s to 1.5 s of take 2 onto take 1 (0.5 s into each take).
    const juce::int64 compStart = 48000, compEnd = 72000;
    r = call ("playlist", R"({"track":"Vocal","action":"comp","name":"Vocal.02","start_seconds":1.0,"end_seconds":1.5})");
    const auto comped = readAudio (bounce ("playlist_comp", 0, end).file);
    const double inside = largestDifference (comped, take2, compStart, compEnd);
    const double before = largestDifference (comped, take1, 0, compStart);
    const double after = largestDifference (comped, take1, compEnd, end);
    expect ("Comp: the range plays take 2 and the rest take 1, each identical to the bit, switching on the exact samples",
            r.ok && track::clips (t).getNumChildren() == 3 && juce::exactlyEqual (inside, 0.0)
                && juce::exactlyEqual (before, 0.0) && juce::exactlyEqual (after, 0.0),
            r.error + juce::String (track::clips (t).getNumChildren()) + " clips; differences " + juce::String (before) + " / "
                + juce::String (inside) + " / " + juce::String (after));
    const auto source = stored.getChild (0);
    expect ("comping copies and never moves: Vocal.02 keeps its whole take, and the session stays valid",
            stored.getNumChildren() == 1 && clip::start (source) == clipStart && clip::length (source) == takeLength
                && findSessionProblem (*session).isEmpty(),
            findSessionProblem (*session));

    um.undo();
    const auto undone = readAudio (bounce ("playlist_comp_undo", 0, end).file);
    expect ("undo takes the whole comp back in one step: take 1 plays unchanged",
            track::clips (t).getNumChildren() == 1 && juce::exactlyEqual (largestDifference (undone, take1, 0, end), 0.0),
            juce::String (track::clips (t).getNumChildren()) + " clips");
    um.redo();

    // Duplicate, rename and delete.
    r = call ("playlist", R"({"track":"Vocal","action":"duplicate"})");
    juce::Array<int> copyIds;
    for (const auto& c : track::clips (t))
        copyIds.add (clip::id (c));
    bool freshIds = copyIds.size() == 3;
    for (const auto& c : playlists::find (t, "Vocal.01").getChildWithName (ids::CLIPS))
        freshIds = freshIds && ! copyIds.contains (clip::id (c));
    const auto duplicated = readAudio (bounce ("playlist_duplicate", 0, end).file);
    expect ("Duplicate: Vocal.03 is a copy of the comp with fresh clip ids, and plays it to the bit",
            r.ok && playlists::activeName (t) == "Vocal.03" && freshIds
                && juce::exactlyEqual (largestDifference (duplicated, comped, 0, end), 0.0),
            r.error + playlists::names (t).joinIntoString (", "));

    r = call ("playlist", R"({"track":"Vocal","action":"rename","name":"Vocal.03","new_name":"Vocal Comp"})");
    const auto taken = call ("playlist", R"({"track":"Vocal","action":"rename","name":"Vocal Comp","new_name":"Vocal.01"})");
    expect ("Rename: the active playlist becomes \"Vocal Comp\"; a name already in use is refused",
            r.ok && playlists::activeName (t) == "Vocal Comp" && ! taken.ok, r.error + " / " + taken.error);

    const auto activeRefused = call ("playlist", R"({"track":"Vocal","action":"delete","name":"Vocal Comp"})");
    r = call ("playlist", R"({"track":"Vocal","action":"delete","name":"Vocal.02"})");
    expect ("Delete: an alternate is removed; the active playlist is refused",
            r.ok && ! activeRefused.ok && playlists::names (t) == juce::StringArray { "Vocal Comp", "Vocal.01" },
            r.error + " / " + playlists::names (t).joinIntoString (", "));
    const auto busRefused = call ("create_track", R"({"kind":"aux","name":"Bus"})").ok
                                && ! call ("playlist", R"({"track":"Bus","action":"new"})").ok;
    expect ("aux tracks have no clips, so the tool refuses playlists on them", busRefused, {});

    // Every playlist survives a save and reload, with its clips and their audio files.
    const auto saved = app.saveSession();
    juce::String error;
    const auto loaded = saved.wasOk() ? Session::load (session->getSessionFile(), error) : nullptr;
    if (loaded != nullptr)
    {
        const auto lt = loaded->getTrack (0);
        // Vocal.01 was stored holding the comp: three clips from two files.
        const auto alternate = playlists::find (lt, "Vocal.01").getChildWithName (ids::CLIPS);
        bool filesFound = alternate.getNumChildren() == 3;
        for (const auto& c : alternate)
            filesFound = filesFound && loaded->resolveStoredPath (c[ids::file].toString()).existsAsFile();
        expect ("save and reload keep every playlist, the active one, and the alternates' clips and files",
                playlists::activeName (lt) == "Vocal Comp" && playlists::names (lt) == playlists::names (t)
                    && track::clips (lt).getNumChildren() == 3 && filesFound && findSessionProblem (*loaded).isEmpty(),
                playlists::names (lt).joinIntoString (", ") + "; " + findSessionProblem (*loaded));
    }
    else
    {
        expect ("save and reload keep every playlist, the active one, and the alternates' clips and files", false,
                saved.getErrorMessage() + error);
    }

    clearTracks();
    state.setProperty (ids::consoleEnabled, consoleBefore[0], nullptr);
    state.setProperty (ids::consoleDrive, consoleBefore[1], nullptr);
    state.setProperty (ids::consoleCharacter, consoleBefore[2], nullptr);
    state.setProperty (ids::consoleCurve, consoleBefore[3], nullptr);
}

//==============================================================================
// Loudness (EBU R128) of bounces, normalisation, dither and FLAC.

void SelfTest::testLoudnessAndExport()
{
    beginGroup ("Loudness and export (EBU R128, normalising, dither, FLAC)");
    clearTracks();
    addToneTrack ("Stereo Tone", 2, media.getChildFile ("tone1k_stereo.wav"), clipStart); // -6.02 dBFS per channel
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;

    auto measureFile = [this] (const juce::File& f)
    {
        const auto audio = readAudio (f);
        dsp::LoudnessMeter m;
        m.prepare (sampleRate, 2);
        dsp::TruePeakMeter tp;
        tp.prepare (2);
        const float* ch[2] = { audio.getReadPointer (0), audio.getReadPointer (audio.getNumChannels() > 1 ? 1 : 0) };
        m.process (ch, 2, audio.getNumSamples());
        tp.process (ch, 2, audio.getNumSamples());
        return std::pair { m.getIntegrated(), tp.getTruePeakDb() };
    };

    // A stereo 1 kHz sine at -6.02 dBFS is -6.02 LUFS (BS.1770's 1 kHz calibration), but
    // this one lasts 2 s, from 0.5 s to 2.5 s of a 2.6 s file, so some 400 ms gating blocks
    // (one every 100 ms) are partly silent: 3 blocks lead in (1/4, 2/4, 3/4 tone), 17 are
    // all tone, 1 leads out (3/4). All pass the gates, so the integrated loudness is
    // -6.02 + 10 log10 ((0.25 + 0.5 + 0.75 + 17 + 0.75) / 21) = -6.40 LUFS.
    const auto plain = bounce ("loudness_plain", 0, end);
    const auto [plainLufs, plainTp] = measureFile (plain.file);
    const double expectedLufs = toneDb + 10.0 * std::log10 ((0.25 + 0.5 + 0.75 + 17.0 + 0.75) / 21.0);
    expectNear ("a bounce reports its integrated loudness (BS.1770 gating of a 2 s, -6.02 dBFS stereo tone)",
                plain.integratedLufs, expectedLufs, 0.02, "LUFS");
    expectNear ("and its true peak (the tone's peak)", plain.truePeakDbtp, toneDb, 0.02, "dBTP");
    expect ("the reported loudness and true peak match the file measured independently",
            std::abs (plainLufs - plain.integratedLufs) < 0.01 && std::abs (plainTp - plain.truePeakDbtp) < 0.01,
            fmt (plainLufs) + " LUFS / " + fmt (plainTp) + " dBTP");

    auto request = app.makeSessionBounceRequest();
    request.start = 0;
    request.end = end;
    request.bitDepth = 32;
    request.format = "wav";
    request.dither = false;
    request.normalise = true;
    request.targetLufs = -14.0;
    request.ceilingDbtp = -1.0;
    request.destination = output.getChildFile ("bounces").getChildFile ("loudness_minus14.wav");
    auto normalised = app.bounceNow (request);
    const auto [normLufs, normTp] = measureFile (normalised.file);
    expect ("normalising to -14 LUFS: the file measures -14.0 LUFS; the first pass is cleaned up",
            normalised.ok && std::abs (normLufs + 14.0) < 0.05 && ! normalised.limitedByCeiling
                && std::abs (normalised.appliedGainDb - (-14.0 - plain.integratedLufs)) < 0.01
                && ! request.destination.getSiblingFile ("loudness_minus14.unnormalised.wav").exists(),
            normalised.error + fmt (normLufs) + " LUFS, gain " + fmt (normalised.appliedGainDb) + " dB");

    request.targetLufs = 0.0; // +6 dB would put the true peak at 0 dBTP, past the -1 dBTP ceiling
    request.destination = output.getChildFile ("bounces").getChildFile ("loudness_ceiling.wav");
    auto ceiling = app.bounceNow (request);
    const auto [ceilLufs, ceilTp] = measureFile (ceiling.file);
    expect ("a target that would pass the true-peak ceiling stops at the ceiling, and says so",
            ceiling.ok && ceiling.limitedByCeiling && std::abs (ceilTp + 1.0) < 0.05 && ceilLufs < -0.9,
            fmt (ceilLufs) + " LUFS, " + fmt (ceilTp) + " dBTP");

    // 16-bit with dither: every sample on the 16-bit grid, error power LSB^2/4.
    request.normalise = false;
    request.bitDepth = 16;
    request.dither = true;
    request.destination = output.getChildFile ("bounces").getChildFile ("dither16.wav");
    const auto dithered = app.bounceNow (request);
    const auto exact = readAudio (plain.file), sixteen = readAudio (dithered.file);
    double errorPower = 0.0;
    bool onGrid = sixteen.getNumSamples() == exact.getNumSamples();
    for (int i = 0; onGrid && i < exact.getNumSamples(); ++i)
    {
        const double y = sixteen.getSample (0, i);
        onGrid = std::abs (y * 32768.0 - std::round (y * 32768.0)) < 1.0e-6;
        errorPower += std::pow (y - exact.getSample (0, i), 2.0);
    }
    const double lsb = 1.0 / 32768.0;
    const double errorRatio = onGrid ? std::sqrt (errorPower / exact.getNumSamples()) / (lsb / 2.0) : 0.0;
    expect ("16-bit bounce with TPDF dither: on the 16-bit grid, error exactly LSB/2 RMS (the dither's design)",
            dithered.ok && onGrid && std::abs (errorRatio - 1.0) < 0.03, "error " + fmt (errorRatio, 3) + " x LSB/2");

    // The same with noise shaping: the error is the dither's, filtered, so its power
    // is LSB^2/4 times the filter's sum f_k^2 (the spectrum is checked in the unit tests).
    request.noiseShaping = true;
    request.destination = output.getChildFile ("bounces").getChildFile ("dither16_shaped.wav");
    const auto shaped = app.bounceNow (request);
    const auto shapedAudio = readAudio (shaped.file);
    double shapedPower = 0.0;
    bool shapedOnGrid = shapedAudio.getNumSamples() == exact.getNumSamples();
    for (int i = 0; shapedOnGrid && i < exact.getNumSamples(); ++i)
    {
        const double y = shapedAudio.getSample (0, i);
        shapedOnGrid = std::abs (y * 32768.0 - std::round (y * 32768.0)) < 1.0e-6;
        shapedPower += std::pow (y - exact.getSample (0, i), 2.0);
    }
    const double shapedRatio = shapedOnGrid ? std::sqrt (shapedPower / exact.getNumSamples()) / (lsb / 2.0) : 0.0;
    expect ("16-bit bounce with noise-shaped dither: on the grid, error exactly sqrt(4.61) x LSB/2 RMS (the shaping filter's gain)",
            shaped.ok && shapedOnGrid && std::abs (shapedRatio / std::sqrt (dsp::Dither::shapedNoisePowerGain) - 1.0) < 0.03,
            "error " + fmt (shapedRatio, 3) + " x LSB/2, expected " + fmt (std::sqrt (dsp::Dither::shapedNoisePowerGain), 3));
    request.noiseShaping = false;

    // FLAC, 24-bit: lossless, so it reads back as the mix within 24-bit rounding.
    request.bitDepth = 24;
    request.dither = false;
    request.format = "flac";
    request.destination = output.getChildFile ("bounces").getChildFile ("export.flac");
    const auto flac = app.bounceNow (request);
    const auto flacAudio = readAudio (flac.file);
    double worst = 0.0;
    for (int ch = 0; ch < 2 && flacAudio.getNumSamples() == exact.getNumSamples(); ++ch)
        for (int i = 0; i < exact.getNumSamples(); ++i)
            worst = std::max (worst, std::abs ((double) flacAudio.getSample (ch, i) - exact.getSample (ch, i)));
    expect ("FLAC 24-bit export reads back as the mix, within half a 24-bit step (rounded, not truncated)",
            flac.ok && flacAudio.getNumSamples() == exact.getNumSamples() && worst <= 0.5 / 8388608.0 + 1.0e-9,
            flac.error + " worst " + juce::String (worst * 8388608.0, 3) + " steps");

    // The live master meter: the engine's 100 ms blocks must give exactly what an
    // offline measurement of the same output gives. (A bare engine whose only
    // output is the click at 120 BPM, -6 dB: a known, repeatable signal.)
    {
        AudioEngine engine;
        engine.prepare (sampleRate, 480);
        engine.getMasterBus().consoleEnabled.store (false);
        engine.clickEnabled.store (true);
        engine.getTransport().play();
        const int total = (int) (6.0 * sampleRate);
        std::vector<float> l ((size_t) total), r ((size_t) total);
        for (int done = 0; done < total; done += 480)
        {
            float* outs[2] = { l.data() + done, r.data() + done };
            engine.renderOffline (outs, 2, std::min (480, total - done));
        }
        dsp::LoudnessMeter live, offline;
        live.prepare (sampleRate, 2);
        offline.prepare (sampleRate, 2);
        AudioEngine::LoudnessBlock block;
        double livePeak = 0.0;
        while (engine.popLoudnessBlock (block))
        {
            live.addSubBlock (block.meanSquare);
            livePeak = std::max (livePeak, (double) block.truePeak);
        }
        // The engine meters the mix before the click is added: it measures silence here.
        expect ("the live master meter excludes the click (a monitoring aid, not part of the mix)",
                live.getIntegrated() <= dsp::LoudnessMeter::absoluteGate && livePeak < 1.0e-9,
                fmt (live.getIntegrated()) + " LUFS");
        juce::ignoreUnused (offline);
    }
    {
        // A held synth note through the mix: live blocks vs. the output measured offline.
        AudioEngine engine;
        engine.prepare (sampleRate, 480);
        engine.getMasterBus().consoleEnabled.store (false);
        auto state = std::make_shared<TrackRealtimeState> (1);
        state->recordArmed.store (true);
        auto graph = std::make_unique<RenderGraph>();
        graph->maxBlockSize = 480;
        graph->sampleRate = sampleRate;
        TrackRender t;
        t.state = state;
        t.numChannels = 2;
        t.inputChannel = -1;
        t.instrument = std::make_shared<InstrumentState>();
        t.buffer.setSize (2, 480);
        t.clipScratch.setSize (2, 480);
        graph->tracks.push_back (std::move (t));
        engine.publishGraph (std::move (graph));
        engine.pushLiveMidi (57, 0.9f, true, 0.0);

        const int total = (int) (6.0 * sampleRate);
        std::vector<float> l ((size_t) total), r ((size_t) total);
        for (int done = 0; done < total; done += 480)
        {
            float* outs[2] = { l.data() + done, r.data() + done };
            engine.renderOffline (outs, 2, std::min (480, total - done));
        }
        dsp::LoudnessMeter live, offline;
        live.prepare (sampleRate, 2);
        offline.prepare (sampleRate, 2);
        dsp::TruePeakMeter offlinePeak;
        offlinePeak.prepare (2);
        const float* channels[2] = { l.data(), r.data() };
        offline.process (channels, 2, total);
        offlinePeak.process (channels, 2, total);
        AudioEngine::LoudnessBlock block;
        double livePeak = 0.0;
        while (engine.popLoudnessBlock (block))
        {
            live.addSubBlock (block.meanSquare);
            livePeak = std::max (livePeak, (double) block.truePeak);
        }
        expect ("the live master loudness meter reads what an offline measurement of the same output reads",
                offline.getIntegrated() > -40.0 && std::abs (live.getIntegrated() - offline.getIntegrated()) < 0.01
                    && std::abs (live.getShortTerm() - offline.getShortTerm()) < 0.01
                    && std::abs (20.0 * std::log10 (std::max (livePeak, 1.0e-12)) - offlinePeak.getTruePeakDb()) < 0.01,
                "live " + fmt (live.getIntegrated()) + " / offline " + fmt (offline.getIntegrated()) + " LUFS; TP "
                    + fmt (20.0 * std::log10 (std::max (livePeak, 1.0e-12))) + " / " + fmt (offlinePeak.getTruePeakDb()) + " dBTP");
    }

    auto& tools = app.getRemoteTools();
    const auto refused = tools.call ("bounce", juce::JSON::parse (R"({"format":"flac","bit_depth":32})"));
    expect ("the bounce tool refuses 32-bit FLAC with a clear message", ! refused.ok && refused.error.contains ("FLAC"), refused.error);
    clearTracks();
}

//==============================================================================
// Stress: a large session must still sum exactly and render faster than real time.

void SelfTest::testStress()
{
    beginGroup ("Stress (128 tracks, 8 buses)");
    auto* session = app.getSession();
    clearTracks();
    setConsole (false);

    constexpr int numTracks = 128, numBuses = 8;
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const double perTrackDb = -20.0 * std::log10 ((double) numTracks); // 128 coherent copies sum back to one

    juce::Array<int> buses;
    for (int b = 0; b < numBuses; ++b)
        buses.add (track::id (session->addTrack ("Bus " + juce::String (b + 1), 2, nullptr, Session::TrackKind::aux)));

    for (int i = 0; i < numTracks; ++i)
    {
        auto t = addToneTrack ("T" + juce::String (i + 1), 1, tone, clipStart);
        t.setProperty (ids::volumeDb, perTrackDb, nullptr);
        // Every track also feeds two buses through silent sends: the routing,
        // bus and delay machinery all run at full size without changing the sum.
        for (int s = 0; s < 2; ++s)
        {
            routing::setSend (*session, t, s, buses[(i + s) % numBuses], nullptr);
            routing::findSend (t, s).setProperty (ids::levelDb, dsp::FaderTaper::minDb, nullptr);
        }
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto r = bounce ("stress_128", 0, end);
    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    const auto audioSeconds = (double) end / sampleRate;

    expectNear ("128 tracks at -42.14 dB sum exactly back to one track's level", db (r.left.peak), toneDb + panCentreDb, 0.02, "dB");
    const auto speed = fmt (audioSeconds, 2) + " s of audio in " + fmt (seconds, 2) + " s ("
                       + fmt (audioSeconds / juce::jmax (seconds, 1.0e-6), 1) + "x real time)";
   #if JUCE_DEBUG || defined (MERIDIAN_SANITIZERS)
    skip ("renders faster than real time (" + juce::String (numTracks + numBuses) + " channels)",
          "not judged in Debug or sanitizer builds (several times slower by design): " + speed);
   #else
    expect ("renders faster than real time (" + juce::String (numTracks + numBuses) + " channels)", seconds < audioSeconds, speed);
   #endif
    expect ("session stays valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));
    clearTracks();
}

//==============================================================================
// Robustness: damaged files and hostile remote input must never crash the
// application or leave the session in an invalid state.

namespace
{
/** juce::Random with a fixed seed is deterministic across runs and platforms. */
juce::var junkValue (juce::Random& rng, const juce::Array<int>& someIds)
{
    static const char* const pool[] = { "-1", "0", "1", "2", "5", "0.5", "nan", "inf", "-inf", "1e308", "-1e308", "1e999",
                                        "abc", "", ".", "..", "999999999999", "-999999999999", "aux", "audio", "read",
                                        "instrument", "midi", "sine", "saw", "128", "60.5", "drums", "synth" };
    if (rng.nextInt (5) == 0 && ! someIds.isEmpty())
        return someIds[rng.nextInt (someIds.size())];
    return juce::String (pool[rng.nextInt ((int) std::size (pool))]);
}

const juce::Identifier* randomSchemaProperty (juce::Random& rng)
{
    static const juce::Identifier* const properties[] = {
        &ids::name, &ids::sampleRate, &ids::bitDepth, &ids::fileFormat, &ids::nextId, &ids::masterVolumeDb,
        &ids::consoleEnabled, &ids::consoleDrive, &ids::consoleCharacter, &ids::consoleCurve, &ids::id, &ids::channels,
        &ids::colour, &ids::volumeDb, &ids::pan, &ids::mute, &ids::solo, &ids::recordArm, &ids::inputChannel,
        &ids::automationMode, &ids::kind, &ids::outputBus, &ids::target, &ids::levelDb, &ids::preFader, &ids::file,
        &ids::start, &ids::length, &ids::offset, &ids::sourceLength, &ids::gainDb, &ids::fadeIn, &ids::fadeOut,
        &ids::slot, &ids::description, &ids::bypass, &ids::parameter, &ids::time, &ids::value,
        &ids::tempo, &ids::pitch, &ids::velocity, &ids::waveform, &ids::detune, &ids::attack, &ids::decay,
        &ids::sustain, &ids::release, &ids::cutoff, &ids::resonance, &ids::filterEnv,
        &ids::engine, &ids::tune, &ids::decayScale, &ids::tone
    };
    return properties[rng.nextInt ((int) std::size (properties))];
}

void collectNodes (const juce::ValueTree& node, juce::Array<juce::ValueTree>& nodes)
{
    nodes.add (node);
    for (const auto& child : node)
        collectNodes (child, nodes);
}

/** 1-5 random edits: junk property values, deleted, duplicated and misplaced elements. */
void mutateSession (juce::ValueTree root, juce::Random& rng)
{
    juce::Array<int> someIds;
    juce::Array<juce::ValueTree> nodes;
    collectNodes (root, nodes);
    for (const auto& n : nodes)
        if (n.hasProperty (ids::id))
            someIds.add (n[ids::id]);

    for (int edit = rng.nextInt (5) + 1; --edit >= 0;)
    {
        nodes.clearQuick();
        collectNodes (root, nodes);
        auto node = nodes[rng.nextInt (nodes.size())];
        switch (rng.nextInt (6))
        {
            case 0: case 1: case 2:
                node.setProperty (*randomSchemaProperty (rng), junkValue (rng, someIds), nullptr);
                break;
            case 3:
                if (node.getNumChildren() > 0)
                    node.removeChild (rng.nextInt (node.getNumChildren()), nullptr);
                break;
            case 4:
                if (node != root)
                    node.getParent().appendChild (node.createCopy(), nullptr); // duplicate ids, slots, lists
                break;
            default:
            {
                auto destination = nodes[rng.nextInt (nodes.size())];
                if (node != root && ! node.isAChildOf (destination) && destination != node && ! destination.isAChildOf (node))
                {
                    node.getParent().removeChild (node, nullptr);
                    destination.appendChild (node, nullptr); // e.g. a TRACK inside CLIPS
                }
                break;
            }
        }
    }
}

bool allFinite (const juce::AudioBuffer<float>& audio)
{
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            if (! std::isfinite (audio.getSample (ch, i)))
                return false;
    return true;
}
} // namespace

void SelfTest::testRobustness()
{
    beginGroup ("Robustness (damaged files, hostile input)");
    const auto saved = app.saveSession();
    expect ("save the test session before the robustness tests", saved.wasOk(), saved.getErrorMessage());
    const auto original = app.getSession()->getSessionFile();

    testDamagedSessions (original);
    testRemoteFuzz();
    testSessionSafety();

    beginGroup ("Robustness (damaged files, hostile input)");
    const auto reopened = app.openSession (original);
    expect ("the original session reopens afterwards", reopened.wasOk(), reopened.getErrorMessage());
    setConsole (false);
}

void SelfTest::testDamagedSessions (const juce::File& originalSession)
{
    const auto folder = output.getChildFile ("damaged");
    folder.createDirectory();
    auto* before = app.getSession();

    // 1. Not a session at all, and a session cut off half way (a crash mid-copy).
    {
        juce::Random rng (1234);
        juce::MemoryBlock junk (4096);
        for (size_t i = 0; i < junk.getSize(); ++i)
            junk[i] = (char) rng.nextInt (256);
        const auto garbage = folder.getChildFile ("garbage.meridian");
        garbage.replaceWithData (junk.getData(), junk.getSize());
        const auto result = app.openSession (garbage);
        expect ("random bytes: refused with an error, current session kept", result.failed() && app.getSession() == before,
                result.getErrorMessage());

        const auto text = originalSession.loadFileAsString();
        const auto truncated = folder.getChildFile ("truncated.meridian");
        truncated.replaceWithText (text.substring (0, text.length() / 2));
        const auto cut = app.openSession (truncated);
        expect ("truncated session file: refused with an error, current session kept", cut.failed() && app.getSession() == before,
                cut.getErrorMessage());

        const auto empty = folder.getChildFile ("empty.meridian");
        empty.replaceWithText ({});
        expect ("empty session file: refused with an error", app.openSession (empty).failed(), {});

        const auto future = folder.getChildFile ("future.meridian");
        future.replaceWithText ("<SESSION formatVersion=\"999\"><TRACKS/></SESSION>");
        const auto newer = app.openSession (future);
        expect ("session from a newer version: refused with a clear message", newer.failed() && newer.getErrorMessage().contains ("newer"),
                newer.getErrorMessage());
    }

    // 2. A hand-crafted session breaking every rule at once.
    const auto tonePath = media.getChildFile ("tone1k_mono.wav").getFullPathName();
    juce::String xml;
    xml << "<SESSION name=\"Hostile\" sampleRate=\"0\" bitDepth=\"7\" fileFormat=\"mp3\" formatVersion=\"1\" nextId=\"2\""
           " masterVolumeDb=\"nan\" consoleDrive=\"99\" consoleCurve=\"5\"><TRACKS><BOGUS/>"
           "<TRACK id=\"5\" name=\"A\" kind=\"audio\" channels=\"9\" volumeDb=\"1e999\" pan=\"-7\" outputBus=\"6\" inputChannel=\"-40\"><CLIPS>"
           "<CLIP id=\"5\" name=\"dup id\" file=\"" << tonePath << "\" start=\"-100\" offset=\"0\" length=\"48000\" sourceLength=\"96000\""
           " fadeIn=\"40000\" fadeOut=\"40000\" gainDb=\"-1e300\"/>"
           "<CLIP id=\"7\" name=\"no length\" file=\"x.wav\" start=\"0\" length=\"-5\"/>"
           "<CLIP id=\"8\" name=\"past end\" file=\"" << tonePath << "\" start=\"0\" offset=\"90000\" length=\"48000\" sourceLength=\"96000\"/>"
           "<CLIP id=\"9\" name=\"missing file\" file=\"does/not/exist.wav\" start=\"0\" offset=\"0\" length=\"48000\" sourceLength=\"0\"/>"
           "<CLIP id=\"10\" name=\"a folder\" file=\".\" start=\"0\" offset=\"0\" length=\"48000\" sourceLength=\"0\"/>"
           "<CLIP id=\"11\" name=\"valid\" file=\"" << tonePath << "\" start=\"60000\" offset=\"0\" length=\"20000\" sourceLength=\"96000\"/>"
           "</CLIPS><CLIPS/><SENDS><SEND slot=\"0\" target=\"6\" levelDb=\"nan\"/><SEND slot=\"0\" target=\"6\"/>"
           "<SEND slot=\"9\" target=\"6\"/><SEND slot=\"1\" target=\"999\"/><SEND slot=\"2\" target=\"5\"/></SENDS>"
           "<INSERTS><INSERT slot=\"0\" description=\"not a plug-in\" state=\"%%%\"/></INSERTS>"
           "<AUTOMATION><LANE parameter=\"volume\"><POINT time=\"-5\" value=\"500\"/><POINT time=\"10\"/></LANE></AUTOMATION></TRACK>"
           "<TRACK id=\"6\" name=\"Bus X\" kind=\"aux\" channels=\"1\" outputBus=\"7\"><SENDS><SEND slot=\"0\" target=\"5\"/></SENDS></TRACK>"
           "<TRACK id=\"7\" name=\"Bus Y\" kind=\"aux\" outputBus=\"6\"/>"
           "<TRACK id=\"7\" name=\"Dup\" kind=\"weird\"><CLIPS>"
           "<CLIP id=\"23\" kind=\"midi\" name=\"midi on audio\" start=\"0\" length=\"100\"/></CLIPS></TRACK>"
           "<TRACK id=\"20\" name=\"Keys\" kind=\"instrument\" channels=\"1\" inputChannel=\"3\" recordArm=\"1\"><CLIPS>"
           "<CLIP id=\"21\" kind=\"midi\" name=\"riff\" start=\"0\" length=\"48000\">"
           "<NOTES><NOTE pitch=\"300\" velocity=\"9\" start=\"-5\" length=\"0\"/><NOTE pitch=\"64.5\" start=\"0\" length=\"960\"/>"
           "<NOTE pitch=\"60\" velocity=\"nan\" start=\"480\" length=\"960\"/><BOGUS/></NOTES><NOTES/><JUNK/></CLIP>"
           "<CLIP id=\"22\" name=\"audio on instrument\" file=\"" << tonePath << "\" start=\"0\" length=\"100\" sourceLength=\"96000\"/>"
           "<CLIP id=\"24\" kind=\"strange\" name=\"odd kind\" start=\"0\" length=\"100\"/>"
           "</CLIPS><SYNTH waveform=\"noise\" engine=\"orchestra\" tune=\"99\" decayScale=\"0\" tone=\"nan\" cutoff=\"nan\" release=\"-4\" gainDb=\"1e999\"/><SYNTH/></TRACK>"
           "</TRACKS></SESSION>";
    const auto hostileFolder = folder.getChildFile ("Hostile");
    hostileFolder.createDirectory();
    const auto hostileFile = hostileFolder.getChildFile ("Hostile.meridian");
    hostileFile.replaceWithText (xml);

    juce::String error;
    auto loaded = Session::load (hostileFile, error);
    expect ("hostile session loads (repaired, not refused)", loaded != nullptr, error);
    juce::String repairedXml;
    if (loaded != nullptr)
    {
        const auto& repairs = loaded->getLoadRepairs();
        expect ("every problem was found and repaired", repairs.size() >= 20, juce::String (repairs.size()) + " repairs, e.g. "
                + repairs.joinIntoString ("; ").substring (0, 300));
        const auto problem = findSessionProblem (*loaded);
        expect ("repaired session passes the independent validator", problem.isEmpty(), problem);
        const auto again = repairSession (*loaded);
        expect ("repair is complete in one pass (a second pass changes nothing)", again.isEmpty(), again.joinIntoString ("; "));
        expect ("routing loop Bus X -> Bus Y -> Bus X was broken",
                ! routing::feeds (*loaded, 6, 6) && ! routing::feeds (*loaded, 7, 7), {});
        expect ("sample rate repaired to 48 kHz", juce::exactlyEqual (loaded->getSampleRate(), 48000.0), juce::String (loaded->getSampleRate()));
        repairedXml = loaded->getState().toXmlString();
    }

    // The repaired session must open in the application and render cleanly,
    // including the clips whose files are missing or are a folder.
    const auto previousHandler = app.showProblem;
    juce::StringArray problems;
    app.showProblem = [&problems] (const juce::String& title, const juce::String& message) { problems.add (title + ": " + message); };

    const auto opened = app.openSession (hostileFile);
    expect ("hostile session opens in the application", opened.wasOk(), opened.getErrorMessage());
    expect ("the user is told the session was repaired", problems.joinIntoString ("\n").contains ("Session repaired"),
            problems.joinIntoString (" | ").substring (0, 200));
    if (opened.wasOk())
    {
        const auto r = bounce ("hostile_render", 0, 96000);
        const auto audio = readAudio (r.file);
        expect ("hostile session renders: finite audio, no NaN or infinity", r.ok && allFinite (audio),
                "peak " + fmt (db (juce::jmax (r.left.peak, r.right.peak)), 1) + " dBFS");
        // The one intact clip (60000-80000) still plays; the missing-file and
        // folder clips are silent instead of failing the whole render.
        double validPeak = 0.0;
        if (audio.getNumSamples() >= 80000)
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                validPeak = juce::jmax (validPeak, (double) audio.getMagnitude (ch, 62000, 16000));
        expect ("the intact clip in the repaired session is audible", db (validPeak) > -30.0,
                "peak " + fmt (db (validPeak), 1) + " dBFS");
    }

    // 3. Fuzz: 600 randomly damaged variants of the repaired session.
    if (repairedXml.isNotEmpty())
    {
        juce::Random rng (0x46555a5a);
        const auto base = juce::ValueTree::fromXml (repairedXml);
        int loadedCount = 0, rendered = 0;
        juce::String firstFailure;
        for (int i = 0; i < 600 && firstFailure.isEmpty(); ++i)
        {
            auto variant = base.createCopy();
            mutateSession (variant, rng);
            const auto file = hostileFolder.getChildFile ("fuzz.meridian");
            if (auto x = variant.createXml())
                x->writeTo (file);
            if (i % 40 == 0)
                file.copyFileTo (hostileFolder.getChildFile ("fuzz_" + juce::String (i) + ".meridian")); // kept for diagnosis
            trace ("session fuzz variant " + juce::String (i));

            juce::String loadError;
            auto s = Session::load (file, loadError);
            if (s == nullptr)
                continue; // refusing a file is a valid outcome; crashing is not
            ++loadedCount;
            if (const auto problem = findSessionProblem (*s); problem.isNotEmpty())
                firstFailure = "variant " + juce::String (i) + ": " + problem;
            else if (const auto again = repairSession (*s); ! again.isEmpty())
                firstFailure = "variant " + juce::String (i) + " needed a second repair: " + again[0];

            // Every 40th variant goes through the whole application and the engine.
            if (i % 40 == 0 && firstFailure.isEmpty() && app.openSession (file).wasOk())
            {
                const auto r = bounce ("fuzz_render", 0, 48000);
                if (! r.ok || ! allFinite (readAudio (r.file)))
                    firstFailure = "variant " + juce::String (i) + " rendered invalid audio";
                ++rendered;
            }
        }
        expect ("600 randomly damaged sessions: every one is refused or repaired to a valid state", firstFailure.isEmpty(),
                firstFailure.isNotEmpty() ? firstFailure
                                          : juce::String (loadedCount) + " loaded and repaired, " + juce::String (rendered)
                                                + " opened and rendered without a crash");
    }

    app.showProblem = previousHandler;
}

void SelfTest::testSessionSafety()
{
    beginGroup ("Session safety (Save As, backups, crash recovery)");
    const auto previousHandler = app.onRecoveryAvailable;
    juce::File offered;
    app.onRecoveryAvailable = [&offered] (const juce::File& backup) { offered = backup; };
    const auto restore = [this, previousHandler] { app.onRecoveryAvailable = previousHandler; };

    Session::Settings settings;
    settings.name = "SafetySession";
    settings.sampleRate = sampleRate;
    const auto created = app.createSession (output, settings, nullptr);
    expect ("create a scratch session", created.wasOk(), created.getErrorMessage());
    if (created.failed())
        return restore();

    const auto file = app.getSession()->getSessionFile();
    const auto inside = app.getSession()->getAudioFolder().getChildFile ("tone.wav");
    media.getChildFile ("tone1k_mono.wav").copyFileTo (inside);
    addToneTrack ("Tone", 1, inside, clipStart);
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const double reference = db (bounce ("safety_reference", 0, end).left.peak);
    app.saveSession();
    expect ("an open session is marked as open (for crash detection)", backups::wasLeftOpen (file), backups::markerFor (file).getFullPathName());

    auto hasTrack = [this] (const juce::String& name)
    {
        for (const auto& t : app.getSession()->getTracksNode())
            if (track::name (t) == name)
                return true;
        return false;
    };

    // Save As in the same folder: a new file, and work continues there.
    const auto copyFile = file.getSiblingFile ("SafetySession copy.meridian");
    auto result = app.saveSessionAs (copyFile);
    expect ("Save As writes the new file and continues there; the original stays",
            result.wasOk() && copyFile.existsAsFile() && app.getSession()->getSessionFile() == copyFile && file.existsAsFile(),
            result.getErrorMessage());
    expect ("Save As moves the open marker to the new file", ! backups::wasLeftOpen (file) && backups::wasLeftOpen (copyFile), {});

    // Save As into another folder: clips inside the old folder must still be found.
    const auto moved = output.getChildFile ("Moved Session").getChildFile ("Moved.meridian");
    result = app.saveSessionAs (moved);
    const auto clipNode = track::clips (app.getSession()->getTrack (0)).getChild (0);
    const auto stored = clipNode[ids::file].toString();
    expect ("Save As into another folder re-stores clip paths so they still resolve",
            result.wasOk() && app.getSession()->resolveStoredPath (stored) == inside, stored);
    expectNear ("the session saved elsewhere plays identically", db (bounce ("safety_moved", 0, end).left.peak), reference, 0.001, "dB");
    result = app.openSession (moved);
    expectNear ("and after re-opening it from disk", result.wasOk() ? db (bounce ("safety_moved_reopened", 0, end).left.peak) : 0.0,
                reference, 0.001, "dB");

    // Backups: snapshots of unsaved work, the newest five kept.
    app.openSession (file);
    int written = 0;
    for (int i = 0; i < 7; ++i)
    {
        app.getSession()->addTrack ("Edit " + juce::String (i), 1, &app.getSession()->getUndoManager());
        written += app.backupIfChanged() ? 1 : 0;
    }
    const auto all = backups::list (file);
    expect ("a backup is written for each change, and only the newest 5 are kept", written == 7 && all.size() == 5,
            juce::String (written) + " written, " + juce::String (all.size()) + " kept");
    expect ("no backup when nothing changed", ! app.backupIfChanged(), {});
    bool newestHasLastEdit = false, allValid = all.size() == 5;
    for (int i = 0; i < all.size(); ++i)
    {
        juce::String error;
        auto snapshot = Session::load (all[i], error);
        allValid = allValid && snapshot != nullptr && findSessionProblem (*snapshot).isEmpty();
        if (i == 0 && snapshot != nullptr)
            for (const auto& t : snapshot->getTracksNode())
                newestHasLastEdit = newestHasLastEdit || track::name (t) == "Edit 6";
    }
    expect ("every backup is a valid session; the newest holds the latest unsaved edit", allValid && newestHasLastEdit, {});
    expect ("backups never touch the session file (it still has none of the edits)",
            [&] { juce::String e; auto s = Session::load (file, e); return s != nullptr && s->getNumTracks() == 1; }(), {});

    // A clean close removes the marker: newer backups are then not offered.
    app.openSession (copyFile);
    offered = juce::File();
    app.openSession (file);
    expect ("after a clean close, nothing is offered for recovery", ! offered.exists() && ! app.getRecoveryBackup().exists(), {});

    // Crash injection: a real second Meridian process opens the session, makes an
    // edit it never saves, writes a backup, and is killed (no clean shutdown).
    app.openSession (copyFile); // so the child is the only one with it open
    const auto ready = output.getChildFile ("crash-child.ready");
    juce::ChildProcess child;
    const bool started = child.start (juce::StringArray { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(),
                                                          "--crash-test-child", file.getFullPathName(), ready.getFullPathName() });
    int waited = 0;
    for (; started && ! ready.existsAsFile() && waited < 60000; waited += 50)
        juce::Thread::sleep (50);
    const bool wasReady = ready.existsAsFile();
    child.kill();
    child.waitForProcessToFinish (10000);
    expect ("a second Meridian process made an unsaved edit and a backup, then was killed", started && wasReady,
            ! started ? juce::String ("could not start the child process")
                      : wasReady ? "ready after about " + juce::String (waited) + " ms, then killed"
                                 : juce::String ("the child never became ready"));
    expect ("the killed process left the session marked open", backups::wasLeftOpen (file), {});

    // Where file times are whole seconds (Linux), the session file and a backup made in the
    // same second look equally old. Reproduce that here: recovery must not depend on file times.
    if (const auto newestBackup = backups::list (file); ! newestBackup.isEmpty())
        file.setLastModificationTime (newestBackup.getFirst().getLastModificationTime());

    offered = juce::File();
    result = app.openSession (file);
    expect ("re-opening after the crash offers the backup with the unsaved work",
            result.wasOk() && offered.existsAsFile() && app.getRecoveryBackup() == offered, offered.getFileName());
    expect ("opening without recovering gives the last saved version", ! hasTrack ("Unsaved Edit"), {});
    if (offered.existsAsFile())
    {
        result = app.recoverFromBackup (offered);
        expect ("recovering restores the unsaved edit, under the original session file, as unsaved",
                result.wasOk() && hasTrack ("Unsaved Edit") && app.getSession()->getSessionFile() == file
                    && app.getSession()->hasUnsavedChanges(),
                result.getErrorMessage());
        expect ("the recovered session plays", std::abs (db (bounce ("safety_recovered", 0, end).left.peak) - reference) < 0.001, {});
        app.saveSession();
    }
    restore();
}

void SelfTest::testRemoteFuzz()
{
    beginGroup ("Robustness (damaged files, hostile input)");
    auto& tools = app.getRemoteTools();

    // A scratch session with something to break: tracks, clips, a bus, a send.
    Session::Settings settings;
    settings.name = "FuzzSession";
    settings.sampleRate = sampleRate;
    const auto created = app.createSession (output, settings, nullptr);
    expect ("create a scratch session for the remote fuzz test", created.wasOk(), created.getErrorMessage());
    if (created.failed())
        return;

    auto* session = app.getSession();
    addToneTrack ("One", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    addToneTrack ("Two", 2, media.getChildFile ("tone1k_stereo.wav"), 0);
    auto bus = session->addTrack ("Bus", 2, nullptr, Session::TrackKind::aux);
    routing::setSend (*session, session->getTrack (0), 0, track::id (bus), nullptr);

    // Deleting a bus takes the routes into it along, and undo restores them.
    {
        auto one = session->getTrack (0);
        routing::setOutput (*session, one, track::id (bus), nullptr);
        app.deleteTrack (bus);
        expect ("deleting a bus removes the sends and outputs that fed it",
                track::sends (one).getNumChildren() == 0 && track::outputBus (one) == 0 && findSessionProblem (*session).isEmpty(),
                findSessionProblem (*session));
        session->getUndoManager().undo();
        bus = session->getTracksNode().getChildWithProperty (ids::name, "Bus");
        expect ("undo brings the bus back with its routes",
                bus.isValid() && track::sends (one).getNumChildren() == 1 && track::outputBus (one) == track::id (bus), {});
        routing::setOutput (*session, one, 0, nullptr);
    }

    // Tool calls with random arguments of every type, including the wrong ones.
    // transport and bounce are left out: they would play or record audio.
    const auto descriptors = tools.describeTools();
    juce::Array<juce::var> toolList;
    if (auto* list = descriptors.getArray())
        toolList.addArray (*list);
    expect ("the tool list is available for fuzzing", ! toolList.isEmpty(), juce::String (toolList.size()) + " tools");
    if (toolList.isEmpty())
        return;

    juce::Random rng (0x544f4f4c);
    auto randomArgument = [&rng] () -> juce::var
    {
        switch (rng.nextInt (9))
        {
            case 0:  return rng.nextInt (8) - 2;
            case 1:  return rng.nextInt() ;
            case 2:  return rng.nextDouble() * 200.0 - 100.0;
            case 3:  return std::numeric_limits<double>::infinity();
            case 4:  return std::numeric_limits<double>::quiet_NaN();
            case 5:  return rng.nextBool();
            case 6:  { static const char* s[] = { "", "One", "Two", "Bus", "master", "A", "edit", "mix", "../../x", "\xe2\x82\xac", "1", "-1" };
                       return juce::String::fromUTF8 (s[rng.nextInt ((int) std::size (s))]); }
            case 7:  return juce::var (juce::Array<juce::var> { 1, "two" });
            default: { auto* o = new juce::DynamicObject(); o->setProperty ("x", 1); return juce::var (o); }
        }
    };

    int calls = 0, succeeded = 0;
    juce::String firstFailure;
    for (int i = 0; i < 3000 && firstFailure.isEmpty(); ++i)
    {
        const auto& descriptor = toolList.getReference (rng.nextInt (toolList.size()));
        const auto name = descriptor["name"].toString();
        if (name == "transport" || name == "bounce")
            continue;

        // Held in a var for the whole iteration: the object is reference-counted,
        // so wrapping a raw pointer in two temporaries would delete it early.
        const juce::var args (new juce::DynamicObject());
        if (auto* properties = descriptor["inputSchema"]["properties"].getDynamicObject())
            for (const auto& p : properties->getProperties())
                if (rng.nextInt (3) != 0)
                    args.getDynamicObject()->setProperty (p.name, randomArgument());

        trace ("tool fuzz " + juce::String (i) + ": " + name + " " + juce::JSON::toString (args, true));
        const auto result = tools.call (name, args);
        ++calls;
        succeeded += result.ok ? 1 : 0;
        if (! result.ok && result.error.isEmpty())
            firstFailure = name + " failed without saying why";
        else if (app.getSession() != nullptr)
            if (const auto problem = findSessionProblem (*app.getSession()); problem.isNotEmpty())
                firstFailure = "after " + name + " " + juce::JSON::toString (args, true) + ": " + problem;
    }
    expect ("3000 random AI tool calls: no crash, every error explained, session always valid", firstFailure.isEmpty(),
            firstFailure.isNotEmpty() ? firstFailure
                                      : juce::String (calls) + " calls, " + juce::String (succeeded) + " accepted, "
                                            + juce::String (calls - succeeded) + " refused with a reason");

    // Raw messages: deep nesting, invalid UTF-8, mutated JSON-RPC.
    const std::string deep (64 * 1024, '[');
    int status = 0;
    const auto deepMcp = juce::JSON::parse (juce::String (tools.handleMcpRequest (deep, status)));
    expect ("64 KB of nested brackets over MCP: refused, no stack overflow", deepMcp["error"].isObject(), {});
    const auto deepWs = juce::JSON::parse (juce::String (tools.handleJsonMessage ("{\"tool\":" + deep)));
    expect ("64 KB of nested brackets over the WebSocket: refused, no stack overflow", deepWs["ok"].isBool() && ! (bool) deepWs["ok"], {});

    const std::string validRequest = R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"set_track","arguments":{"track":1,"volume_db":-6}}})";
    juce::String mcpFailure;
    for (int i = 0; i < 2000 && mcpFailure.isEmpty(); ++i)
    {
        auto body = validRequest;
        for (int edit = rng.nextInt (4) + 1; --edit >= 0;)
        {
            const auto at = (size_t) rng.nextInt ((int) body.size());
            switch (rng.nextInt (3))
            {
                case 0:  body[at] = (char) rng.nextInt (256); break;
                case 1:  body.insert (at, 1, (char) rng.nextInt (256)); break;
                default: body.erase (at, 1); break;
            }
        }
        const auto reply = tools.handleMcpRequest (body, status);
        if (status == 202 ? ! reply.empty() : ! juce::JSON::parse (juce::String::fromUTF8 (reply.data(), (int) reply.size())).isObject())
            mcpFailure = "reply to a mutated request was not a JSON object: " + juce::String (reply).substring (0, 120);
        if (app.getSession() != nullptr && mcpFailure.isEmpty())
            if (const auto problem = findSessionProblem (*app.getSession()); problem.isNotEmpty())
                mcpFailure = "a mutated request broke the session: " + problem;
    }
    expect ("2000 mutated MCP requests: every reply well-formed, session always valid", mcpFailure.isEmpty(), mcpFailure);
}

void SelfTest::testMcpTools()
{
    beginGroup ("MCP server and AI tools");
    auto& tools = app.getRemoteTools();
    clearTracks();
    addToneTrack ("Vocal", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);

    int nextId = 1;
    auto rpc = [&] (const juce::String& method, const juce::var& params) -> juce::var
    {
        auto* request = new juce::DynamicObject();
        request->setProperty ("jsonrpc", "2.0");
        request->setProperty ("id", nextId++);
        request->setProperty ("method", method);
        request->setProperty ("params", params);
        int status = 0;
        const auto reply = tools.handleMcpRequest (juce::JSON::toString (juce::var (request), true).toStdString(), status);
        return juce::JSON::parse (juce::String (reply));
    };
    auto toolCall = [&] (const juce::String& name, const juce::String& argumentsJson) -> juce::var
    {
        auto* params = new juce::DynamicObject();
        params->setProperty ("name", name);
        params->setProperty ("arguments", juce::JSON::parse (argumentsJson));
        return rpc ("tools/call", juce::var (params));
    };
    auto callText = [] (const juce::var& response) { return response["result"]["content"][0]["text"].toString(); };
    auto failed = [] (const juce::var& response) { return (bool) response["result"]["isError"]; };

    auto* init = new juce::DynamicObject();
    init->setProperty ("protocolVersion", "2025-06-18");
    const auto initialised = rpc ("initialize", juce::var (init));
    expect ("initialize negotiates the protocol and advertises tools",
            initialised["result"]["protocolVersion"].toString() == "2025-06-18"
                && initialised["result"]["capabilities"]["tools"].isObject(),
            juce::JSON::toString (initialised["result"]["serverInfo"], true));

    int notificationStatus = 0;
    tools.handleMcpRequest (R"({"jsonrpc":"2.0","method":"notifications/initialized"})", notificationStatus);
    expect ("notifications are accepted without a reply (HTTP 202)", notificationStatus == 202, juce::String (notificationStatus));

    const auto list = rpc ("tools/list", juce::var (new juce::DynamicObject()));
    const auto* toolArray = list["result"]["tools"].getArray();
    bool allDescribed = toolArray != nullptr;
    if (toolArray != nullptr)
        for (const auto& tool : *toolArray)
            allDescribed = allDescribed && tool["inputSchema"].isObject() && tool["description"].toString().length() > 20;
    expect ("tools/list describes all 43 tools, each with a description and a JSON schema",
            allDescribed && toolArray->size() == 43, juce::String (toolArray != nullptr ? toolArray->size() : 0) + " tools");

    auto r = toolCall ("get_status", "{}");
    expect ("get_status returns the session and tracks", ! failed (r) && callText (r).contains ("\"Vocal\""), callText (r).substring (0, 160));

    r = toolCall ("create_track", R"({"kind":"aux","name":"Verb"})");
    expect ("create_track makes an aux bus", ! failed (r) && app.getSession()->getNumTracks() == 2, callText (r).substring (0, 160));

    r = toolCall ("set_send", R"({"track":"Vocal","slot":"A","bus":"Verb","level_db":-8,"pre_fader":false})");
    const auto vocal = app.getSession()->getTrack (0);
    expect ("set_send routes the vocal into the bus at -8 dB",
            ! failed (r) && juce::exactlyEqual (send::levelDb (routing::findSend (vocal, 0)), -8.0), callText (r).substring (0, 200));

    r = toolCall ("set_track", R"({"track":1,"volume_db":-3.5,"pan":-0.4,"solo":true})");
    expect ("set_track changes several settings at once",
            ! failed (r) && juce::exactlyEqual (track::volumeDb (vocal), -3.5) && juce::exactlyEqual (track::pan (vocal), -0.4)
                && track::solo (vocal),
            callText (r).substring (0, 160));

    r = toolCall ("split_clip", R"({"track":"Vocal","seconds":1.0})");
    r = toolCall ("list_clips", R"({"track":"vocal"})");
    expect ("split_clip and list_clips (names are case-insensitive)",
            ! failed (r) && track::clips (vocal).getNumChildren() == 2 && clip::start (track::clips (vocal).getChild (1)) == 48000
                && juce::JSON::parse (callText (r))["clips"].size() == 2,
            callText (r).substring (0, 200));

    r = toolCall ("set_clip", R"({"track":"Vocal","clip":2,"fade_out_seconds":0.25,"gain_db":-2})");
    expect ("set_clip sets fades and clip gain",
            ! failed (r) && clip::fadeOut (track::clips (vocal).getChild (1)) == 12000
                && juce::exactlyEqual (clip::gainDb (track::clips (vocal).getChild (1)), -2.0),
            callText (r));

    if (testPlugin != nullptr)
    {
        // As if the user had scanned it; removed again so their plug-in list stays clean.
        auto& known = app.getPluginHost().getKnownPlugins();
        known.addType (*testPlugin);
        r = toolCall ("list_plugins", R"({"filter":"meridian"})");
        expect ("list_plugins shows scanned plug-ins", ! failed (r) && callText (r).contains ("Meridian Test Gain"), callText (r).substring (0, 160));
        r = toolCall ("insert_plugin", R"({"track":"Verb","slot":"A","plugin":"test gain"})");
        expect ("insert_plugin finds a plug-in by partial name", ! failed (r) && callText (r).contains ("Meridian Test Gain"),
                callText (r).substring (0, 200));
        known.removeType (*testPlugin);
    }

    r = toolCall ("set_output", R"({"track":"Verb","destination":"Vocal"})");
    expect ("routing into an audio track is refused with an explanation", failed (r) && callText (r).contains ("aux"), callText (r));

    r = toolCall ("set_track", R"({"track":9})");
    expect ("unknown tracks give a useful error", failed (r) && callText (r).contains ("session has 2"), callText (r));

    const int clipsBefore = track::clips (vocal).getNumChildren();
    toolCall ("delete_clip", R"({"track":"Vocal","clip":2})");
    r = toolCall ("undo", "{}");
    expect ("undo reverts the AI's last edit", ! failed (r) && track::clips (vocal).getNumChildren() == clipsBefore
                && callText (r).contains ("AI: Delete Clip"), callText (r));

    r = toolCall ("show_window", R"({"window":"mix"})");
    expect ("show_window switches to the mixer", ! failed (r) && main.getActiveWindow() == WindowKind::mix, callText (r));
    main.showWindow (WindowKind::edit);

    // Volume automation drawn by the AI, verified in the audio: a straight line
    // from -40 dB to 0 dB must pass -20 dB halfway (interpolation in dB, as drawn).
    clearTracks();
    addToneTrack ("Automated", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    r = toolCall ("set_volume_automation", R"({"track":"Automated","points":[{"seconds":0.5,"db":-40},{"seconds":1.5,"db":0}]})");
    expect ("set_volume_automation writes the breakpoints", ! failed (r) && callText (r).contains ("-40"), callText (r));
    {
        const auto bounced = bounce ("ai_automation", 0, clipStart + (juce::int64) (2.0 * sampleRate));
        const auto audio = readAudio (bounced.file);
        const double unity = 0.5 * std::sqrt (0.5) / std::sqrt (2.0); // RMS of the tone at 0 dB (mono, centre)
        const auto mid = dsp::measureLevels (audio.getReadPointer (0, 48000 - 240), 480); // t = 1.0 s
        expectNear ("halfway along the line the level is -20 dB (linear in dB, as drawn)",
                    db (mid.rms / unity), -20.0, 0.5, "dB");
    }
    r = toolCall ("set_volume_automation", R"({"track":"Automated","points":[]})");
    expect ("an empty point list removes the automation", ! failed (r) && callText (r).contains ("removed"), callText (r));

    const auto viaWebSocket = juce::String (tools.handleJsonMessage (R"({"tool":"get_status","arguments":{}})"));
    expect ("the same tools work as JSON over the WebSocket", (bool) juce::JSON::parse (viaWebSocket)["ok"], viaWebSocket.substring (0, 80));

    const auto unknown = rpc ("resources/list", juce::var (new juce::DynamicObject()));
    expect ("unsupported methods return a JSON-RPC error", (int) unknown["error"]["code"] == -32601, juce::JSON::toString (unknown, true));
    clearTracks();
}

void SelfTest::testRemoteCommands()
{
    beginGroup ("AI remote commands (in-process)");
    auto* session = app.getSession();
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    auto t1 = addToneTrack ("Remote 1", 1, tone, clipStart);
    auto t2 = addToneTrack ("Remote 2", 1, tone, clipStart);

    auto send = [this] (const char* command) { return juce::String (remote::handleRemoteMessage (command, app)); };

    auto reply = send ("MUTE_TRACK_1");
    expect ("MUTE_TRACK_1 mutes track 1", reply.contains ("\"ok\":true") && track::mute (t1), reply);
    reply = send ("MUTE_TRACK_1");
    expect ("MUTE_TRACK_1 again unmutes it", ! track::mute (t1), reply);

    reply = send ("SET_VOLUME_TRACK_2_-12.5dB");
    expect ("SET_VOLUME_TRACK_2_-12.5dB sets the fader", juce::exactlyEqual (track::volumeDb (t2), -12.5), reply);
    expect ("remote edits are undoable", session->getUndoManager().canUndo(), session->getUndoManager().getUndoDescription());

    const auto before = main.getActiveWindow();
    reply = send ("TOGGLE_WINDOW");
    expect ("TOGGLE_WINDOW switches window", main.getActiveWindow() != before, reply);
    send ("TOGGLE_WINDOW");

    reply = send ("STATUS");
    expect ("STATUS lists both tracks", reply.contains ("\"Remote 1\"") && reply.contains ("\"Remote 2\""), reply.substring (0, 200));

    reply = send ("MUTE_TRACK_99");
    expect ("an out-of-range track is refused", reply.contains ("\"ok\":false"), reply);

    reply = send ("BOUNCE");
    expect ("BOUNCE starts a background mixdown", reply.contains ("\"ok\":true") && app.isBouncing(), reply);
}

//==============================================================================
// Phase 2: timed

void SelfTest::timerCallback()
{
    ++ticks;

    switch (phase)
    {
        case 1: // wait for the remote BOUNCE
            if (! app.isBouncing())
            {
                beginGroup ("AI remote commands (in-process)");
                const auto& last = app.getLastBounce();
                expect ("remote BOUNCE finished and wrote a file", last.ok && last.file.existsAsFile(),
                        last.ok ? last.file.getFullPathName() : last.error);
                ticks = 0;
                if (setUpDelayCompensationTest())
                {
                    phase = 7;
                }
                else
                {
                    startLivePlayback();
                    phase = 2;
                }
            }
            else if (ticks > 600)
            {
                expect ("remote BOUNCE finished within 60 s", false, "still running");
                phase = 3;
            }
            break;

        case 2: // live playback for 1.5 s
            if (ticks >= 15)
            {
                finishLivePlayback();
                ticks = 0;
                phase = 3;
            }
            break;

        case 3: // the stop's return-to-start is applied by the audio thread a block later
            if (ticks >= 2)
            {
                checkStopReturned();
                ticks = 0;
                if (includeRecording && startRecordingTest())
                {
                    phase = 5;
                }
                else
                {
                    testUserInterface();
                    testPianoRoll();
                    testPlaylistSelector();
                    testEditModes();
                    testAutomationLanes();
                    testMixSections();
                    testTempoRuler();
                    buildShowcaseSession(); // for the screenshots
                    phase = 4;
                }
            }
            break;

        case 5: // record for 1.2 s
            if (ticks >= 12)
            {
                app.transportStop();
                ticks = 0;
                phase = 6;
            }
            break;

        case 6: // the take becomes a clip once the audio thread has released it
            if (track::clips (recordTrack).getNumChildren() > 0 || ticks >= 50)
            {
                finishRecordingTest();
                testUserInterface();
                testPianoRoll();
                testPlaylistSelector();
                testEditModes();
                testAutomationLanes();
                testMixSections();
                testTempoRuler();
                buildShowcaseSession();
                ticks = 0;
                phase = 4;
            }
            break;

        case 7: // plug-in latency reaches the host asynchronously in VST3; give it a moment
            if (ticks >= 5)
            {
                checkDelayCompensation();
                startLivePlayback();
                ticks = 0;
                phase = 2;
            }
            break;

        case 4: // let waveforms load, then screenshots
            if (ticks >= 20)
            {
                captureScreens();
                stopTimer();
                finish();
            }
            break;

        default:
            break;
    }
}

//==============================================================================
// Plug-in delay compensation (timed: VST3 reports latency changes asynchronously)

bool SelfTest::setUpDelayCompensationTest()
{
    beginGroup ("Plug-in delay compensation");
    if (testPlugin == nullptr)
    {
        expect ("test plug-in available for latency tests", false, "not found");
        return false;
    }

    auto* session = app.getSession();
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    clearTracks();

    // A latent plug-in: 0 dB gain, 1000 samples of latency (about 21 ms).
    auto addLatentPlugin = [this] (juce::ValueTree t)
    {
        juce::String error;
        app.getBridge().insertPlugin (t, 0, *testPlugin, error);
        if (auto slot = app.getBridge().getInsert (track::id (t), 0); slot != nullptr && slot->plugin != nullptr)
        {
            slot->plugin->getParameters()[0]->setValueNotifyingHost (0.5f);              // gain 0 dB
            slot->plugin->getParameters()[1]->setValueNotifyingHost (1000.0f / 8192.0f); // latency 1000
        }
    };

    pdcLatentTrack = addToneTrack ("Latent", 1, tone, clipStart);
    addLatentPlugin (pdcLatentTrack);
    pdcDryTrack = addToneTrack ("Dry", 1, tone, clipStart);

    auto sendSource = addToneTrack ("Send Source", 1, tone, clipStart);
    auto bus = session->addTrack ("Latent Bus", 2, nullptr, Session::TrackKind::aux);
    addLatentPlugin (bus);
    routing::setSend (*session, sendSource, 0, track::id (bus), nullptr);
    sendSource.setProperty (ids::mute, true, nullptr); // the bus scenario runs second
    return true;
}

void SelfTest::checkDelayCompensation()
{
    beginGroup ("Plug-in delay compensation");
    auto* session = app.getSession();
    const auto end = clipStart + (juce::int64) (2.0 * sampleRate) + 4800;
    const double dry = toneDb + panCentreDb;

    const auto slot = app.getBridge().getInsert (track::id (pdcLatentTrack), 0);
    expect ("plug-in reports its latency to the host (1000 samples)",
            slot != nullptr && slot->plugin != nullptr && slot->plugin->getLatencySamples() == 1000,
            slot != nullptr && slot->plugin != nullptr ? juce::String (slot->plugin->getLatencySamples()) : juce::String ("no plug-in"));
    expect ("live engine picked up the new latency (1000 samples)", app.getEngine().getPluginLatencySamples() == 1000,
            juce::String (app.getEngine().getPluginLatencySamples()) + " samples");

    auto r = bounce ("pdc_on", 0, end);
    expectNear ("latent and dry copies of a tone sum perfectly (+6.02 dB)", db (r.left.peak), dry + 6.0206, 0.02, "dB");

    pdcDryTrack.setProperty (ids::mute, true, nullptr);
    r = bounce ("pdc_alignment", 0, end);
    {
        const auto audio = readAudio (r.file);
        int onset = -1;
        for (int i = 0; i < audio.getNumSamples() && onset < 0; ++i)
            if (std::abs (audio.getSample (0, i)) > 1.0e-4f)
                onset = i;
        expect ("latent track is still placed exactly on the timeline", onset >= (int) clipStart && onset <= (int) clipStart + 1,
                "first sound at sample " + juce::String (onset) + ", clip starts at " + juce::String (clipStart));
    }
    pdcDryTrack.setProperty (ids::mute, false, nullptr);

    app.getBridge().setDelayCompensationEnabled (false);
    r = bounce ("pdc_off", 0, end);
    expectNear ("with compensation OFF the paths are 1000 samples apart and partly cancel (proves the test can fail)",
                db (r.left.peak), dry + db (std::sqrt (3.0)), 0.1, "dB");
    app.getBridge().setDelayCompensationEnabled (true);

    // Latency on a bus: the dry path into the master must wait for the bus.
    pdcLatentTrack.setProperty (ids::mute, true, nullptr);
    pdcDryTrack.setProperty (ids::mute, true, nullptr);
    auto sendSource = session->getTracksNode().getChildWithProperty (ids::name, "Send Source");
    auto bus = session->getTracksNode().getChildWithProperty (ids::name, "Latent Bus");
    sendSource.setProperty (ids::mute, false, nullptr);
    r = bounce ("pdc_bus", 0, end);
    expectNear ("a send through a latent bus stays aligned with the direct sound", db (r.left.peak), dry + 6.0206, 0.02, "dB");

    app.getBridge().setPluginBypassed (bus, 0, true);
    r = bounce ("pdc_bus_bypassed", 0, end);
    expectNear ("bypassing the latent plug-in re-aligns automatically", db (r.left.peak), dry + 6.0206, 0.02, "dB");
    clearTracks();
}

void SelfTest::buildShowcaseSession()
{
    // A realistic little session, used for live playback and the screenshots.
    clearTracks();
    auto* session = app.getSession();
    const auto stereoTone = media.getChildFile ("tone1k_stereo.wav");
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    auto drums = addToneTrack ("Drums", 2, stereoTone, 0);
    auto bass = addToneTrack ("Bass", 1, tone, 48000);
    auto vox = addToneTrack ("Lead Vox", 1, media.getChildFile ("tone500_mono.wav"), 96000);
    auto keys = addToneTrack ("Keys", 2, stereoTone, 24000);
    session->addTrack ("Guitar", 1, nullptr);
    auto verb = session->addTrack ("Reverb Bus", 2, nullptr, Session::TrackKind::aux);
    routing::setSend (*session, vox, 0, track::id (verb), nullptr);
    routing::setSend (*session, keys, 0, track::id (verb), nullptr);
    routing::findSend (keys, 0).setProperty (ids::levelDb, -12.0, nullptr);
    bass.setProperty (ids::volumeDb, -4.5, nullptr);
    bass.setProperty (ids::pan, -0.3, nullptr);
    {
        auto lane = automation::getOrCreateVolumeLane (bass, nullptr);
        for (const auto& [seconds, level] : { std::pair { 1.0, -30.0 }, { 2.0, -2.0 }, { 3.2, -2.0 }, { 4.0, -18.0 } })
            automation::addPoint (lane, (juce::int64) (seconds * sampleRate), level, nullptr);
    }
    // An instrument track with chords, so the screenshots show MIDI too.
    auto synth = session->addTrack ("Synth", 2, nullptr, Session::TrackKind::instrument);
    auto synthClip = session->addMidiClip (synth, 0, 16 * 24000, "Chords", nullptr); // 4 bars at 120 BPM
    const int chords[4][3] = { { 60, 64, 67 }, { 59, 62, 67 }, { 57, 60, 64 }, { 57, 60, 65 } };
    for (int bar = 0; bar < 4; ++bar)
        for (const int p : chords[bar])
            midimodel::addNote (synthClip, { p, 0.7f, bar * 3840, 3600 }, nullptr);
    for (int i = 0; i < 16; ++i)
        midimodel::addNote (synthClip, { 72 + (i % 3) * 4, 0.9f, i * 960, 480 }, nullptr);
    synth.setProperty (ids::volumeDb, -8.0, nullptr);

    // A drum machine playing a simple groove (also exercises drums in live playback).
    auto drumMachine = session->addTrack ("Drum Machine", 2, nullptr, Session::TrackKind::drums);
    auto drumClip = session->addMidiClip (drumMachine, 0, 16 * 24000, "Groove", nullptr);
    for (int step = 0; step < 32; ++step)
    {
        midimodel::addNote (drumClip, { 42, step % 2 == 0 ? 0.8f : 0.5f, step * 480, 240 }, nullptr);
        if (step % 8 == 0 || step % 8 == 3) midimodel::addNote (drumClip, { 36, 1.0f, step * 480, 240 }, nullptr);
        if (step % 8 == 4)                  midimodel::addNote (drumClip, { 38, 0.9f, step * 480, 240 }, nullptr);
    }
    drumMachine.setProperty (ids::volumeDb, -10.0, nullptr);

    vox.setProperty (ids::solo, false, nullptr);
    vox.setProperty (ids::pan, 0.25, nullptr);
    drums.setProperty (ids::mute, false, nullptr);
    track::clips (vox).getChild (0).setProperty (ids::fadeIn, 9600, nullptr);
    track::clips (vox).getChild (0).setProperty (ids::fadeOut, 14400, nullptr);
    setConsole (true, 6.0, 0.35);

    auto& view = main.getEditView().getEditState();
    view.setView (0, 400.0);
    view.clearSelection();
}

void SelfTest::startLivePlayback()
{
    beginGroup ("Live playback (audio device)");
    buildShowcaseSession();

    if (! app.getEngine().isDeviceRunning())
    {
        if (allowNoAudioDevice)
            skip ("live playback through the audio device", "this machine has no audio device");
        else
            expect ("audio device available", false, "no device running: live playback not tested");
        return;
    }

    app.getBridge().rebuildNow();
    app.getEngine().collectDiskUnderruns(); // reset the counter
    app.locate (0);
    livePositionAtStart = app.getEngine().getTransport().getPosition();
    const auto started = app.transportPlay();
    expect ("transport starts", started.wasOk(), started.getErrorMessage());
}

void SelfTest::finishLivePlayback()
{
    if (! app.getEngine().isDeviceRunning())
        return;

    const auto position = app.getEngine().getTransport().getPosition();
    const int underruns = app.getEngine().collectDiskUnderruns();
    app.transportStop();

    expectNear ("playhead advanced in real time (1.5 s)", (double) (position - livePositionAtStart) / sampleRate, 1.5, 0.35, "s");
    expect ("disk streaming kept up (no underruns)", underruns == 0, juce::String (underruns) + " blocks with missing data");
}

void SelfTest::checkStopReturned()
{
    if (! app.getEngine().isDeviceRunning())
        return;
    beginGroup ("Live playback (audio device)");
    const auto position = app.getEngine().getTransport().getPosition();
    expect ("stop returns to the start point", position == livePositionAtStart, "position " + juce::String (position));
}

//==============================================================================
// Recording (opt-in: uses the microphone)

bool SelfTest::startRecordingTest()
{
    beginGroup ("Recording (microphone)");
    auto* device = app.getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr || ! app.getEngine().isDeviceRunning())
    {
        expect ("audio device available for recording", false, "no device running");
        return false;
    }

    const int inputs = device->getActiveInputChannels().countNumberOfSetBits();
    expect ("audio input available", inputs > 0, juce::String (inputs) + " active input(s) on " + device->getName());
    if (inputs == 0)
        return false;

    clearTracks();
    recordTrack = app.getSession()->addTrack ("Rec Test", 1, nullptr);
    recordTrack.setProperty (ids::inputChannel, 0, nullptr);
    recordTrack.setProperty (ids::recordArm, true, nullptr);
    app.getBridge().rebuildNow();

    recordStartPosition = app.getEngine().getTransport().getPosition();
    const auto started = app.transportRecord();
    expect ("record starts on the armed track", started.wasOk(), started.getErrorMessage());
    return started.wasOk();
}

void SelfTest::finishRecordingTest()
{
    beginGroup ("Recording (microphone)");
    auto clipsNode = track::clips (recordTrack);
    expect ("the take becomes a clip on the armed track", clipsNode.getNumChildren() == 1,
            juce::String (clipsNode.getNumChildren()) + " clip(s)");
    if (clipsNode.getNumChildren() == 0)
        return;

    auto c = clipsNode.getChild (0);
    const auto file = app.getSession()->resolveStoredPath (c[ids::file].toString());
    expectNear ("take length matches the recording time", (double) clip::length (c) / sampleRate, 1.2, 0.3, "s");
    const auto compensation = app.getBridge().getLastRecordingCompensation();
    expect ("take is moved earlier by the round-trip latency (device in + out + engine)",
            clip::start (c) - clip::offset (c) == recordStartPosition - compensation,
            "clip at " + juce::String (clip::start (c)) + " (offset " + juce::String (clip::offset (c)) + "), recording started at "
                + juce::String (recordStartPosition) + ", compensation " + juce::String (compensation) + " samples");
    expect ("take is stored in the session's Audio Files folder", file.isAChildOf (app.getSession()->getAudioFolder()),
            file.getFileName());

    {
        std::unique_ptr<juce::AudioFormatReader> reader (app.getFormatManager().createReaderFor (file));
        expect ("take file is a valid 24-bit, 48 kHz WAV",
                reader != nullptr && reader->bitsPerSample == 24 && juce::exactlyEqual (reader->sampleRate, 48000.0),
                reader != nullptr ? juce::String ((int) reader->bitsPerSample) + "-bit, " + juce::String (reader->sampleRate) + " Hz"
                                  : juce::String ("unreadable"));

        if (reader != nullptr)
        {
            juce::AudioBuffer<float> audio (1, (int) reader->lengthInSamples);
            reader->read (audio.getArrayOfWritePointers(), 1, 0, audio.getNumSamples());
            const auto stats = dsp::measureLevels (audio.getReadPointer (0), audio.getNumSamples());
            expect ("microphone delivered sound (not digital silence)", db (stats.peak) > -120.0,
                    "peak " + fmt (db (stats.peak), 1) + " dBFS, RMS " + fmt (db (stats.rms), 1)
                        + " dBFS" + (db (stats.peak) <= -120.0 ? ". If silent: check Windows Settings > Privacy > Microphone" : ""));
        }
    }

    // Privacy: the recording is measured, never kept. It is deleted at the end of
    // the run, once the engine has closed its reader (Windows locks open files).
    recordTrack.setProperty (ids::recordArm, false, nullptr);
    clipsNode.removeChild (c, nullptr);
    recordingToDelete = file;
    clearTracks();
}

//==============================================================================
// User interface: the real components, driven with simulated mouse input

juce::MouseEvent SelfTest::mouseEvent (juce::Component& c, juce::Point<float> position, juce::Point<float> downPosition,
                                       bool dragged, juce::ModifierKeys modifiers)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), position, modifiers,
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, now, downPosition, now, 1, dragged };
}

void SelfTest::drag (juce::Component& c, juce::Point<float> from, juce::Point<float> delta, juce::ModifierKeys modifiers)
{
    c.mouseDown (mouseEvent (c, from, from, false, modifiers));
    c.mouseDrag (mouseEvent (c, from + delta, from, true, modifiers));
    c.mouseUp (mouseEvent (c, from + delta, from, true, modifiers));
}

void SelfTest::testPianoRoll()
{
    beginGroup ("MIDI editor (piano roll, simulated mouse)");
    auto* session = app.getSession();
    auto& um = session->getUndoManager();
    auto& editView = main.getEditView();
    main.showWindow (WindowKind::edit);

    auto keys = session->addTrack ("Piano Roll Test", 2, nullptr, Session::TrackKind::instrument);
    auto clipNode = session->addMidiClip (keys, 0, 8 * 24000, "Riff", nullptr);
    um.clearUndoHistory();

    editView.openPianoRoll (clipNode);
    auto* pr = editView.getPianoRoll();
    expect ("double-click/menu opens the MIDI editor for the clip", pr != nullptr && pr->getClip() == clipNode, {});
    if (pr == nullptr)
        return;

    auto notes = clip::notes (clipNode);
    const float mid = pr->rowHeight() * 0.5f;
    auto at = [&] (int64_t tick, int pitch, float dx = 3.0f) { return pr->positionFor (tick, pitch) + juce::Point<float> (dx, mid); };
    auto click = [&] (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        pr->mouseDown (mouseEvent (*pr, p, p, false, mods));
        pr->mouseUp (mouseEvent (*pr, p, p, false, mods));
    };

    // Click on empty grid: a 1/16 note on the beat and pitch under the mouse.
    pr->setGridTicks (240);
    click (at (960 + 10, 64));
    auto note = notes.getChild (0);
    expect ("click adds a note snapped to the 1/16 grid (E4 on beat 2)",
            notes.getNumChildren() == 1 && (int) note[ids::pitch] == 64 && (juce::int64) note[ids::start] == 960
                && (juce::int64) note[ids::length] == 240,
            juce::String (notes.getNumChildren()) + " notes; pitch " + note[ids::pitch].toString() + ", start "
                + note[ids::start].toString() + ", length " + note[ids::length].toString());

    // Drag its right edge out to a quarter note.
    const auto edge = pr->positionFor (960 + 240, 64) + juce::Point<float> (-2.0f, mid);
    drag (*pr, edge, { pr->positionFor (1920, 64).x - pr->positionFor (1200, 64).x, 0.0f });
    expect ("dragging the right edge sets the length (one beat)", (juce::int64) note[ids::length] == 960, note[ids::length].toString());

    // Drag the body one beat later and two semitones up.
    drag (*pr, at (960, 64, 20.0f), { pr->positionFor (1920, 64).x - pr->positionFor (960, 64).x, -2.0f * pr->rowHeight() });
    expect ("dragging moves the note in time and pitch (F#4 on beat 3)",
            (juce::int64) note[ids::start] == 1920 && (int) note[ids::pitch] == 66,
            "start " + note[ids::start].toString() + ", pitch " + note[ids::pitch].toString());

    // A second note, both selected, transposed an octave.
    click (at (0, 60));
    click (at (1920, 66, 20.0f), juce::ModifierKeys::shiftModifier);
    expect ("shift-click selects several notes", pr->getSelection().size() == 2, juce::String ((int) pr->getSelection().size()));
    pr->transposeSelection (12);
    bool transposed = true;
    for (const auto& n : notes)
        transposed = transposed && ((int) n[ids::pitch] == 72 || (int) n[ids::pitch] == 78);
    expect ("transpose +12 moves the selected notes up an octave", transposed && notes.getNumChildren() == 2, {});

    // Quantize: an off-grid note snaps to the quarter-note grid.
    notes.getChild (1).setProperty (ids::start, 1000, nullptr);
    pr->setGridTicks (960);
    pr->quantizeSelection();
    expect ("quantize snaps notes to the grid (1000 -> 960 ticks)", (juce::int64) notes.getChild (1)[ids::start] == 960,
            notes.getChild (1)[ids::start].toString());

    // Delete with the keyboard, double-click to delete, undo to restore.
    click (at (960, 72, 6.0f)); // the quantized note, now on beat 2
    pr->keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
    expect ("Delete removes the selected note", notes.getNumChildren() == 1, juce::String (notes.getNumChildren()));
    const auto remaining = notes.getChild (0);
    pr->mouseDoubleClick (mouseEvent (*pr, at ((juce::int64) remaining[ids::start], (int) remaining[ids::pitch], 6.0f),
                                      at ((juce::int64) remaining[ids::start], (int) remaining[ids::pitch], 6.0f), false));
    expect ("double-click deletes a note", notes.getNumChildren() == 0, juce::String (notes.getNumChildren()));
    um.undo();
    um.undo();
    expect ("undo brings both notes back", notes.getNumChildren() == 2, juce::String (notes.getNumChildren()));
    expect ("piano-roll edits leave the session valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));

    // The controller lane: the pitch wheel, then the mod wheel, drawn with the mouse.
    beginGroup ("MIDI editor: controller lane (simulated mouse)");
    pr->setGridTicks (960);
    const auto lane = pr->laneArea();
    const auto xAt = [&] (juce::int64 tick) { return pr->positionFor (tick, 60).x + 2.0f; };
    click ({ xAt (960), pr->yForLaneValue (1.0f) });
    auto bend = midimodel::readControl (clipNode, midi::pitchBend);
    expect ("a click at the top of the Bend lane sets the pitch wheel to +1 from that grid position",
            lane.getHeight() > 0 && bend.size() == 1 && bend[0].tick == 960 && juce::exactlyEqual (bend[0].value, 1.0f),
            juce::String ((int) bend.size()) + " points");
    click ({ xAt (1920), pr->yForLaneValue (0.0f) + 1.0f });
    bend = midimodel::readControl (clipNode, midi::pitchBend);
    expect ("a click just off the centre snaps to rest (0)", bend.size() == 2 && juce::exactlyEqual (bend[1].value, 0.0f),
            bend.size() == 2 ? fmt (bend[1].value) : juce::String ((int) bend.size()) + " points");
    click ({ xAt (960), pr->yForLaneValue (1.0f) }, juce::ModifierKeys::altModifier);
    expect ("Alt-click deletes the point under the mouse", midimodel::readControl (clipNode, midi::pitchBend).size() == 1, {});
    um.undo();
    expect ("undo puts it back (one step)", midimodel::readControl (clipNode, midi::pitchBend).size() == 2, {});

    pr->setLaneControl (midi::modWheel);
    const juce::Point<float> from { xAt (0), pr->yForLaneValue (0.5f) };
    pr->mouseDown (mouseEvent (*pr, from, from, false, {}));
    for (int beat = 1; beat <= 3; ++beat)
    {
        const juce::Point<float> to { xAt (beat * 960), pr->yForLaneValue (0.5f) };
        pr->mouseDrag (mouseEvent (*pr, to, from, true, {}));
    }
    pr->mouseUp (mouseEvent (*pr, from, from, true, {}));
    const auto mod = midimodel::readControl (clipNode, midi::modWheel);
    bool halfway = mod.size() == 4;
    for (size_t i = 0; halfway && i < mod.size(); ++i)
        halfway = mod[i].tick == (int64_t) i * 960 && std::abs (mod[i].value - 0.5f) < 0.02f;
    expect ("dragging across the Mod lane writes a point per grid position at the drawn value (0.5 on beats 1-4)", halfway,
            juce::String ((int) mod.size()) + " points");
    um.undo();
    expect ("the whole drag is one undo step", midimodel::readControl (clipNode, midi::modWheel).empty(), {});
    expect ("controller edits leave the session valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));
    snapshot (*pr, "piano_roll_lane.png");

    editView.closePianoRoll();
    session->removeTrack (keys, nullptr);
    um.clearUndoHistory();
}

void SelfTest::testUserInterface()
{
    beginGroup ("Edit window (Smart Tool, simulated mouse)");
    auto* session = app.getSession();
    auto& um = session->getUndoManager();
    auto& editView = main.getEditView();
    auto& view = editView.getEditState();

    main.showWindow (WindowKind::edit);
    clearTracks();
    auto t = addToneTrack ("UI Test", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    addToneTrack ("UI Test 2", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    view.setView (0, 400.0);
    view.setTool (EditTool::smart);
    view.setMode (EditMode::slip);

    auto clipNode = track::clips (t).getChild (0);
    auto* lane = editView.getLane (0);
    auto* clipView = lane != nullptr ? lane->getClipComponent (0) : nullptr;
    expect ("clip appears in the Edit window", clipView != nullptr && clipView->isVisible(),
            clipView != nullptr ? clipView->getBounds().toString() : juce::String ("missing"));
    if (clipView == nullptr)
        return;

    const auto spp = view.samplesPerPixel;
    const float h = (float) clipView->getHeight();
    auto startX = [&] { return (float) (view.sampleToX (clip::start (clipNode)) - clipView->getX()); };
    auto endX = [&] { return (float) (view.sampleToX (clip::end (clipNode)) - clipView->getX()); };

    // Grabber zone (lower half): move the clip 100 px to the right.
    drag (*clipView, { startX() + 80.0f, h * 0.75f }, { 100.0f, 0.0f });
    expectNear ("Smart Tool lower half moves the clip", (double) clip::start (clipNode), (double) clipStart + 100.0 * spp, 0.0, "samples");
    um.undo();
    expectNear ("undo puts it back", (double) clip::start (clipNode), (double) clipStart, 0.0, "samples");

    // Right edge: trim 50 px shorter.
    const auto length = clip::length (clipNode);
    drag (*clipView, { endX() - 2.0f, h * 0.6f }, { -50.0f, 0.0f });
    expectNear ("Smart Tool right edge trims the end", (double) clip::length (clipNode), (double) length - 50.0 * spp, 0.0, "samples");

    // Left edge: trim 25 px; the audio must stay where it was on the timeline.
    const auto endBefore = clip::end (clipNode);
    drag (*clipView, { startX() + 2.0f, h * 0.6f }, { 25.0f, 0.0f });
    expect ("Smart Tool left edge trims the start, end stays put",
            clip::start (clipNode) == clipStart + (juce::int64) (25.0 * spp) && clip::end (clipNode) == endBefore
                && clip::offset (clipNode) == (juce::int64) (25.0 * spp),
            "start " + juce::String (clip::start (clipNode)) + ", offset " + juce::String (clip::offset (clipNode))
                + ", end " + juce::String (clip::end (clipNode)));

    // Top-left corner: fade-in.
    drag (*clipView, { startX() + 10.0f, 5.0f }, { 30.0f, 0.0f });
    expectNear ("Smart Tool top corner draws a fade-in", (double) clip::fadeIn (clipNode), 30.0 * spp, 0.0, "samples");

    // Clip gain: the handle at the bottom left, dragged 24 px up = +6 dB; double-click resets it.
    {
        const auto handle = clipView->gainHandleBounds();
        expect ("an audio clip shows its clip-gain handle", ! handle.isEmpty(), handle.toString());
        if (! handle.isEmpty())
        {
            drag (*clipView, handle.getCentre(), { 0.0f, -24.0f });
            const double dragged = clip::gainDb (clipNode);
            // An exact level needs the clean bus (the demo session above turned the console on)
            // and this track alone ("UI Test 2" plays the same tone).
            auto other = session->getTrack (1);
            setConsole (false);
            other.setProperty (ids::mute, true, nullptr);
            const auto louder = bounce ("clip_gain_plus6", 0, clip::end (clipNode));
            other.setProperty (ids::mute, false, nullptr);
            setConsole (true, 6.0, 0.35);
            expect ("dragging the clip-gain handle up 24 px sets +6 dB, and the clip plays 6 dB louder",
                    juce::exactlyEqual (dragged, 6.0) && std::abs (db (louder.left.peak) - (toneDb + panCentreDb + 6.0)) < 0.05,
                    fmt (dragged) + " dB set; peak " + fmt (db (louder.left.peak)) + " dB");
            snapshot (*clipView, "clip_gain.png");
            clipView->mouseDoubleClick (mouseEvent (*clipView, handle.getCentre(), handle.getCentre(), false));
            const double reset = clip::gainDb (clipNode);
            um.undo();
            expect ("double-clicking the handle resets to 0 dB (one undo step)",
                    juce::exactlyEqual (reset, 0.0) && juce::exactlyEqual (clip::gainDb (clipNode), 6.0),
                    fmt (reset) + " dB, then " + fmt (clip::gainDb (clipNode)) + " dB after undo");
            clipNode.setProperty (ids::gainDb, 0.0, nullptr);
        }
    }

    // Upper half: time selection (Selector).
    drag (*clipView, { startX() + 60.0f, h * 0.3f }, { 80.0f, 0.0f });
    expect ("Smart Tool upper half makes a time selection on this track",
            view.hasTimeRange() && view.selectionEnd - view.selectionStart == (juce::int64) (80.0 * spp)
                && view.selectedTrackIds.contains (track::id (t)),
            juce::String (view.selectionStart) + " .. " + juce::String (view.selectionEnd));

    // Menu commands on that selection.
    auto& commands = main.getCommandManager();
    commands.invokeDirectly (commands::separateClip, false);
    expect ("Separate Clip (Ctrl+E) splits at both selection edges", track::clips (t).getNumChildren() == 3,
            juce::String (track::clips (t).getNumChildren()) + " clips");
    commands.invokeDirectly (commands::deleteSelection, false);
    expect ("Delete clears the selected range", track::clips (t).getNumChildren() == 2,
            juce::String (track::clips (t).getNumChildren()) + " clips");
    commands.invokeDirectly (commands::undo, false);
    expect ("Undo (Ctrl+Z) restores it", track::clips (t).getNumChildren() == 3, juce::String (track::clips (t).getNumChildren()) + " clips");

    // Pencil: draw a rising volume line over the clips, then edit it.
    beginGroup ("Pencil (automation drawing, simulated mouse)");
    commands.invokeDirectly (commands::pencilTool, false);
    expect ("F10 selects the Pencil", view.tool == EditTool::pencil, {});

    const float laneHeight = (float) lane->getHeight();
    auto yFor = [&] (double dbValue) { return 4.0f + (laneHeight - 8.0f) * (float) (1.0 - dsp::FaderTaper::dbToPosition (dbValue)); };
    const juce::Point<float> strokeStart ((float) view.sampleToX (clipStart + 4800), laneHeight * 0.9f);

    lane->mouseDown (mouseEvent (*lane, strokeStart, strokeStart, false));
    for (int i = 1; i <= 10; ++i)
        lane->mouseDrag (mouseEvent (*lane, strokeStart + juce::Point<float> (i * 20.0f, -0.07f * laneHeight * (float) i), strokeStart, true));
    lane->mouseUp (mouseEvent (*lane, strokeStart, strokeStart, true));

    auto drawn = automation::points (automation::volumeLane (t));
    expect ("a pencil stroke over the clips creates breakpoints", drawn.size() >= 10, juce::String ((int) drawn.size()) + " points");
    if (drawn.size() >= 2)
    {
        expect ("the drawn line rises left to right", drawn.front().value < drawn.back().value,
                fmt (drawn.front().value, 1) + " dB to " + fmt (drawn.back().value, 1) + " dB");

        const auto first = drawn.front();
        const juce::Point<float> firstPos ((float) view.sampleToX (first.time), yFor (first.value));
        drag (*lane, firstPos, { 0.0f, -25.0f });
        const auto moved = automation::points (automation::volumeLane (t)).front();
        expect ("dragging a point with the Pencil moves it", moved.value > first.value + 1.0,
                fmt (first.value, 1) + " dB -> " + fmt (moved.value, 1) + " dB");

        const auto before = automation::points (automation::volumeLane (t));
        const juce::Point<float> lastPos ((float) view.sampleToX (before.back().time), yFor (before.back().value));
        lane->mouseDown (mouseEvent (*lane, lastPos, lastPos, false, juce::ModifierKeys::altModifier));
        lane->mouseUp (mouseEvent (*lane, lastPos, lastPos, false, juce::ModifierKeys::altModifier));
        expect ("Alt-click deletes a point", automation::points (automation::volumeLane (t)).size() == before.size() - 1,
                juce::String ((int) automation::points (automation::volumeLane (t)).size()) + " points left");

        um.undo();
        expect ("undo restores the deleted point", automation::points (automation::volumeLane (t)).size() == before.size(), {});
    }
    view.setTool (EditTool::smart);

    beginGroup ("Mix window (real controls)");
    main.showWindow (WindowKind::mix);
    auto* strip = main.getMixView().getStrip (0);
    expect ("channel strip exists for track 1", strip != nullptr, {});
    if (strip != nullptr)
    {
        strip->getFader().setValue (dsp::FaderTaper::dbToPosition (-6.0), juce::sendNotificationSync);
        expectNear ("fader move sets the track volume", track::volumeDb (t), -6.0, 1.0e-9, "dB");

        strip->getPanKnob().setValue (0.5, juce::sendNotificationSync);
        expect ("pan knob sets pan and shows 50>", juce::exactlyEqual (track::pan (t), 0.5) && strip->getPanReadout() == "50>",
                "pan " + fmt (track::pan (t)) + ", readout " + strip->getPanReadout());

        strip->getMuteButton().onClick();
        expect ("mute key mutes the track", track::mute (t), {});
        expect ("mute reaches the engine immediately", app.getBridge().getTrackState (track::id (t))->muted.load(), {});
        strip->getMuteButton().onClick();
    }

    const auto before = main.getActiveWindow();
    commands.invokeDirectly (commands::toggleWindow, false);
    expect ("Ctrl+= toggles Edit/Mix", main.getActiveWindow() != before, {});

    app.addTracks (2, 2, "Audio");
    expect ("New Tracks creates numbered tracks",
            session->getTracksNode().getChildWithProperty (ids::name, "Audio 1").isValid()
                && session->getTracksNode().getChildWithProperty (ids::name, "Audio 2").isValid(),
            juce::String (session->getNumTracks()) + " tracks");

    main.showWindow (WindowKind::edit);
}

//==============================================================================
// The track header's playlist selector, driven through its menu items (the
// self test has no mouse for pop-up menus; the items are what a click runs).

void SelfTest::testPlaylistSelector()
{
    beginGroup ("Edit window: playlist selector");
    auto* session = app.getSession();
    auto& editView = main.getEditView();
    auto& view = editView.getEditState();
    main.showWindow (WindowKind::edit);

    clearTracks();
    auto t = addToneTrack ("Take", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    session->addTrack ("Aux", 2, nullptr, Session::TrackKind::aux);
    auto* header = editView.getHeader (0);
    auto* lane = editView.getLane (0);
    expect ("each audio track header shows its active playlist", header != nullptr && header->getPlaylistButtonText() == "Take.01",
            header != nullptr ? header->getPlaylistButtonText() : juce::String ("no header"));
    if (header == nullptr || lane == nullptr)
        return;

    auto clipsShown = [lane]
    {
        int n = 0;
        while (lane->getClipComponent (n) != nullptr)
            ++n;
        return n;
    };
    auto enabledItems = [] (const juce::PopupMenu& menu)
    {
        juce::Array<int> found;
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
            if (it.getItem().itemID != 0 && it.getItem().isEnabled)
                found.add (it.getItem().itemID);
        return found;
    };

    header->runPlaylistItem (TrackHeader::newPlaylist);
    expect ("New Playlist from the header: the button reads Take.02 and the lane empties",
            header->getPlaylistButtonText() == "Take.02" && clipsShown() == 0,
            header->getPlaylistButtonText() + ", " + juce::String (clipsShown()) + " clips shown");

    session->addClip (t, media.getChildFile ("tone500_mono.wav"), clipStart, 0, 48000, 96000, "Take 2", nullptr);
    view.setTimeSelection (clipStart + 12000, clipStart + 24000, {}); // a range on another track (none): comping stays off
    auto items = enabledItems (header->buildPlaylistMenu());
    expect ("the menu lists both playlists and offers Delete for the other, but not Comp without a range on this track",
            items.contains (TrackHeader::switchTo + 0) && items.contains (TrackHeader::switchTo + 1)
                && items.contains (TrackHeader::deleteFrom + 0) && ! items.contains (TrackHeader::deleteFrom + 1)
                && ! items.contains (TrackHeader::compFrom + 0)
                && items.contains (TrackHeader::newPlaylist) && items.contains (TrackHeader::duplicatePlaylist),
            juce::String (items.size()) + " enabled items");

    header->runPlaylistItem (TrackHeader::switchTo + 0); // names are sorted: 0 = Take.01
    expect ("choosing Take.01 switches back: the lane shows take 1's clip",
            header->getPlaylistButtonText() == "Take.01" && clipsShown() == 1
                && clip::length (track::clips (t).getChild (0)) == 96000,
            header->getPlaylistButtonText() + ", " + juce::String (clipsShown()) + " clips shown");

    view.setTimeSelection (clipStart + 12000, clipStart + 24000, { track::id (t) });
    header->runPlaylistItem (TrackHeader::compFrom + 1); // 1 = Take.02
    expect ("Comp Selection From Take.02 replaces the selected range: three clips, the middle one from take 2",
            clipsShown() == 3 && track::clips (t).getNumChildren() == 3
                && clip::start (track::clips (t).getChild (2)) == clipStart + 12000
                && clip::length (track::clips (t).getChild (2)) == 12000
                && clip::offset (track::clips (t).getChild (2)) == 12000,
            juce::String (track::clips (t).getNumChildren()) + " clips");
    snapshot (*header, "playlist_selector.png");

    session->getUndoManager().undo();
    expect ("one undo removes the comp", track::clips (t).getNumChildren() == 1, juce::String (track::clips (t).getNumChildren()) + " clips");

    auto* auxHeader = editView.getHeader (1);
    expect ("aux tracks show no playlist selector and offer no playlist items",
            auxHeader != nullptr && auxHeader->getPlaylistButtonText().isEmpty() && auxHeader->buildPlaylistMenu().getNumItems() == 0,
            {});
    view.clearSelection();
    clearTracks();
}

//==============================================================================
// Shuffle and Spot edit modes, with the simulated mouse. Three clips butted
// together, A [0.5 s, 1 s), B [1 s, 1.5 s), C [1.5 s, 2 s), and a volume
// breakpoint inside C at 1.6 s, so every check is an exact sample position.

void SelfTest::testEditModes()
{
    beginGroup ("Edit window: Shuffle and Spot modes");
    auto* session = app.getSession();
    auto& um = session->getUndoManager();
    auto& editView = main.getEditView();
    auto& view = editView.getEditState();
    auto& commands = main.getCommandManager();
    main.showWindow (WindowKind::edit);

    clearTracks();
    const auto tone = media.getChildFile ("tone1k_mono.wav");
    auto t = session->addTrack ("Shuffle", 1, nullptr);
    auto a = session->addClip (t, tone, 24000, 0, 24000, 96000, "A", nullptr);
    auto b = session->addClip (t, tone, 48000, 24000, 24000, 96000, "B", nullptr);
    auto c = session->addClip (t, tone, 72000, 48000, 24000, 96000, "C", nullptr);
    auto lane = automation::getOrCreateVolumeLane (t, nullptr);
    automation::addPoint (lane, 76800, -6.0, nullptr);
    um.clearUndoHistory();
    view.setView (0, 400.0); // 400 samples per pixel: a 50 px drag is exactly 20000 samples
    view.setTool (EditTool::smart);

    auto starts = [&] { return juce::String (clip::start (a)) + " / " + juce::String (clip::start (b)) + " / " + juce::String (clip::start (c)); };
    auto pointTime = [&] { const auto p = automation::points (lane); return p.size() == 1 ? p[0].time : (juce::int64) -1; };
    auto viewOf = [&editView] (const juce::ValueTree& clipNode) -> ClipComponent*
    {
        if (auto* trackLane = editView.getLane (0))
            for (int i = 0; auto* cc = trackLane->getClipComponent (i); ++i)
                if (cc->getClip() == clipNode)
                    return cc;
        return nullptr;
    };

    commands.invokeDirectly (commands::shuffleMode, false);
    expect ("F1 selects Shuffle mode", view.mode == EditMode::shuffle, {});

    // Delete a clip: the gap closes, automation included.
    view.selectClip (b);
    commands.invokeDirectly (commands::deleteSelection, false);
    expect ("Shuffle: deleting B closes the gap (C moves left 24000 samples, and so does its automation)",
            track::clips (t).getNumChildren() == 2 && clip::start (a) == 24000 && clip::start (c) == 48000 && pointTime() == 52800,
            "A " + juce::String (clip::start (a)) + ", C " + juce::String (clip::start (c)) + ", point " + juce::String (pointTime()));
    // Measured on the clean bus: the console the demo session turned on would ring briefly after C stops.
    setConsole (false);
    const auto closed = readAudio (bounce ("shuffle_delete", 0, 120000).file);
    setConsole (true, 6.0, 0.35);
    const auto inGap = dsp::measureLevels (closed.getReadPointer (0, 48000), 24000);
    const auto afterEnd = dsp::measureLevels (closed.getReadPointer (0, 72000), 24000);
    // The tone at -6 dB (the breakpoint) through the centre pan law has an RMS of 0.125.
    expect ("and the audio really moved: C now plays where B was, and nothing plays where C was",
            inGap.rms > 0.1 && db (afterEnd.peak) < silentDb, "RMS " + fmt (inGap.rms) + ", then peak " + fmt (db (afterEnd.peak)) + " dB");
    um.undo();
    expect ("undo puts B back and C and its automation where they were",
            track::clips (t).getNumChildren() == 3 && clip::start (c) == 72000 && pointTime() == 76800, starts());

    // Clear a time range: everything after it moves left by its length.
    view.setTimeSelection (36000, 48000, { track::id (t) });
    commands.invokeDirectly (commands::deleteSelection, false);
    expect ("Shuffle: clearing 12000 samples of A moves B, C and the automation left by 12000",
            clip::length (a) == 12000 && clip::start (b) == 36000 && clip::start (c) == 60000 && pointTime() == 64800,
            starts() + ", point " + juce::String (pointTime()));
    um.undo();
    view.clearSelection();

    // Trim A's end 50 px shorter: B and C follow.
    if (auto* av = viewOf (a))
    {
        const float h = (float) av->getHeight();
        const auto endX = (float) (view.sampleToX (clip::end (a)) - av->getX());
        drag (*av, { endX - 2.0f, h * 0.6f }, { -50.0f, 0.0f });
    }
    expect ("Shuffle: trimming A's end 20000 samples shorter pulls B, C and the automation 20000 earlier",
            clip::length (a) == 4000 && clip::start (b) == 28000 && clip::start (c) == 52000 && pointTime() == 56800,
            starts() + ", point " + juce::String (pointTime()));
    um.undo();
    expect ("undo restores every clip and the automation in one step",
            clip::length (a) == 24000 && clip::start (b) == 48000 && clip::start (c) == 72000 && pointTime() == 76800, starts());

    // Trim A's start 25 px later: A keeps its place, its audio and everything after slide left.
    if (auto* av = viewOf (a))
    {
        const float h = (float) av->getHeight();
        const auto startX = (float) (view.sampleToX (clip::start (a)) - av->getX());
        drag (*av, { startX + 2.0f, h * 0.6f }, { 25.0f, 0.0f });
    }
    expect ("Shuffle: trimming A's start keeps A at 0.5 s and slides its audio, B, C and the automation 10000 earlier",
            clip::start (a) == 24000 && clip::offset (a) == 10000 && clip::length (a) == 14000
                && clip::start (b) == 38000 && clip::start (c) == 62000 && pointTime() == 66800,
            starts() + ", A offset " + juce::String (clip::offset (a)) + ", point " + juce::String (pointTime()));
    um.undo();

    // Drag C to the front: it drops in at A's start and A and B make room.
    if (auto* cv = viewOf (c))
    {
        const float h = (float) cv->getHeight();
        const auto startX = (float) (view.sampleToX (clip::start (c)) - cv->getX());
        drag (*cv, { startX + 30.0f, h * 0.75f }, { -120.0f, 0.0f });
    }
    expect ("Shuffle: dragging C to the front reorders the clips to C, A, B, still butted together",
            clip::start (c) == 24000 && clip::start (a) == 48000 && clip::start (b) == 72000, starts());
    um.undo();
    expect ("undo restores the order A, B, C", clip::start (a) == 24000 && clip::start (b) == 48000 && clip::start (c) == 72000, starts());

    // Spot: a click asks for a time instead of moving the clip.
    commands.invokeDirectly (commands::spotMode, false);
    if (auto* cv = viewOf (c))
    {
        const float h = (float) cv->getHeight();
        const auto startX = (float) (view.sampleToX (clip::start (c)) - cv->getX());
        drag (*cv, { startX + 30.0f, h * 0.75f }, { -40.0f, 0.0f });
    }
    auto* modal = juce::ModalComponentManager::getInstance()->getModalComponent (0);
    expect ("F3 selects Spot mode; dragging a clip opens the Spot dialog and does not move it",
            view.mode == EditMode::spot && modal != nullptr && modal->getName() == "Spot Clip" && clip::start (c) == 72000,
            (modal != nullptr ? modal->getName() : juce::String ("no dialog")) + ", C at " + juce::String (clip::start (c)));
    juce::ModalComponentManager::getInstance()->cancelAllModalComponents();

    bool typed = false, samples = false, refused = false;
    if (auto* cv = viewOf (c))
    {
        typed = cv->spotTo ("0:02.000") && clip::start (c) == 96000;
        samples = cv->spotTo ("12345 smp") && clip::start (c) == 12345;
        refused = ! cv->spotTo ("1:75") && ! cv->spotTo ("abc") && ! cv->spotTo ("") && clip::start (c) == 12345;
    }
    expect ("Spot: \"0:02.000\" puts C at exactly 2 s (96000 samples), and \"12345 smp\" at that sample", typed && samples, starts());
    expect ("Spot refuses text that is not a time (\"1:75\", \"abc\", empty) and leaves the clip alone", refused, starts());
    um.undo();
    expect ("each spot is one undo step", clip::start (c) == 96000, starts());

    commands.invokeDirectly (commands::slipMode, false);
    clearTracks();
}

//==============================================================================
// Automation lanes in the Edit window: the header's view selector picks the
// lane, and the Pencil draws on it in that lane's own scale.

void SelfTest::testAutomationLanes()
{
    beginGroup ("Edit window: automation lanes (Pencil, simulated mouse)");
    auto* session = app.getSession();
    auto& editView = main.getEditView();
    auto& view = editView.getEditState();
    auto& tools = app.getRemoteTools();
    main.showWindow (WindowKind::edit);

    clearTracks();
    auto t = addToneTrack ("Lanes", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    session->addTrack ("Verb", 2, nullptr, Session::TrackKind::aux);
    view.setView (0, 400.0);
    auto* header = editView.getHeader (0);
    auto* lane = editView.getLane (0);
    if (header == nullptr || lane == nullptr)
    {
        expect ("the track has a header and a lane", false, {});
        return;
    }

    auto itemIds = [] (const juce::PopupMenu& menu)
    {
        juce::Array<int> found;
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
            if (it.getItem().itemID != 0)
                found.add (it.getItem().itemID);
        return found;
    };
    expect ("each track header shows its automation view, volume by default; the menu offers volume, pan and mute",
            header->getViewButtonText() == "volume" && itemIds (header->buildViewMenu()) == juce::Array<int> { 1, 2, 3 },
            header->getViewButtonText());

    const float h = (float) lane->getHeight();
    auto click = [this, lane] (float x, float y) { drag (*lane, { x, y }, { 0.0f, 0.0f }); };
    auto pointsOf = [&t] (const juce::String& parameter) { return automation::points (automation::lane (t, parameter)); };
    view.setTool (EditTool::pencil);

    // Pan: the top is hard left, the bottom hard right.
    header->runViewItem (2);
    click (100.0f, 4.0f);
    click (200.0f, h - 4.0f);
    const auto pan = pointsOf ("pan");
    expect ("Pan view: Pencil clicks at the top and bottom write hard left at 40000 and hard right at 80000; volume is untouched",
            header->getViewButtonText() == "pan" && pan.size() == 2 && pan[0].time == 40000 && juce::exactlyEqual (pan[0].value, -1.0)
                && pan[1].time == 80000 && juce::exactlyEqual (pan[1].value, 1.0) && pointsOf ("volume").empty(),
            juce::String ((int) pan.size()) + " points");

    // The drawn lane plays: before its first point it holds hard left.
    setConsole (false);
    const auto panned = readAudio (bounce ("lane_pan", 0, 40000).file);
    setConsole (true, 6.0, 0.35);
    const auto left = dsp::measureLevels (panned.getReadPointer (0, (int) clipStart), 12000);
    const auto right = dsp::measureLevels (panned.getReadPointer (1, (int) clipStart), 12000);
    expect ("and it plays: hard left puts the whole tone on the left and nothing on the right",
            std::abs (db (left.peak) - toneDb) < 0.05 && db (right.peak) < silentDb,
            "left " + fmt (db (left.peak)) + " dB, right " + fmt (db (right.peak)) + " dB");

    // Created after that bounce: the bus would put the tone on both sides.
    tools.call ("set_send", juce::JSON::parse (R"({"track":"Lanes","slot":"A","bus":"Verb","level_db":-10})"));
    expect ("a track with a send also offers that send's level", itemIds (header->buildViewMenu()).contains (10), {});

    // Mute: steps only, muted at the top.
    header->runViewItem (3);
    click (150.0f, 6.0f);
    click (250.0f, h * 0.6f);
    const auto mute = pointsOf ("mute");
    expect ("Mute view: the Pencil writes exact steps (1 = muted near the top, 0 below the middle)",
            header->getViewButtonText() == "mute" && mute.size() == 2 && juce::exactlyEqual (mute[0].value, 1.0)
                && juce::exactlyEqual (mute[1].value, 0.0),
            juce::String ((int) mute.size()) + " points");

    // Send A: the fader's scale.
    header->runViewItem (10);
    const float yMinus6 = 4.0f + (h - 8.0f) * (float) (1.0 - dsp::FaderTaper::dbToPosition (-6.0));
    click (300.0f, yMinus6);
    const auto sendPoints = pointsOf ("send:0");
    expect ("Send A view: a click at the fader scale's -6 dB writes -6 dB on the send lane",
            header->getViewButtonText() == "send A" && sendPoints.size() == 1 && std::abs (sendPoints[0].value + 6.0) < 0.01,
            sendPoints.empty() ? juce::String ("no point") : fmt (sendPoints[0].value) + " dB");
    snapshot (*lane, "automation_lane_send.png");

    // A plug-in control: its normalised range, 1 at the top.
    juce::PluginDescription eq;
    for (const auto& d : BuiltinEffectFormat::getDescriptions())
        if (d.name == "Meridian EQ")
            eq = d;
    juce::String error;
    app.getBridge().insertPlugin (t, 0, eq, error);
    int bandGain = -1;
    if (const auto insert = app.getBridge().getInsert (track::id (t), 0); insert != nullptr && insert->plugin != nullptr)
        for (int i = 0; i < insert->plugin->getParameters().size(); ++i)
            if (insert->plugin->getParameters()[i]->getName (64) == "Band 2 Gain")
                bandGain = i;
    expect ("an inserted plug-in's controls are offered in a submenu",
            bandGain >= 0 && itemIds (header->buildViewMenu()).contains (TrackHeader::pluginItemBase + bandGain), error);
    header->runViewItem (TrackHeader::pluginItemBase + juce::jmax (0, bandGain));
    click (350.0f, 4.0f);
    click (450.0f, h - 4.0f);
    const auto controlPoints = pointsOf (automation::pluginParameter (0, bandGain));
    expect ("Insert A view: the header reads \"A: Band 2 Gain\", and Pencil clicks write the control's full range (1 at the top, 0 at the bottom)",
            header->getViewButtonText() == "A: Band 2 Gain" && controlPoints.size() == 2
                && juce::exactlyEqual (controlPoints[0].value, 1.0) && juce::exactlyEqual (controlPoints[1].value, 0.0),
            header->getViewButtonText() + ", " + juce::String ((int) controlPoints.size()) + " points");
    tools.call ("remove_plugin", juce::JSON::parse (R"({"track":"Lanes","slot":"A"})"));
    expect ("removing the plug-in returns the view to volume", header->getViewButtonText() == "volume", header->getViewButtonText());

    // Removing the send takes its view with it.
    header->runViewItem (10);
    tools.call ("remove_send", juce::JSON::parse (R"({"track":"Lanes","slot":"A"})"));
    expect ("removing the send returns the view to volume", header->getViewButtonText() == "volume", header->getViewButtonText());

    view.setTool (EditTool::smart);
    clearTracks();
}

//==============================================================================
// The Tempo ruler: adding, undoing and deleting tempo and time-signature changes.

void SelfTest::testTempoRuler()
{
    beginGroup ("Edit window: Tempo ruler");
    auto* session = app.getSession();
    auto& um = session->getUndoManager();
    auto& ruler = main.getEditView().getRuler();
    midimodel::clearTempoAndMeterChanges (*session, nullptr);
    session->getState().setProperty (ids::tempo, 120.0, nullptr);
    um.clearUndoHistory();

    const auto start = juce::StringArray { "120 bpm 4/4" };
    expect ("the Tempo ruler shows the starting tempo and time signature", ruler.getTempoLabels() == start,
            ruler.getTempoLabels().joinIntoString (" | "));

    // Bar 5 (0-based 4) starts at tick 4 x 3840 = 15360 in 4/4.
    ruler.setTempoAtBar (4, 90.5, 3, 4);
    auto tempo = midimodel::tempoMap (*session);
    auto meter = midimodel::meterMap (*session);
    expect ("a change at bar 5: 90.5 bpm and 3/4 from its first tick, and the ruler shows it",
            juce::exactlyEqual (tempo.bpmAt (15360), 90.5) && juce::exactlyEqual (tempo.bpmAt (15359), 120.0)
                && meter.meterAt (4).numerator == 3 && meter.meterAt (3).numerator == 4
                && ruler.getTempoLabels() == juce::StringArray { "120 bpm 4/4", "90.5 bpm 3/4" },
            ruler.getTempoLabels().joinIntoString (" | "));

    um.undo();
    expect ("one undo removes both the tempo and the time signature", ruler.getTempoLabels() == start,
            ruler.getTempoLabels().joinIntoString (" | "));
    um.redo();
    ruler.deleteTempoChange (15360, 4);
    expect ("Delete This Change removes them", ruler.getTempoLabels() == start, ruler.getTempoLabels().joinIntoString (" | "));

    ruler.setTempoAtBar (0, 100.0, 4, 4);
    expect ("editing bar 1 sets the starting tempo (and adds no time signature: 4/4 is unchanged)",
            juce::exactlyEqual (session->getTempo(), 100.0) && ruler.getTempoLabels() == juce::StringArray { "100 bpm 4/4" }
                && session->getState().getChildWithName (ids::METER_MAP).getNumChildren() == 0,
            ruler.getTempoLabels().joinIntoString (" | "));
    expect ("tempo edits leave the session valid", findSessionProblem (*session).isEmpty(), findSessionProblem (*session));

    // A ramp: 120 BPM gliding to 240 over bar 1 (four beats) takes 2 ln 2 s, so bar 2
    // starts at 1.3863 s; a note there sounds on exactly that sample.
    session->getState().setProperty (ids::tempo, 120.0, nullptr);
    ruler.setTempoAtBar (1, 240.0, 4, 4);
    ruler.setRampAt (0, true);
    const auto rampEnd = (int) std::llround (2.0 * std::log (2.0) * sampleRate);
    expect ("Ramp to Next Change: the ruler shows the ramp, and bar 2 starts after exactly 2 ln 2 s",
            ruler.getTempoLabels() == juce::StringArray { "120 bpm (ramp) 4/4", "240 bpm" }
                && midimodel::tempoMap (*session).ticksToSamples (3840, sampleRate) == rampEnd,
            ruler.getTempoLabels().joinIntoString (" | ") + "; bar 2 at "
                + juce::String (midimodel::tempoMap (*session).ticksToSamples (3840, sampleRate)) + ", expected " + juce::String (rampEnd));

    auto& tools = app.getRemoteTools();
    tools.call ("create_track", juce::JSON::parse (R"({"kind":"instrument","name":"Ramp"})"));
    tools.call ("add_midi_clip", juce::JSON::parse (R"({"track":"Ramp","start_beat":0,"length_beats":8})"));
    tools.call ("add_notes", juce::JSON::parse (R"({"track":"Ramp","clip":1,"notes":[{"pitch":69,"start_beats":4,"length_beats":1,"velocity":1}]})"));
    {
        // On the clean bus: the console's linear-phase oversampling filter (61 samples)
        // would ring ahead of the attack and blur where it starts.
        setConsole (false);
        const auto audio = readAudio (bounce ("tempo_ramp_note", 0, (juce::int64) (2.0 * sampleRate)).file);
        setConsole (true, 6.0, 0.35);
        int onset = -1;
        for (int i = 0; i < audio.getNumSamples() && onset < 0; ++i)
            if (! juce::exactlyEqual (audio.getSample (0, i), 0.0f))
                onset = i;
        // As in the tempo-map test: the synth's oversampling filter rings up to 16 samples ahead.
        expect ("a note on bar 2 after the ramp sounds at exactly 2 ln 2 s",
                onset >= rampEnd - synth::PolySynth::getLatencySamples() && onset <= rampEnd,
                "first sound at " + juce::String (onset) + ", bar 2 at " + juce::String (rampEnd));
    }
    clearTracks();
    ruler.setRampAt (0, false);
    expect ("Ramp to Next Change toggles off again: bar 2 is back at 2 s",
            midimodel::tempoMap (*session).ticksToSamples (3840, sampleRate) == (juce::int64) (2.0 * sampleRate),
            juce::String (midimodel::tempoMap (*session).ticksToSamples (3840, sampleRate)));

    session->getState().setProperty (ids::tempo, 120.0, nullptr);
    midimodel::clearTempoAndMeterChanges (*session, nullptr);
    um.clearUndoHistory();
}

//==============================================================================
// Mix window sections: hiding Inserts F-J and Sends A-E gives the fader the room.

void SelfTest::testMixSections()
{
    beginGroup ("Mix window: Inserts F-J and Sends A-E shown or hidden");
    auto& mix = main.getMixView();
    auto& commands = main.getCommandManager();
    clearTracks();
    addToneTrack ("Strip", 1, media.getChildFile ("tone1k_mono.wav"), clipStart);
    main.showWindow (WindowKind::mix);

    const auto saved = mix.getSections(); // the user's setting: restored at the end
    mix.setSections ({ true, true });
    auto* strip = mix.getStrip (0);
    if (strip == nullptr)
    {
        expect ("the track has a channel strip", false, {});
        return;
    }
    const int fullStrip = strip->getHeight(), fullFader = strip->getFader().getHeight();

    commands.invokeDirectly (commands::mixShowInsertsFJ, false);
    commands.invokeDirectly (commands::mixShowSends, false);
    const int compactStrip = strip->getHeight(), compactFader = strip->getFader().getHeight();
    expect ("Window > Mix Window: Inserts F-J and Sends A-E turn off, in the view and in every strip",
            ! mix.getSections().insertsFJ && ! mix.getSections().sends && ! strip->getSections().insertsFJ && ! strip->getSections().sends,
            {});
    expect ("a full strip needs 680 px and a compact one 196 px less (two sections of 98 px)",
            ChannelStrip::minimumHeight ({ true, true }) == 680 && ChannelStrip::minimumHeight ({ false, false }) == 484,
            juce::String (ChannelStrip::minimumHeight ({ false, false })) + " px");
    expect ("the fader gets exactly the room the two sections gave up",
            compactFader - fullFader == (compactStrip - fullStrip) + 196,
            "fader " + juce::String (fullFader) + " -> " + juce::String (compactFader) + " px, strip "
                + juce::String (fullStrip) + " -> " + juce::String (compactStrip) + " px");
    snapshot (mix, "mix_compact.png");

    commands.invokeDirectly (commands::mixShowInsertsFJ, false);
    commands.invokeDirectly (commands::mixShowSends, false);
    expect ("the same commands show them again, with the fader back to its full-strip size",
            mix.getSections().insertsFJ && mix.getSections().sends && strip->getFader().getHeight() == fullFader,
            juce::String (strip->getFader().getHeight()) + " px");

    mix.setSections (saved);
    main.showWindow (WindowKind::edit);
    clearTracks();
}

void SelfTest::snapshot (juce::Component& component, const juce::String& fileName)
{
    const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f);
    juce::PNGImageFormat png;
    const auto file = output.getChildFile ("screens").getChildFile (fileName);
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream stream (file);
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    expect ("screenshot " + fileName, ok && image.isValid(), file.getFullPathName());
}

void SelfTest::captureScreens()
{
    beginGroup ("Screens");
    main.showWindow (WindowKind::edit);
    snapshot (main, "01_edit_window.png");
    main.showWindow (WindowKind::mix);
    snapshot (main, "02_mix_window.png");
    main.showWindow (WindowKind::edit);

    DashboardComponent dashboard (app, [] (std::function<void()>) {});
    snapshot (dashboard, "03_dashboard.png");

    PlaybackEngineComponent playbackEngine (app);
    snapshot (playbackEngine, "04_playback_engine.png");

    NewTracksComponent newTracks (app);
    snapshot (newTracks, "05_new_tracks.png");

    auto firstClip = track::clips (app.getSession()->getTrack (0)).getChild (0);
    TimeStretchComponent elastic (app, firstClip);
    snapshot (elastic, "06_elastic_audio.png");

    // The MIDI editor on the showcase's chord clip.
    const auto synth = app.getSession()->getTracksNode().getChildWithProperty (ids::name, "Synth");
    main.getEditView().openPianoRoll (track::clips (synth).getChild (0));
    if (auto* pr = main.getEditView().getPianoRoll())
        snapshot (*pr, "07_piano_roll.png");
    else
        expect ("MIDI editor opens for the screenshot", false, {});

    // On a drum track the rows are the kit's drums.
    const auto drums = app.getSession()->getTracksNode().getChildWithProperty (ids::name, "Drum Machine");
    main.getEditView().openPianoRoll (track::clips (drums).getChild (0));
    if (auto* pr = main.getEditView().getPianoRoll())
        snapshot (*pr, "08_drum_editor.png");
    else
        expect ("drum editor opens for the screenshot", false, {});
    main.getEditView().closePianoRoll();
}

void SelfTest::finish()
{
    if (recordingToDelete != juce::File())
    {
        beginGroup ("Recording (microphone)");
        recordingToDelete.deleteFile();
        expect ("test recording deleted afterwards", ! recordingToDelete.exists(), recordingToDelete.getFileName());
    }

    if (assertionLog != nullptr)
    {
        beginGroup ("Internal consistency");
        auto fired = assertionLog->getFailures();

        // On machines without audio hardware (CI servers), JUCE's device layer
        // asserts when the platform's audio/MIDI service cannot be opened at all
        // (e.g. no ALSA sequencer). That is a property of the machine, so those
        // are reported as skipped; every other assertion still fails the run.
        if (allowNoAudioDevice)
        {
            juce::StringArray environmental;
            for (int i = fired.size(); --i >= 0;)
                if (fired[i].contains ("juce_Midi_") || fired[i].contains ("juce_ALSA") || fired[i].contains ("juce_JackAudio"))
                {
                    environmental.add (fired[i]);
                    fired.remove (i);
                }
            if (! environmental.isEmpty())
                skip ("audio/MIDI device layer assertions", "no audio or MIDI service on this machine: "
                                                               + environmental.joinIntoString ("; "));
        }
        expect ("no JUCE assertion fired during the whole run", fired.isEmpty(), fired.joinIntoString ("; "));
    }

    int failures = 0, skipped = 0;
    juce::String text;
    std::string json = "[";

    juce::String currentGroup;
    for (const auto& c : checks)
    {
        if (c.group != currentGroup)
        {
            currentGroup = c.group;
            text << "\n== " << c.group << " ==\n";
        }
        text << (c.skipped ? "[SKIP] " : c.passed ? "[PASS] " : "[FAIL] ") << c.name << "\n       " << c.detail << "\n";
        failures += c.passed ? 0 : 1;
        skipped += c.skipped ? 1 : 0;

        if (json.size() > 1)
            json += ",";
        json += remote::JsonObject()
                    .addString ("group", c.group.toStdString())
                    .addString ("name", c.name.toStdString())
                    .addBool ("passed", c.passed)
                    .addBool ("skipped", c.skipped)
                    .addString ("detail", c.detail.toStdString())
                    .toString();
    }
    json += "]";

    text = "Meridian self-test: " + juce::String ((int) checks.size() - failures - skipped) + " passed, "
           + juce::String (failures) + " failed"
           + (skipped > 0 ? ", " + juce::String (skipped) + " skipped" : juce::String())
           + "\n" + text;
    output.getChildFile ("report.txt").replaceWithText (text);
    output.getChildFile ("report.json").replaceWithText (json);

    app.showProblem = nullptr;
    if (onFinished)
        onFinished (failures);
}

} // namespace meridian
