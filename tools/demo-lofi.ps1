# The launch demo, scripted: an 8-bar lo-fi hip-hop beat made and mixed inside
# Meridian through its AI tools, in the four steps the demo video asks Claude for.
#   1. compose: drums (laid back, swung hats), jazzy 7th chords, a warm bass
#   2. mix: drum bus with glue compression, darker keys, reverb bus, a limited mix bus, Warm Tape master
#   3. arrange: keys fade in over bar 1, drums drop out for bar 7
#   4. export: MP3 at -14 LUFS
# It is the rehearsal of the video (every step must work and sound right) and a
# fallback demo. Every sound is Meridian's built-in drum machine and synthesiser.
#
# Meridian must be running with a session open (an empty one is fine):
#   .\tools\demo-lofi.ps1
[CmdletBinding()]
param([string]$Url = 'ws://127.0.0.1:47800/', [switch]$NoBounce)

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

$bpm = 85.0
$bars = 8
$beatSeconds = 60.0 / $bpm
$rand = New-Object Random 85   # fixed seed: the same "human" feel every run

# --- 1. Compose ------------------------------------------------------------------------
Write-Host "1. Composing..."
Invoke-Tool 'set_tempo' @{ bpm = $bpm } | Out-Null

# Drums: kick on 1 and the "and" of 3, a late ghost kick; snare on 2 and 4; hats on swung eighths.
Invoke-Tool 'create_track' @{ kind = 'drums'; name = 'Drums' } | Out-Null
Invoke-Tool 'set_synth' @{ track = 'Drums'; engine = 'drums'; tune_semitones = -2; decay_scale = 0.8; tone = 0.35 } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Drums'; start_beat = 0; length_beats = $bars * 4; name = 'Groove' } | Out-Null
$drums = @()
$late = 0.03                   # laid back: everything a touch behind the grid
$swing = 0.08                  # offbeat eighths pushed later
for ($bar = 0; $bar -lt $bars; $bar++) {
    $b = $bar * 4
    $drums += Note 36 ($b + 0 + $late) 0.5 0.95
    $drums += Note 36 ($b + 2.5 + $swing + $late) 0.5 0.8
    if ($bar % 2 -eq 1) { $drums += Note 36 ($b + 3.75 + $late) 0.25 0.45 }
    $drums += Note 38 ($b + 1 + $late + 0.02) 0.5 (0.78 + 0.06 * $rand.NextDouble())
    $drums += Note 38 ($b + 3 + $late + 0.02) 0.5 (0.82 + 0.06 * $rand.NextDouble())
    for ($e = 0; $e -lt 8; $e++) {
        $offbeat = $e % 2 -eq 1
        $start = $b + $e * 0.5 + $(if ($offbeat) { $swing } else { 0 }) + $late
        $pitch = $(if ($bar % 4 -eq 3 -and $e -eq 7) { 46 } else { 42 })   # an open hat to turn the phrase
        $drums += Note $pitch $start 0.25 ($(if ($offbeat) { 0.32 } else { 0.5 }) + 0.08 * $rand.NextDouble())
    }
}
Invoke-Tool 'add_notes' @{ track = 'Drums'; clip = 1; notes = $drums } | Out-Null

# Keys: rootless voicings, Dm9 - G13 - Cmaj9 - Am7, one bar each, played twice.
# Each bar: the chord on 1 (held), again on the "and" of 2, a little late and soft.
$voicings = @(
    @(53, 57, 60, 64),   # Dm9:   F A C E
    @(53, 59, 64, 69),   # G13:   F B E A
    @(52, 55, 59, 62),   # Cmaj9: E G B D
    @(55, 60, 64, 67)    # Am7:   G C E G
)
Invoke-Tool 'create_track' @{ kind = 'instrument'; name = 'Keys' } | Out-Null
Invoke-Tool 'set_synth' @{ track = 'Keys'; engine = 'synth'; waveform = 'saw'; detune_cents = 7; attack = 0.01; decay = 1.4;
                           sustain = 0.35; release = 0.7; cutoff_hz = 1100; resonance = 0.1; filter_env_octaves = 1.2; gain_db = -6 } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Keys'; start_beat = 0; length_beats = $bars * 4; name = 'Chords' } | Out-Null
$keys = @()
for ($bar = 0; $bar -lt $bars; $bar++) {
    $b = $bar * 4
    $chord = $voicings[$bar % 4]
    for ($i = 0; $i -lt $chord.Count; $i++) {
        $strum = 0.012 * $i   # a slight roll, low to high
        $keys += Note $chord[$i] ($b + 0.05 + $strum) 1.4 (0.62 + 0.05 * $rand.NextDouble())
        $keys += Note $chord[$i] ($b + 1.5 + 0.06 + $strum) 1.9 (0.48 + 0.05 * $rand.NextDouble())
    }
}
Invoke-Tool 'add_notes' @{ track = 'Keys'; clip = 1; notes = $keys } | Out-Null

# Bass: the roots, on 1 and pushed into the next bar, with a pickup.
$roots = @(38, 43, 36, 45)   # D G C A
Invoke-Tool 'create_track' @{ kind = 'instrument'; name = 'Bass' } | Out-Null
Invoke-Tool 'set_synth' @{ track = 'Bass'; engine = 'synth'; waveform = 'sine'; detune_cents = 0; attack = 0.005; decay = 0.8;
                           sustain = 0.6; release = 0.15; cutoff_hz = 600; resonance = 0; filter_env_octaves = 0; gain_db = -3 } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Bass'; start_beat = 0; length_beats = $bars * 4; name = 'Bassline' } | Out-Null
$bass = @()
for ($bar = 0; $bar -lt $bars; $bar++) {
    $b = $bar * 4
    $root = $roots[$bar % 4]
    $next = $roots[($bar + 1) % 4]
    $bass += Note $root ($b + 0.03) 1.6 0.85
    $bass += Note $root ($b + 2.5 + 0.08) 0.75 0.7
    $bass += Note ($next - 1 + 12 * [int]($next - 1 -lt 36)) ($b + 3.5 + 0.08) 0.45 0.6   # chromatic approach into the next root
}
Invoke-Tool 'add_notes' @{ track = 'Bass'; clip = 1; notes = $bass } | Out-Null

# --- 2. Mix ------------------------------------------------------------------------------
Write-Host "2. Mixing..."
Invoke-Tool 'set_up' @{ goal = 'drum bus'; name = 'Drum Bus' } | Out-Null
Invoke-Tool 'set_output' @{ track = 'Drums'; destination = 'Drum Bus' } | Out-Null

Invoke-Tool 'insert_plugin' @{ track = 'Keys'; slot = 'A'; plugin = 'Meridian EQ' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Keys'; slot = 'A'; parameter = 'lpOn'; value = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Keys'; slot = 'A'; parameter = 'lpFreq'; value = 3200 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Keys'; slot = 'A'; parameter = 'hpOn'; value = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Keys'; slot = 'A'; parameter = 'hpFreq'; value = 150 } | Out-Null

Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Reverb' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Reverb'; slot = 'A'; plugin = 'Meridian Reverb' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Reverb'; slot = 'A'; parameter = 'mix'; normalized = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Reverb'; slot = 'A'; parameter = 'size'; normalized = 0.7 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Keys'; slot = 'A'; bus = 'Reverb'; level_db = -9 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Drums'; slot = 'A'; bus = 'Reverb'; level_db = -22 } | Out-Null

# Balance, measured: alone, the drums play near -30 dB RMS, the keys -26.5 and the
# sine bass -17.4, so the bass would bury everything. These levels put all three
# within a couple of dB of each other.
Invoke-Tool 'set_track' @{ track = 'Drums'; volume_db = 6 } | Out-Null
Invoke-Tool 'set_track' @{ track = 'Drum Bus'; volume_db = 3 } | Out-Null
Invoke-Tool 'set_track' @{ track = 'Keys'; volume_db = 0 } | Out-Null
Invoke-Tool 'set_track' @{ track = 'Bass'; volume_db = -9 } | Out-Null
Invoke-Tool 'set_master' @{ preset = 'Warm Tape' } | Out-Null

# Loudness: the drum hits peak high against the average level, so on its own the
# mix reaches only about -18 LUFS under a -1 dBTP ceiling. A mix bus with Meridian
# Limiter (5 dB in, ceiling -1.2 dBTP) brings it to streaming loudness.
Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Mix Bus' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Mix Bus'; slot = 'A'; plugin = 'Meridian Limiter' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Mix Bus'; slot = 'A'; parameter = 'input'; value = 5 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Mix Bus'; slot = 'A'; parameter = 'ceiling'; value = -1.2 } | Out-Null
foreach ($source in 'Drum Bus', 'Keys', 'Bass', 'Reverb') {
    Invoke-Tool 'set_output' @{ track = $source; destination = 'Mix Bus' } | Out-Null
}

# --- 3. Arrange ----------------------------------------------------------------------------
Write-Host "3. Arranging..."
$barSeconds = 4 * $beatSeconds
Invoke-Tool 'set_automation' @{ track = 'Keys'; parameter = 'volume';
    points = @(@{ seconds = 0; value = -40 }, @{ seconds = [Math]::Round($barSeconds, 3); value = 0 }) } | Out-Null
Invoke-Tool 'set_automation' @{ track = 'Drums'; parameter = 'mute';
    points = @(@{ seconds = 0; value = 0 }, @{ seconds = [Math]::Round(6 * $barSeconds, 3); value = 1 },
               @{ seconds = [Math]::Round(7 * $barSeconds, 3); value = 0 }) } | Out-Null

# --- 4. Export -------------------------------------------------------------------------------
if (-not $NoBounce) {
    Write-Host "4. Bouncing..."
    Invoke-Tool 'bounce' @{ format = 'mp3'; normalize_lufs = -14; end_seconds = [Math]::Round($bars * $barSeconds + 1.5, 3) } | Out-Null
    do {
        Start-Sleep -Milliseconds 500
        $status = Invoke-Tool 'get_status'
    } while ($status.bounce.running)
    $status.bounce | ConvertTo-Json -Depth 4
}
Write-Host "Done."
