# Creates "Evening Lights" as a Meridian session: a mellow piece at 80 BPM in C major,
# about a minute long. Progression Fmaj7 - Em7 - Dm7 - Cmaj7.
#
#   Intro (2 bars) - A (8 bars) - B (8 bars) - Outro (2 bars)
#
# Five stereo tracks are synthesised in code (Meridian has no software instruments
# yet): Keys (FM electric piano), Lead (vibraphone-like mallet), Pad, Bass, Drums
# (swung). Keys, Lead, Pad and the snare get a Freeverb-style room reverb, and the
# lead also gets a soft dotted-eighth echo. Prints the session file.
#
#   $s = .\tools\make-song-evening.ps1
#   & .\build\app\src\Meridian_artefacts\Release\Meridian.exe $s
#
# Deterministic: the same script always writes the same audio.
param(
    [string]$Root = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Meridian Sessions'),
    [string]$Name = 'Evening Lights'
)

$ErrorActionPreference = 'Stop'

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.IO;

public static class Evening
{
    public const int Rate = 48000;
    public const double Beat = 60.0 / 80.0;

    static double Freq (int midi) { return 440.0 * Math.Pow (2.0, (midi - 69) / 12.0); }

    static uint noiseState = 0x1234567u;
    public static double Noise ()
    {
        noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
        return noiseState / 2147483648.0 - 1.0;
    }

    public static double[][] NewTrack (double seconds)
    {
        int n = (int) (seconds * Rate);
        return new[] { new double[n], new double[n] };
    }

    static void Write (double[][] t, int i, double v, double pan)
    {
        if (i < 0 || i >= t[0].Length) return;
        double a = (pan + 1.0) * Math.PI / 4.0;
        t[0][i] += v * Math.Cos (a);
        t[1][i] += v * Math.Sin (a);
    }

    static double Release (double t, double length, double release)
    {
        return t <= length ? 1.0 : Math.Max (0.0, 1.0 - (t - length) / release);
    }

    // Electric piano: two-operator FM (1:1) whose brightness falls as the note decays,
    // plus a quiet, fast-fading "tine" partial; gentle stereo tremolo.
    public static void Keys (double[][] t, double beat, double beats, int midi, double velocity)
    {
        double f = Freq (midi), length = beats * Beat, release = 0.35;
        int first = (int) (beat * Beat * Rate), count = (int) ((length + release) * Rate);
        double pc = 0, pm = 0, pt = 0;
        for (int i = 0; i < count; i++)
        {
            double s = (double) i / Rate;
            pc += 2 * Math.PI * f / Rate; pm += 2 * Math.PI * f / Rate; pt += 2 * Math.PI * f * 7.0 / Rate;
            double index = (0.25 + 1.6 * velocity * Math.Exp (-s * 3.0));
            double body = Math.Sin (pc + index * Math.Sin (pm));
            double tine = 0.12 * velocity * Math.Sin (pt) * Math.Exp (-s * 9.0);
            double amp = (0.25 + 0.75 * Math.Exp (-s * 1.1)) * Math.Min (1.0, s / 0.004) * Release (s, length, release);
            double trem = 0.18 * Math.Sin (2 * Math.PI * 4.2 * (beat * Beat + s));
            Write (t, first + i, velocity * (body + tine) * amp, trem);
        }
    }

    // Lead: mallet / vibraphone. Fundamental, a 4x partial that fades quickly, slow tremolo.
    public static void Lead (double[][] t, double beat, double beats, int midi, double velocity)
    {
        double f = Freq (midi), length = beats * Beat, release = 0.6;
        int first = (int) (beat * Beat * Rate), count = (int) ((length + release) * Rate);
        double p1 = 0, p4 = 0, p10 = 0;
        for (int i = 0; i < count; i++)
        {
            double s = (double) i / Rate;
            p1 += 2 * Math.PI * f / Rate; p4 += 2 * Math.PI * f * 4.0 / Rate; p10 += 2 * Math.PI * f * 10.0 / Rate;
            double v = Math.Sin (p1) + 0.35 * Math.Sin (p4) * Math.Exp (-s * 6.0) + 0.06 * Math.Sin (p10) * Math.Exp (-s * 20.0);
            double amp = Math.Exp (-s * 0.9) * Math.Min (1.0, s / 0.003) * Release (s, length, release);
            double trem = 1.0 + 0.12 * Math.Sin (2 * Math.PI * 5.0 * s);
            Write (t, first + i, velocity * v * amp * trem, 0.12);
        }
    }

    // Pad: detuned, softly filtered saws, slow swell.
    public static void Pad (double[][] t, double beat, double beats, int[] notes, double gain)
    {
        double length = beats * Beat, release = 1.5, attack = 1.2;
        int first = (int) (beat * Beat * Rate), count = (int) ((length + release) * Rate);
        double[] detune = { -0.004, 0.0035 }, pans = { -0.8, 0.8 };
        foreach (int midi in notes)
            for (int d = 0; d < 2; d++)
            {
                double f = Freq (midi) * (1.0 + detune[d]), ph = d * 2.1 + midi;
                for (int i = 0; i < count; i++)
                {
                    double s = (double) i / Rate;
                    ph += 2 * Math.PI * f / Rate;
                    double v = 0.0;
                    for (int k = 1; k <= 6; k++)
                        v += Math.Sin (k * ph) / (k * k);
                    Write (t, first + i, gain * v * Math.Min (1.0, s / attack) * Release (s, length, release), pans[d]);
                }
            }
    }

    // Bass: round sub with a little 2nd harmonic, plucked envelope.
    public static void Bass (double[][] t, double beat, double beats, int midi, double velocity)
    {
        double f = Freq (midi), length = beats * Beat - 0.03, release = 0.09;
        int first = (int) (beat * Beat * Rate), count = (int) ((length + release) * Rate);
        double ph = 0;
        for (int i = 0; i < count; i++)
        {
            double s = (double) i / Rate;
            ph += 2 * Math.PI * f / Rate;
            double v = Math.Sin (ph) + 0.22 * Math.Sin (2 * ph) + 0.05 * Math.Sin (3 * ph);
            double amp = (0.55 + 0.45 * Math.Exp (-s * 4.0)) * Math.Min (1.0, s / 0.006) * Release (s, length, release);
            Write (t, first + i, velocity * v * amp, 0.0);
        }
    }

    public static void Kick (double[][] t, double beat, double velocity)
    {
        int first = (int) (beat * Beat * Rate), count = (int) (0.5 * Rate);
        double ph = 0;
        for (int i = 0; i < count; i++)
        {
            double s = (double) i / Rate;
            ph += 2 * Math.PI * (48.0 + 70.0 * Math.Exp (-s * 25.0)) / Rate;
            Write (t, first + i, velocity * Math.Sin (ph) * Math.Exp (-s * 6.0) * Math.Min (1.0, s / 0.002), 0.0);
        }
    }

    // A soft, brushed snare: filtered noise with a long tail and a little body tone.
    public static void Snare (double[][] t, double beat, double velocity)
    {
        int first = (int) (beat * Beat * Rate), count = (int) (0.45 * Rate);
        double ph = 0, low = 0, prev = 0;
        for (int i = 0; i < count; i++)
        {
            double s = (double) i / Rate;
            ph += 2 * Math.PI * 175.0 / Rate;
            double n = Noise ();
            low += 0.35 * (n - low);              // gentle low-pass: softer, darker
            double band = low - prev; prev = low;   // then remove the lows
            double v = 1.6 * band * Math.Exp (-s * 9.0) + 0.35 * Math.Sin (ph) * Math.Exp (-s * 30.0);
            Write (t, first + i, velocity * v, -0.08);
        }
    }

    public static void Hat (double[][] t, double beat, double velocity, bool open)
    {
        int first = (int) (beat * Beat * Rate), count = (int) ((open ? 0.4 : 0.07) * Rate);
        double prev = 0;
        for (int i = 0; i < count; i++)
        {
            double s = (double) i / Rate;
            double n = Noise (), high = n - prev; prev = n;
            Write (t, first + i, velocity * 0.45 * high * Math.Exp (-s * (open ? 9.0 : 60.0)), 0.3);
        }
    }

    // Freeverb-style room: 8 damped comb filters in parallel, 4 all-passes in series,
    // per channel, with the right channel's delays spread for width. Adds `wet` of
    // the reverb to the dry signal.
    public static void Reverb (double[][] t, double wet, double roomSize, double damping)
    {
        int[] combs = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        int[] allpasses = { 556, 441, 341, 225 };
        double scale = Rate / 44100.0, feedback = 0.7 + 0.28 * roomSize;
        int n = t[0].Length;
        var input = new double[n];
        for (int i = 0; i < n; i++) input[i] = 0.5 * (t[0][i] + t[1][i]) * 0.015;

        for (int ch = 0; ch < 2; ch++)
        {
            int spread = ch == 0 ? 0 : 23;
            var output = new double[n];
            foreach (int c in combs)
            {
                int len = (int) ((c + spread) * scale);
                var buf = new double[len]; int pos = 0; double store = 0;
                for (int i = 0; i < n; i++)
                {
                    double y = buf[pos];
                    store = y * (1 - damping) + store * damping;
                    buf[pos] = input[i] + store * feedback;
                    if (++pos >= len) pos = 0;
                    output[i] += y;
                }
            }
            foreach (int a in allpasses)
            {
                int len = (int) ((a + spread) * scale);
                var buf = new double[len]; int pos = 0;
                for (int i = 0; i < n; i++)
                {
                    double b = buf[pos];
                    double y = -output[i] + b;
                    buf[pos] = output[i] + b * 0.5;
                    if (++pos >= len) pos = 0;
                    output[i] = y;
                }
            }
            for (int i = 0; i < n; i++) t[ch][i] += wet * output[i];
        }
    }

    public static void Echo (double[][] t, double mix, double feedback)
    {
        int delay = (int) (0.75 * Beat * Rate);
        for (int ch = 0; ch < 2; ch++)
        {
            double[] x = t[ch], wet = new double[x.Length];
            int d = ch == 0 ? delay : delay + 720;
            double low = 0;
            for (int i = d; i < x.Length; i++)
            {
                low += 0.25 * (x[i - d] + feedback * wet[i - d] - low); // each repeat a little darker
                wet[i] = low;
            }
            for (int i = 0; i < x.Length; i++) x[i] += mix * wet[i];
        }
    }

    public static void Normalise (double[][] t, double peakDb)
    {
        double peak = 1e-12;
        foreach (var ch in t) foreach (var v in ch) peak = Math.Max (peak, Math.Abs (v));
        double g = Math.Pow (10.0, peakDb / 20.0) / peak;
        foreach (var ch in t) for (int i = 0; i < ch.Length; i++) ch[i] *= g;
    }

    // 24-bit stereo PCM WAV with TPDF dither.
    public static void Save (double[][] t, string path)
    {
        int frames = t[0].Length, dataBytes = frames * 6;
        using (var w = new BinaryWriter (File.Create (path)))
        {
            w.Write (new[] { (byte) 'R', (byte) 'I', (byte) 'F', (byte) 'F' }); w.Write (36 + dataBytes);
            w.Write (new[] { (byte) 'W', (byte) 'A', (byte) 'V', (byte) 'E' });
            w.Write (new[] { (byte) 'f', (byte) 'm', (byte) 't', (byte) ' ' }); w.Write (16);
            w.Write ((short) 1); w.Write ((short) 2); w.Write (Rate); w.Write (Rate * 6); w.Write ((short) 6); w.Write ((short) 24);
            w.Write (new[] { (byte) 'd', (byte) 'a', (byte) 't', (byte) 'a' }); w.Write (dataBytes);
            for (int i = 0; i < frames; i++)
                for (int ch = 0; ch < 2; ch++)
                {
                    int v = (int) Math.Round (Math.Max (-1.0, Math.Min (1.0, t[ch][i])) * 8388607.0 + (Noise () + Noise ()) * 0.5);
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

$bars = 20
$seconds = $bars * 4 * [Evening]::Beat + 5.0
$keys = [Evening]::NewTrack($seconds); $lead = [Evening]::NewTrack($seconds); $pad = [Evening]::NewTrack($seconds)
$bass = [Evening]::NewTrack($seconds); $drums = [Evening]::NewTrack($seconds)

# Fmaj7, Em7, Dm7, Cmaj7: close voicings around middle C, plus bass roots.
$chords = @('53 57 60 64', '52 55 59 62', '50 53 57 60', '48 52 55 59')
$roots  = @(41, 40, 38, 36)

# Melody, one string per bar: "midi:beats", midi 0 = rest. (Original melody.)
$melodyA = @('0:0.5 69:0.5 72:0.5 76:1.5 79:1', '76:1 74:0.5 71:0.5 74:2', '0:0.5 65:0.5 69:0.5 72:1.5 74:1', '76:1.5 74:0.5 72:2')
$melodyA2 = @('0:0.5 69:0.5 72:0.5 76:1.5 79:1', '76:1 74:0.5 71:0.5 74:2', '0:0.5 65:0.5 69:0.5 72:1.5 74:1', '71:1 72:0.5 74:0.5 76:2')
$melodyB = @('81:1.5 79:0.5 76:1 79:1', '79:1 76:0.5 74:0.5 71:2', '0:0.5 74:0.5 77:0.5 81:1.5 79:1', '76:3 0:1',
             '81:1 84:1 83:0.5 81:0.5 79:1', '79:1.5 76:0.5 74:1 76:1', '77:1 76:0.5 74:0.5 72:1 74:1', '72:4')
$melody = @('', '') + $melodyA + $melodyA2 + $melodyB + @('', '')   # 20 bars

for ($bar = 0; $bar -lt $bars; $bar++) {
    $b0 = $bar * 4
    $c = $bar % 4
    $notes = [int[]]($chords[$c] -split ' ')
    $outro = $bar -ge 18
    if ($bar -eq 19) { $notes = [int[]](48, 52, 55, 59, 62); $c = 3 }   # final Cmaj9

    # Keys: a long chord on beat 1 and a softer push on the "and" of 3, slightly humanised.
    foreach ($n in $notes) {
        $h = [Evening]::Noise() * 0.01
        if ($outro) { [Evening]::Keys($keys, $b0 + $h, 4, $n, 0.5) }
        else {
            [Evening]::Keys($keys, $b0 + $h, 2.4, $n, 0.55)
            [Evening]::Keys($keys, $b0 + 2.5 + $h, 1.4, $n, 0.38)
        }
    }

    [Evening]::Pad($pad, $b0, 4, [int[]]($notes | ForEach-Object { $_ - 12 }), 0.06)

    # Melody
    $beat = $b0
    if ($melody[$bar] -ne '') {
        foreach ($token in ($melody[$bar] -split ' ')) {
            $p = $token -split ':'; $m = [int]$p[0]; $len = [double]$p[1]
            if ($m -gt 0) { [Evening]::Lead($lead, $beat, $len, $m, 0.5) }
            $beat += $len
        }
    }

    # Bass and drums play in the A and B sections (bars 3-18).
    if ($bar -ge 2 -and $bar -lt 18) {
        $r = $roots[$c]
        [Evening]::Bass($bass, $b0, 1.5, $r, 0.55); [Evening]::Bass($bass, $b0 + 1.5, 0.5, $r, 0.4)
        [Evening]::Bass($bass, $b0 + 2.5, 1.0, $r + 7, 0.45); [Evening]::Bass($bass, $b0 + 3.5, 0.5, $r + 12, 0.35)

        [Evening]::Kick($drums, $b0, 0.85); [Evening]::Kick($drums, $b0 + 1.5, 0.55)
        if ($bar % 2 -eq 1) { [Evening]::Kick($drums, $b0 + 2.75, 0.4) }
        [Evening]::Snare($drums, $b0 + 1, 0.5); [Evening]::Snare($drums, $b0 + 3, 0.55)
        for ($e = 0; $e -lt 8; $e++) {
            $swing = 0.0; if ($e % 2 -eq 1) { $swing = 0.09 }               # swung eighths
            $vel = 0.22; if ($e % 2 -eq 1) { $vel = 0.14 }
            $open = ($e -eq 7 -and $bar % 4 -eq 3)                          # open hat into each new phrase
            [Evening]::Hat($drums, $b0 + $e * 0.5 + $swing, $vel + [Evening]::Noise() * 0.03, $open)
        }
    }
    elseif ($bar -eq 18) {
        [Evening]::Bass($bass, $b0, 4, $roots[0], 0.5)
    }
    elseif ($bar -eq 19) {
        [Evening]::Bass($bass, $b0, 4, 36, 0.5)
    }
}

# Space: echo on the lead, then a room for everything melodic.
[Evening]::Echo($lead, 0.3, 0.35)
[Evening]::Reverb($lead, 0.45, 0.8, 0.35)
[Evening]::Reverb($keys, 0.3, 0.75, 0.4)
[Evening]::Reverb($pad, 0.4, 0.85, 0.5)
[Evening]::Reverb($drums, 0.12, 0.6, 0.5)

[Evening]::Normalise($keys, -6.0); [Evening]::Normalise($lead, -6.0); [Evening]::Normalise($pad, -9.0)
[Evening]::Normalise($bass, -6.0); [Evening]::Normalise($drums, -5.0)

# (A dictionary entry named "Keys" would hide the dictionary's own .Keys, so plain pairs.)
$stems = @(@('Keys', $keys), @('Lead', $lead), @('Pad', $pad), @('Bass', $bass), @('Drums', $drums))
foreach ($stem in $stems) { [Evening]::Save($stem[1], (Join-Path $audio "$($stem[0]).wav")) }

$frames = $keys[0].Length
$tracks = @(
    @{ Name = 'Keys';  Colour = 'ffd98a3d'; Vol = -5.0; Pan = -0.15 },
    @{ Name = 'Lead';  Colour = 'ffd4b44a'; Vol = -4.0; Pan = 0.1 },
    @{ Name = 'Pad';   Colour = 'ff8a6fd1'; Vol = -9.0; Pan = 0.0 },
    @{ Name = 'Bass';  Colour = 'ff4a9fd8'; Vol = -5.0; Pan = 0.0 },
    @{ Name = 'Drums'; Colour = 'ffd0564f'; Vol = -8.0; Pan = 0.0 }
)
$id = 1
$trackXml = foreach ($t in $tracks) {
    $tid = $id; $cid = $id + 1; $id += 2
    @"
    <TRACK id="$tid" name="$($t.Name)" kind="audio" channels="2" colour="$($t.Colour)" volumeDb="$($t.Vol)" pan="$($t.Pan)" mute="0" solo="0" recordArm="0" inputChannel="0" automationMode="read" outputBus="0">
      <CLIPS>
        <CLIP id="$cid" name="$($t.Name)" file="Audio Files/$($t.Name).wav" start="0" length="$frames" offset="0" sourceLength="$frames" gainDb="0.0" fadeIn="0" fadeOut="96000"/>
      </CLIPS>
      <INSERTS/>
      <SENDS/>
      <AUTOMATION/>
    </TRACK>
"@
}

$xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<SESSION name="$Name" sampleRate="48000.0" bitDepth="24" fileFormat="wav" formatVersion="1" nextId="$id" masterVolumeDb="0.0" consoleEnabled="1" consoleDrive="5.0" consoleCharacter="0.35" consoleCurve="0">
  <TRACKS>
$($trackXml -join "`n")
  </TRACKS>
</SESSION>
"@
$sessionFile = Join-Path $folder "$Name.meridian"
[IO.File]::WriteAllText($sessionFile, $xml, (New-Object Text.UTF8Encoding($false)))
$sessionFile
