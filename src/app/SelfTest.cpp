#include "SelfTest.h"

#include "CommandIds.h"
#include "media/AudioImporter.h"
#include "model/Automation.h"
#include "model/Routing.h"
#include "model/SessionRepair.h"
#include "media/TimeStretchEngine.h"
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
        testSessionRoundTrip();
        testImportAndResampling();
        testElasticAudio();
        testPluginHosting();
        testBusesAndSends();
        testGoldenRenders();
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
   #if JUCE_DEBUG
    skip ("renders faster than real time (" + juce::String (numTracks + numBuses) + " channels)",
          "not judged in a Debug build (unoptimised, ~10x slower): " + speed);
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
                                        "abc", "", ".", "..", "999999999999", "-999999999999", "aux", "audio", "read" };
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
        &ids::slot, &ids::description, &ids::bypass, &ids::parameter, &ids::time, &ids::value
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
           "<TRACK id=\"7\" name=\"Dup\" kind=\"weird\"/>"
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
    expect ("tools/list describes every tool with a JSON schema",
            toolArray != nullptr && toolArray->size() == 25 && (*toolArray)[0]["inputSchema"].isObject(),
            juce::String (toolArray != nullptr ? toolArray->size() : 0) + " tools");

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
        expect ("the drawn line rises left to right", drawn.front().db < drawn.back().db,
                fmt (drawn.front().db, 1) + " dB to " + fmt (drawn.back().db, 1) + " dB");

        const auto first = drawn.front();
        const juce::Point<float> firstPos ((float) view.sampleToX (first.time), yFor (first.db));
        drag (*lane, firstPos, { 0.0f, -25.0f });
        const auto moved = automation::points (automation::volumeLane (t)).front();
        expect ("dragging a point with the Pencil moves it", moved.db > first.db + 1.0,
                fmt (first.db, 1) + " dB -> " + fmt (moved.db, 1) + " dB");

        const auto before = automation::points (automation::volumeLane (t));
        const juce::Point<float> lastPos ((float) view.sampleToX (before.back().time), yFor (before.back().db));
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
