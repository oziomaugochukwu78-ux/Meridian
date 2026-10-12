#include "SelfTest.h"

#include "CommandIds.h"
#include "media/AudioImporter.h"
#include "model/Routing.h"
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
        testRemoteCommands();
    }

    phase = 1;
    startTimer (100);
}

//==============================================================================
void SelfTest::expect (const juce::String& name, bool passed, const juce::String& detail)
{
    checks.push_back ({ group, name, passed, detail });
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
                track::name (lt) == "Saved Track" && track::volumeDb (lt) == -3.5 && track::pan (lt) == -0.25,
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
                track::outputBus (lt) == busId && ls.isValid() && send::target (ls) == track::id (bus2) && send::levelDb (ls) == -9.5,
                "output " + juce::String (track::outputBus (lt)) + ", send level " + fmt (send::levelDb (ls)));
    }
    else
    {
        expect ("reload routed session", false, error);
    }
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
    expect ("SET_VOLUME_TRACK_2_-12.5dB sets the fader", track::volumeDb (t2) == -12.5, reply);
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
                startLivePlayback();
                ticks = 0;
                phase = 2;
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
    expect ("take starts where recording started", clip::start (c) == recordStartPosition,
            "clip at " + juce::String (clip::start (c)) + ", recording started at " + juce::String (recordStartPosition));
    expect ("take is stored in the session's Audio Files folder", file.isAChildOf (app.getSession()->getAudioFolder()),
            file.getFileName());

    {
        std::unique_ptr<juce::AudioFormatReader> reader (app.getFormatManager().createReaderFor (file));
        expect ("take file is a valid 24-bit, 48 kHz WAV",
                reader != nullptr && reader->bitsPerSample == 24 && reader->sampleRate == 48000.0,
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

    beginGroup ("Mix window (real controls)");
    main.showWindow (WindowKind::mix);
    auto* strip = main.getMixView().getStrip (0);
    expect ("channel strip exists for track 1", strip != nullptr, {});
    if (strip != nullptr)
    {
        strip->getFader().setValue (dsp::FaderTaper::dbToPosition (-6.0), juce::sendNotificationSync);
        expectNear ("fader move sets the track volume", track::volumeDb (t), -6.0, 1.0e-9, "dB");

        strip->getPanKnob().setValue (0.5, juce::sendNotificationSync);
        expect ("pan knob sets pan and shows 50>", track::pan (t) == 0.5 && strip->getPanReadout() == "50>",
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

    int failures = 0;
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
        text << (c.passed ? "[PASS] " : "[FAIL] ") << c.name << "\n       " << c.detail << "\n";
        failures += c.passed ? 0 : 1;

        if (json.size() > 1)
            json += ",";
        json += remote::JsonObject()
                    .addString ("group", c.group.toStdString())
                    .addString ("name", c.name.toStdString())
                    .addBool ("passed", c.passed)
                    .addString ("detail", c.detail.toStdString())
                    .toString();
    }
    json += "]";

    text = "Meridian self-test: " + juce::String ((int) checks.size() - failures) + " passed, "
           + juce::String (failures) + " failed\n" + text;
    output.getChildFile ("report.txt").replaceWithText (text);
    output.getChildFile ("report.json").replaceWithText (json);

    app.showProblem = nullptr;
    if (onFinished)
        onFinished (failures);
}

} // namespace meridian
