# The launch demo, scripted: a 16-bar lo-fi hip-hop beat made and mixed inside
# Meridian through its AI tools, with real recorded sounds in Meridian's sampler.
#   1. compose: sampled boom-bap drums (swung hats, ghost snares), a sampled piano
#      playing Dm9 - G13 - Cmaj9 - A7b13 with a melody, a warm sine bass
#   2. mix: drum bus with glue compression, Meridian Lo-Fi on the piano (wow, vinyl,
#      darker tone), a reverb bus, a limited mix bus, Warm Tape master
#   3. arrange: intro (piano alone, fading in), groove, melody from bar 7, a break
#      in bars 11-12 (drums and bass out), back in for the last four bars
#   4. export: MP3 at -14 LUFS
#
# Sounds are not part of the repository (their licences allow use, not
# redistribution). Point the script at a drum kit and at piano notes:
#   -DrumKit  a folder with kick\, snare\ and hats\ subfolders of one-shots
#             (e.g. the free "Lo-Fi Hip-Hop Drum Kit" by ItsLucid from soundpacks.com)
#   -Piano    a folder with Piano.mf.<note>.wav files, e.g. C3, E3, Ab3, C4, E4, Ab4
#             (the University of Iowa piano samples, with their leading silence trimmed)
#
# Meridian must be running with a session open (an empty one is fine):
#   .\tools\demo-lofi.ps1 -DrumKit '...\lo-fi drum kit by itslucid' -Piano '...\Iowa Piano\trimmed'
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$DrumKit,
    [Parameter(Mandatory)] [string]$Piano,
    [string]$Kick = 'kick 08.wav', [string]$Snare = 'snare 08.wav', [string]$Ghost = 'snare 11.wav',
    [string]$ClosedHat = 'hat 02.wav', [string]$OpenHat = 'hat 01.wav',
    [string]$Url = 'ws://127.0.0.1:47800/', [switch]$NoBounce)

$ErrorActionPreference = 'Stop'

$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [Threading.CancellationToken]::None
$ws.ConnectAsync([Uri]$Url, $ct).Wait(5000) | Out-Null
if ($ws.State -ne 'Open') { throw "Could not connect to $Url (is Meridian running?)" }

function Invoke-Tool([string]$tool, [hashtable]$arguments = @{}) {
    $json = @{ tool = $tool; arguments = $arguments } | ConvertTo-Json -Depth 8 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $ws.SendAsync((New-Object ArraySegment[byte] (,$bytes)), 'Text', $true, $ct).Wait()
    $buffer = New-Object byte[] 1048576
    $result = $ws.ReceiveAsync((New-Object ArraySegment[byte] (,$buffer)), $ct).Result
    $reply = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count) | ConvertFrom-Json
    if (-not $reply.ok) { throw "$tool failed: $($reply.error)" }
    $reply.result
}

function Note([int]$pitch, [double]$start, [double]$length, [double]$velocity) {
    @{ pitch = $pitch; start_beats = [Math]::Round($start, 4); length_beats = $length;
       velocity = [Math]::Round([Math]::Max(0.05, [Math]::Min(1.0, $velocity)), 3) }
}
function PathOf([string]$folder, [string]$file) { (Join-Path $folder $file) -replace '\\', '/' }

$bpm = 80.0
$bars = 16
$beatSeconds = 60.0 / $bpm
$barSeconds = 4 * $beatSeconds
$rand = New-Object Random 80   # fixed seed: the same "human" feel every run
$swing = 0.07                  # offbeat eighths pushed later
$late = 0.025                  # laid back: everything a touch behind the grid
function Plays-Drums([int]$bar) { ($bar -ge 2 -and $bar -le 9) -or $bar -ge 12 }   # bars 3-10 and 13-16

# --- 1. Compose ------------------------------------------------------------------------
Write-Host "1. Composing..."
Invoke-Tool 'set_tempo' @{ bpm = $bpm } | Out-Null

# Drums: sampled one-shots on the General MIDI keys.
Invoke-Tool 'create_track' @{ kind = 'instrument'; name = 'Drums' } | Out-Null
Invoke-Tool 'set_sampler' @{ track = 'Drums'; mode = 'drums'; gain_db = -6; samples = @(
    @{ file = PathOf "$DrumKit\kick" $Kick; note = 36 },
    @{ file = PathOf "$DrumKit\snare" $Ghost; note = 37; gain_db = -4 },
    @{ file = PathOf "$DrumKit\snare" $Snare; note = 38 },
    @{ file = PathOf "$DrumKit\hats" $ClosedHat; note = 42; gain_db = -6 },
    @{ file = PathOf "$DrumKit\hats" $OpenHat; note = 46; gain_db = -9 }) } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Drums'; start_beat = 0; length_beats = $bars * 4; name = 'Boom Bap' } | Out-Null
$drums = @()
for ($bar = 0; $bar -lt $bars; $bar++) {
    if (-not (Plays-Drums $bar)) { continue }
    $b = $bar * 4
    $drums += Note 36 ($b + 0 + $late) 0.5 0.95
    $drums += Note 36 ($b + 2.5 + $swing + $late) 0.5 0.85
    if ($bar % 2 -eq 1) { $drums += Note 36 ($b + 3.5 + $swing + $late) 0.25 0.6 }
    $drums += Note 38 ($b + 1 + $late + 0.015) 0.5 (0.82 + 0.06 * $rand.NextDouble())
    $drums += Note 38 ($b + 3 + $late + 0.015) 0.5 (0.86 + 0.06 * $rand.NextDouble())
    $drums += Note 37 ($b + 1.75 + $late) 0.25 (0.32 + 0.08 * $rand.NextDouble())     # ghost notes
    if ($bar % 2 -eq 0) { $drums += Note 37 ($b + 3.75 + $late) 0.25 (0.28 + 0.08 * $rand.NextDouble()) }
    for ($e = 0; $e -lt 8; $e++) {
        $offbeat = $e % 2 -eq 1
        $start = $b + $e * 0.5 + $(if ($offbeat) { $swing } else { 0 }) + $late
        $open = $bar % 2 -eq 1 -and $e -eq 7                                            # an open hat turns each phrase
        $drums += Note $(if ($open) { 46 } else { 42 }) $start 0.25 ($(if ($offbeat) { 0.42 } else { 0.62 }) + 0.1 * $rand.NextDouble())
    }
}
Invoke-Tool 'add_notes' @{ track = 'Drums'; clip = 1; notes = $drums } | Out-Null

# Piano: six recorded notes a major third apart; levels evened out per note.
$pianoZones = @(
    @{ file = PathOf $Piano 'Piano.mf.C3.wav';  root_note = 48; low_note = 36; high_note = 49; gain_db = 2.9 },
    @{ file = PathOf $Piano 'Piano.mf.E3.wav';  root_note = 52; low_note = 50; high_note = 53; gain_db = 1.3 },
    @{ file = PathOf $Piano 'Piano.mf.Ab3.wav'; root_note = 56; low_note = 54; high_note = 57; gain_db = 3.0 },
    @{ file = PathOf $Piano 'Piano.mf.C4.wav';  root_note = 60; low_note = 58; high_note = 61; gain_db = 5.2 },
    @{ file = PathOf $Piano 'Piano.mf.E4.wav';  root_note = 64; low_note = 62; high_note = 65; gain_db = 7.1 },
    @{ file = PathOf $Piano 'Piano.mf.Ab4.wav'; root_note = 68; low_note = 66; high_note = 84; gain_db = 0.0 })
Invoke-Tool 'create_track' @{ kind = 'instrument'; name = 'Piano' } | Out-Null
Invoke-Tool 'set_sampler' @{ track = 'Piano'; mode = 'keys'; release = 0.6; gain_db = 6; samples = $pianoZones } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Piano'; start_beat = 0; length_beats = $bars * 4; name = 'Keys' } | Out-Null

# Dm9 - G13 - Cmaj9 - A7b13, one bar each: rootless voicings in the piano's middle.
$voicings = @(
    @(50, 57, 60, 64, 65),   # Dm9:    D A C E F
    @(53, 59, 64, 69),       # G13:    F B E A
    @(52, 55, 59, 62),       # Cmaj9:  E G B D
    @(55, 61, 65)            # A7b13:  G C# F
)
$pianoNotes = @()   # (not $piano: PowerShell names ignore case, and -Piano is the folder)
for ($bar = 0; $bar -lt $bars; $bar++) {
    $b = $bar * 4
    $chord = $voicings[$bar % 4]
    $last = $bar -eq $bars - 1
    for ($i = 0; $i -lt $chord.Count; $i++) {
        $roll = 0.018 * $i   # rolled, low to high
        if ($last) { $pianoNotes += Note $chord[$i] ($b + 0.04 + $roll) 3.8 (0.58 + 0.05 * $rand.NextDouble()); continue }
        $pianoNotes += Note $chord[$i] ($b + 0.04 + $roll) 1.45 (0.6 + 0.06 * $rand.NextDouble())
        $pianoNotes += Note $chord[$i] ($b + 1.5 + $swing + $roll) 2.3 (0.44 + 0.06 * $rand.NextDouble())
    }
}
# The melody, from bar 7 (not in the last bar): a two-bar phrase over each pair of chords.
$phraseA = @( @(0.5, 74, 0.5), @(1, 76, 1.0), @(2.5, 72, 0.5), @(3, 69, 1.5),          # over Dm9
              @(4.5, 71, 0.5), @(5, 74, 1.5), @(7, 72, 0.75) )                         # over G13
$phraseB = @( @(0, 76, 0.75), @(1, 79, 1.0), @(2, 76, 0.5), @(2.5, 74, 1.0),           # over Cmaj9
              @(4.5, 73, 0.5), @(5, 76, 0.5), @(5.5, 73, 1.75) )                       # over A7b13
for ($bar = 6; $bar -lt $bars - 1; $bar += 2) {
    $phrase = $(if ($bar % 4 -eq 0) { $phraseA } else { $phraseB })
    foreach ($n in $phrase) {
        $pianoNotes += Note $n[1] ($bar * 4 + $n[0] + $late + 0.01 * $rand.NextDouble()) $n[2] (0.62 + 0.1 * $rand.NextDouble())
    }
}
Invoke-Tool 'add_notes' @{ track = 'Piano'; clip = 1; notes = $pianoNotes } | Out-Null

# Bass: a warm sine on the roots, with the drums (out in the intro and the break).
$roots = @(38, 43, 36, 45)   # D G C A
Invoke-Tool 'create_track' @{ kind = 'instrument'; name = 'Bass' } | Out-Null
Invoke-Tool 'set_synth' @{ track = 'Bass'; engine = 'synth'; waveform = 'sine'; detune_cents = 0; attack = 0.008; decay = 0.9;
                           sustain = 0.55; release = 0.15; cutoff_hz = 500; resonance = 0; filter_env_octaves = 0; gain_db = -3 } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Bass'; start_beat = 0; length_beats = $bars * 4; name = 'Bassline' } | Out-Null
$bass = @()
for ($bar = 0; $bar -lt $bars; $bar++) {
    if (-not (Plays-Drums $bar)) { continue }
    $b = $bar * 4
    $root = $roots[$bar % 4]
    $next = $roots[($bar + 1) % 4]
    if ($bar -eq $bars - 1) { $bass += Note $root ($b + 0.03) 3.8 0.85; continue }
    $bass += Note $root ($b + 0.03) 1.6 0.85
    $bass += Note ($root + 12) ($b + 2.5 + $swing) 0.4 0.55
    $bass += Note ($next - 1 + 12 * [int]($next - 1 -lt 36)) ($b + 3.5 + $swing) 0.45 0.6   # chromatic approach
}
Invoke-Tool 'add_notes' @{ track = 'Bass'; clip = 1; notes = $bass } | Out-Null

# --- 2. Mix ------------------------------------------------------------------------------
Write-Host "2. Mixing..."
Invoke-Tool 'set_up' @{ goal = 'drum bus'; name = 'Drum Bus' } | Out-Null
Invoke-Tool 'set_output' @{ track = 'Drums'; destination = 'Drum Bus' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Drum Bus'; slot = 'B'; plugin = 'Meridian Lo-Fi' } | Out-Null
foreach ($setting in @(@('wow', 0), @('flutter', 0), @('noise', 0), @('bits', 12), @('tone', 9000), @('mix', 0.7))) {
    Invoke-Tool 'set_plugin_parameter' @{ track = 'Drum Bus'; slot = 'B'; parameter = $setting[0]; value = $setting[1] } | Out-Null
}

Invoke-Tool 'insert_plugin' @{ track = 'Piano'; slot = 'A'; plugin = 'Meridian Lo-Fi' } | Out-Null
foreach ($setting in @(@('wow', 0.45), @('flutter', 0.35), @('noise', 0.3), @('tone', 5000), @('mix', 1))) {
    Invoke-Tool 'set_plugin_parameter' @{ track = 'Piano'; slot = 'A'; parameter = $setting[0]; value = $setting[1] } | Out-Null
}
Invoke-Tool 'insert_plugin' @{ track = 'Piano'; slot = 'B'; plugin = 'Meridian EQ' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Piano'; slot = 'B'; parameter = 'hpOn'; value = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Piano'; slot = 'B'; parameter = 'hpFreq'; value = 120 } | Out-Null

Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Reverb' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Reverb'; slot = 'A'; plugin = 'Meridian Reverb' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Reverb'; slot = 'A'; parameter = 'mix'; normalized = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Reverb'; slot = 'A'; parameter = 'size'; normalized = 0.65 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Piano'; slot = 'A'; bus = 'Reverb'; level_db = -10 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Drums'; slot = 'A'; bus = 'Reverb'; level_db = -24 } | Out-Null

# Balance, measured: alone, at 0 dB faders the drums play near -29 dB RMS, the piano -30 and the
# sine bass -20, which would bury the rest. These faders put the three within about 2 dB.
Invoke-Tool 'set_track' @{ track = 'Drums'; volume_db = 4 } | Out-Null
Invoke-Tool 'set_track' @{ track = 'Piano'; volume_db = 4 } | Out-Null
Invoke-Tool 'set_track' @{ track = 'Bass'; volume_db = -18 } | Out-Null
Invoke-Tool 'set_master' @{ preset = 'Warm Tape' } | Out-Null

Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Mix Bus' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Mix Bus'; slot = 'A'; plugin = 'Meridian Limiter' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Mix Bus'; slot = 'A'; parameter = 'input'; value = 7.5 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Mix Bus'; slot = 'A'; parameter = 'ceiling'; value = -1.2 } | Out-Null
foreach ($source in 'Drum Bus', 'Piano', 'Bass', 'Reverb') {
    Invoke-Tool 'set_output' @{ track = $source; destination = 'Mix Bus' } | Out-Null
}

# --- 3. Arrange ----------------------------------------------------------------------------
Write-Host "3. Arranging..."
Invoke-Tool 'set_automation' @{ track = 'Piano'; parameter = 'volume';
    points = @(@{ seconds = 0; value = -14 }, @{ seconds = [Math]::Round($barSeconds, 3); value = 4 }) } | Out-Null

# --- 4. Export -------------------------------------------------------------------------------
if (-not $NoBounce) {
    Write-Host "4. Bouncing..."
    Invoke-Tool 'bounce' @{ format = 'mp3'; normalize_lufs = -14; end_seconds = [Math]::Round($bars * $barSeconds + 3.0, 3) } | Out-Null
    do {
        Start-Sleep -Milliseconds 500
        $status = Invoke-Tool 'get_status'
    } while ($status.bounce.running)
    $status.bounce | ConvertTo-Json -Depth 4
}
Write-Host "Done."
