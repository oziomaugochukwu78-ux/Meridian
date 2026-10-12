# Creates a short demo song as a Meridian session: "First Melody", 100 BPM, A minor,
# Am - F - C - G. Four stereo tracks (Lead, Chords, Bass, Drums) are synthesised here
# in code (Meridian has no software instruments yet) and placed on the timeline,
# with the master console set to a gentle warm saturation. Prints the session file.
#
#   $s = .\tools\make-demo-song.ps1
#   & .\build\app\src\Meridian_artefacts\Release\Meridian.exe $s
#
# Everything is deterministic: the same script always writes the same audio.
param(
    [string]$Root = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Meridian Sessions'),
    [string]$Name = 'First Melody'
)

$ErrorActionPreference = 'Stop'

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.IO;

public static class DemoSynth
{
    public const int Rate = 48000;
    public const double Bpm = 100.0;
    public const double Beat = 60.0 / Bpm;

    static double Freq (int midi) { return 440.0 * Math.Pow (2.0, (midi - 69) / 12.0); }

    public static double[][] NewTrack (double seconds)
    {
        int n = (int) (seconds * Rate);
        return new[] { new double[n], new double[n] };
    }

    // Linear attack, exponential-ish decay towards sustain, linear release after the note ends.
    static double Envelope (double t, double length, double attack, double decay, double sustain, double release)
    {
        double e;
        if (t < attack) e = t / attack;
        else e = sustain + (1.0 - sustain) * Math.Exp (-(t - attack) / decay);
        if (t > length) e *= Math.Max (0.0, 1.0 - (t - length) / release);
        return e;
    }

    static void Write (double[][] track, int i, double value, double pan)
    {
        if (i < 0 || i >= track[0].Length) return;
        double angle = (pan + 1.0) * Math.PI / 4.0; // constant-power pan
        track[0][i] += value * Math.Cos (angle);
        track[1][i] += value * Math.Sin (angle);
    }

    // Lead: a soft bell-like pluck. Upper harmonics fade faster than the fundamental; light vibrato.
    public static void Lead (double[][] track, double startBeat, double beats, int midi, double gain)
    {
        double f = Freq (midi), start = startBeat * Beat, length = beats * Beat - 0.03, release = 0.25;
        int first = (int) (start * Rate), count = (int) ((length + release) * Rate);
        double phase = 0.0;
        for (int i = 0; i < count; i++)
        {
            double t = (double) i / Rate;
            double vibrato = 1.0 + 0.0025 * Math.Sin (2 * Math.PI * 5.2 * t) * Math.Min (1.0, Math.Max (0.0, (t - 0.25) * 3.0));
            phase += 2 * Math.PI * f * vibrato / Rate;
            double s = 0.0;
            for (int k = 1; k <= 6; k++)
                s += Math.Sin (k * phase) / Math.Pow (k, 1.6) * Math.Exp (-t * 1.4 * k);
            s += 0.18 * Math.Sin (phase); // a little steady body under the pluck
            Write (track, first + i, gain * s * Envelope (t, length, 0.006, 0.5, 0.45, release), 0.1);
        }
    }

    // Chords: three slightly detuned band-limited saws per note, spread left/right, slow attack.
    public static void Pad (double[][] track, double startBeat, double beats, int[] notes, double gain)
    {
        double start = startBeat * Beat, length = beats * Beat, release = 0.8;
        int first = (int) (start * Rate), count = (int) ((length + release) * Rate);
        double[] detune = { -0.006, 0.0, 0.006 };
        double[] pans = { -0.7, 0.0, 0.7 };
        foreach (int midi in notes)
            for (int d = 0; d < 3; d++)
            {
                double f = Freq (midi) * (1.0 + detune[d]), phase = d * 1.3;
                for (int i = 0; i < count; i++)
                {
                    double t = (double) i / Rate;
                    phase += 2 * Math.PI * f / Rate;
                    double s = 0.0;
                    for (int k = 1; k <= 8 && f * k < 9000.0; k++)
                        s += Math.Sin (k * phase) / Math.Pow (k, 1.4);
                    Write (track, first + i, gain * s * Envelope (t, length, 0.35, 1.0, 0.8, release), pans[d]);
                }
            }
    }

    // Bass: rounded sine with a touch of 2nd and 3rd harmonic so it is audible on small speakers.
    public static void Bass (double[][] track, double startBeat, double beats, int midi, double gain)
    {
        double f = Freq (midi), start = startBeat * Beat, length = beats * Beat - 0.04, release = 0.08;
        int first = (int) (start * Rate), count = (int) ((length + release) * Rate);
        double phase = 0.0;
        for (int i = 0; i < count; i++)
        {
            double t = (double) i / Rate;
            phase += 2 * Math.PI * f / Rate;
            double s = Math.Sin (phase) + 0.3 * Math.Sin (2 * phase) + 0.12 * Math.Sin (3 * phase);
            Write (track, first + i, gain * s * Envelope (t, length, 0.005, 0.3, 0.6, release), 0.0);
        }
    }

    static uint noiseState = 0x2545F491u;
    static double Noise ()
    {
        noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
        return noiseState / 2147483648.0 - 1.0;
    }

    public static void Kick (double[][] track, double beat, double gain)
    {
        int first = (int) (beat * Beat * Rate), count = (int) (0.45 * Rate);
        double phase = 0.0;
        for (int i = 0; i < count; i++)
        {
            double t = (double) i / Rate;
            double f = 45.0 + 85.0 * Math.Exp (-t * 30.0); // pitch drop: the "thump"
            phase += 2 * Math.PI * f / Rate;
            double click = t < 0.003 ? 0.4 * (1.0 - t / 0.003) : 0.0;
            Write (track, first + i, gain * (Math.Sin (phase) * Math.Exp (-t * 7.0) + click), 0.0);
        }
    }

    public static void Snare (double[][] track, double beat, double gain)
    {
        int first = (int) (beat * Beat * Rate), count = (int) (0.3 * Rate);
        double phase = 0.0, previous = 0.0;
        for (int i = 0; i < count; i++)
        {
            double t = (double) i / Rate;
            phase += 2 * Math.PI * 185.0 / Rate;
            double n = Noise (), bright = n - 0.5 * previous; previous = n;
            double s = 0.55 * bright * Math.Exp (-t * 16.0) + 0.5 * Math.Sin (phase) * Math.Exp (-t * 28.0);
            Write (track, first + i, gain * s, 0.05);
        }
    }

    public static void Hat (double[][] track, double beat, double gain)
    {
        int first = (int) (beat * Beat * Rate), count = (int) (0.08 * Rate);
        double previous = 0.0;
        for (int i = 0; i < count; i++)
        {
            double t = (double) i / Rate;
            double n = Noise (), high = n - previous; previous = n; // first difference: keeps the highs
            Write (track, first + i, gain * 0.5 * high * Math.Exp (-t * 55.0), 0.25);
        }
    }

    // A dotted-eighth stereo echo, mixed in with the dry signal.
    public static void Echo (double[][] track, double mix, double feedback)
    {
        int delay = (int) (0.75 * Beat * Rate);
        for (int ch = 0; ch < 2; ch++)
        {
            double[] x = track[ch], wet = new double[x.Length];
            int d = ch == 0 ? delay : delay + 480; // 10 ms offset between sides: width
            for (int i = d; i < x.Length; i++)
                wet[i] = x[i - d] + feedback * wet[i - d];
            for (int i = 0; i < x.Length; i++)
                x[i] += mix * wet[i];
        }
    }

    public static void Normalise (double[][] track, double peakDb)
    {
        double peak = 1e-12;
        foreach (var ch in track) foreach (var v in ch) peak = Math.Max (peak, Math.Abs (v));
        double g = Math.Pow (10.0, peakDb / 20.0) / peak;
        foreach (var ch in track) for (int i = 0; i < ch.Length; i++) ch[i] *= g;
    }

    // 24-bit stereo PCM WAV with TPDF dither.
    public static void Save (double[][] track, string path)
    {
        int frames = track[0].Length, dataBytes = frames * 2 * 3;
        using (var w = new BinaryWriter (File.Create (path)))
        {
            w.Write (new[] { (byte) 'R', (byte) 'I', (byte) 'F', (byte) 'F' }); w.Write (36 + dataBytes);
            w.Write (new[] { (byte) 'W', (byte) 'A', (byte) 'V', (byte) 'E' });
            w.Write (new[] { (byte) 'f', (byte) 'm', (byte) 't', (byte) ' ' }); w.Write (16);
            w.Write ((short) 1); w.Write ((short) 2); w.Write (Rate); w.Write (Rate * 2 * 3); w.Write ((short) 6); w.Write ((short) 24);
            w.Write (new[] { (byte) 'd', (byte) 'a', (byte) 't', (byte) 'a' }); w.Write (dataBytes);
            for (int i = 0; i < frames; i++)
                for (int ch = 0; ch < 2; ch++)
                {
                    double dither = (Noise () + Noise ()) * 0.5;
                    int v = (int) Math.Round (Math.Max (-1.0, Math.Min (1.0, track[ch][i])) * 8388607.0 + dither);
                    v = Math.Max (-8388608, Math.Min (8388607, v));
                    w.Write ((byte) (v & 0xFF)); w.Write ((byte) ((v >> 8) & 0xFF)); w.Write ((byte) ((v >> 16) & 0xFF));
                }
        }
    }
}
'@

$folder = Join-Path $Root $Name
if (Test-Path $folder) { throw "'$folder' already exists. Pass -Name to choose another name." }
$audio = Join-Path $folder 'Audio Files'
New-Item -ItemType Directory -Force -Path $audio | Out-Null

$bars = 9                      # 8 bars of song plus a final chord
$seconds = $bars * 4 * [DemoSynth]::Beat + 2.5
$lead = [DemoSynth]::NewTrack($seconds); $pad = [DemoSynth]::NewTrack($seconds)
$bass = [DemoSynth]::NewTrack($seconds); $drums = [DemoSynth]::NewTrack($seconds)

# Harmony: Am - F - C - G, twice, then Am.
$chords = @(@(57, 60, 64), @(53, 57, 60), @(55, 60, 64), @(55, 59, 62))
$roots  = @(45, 41, 48, 43)

# Melody (original): MIDI note and length in beats, bar by bar.
$melody = @(
    @(@(76,1), @(74,0.5), @(72,0.5), @(74,1), @(76,1)),     # Am
    @(@(72,1), @(69,1), @(72,0.5), @(74,0.5), @(72,1)),     # F
    @(@(76,1.5), @(79,0.5), @(76,1), @(74,1)),              # C
    @(@(74,1.5), @(72,0.5), @(71,2)),                       # G
    @(@(81,1), @(79,0.5), @(76,0.5), @(79,1), @(81,1)),     # Am
    @(@(81,1), @(77,1), @(76,1), @(72,1)),                  # F
    @(@(76,1), @(74,0.5), @(76,0.5), @(79,1), @(76,1)),     # C
    @(@(74,1), @(71,1), @(69,2)),                           # G -> resolves to A
    @(,@(69,4))                                             # Am (final); the comma stops PowerShell flattening a one-note bar
)

for ($bar = 0; $bar -lt $bars; $bar++) {
    $b0 = $bar * 4
    $c = $bar % 4; if ($bar -eq 8) { $c = 0 }

    $beat = $b0
    foreach ($note in $melody[$bar]) { [DemoSynth]::Lead($lead, $beat, $note[1], $note[0], 0.5); $beat += $note[1] }

    [DemoSynth]::Pad($pad, $b0, 4, [int[]]$chords[$c], 0.08)

    if ($bar -lt 8) {
        # root on 1 and 3, a pickup eighth before beat 3, the fifth on the "and" of 4
        [DemoSynth]::Bass($bass, $b0, 1.5, $roots[$c], 0.5)
        [DemoSynth]::Bass($bass, $b0 + 1.5, 0.5, $roots[$c], 0.4)
        [DemoSynth]::Bass($bass, $b0 + 2, 1.5, $roots[$c], 0.5)
        [DemoSynth]::Bass($bass, $b0 + 3.5, 0.5, $roots[$c] + 7, 0.4)

        [DemoSynth]::Kick($drums, $b0, 0.9); [DemoSynth]::Kick($drums, $b0 + 2, 0.9); [DemoSynth]::Kick($drums, $b0 + 2.5, 0.5)
        [DemoSynth]::Snare($drums, $b0 + 1, 0.6); [DemoSynth]::Snare($drums, $b0 + 3, 0.6)
        for ($e = 0; $e -lt 8; $e++) { $g = 0.25; if ($e % 2 -eq 1) { $g = 0.16 }; [DemoSynth]::Hat($drums, $b0 + $e * 0.5, $g) }
    } else {
        [DemoSynth]::Bass($bass, $b0, 4, $roots[0], 0.5)
        [DemoSynth]::Kick($drums, $b0, 0.9)
    }
}

[DemoSynth]::Echo($lead, 0.35, 0.35)
[DemoSynth]::Normalise($lead, -6.0); [DemoSynth]::Normalise($pad, -9.0)
[DemoSynth]::Normalise($bass, -6.0); [DemoSynth]::Normalise($drums, -4.0)

$files = [ordered]@{ Lead = $lead; Chords = $pad; Bass = $bass; Drums = $drums }
foreach ($key in $files.Keys) { [DemoSynth]::Save($files[$key], (Join-Path $audio "$key.wav")) }

$frames = $lead[0].Length
$tracks = @(
    @{ Name = 'Lead';   Colour = 'ffd4b44a'; Vol = -3.0 },
    @{ Name = 'Chords'; Colour = 'ff8a6fd1'; Vol = -6.0 },
    @{ Name = 'Bass';   Colour = 'ff4a9fd8'; Vol = -4.0 },
    @{ Name = 'Drums';  Colour = 'ffd0564f'; Vol = -4.0 }
)
$id = 1
$trackXml = foreach ($t in $tracks) {
    $tid = $id; $cid = $id + 1; $id += 2
    @"
    <TRACK id="$tid" name="$($t.Name)" kind="audio" channels="2" colour="$($t.Colour)" volumeDb="$($t.Vol)" pan="0.0" mute="0" solo="0" recordArm="0" inputChannel="0" automationMode="read" outputBus="0">
      <CLIPS>
        <CLIP id="$cid" name="$($t.Name)" file="Audio Files/$($t.Name).wav" start="0" length="$frames" offset="0" sourceLength="$frames" gainDb="0.0" fadeIn="0" fadeOut="24000"/>
      </CLIPS>
      <INSERTS/>
      <SENDS/>
      <AUTOMATION/>
    </TRACK>
"@
}

$xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<SESSION name="$Name" sampleRate="48000.0" bitDepth="24" fileFormat="wav" formatVersion="1" nextId="$id" masterVolumeDb="0.0" consoleEnabled="1" consoleDrive="4.0" consoleCharacter="0.3" consoleCurve="0">
  <TRACKS>
$($trackXml -join "`n")
  </TRACKS>
</SESSION>
"@
$sessionFile = Join-Path $folder "$Name.meridian"
[IO.File]::WriteAllText($sessionFile, $xml, (New-Object Text.UTF8Encoding($false)))
$sessionFile
