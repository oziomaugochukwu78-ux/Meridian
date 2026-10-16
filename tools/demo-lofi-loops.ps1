# The launch demo, sample-based (how most lo-fi is made): Meridian's AI tools arrange
# and mix human-played loops into an 84-second track.
#
#   bars  1-4   intro      melody A, muffled; the filter opens (anticipation)
#   bars  5-12  section A  drums and bass enter; the melody ducks on every kick
#   bars 13-16  break      melody B alone, darker (contrast)
#   bars 17-24  section B  everything back; the drums stop for the last beat (surprise)
#   bars 25-28  outro      melody A; the filter closes into the vinyl
#
# The bass follows the loops' chords, found by analysing them (the lowest strong
# pitch of each bar), and plays with the drum loop's kicks (beats 1 and 3-and).
#
# Loops are not in the repository. Give the folder of the free "Lofi Sample Pack"
# by Into Infinity (Samples By Heuse): its Guitar Loops and Drum Loops folders.
#   .\tools\demo-lofi-loops.ps1 -Pack '...\Free Lofi Sample Pack - Into Infinity'
[CmdletBinding()]
param([Parameter(Mandatory)] [string]$Pack, [string]$Url = 'ws://127.0.0.1:47800/', [switch]$NoBounce)

$ErrorActionPreference = 'Stop'
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [Threading.CancellationToken]::None
$ws.ConnectAsync([Uri]$Url, $ct).Wait(5000) | Out-Null
if ($ws.State -ne 'Open') { throw "Could not connect to $Url (is Meridian running?)" }

function Invoke-Tool([string]$tool, [hashtable]$arguments = @{}) {
    $json = @{ tool = $tool; arguments = $arguments } | ConvertTo-Json -Depth 8 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $ws.SendAsync((New-Object ArraySegment[byte] (,$bytes)), 'Text', $true, $ct).Wait()
    $buffer = New-Object byte[] 4194304
    $result = $ws.ReceiveAsync((New-Object ArraySegment[byte] (,$buffer)), $ct).Result
    $reply = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count) | ConvertFrom-Json
    if (-not $reply.ok) { throw "$tool failed: $($reply.error)" }
    $reply.result
}
function Clips([string]$track) {
    $r = Invoke-Tool 'list_clips' @{ track = $track }
    $list = $(if ($r.clips) { $r.clips } else { $r })
    @($list | Where-Object { $_.track -eq $track })
}
# Imports a whole file at `at` seconds (the import runs in the background), then cuts it at `until`.
function Place([string]$track, [string]$file, [double]$at, [double]$until = -1, [double]$gainDb = 0) {
    $before = @(Clips $track).Count   # @(): one result must still count as 1 (PowerShell 5)
    Invoke-Tool 'import_audio' @{ file = $file; track = $track; position_seconds = $at } | Out-Null
    $deadline = (Get-Date).AddSeconds(90)
    while (@(Clips $track).Count -le $before) {
        if ((Get-Date) -gt $deadline) { throw "import of $file timed out" }
        Start-Sleep -Milliseconds 300
    }
    if ($until -gt $at) {
        Invoke-Tool 'split_clip' @{ track = $track; seconds = $until } | Out-Null
        $rest = Clips $track | Where-Object { [Math]::Abs($_.start_seconds - $until) -lt 0.01 }
        foreach ($r in @($rest)) { Invoke-Tool 'delete_clip' @{ track = $track; clip = $r.clip } | Out-Null }
    }
    $placed = Clips $track | Where-Object { [Math]::Abs($_.start_seconds - $at) -lt 0.01 } | Select-Object -First 1
    Invoke-Tool 'set_clip' @{ track = $track; clip = $placed.clip; gain_db = $gainDb; fade_out_seconds = 0.02 } | Out-Null
}
function Note([int]$pitch, [double]$start, [double]$length, [double]$velocity) {
    @{ pitch = $pitch; start_beats = [Math]::Round($start, 4); length_beats = $length; velocity = $velocity }
}

$bpm = 80.0
$bar = 4 * 60.0 / $bpm          # 3 s
$melodyA = Join-Path $Pack 'Guitar Loops\IntoInfinity_LofiPack_MelodyLoop_04_Gminor_80BPM.wav'
$melodyB = Join-Path $Pack 'Guitar Loops\IntoInfinity_LofiPack_MelodyLoop_05_Gminor_80BPM.wav'
$drumLoop = Join-Path $Pack 'Drum Loops\IntoInfinity_LofiPack_DrumLoop_01_80BPM.wav'
foreach ($f in $melodyA, $melodyB, $drumLoop) { if (-not (Test-Path $f)) { throw "Missing $f" } }

# --- 1. Arrange the loops ----------------------------------------------------------------
Write-Host "1. Arranging the loops..."
Invoke-Tool 'set_tempo' @{ bpm = $bpm } | Out-Null
foreach ($name in 'Melody A', 'Melody B', 'Drums') { Invoke-Tool 'create_track' @{ kind = 'audio'; channels = 2; name = $name } | Out-Null }
Place 'Melody A' $melodyA 0 (4 * $bar) -2               # intro: its first four bars
Place 'Melody A' $melodyA (4 * $bar) -1 -2              # section A: the whole phrase, its tail ringing into the break
Place 'Melody B' $melodyB (12 * $bar) (16 * $bar) -2    # break: first four bars
Place 'Melody B' $melodyB (16 * $bar) -1 -2             # section B
Place 'Melody A' $melodyA (24 * $bar) (28 * $bar) -2    # outro
Place 'Drums' $drumLoop (4 * $bar) -1 3                 # section A
Place 'Drums' $drumLoop (16 * $bar) (24 * $bar - 60.0 / $bpm) 3   # section B, stopping one beat early

# --- 2. Bass, following the analysed chords and locked to the kicks ------------------------------
Write-Host "2. Writing the bass..."
$rootsA = @(43, 46, 45, 45, 43, 46, 48, 50)    # G Bb A A G Bb C D   (melody A, bars 1-8)
$rootsB = @(43, 46, 45, 45, 46, 46, 48, 45)    # G Bb A A Bb Bb C A  (melody B)
Invoke-Tool 'create_track' @{ kind = 'instrument'; name = 'Bass' } | Out-Null
Invoke-Tool 'set_synth' @{ track = 'Bass'; engine = 'synth'; waveform = 'sine'; detune_cents = 0; attack = 0.006; decay = 1.2;
                           sustain = 0.5; release = 0.12; cutoff_hz = 400; resonance = 0; filter_env_octaves = 0; gain_db = -3 } | Out-Null
Invoke-Tool 'add_midi_clip' @{ track = 'Bass'; start_beat = 0; length_beats = 28 * 4; name = 'Bassline' } | Out-Null
$bass = @()
foreach ($section in @(@{ first = 4; roots = $rootsA }, @{ first = 16; roots = $rootsB })) {
    for ($i = 0; $i -lt 8; $i++) {
        $b = ($section.first + $i) * 4
        $root = $section.roots[$i]
        $bass += Note $root ($b + 0.0) 2.3 0.9            # with the kick on 1
        $bass += Note $root ($b + 2.5) 1.3 0.75           # with the kick on 3-and
        if ($i % 2 -eq 1 -and $i -lt 7) {                  # a passing note into the next bar's root
            $next = $section.roots[$i + 1]
            $bass += Note ($next + $(if ($next -gt $root) { -1 } else { 1 })) ($b + 3.75) 0.25 0.6
        }
    }
}
Invoke-Tool 'add_notes' @{ track = 'Bass'; clip = 1; notes = $bass } | Out-Null

# --- 3. Mix ----------------------------------------------------------------------------------
Write-Host "3. Mixing..."
foreach ($m in 'Melody A', 'Melody B') {
    Invoke-Tool 'insert_plugin' @{ track = $m; slot = 'A'; plugin = 'Meridian Lo-Fi' } | Out-Null
    foreach ($s in @(@('wow', 0.35), @('flutter', 0.3), @('noise', 0.25), @('tone', 8000), @('mix', 1))) {
        Invoke-Tool 'set_plugin_parameter' @{ track = $m; slot = 'A'; parameter = $s[0]; value = $s[1] } | Out-Null
    }
    Invoke-Tool 'insert_plugin' @{ track = $m; slot = 'B'; plugin = 'Meridian EQ' } | Out-Null
    Invoke-Tool 'set_plugin_parameter' @{ track = $m; slot = 'B'; parameter = 'hpOn'; value = 1 } | Out-Null
    Invoke-Tool 'set_plugin_parameter' @{ track = $m; slot = 'B'; parameter = 'hpFreq'; value = 140 } | Out-Null   # room for the bass
}
Invoke-Tool 'set_up' @{ goal = 'drum bus'; name = 'Drum Bus' } | Out-Null
Invoke-Tool 'set_output' @{ track = 'Drums'; destination = 'Drum Bus' } | Out-Null
Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Reverb' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Reverb'; slot = 'A'; plugin = 'Meridian Reverb' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Reverb'; slot = 'A'; parameter = 'mix'; normalized = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Reverb'; slot = 'A'; parameter = 'size'; normalized = 0.7 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Melody A'; slot = 'A'; bus = 'Reverb'; level_db = -16 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Melody B'; slot = 'A'; bus = 'Reverb'; level_db = -16 } | Out-Null
Invoke-Tool 'set_master' @{ preset = 'Warm Tape' } | Out-Null
Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Mix Bus' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Mix Bus'; slot = 'A'; plugin = 'Meridian Limiter' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Mix Bus'; slot = 'A'; parameter = 'input'; value = 1.5 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Mix Bus'; slot = 'A'; parameter = 'ceiling'; value = -1.2 } | Out-Null
foreach ($source in 'Melody A', 'Melody B', 'Drum Bus', 'Bass', 'Reverb') {
    Invoke-Tool 'set_output' @{ track = $source; destination = 'Mix Bus' } | Out-Null
}
# Balance, measured alone: the melodies near -20 dB RMS, the drum loop -30 and the bass -27; the clip
# gains above and these faders bring drums and bass up beside the melodies.
Invoke-Tool 'set_track' @{ track = 'Bass'; volume_db = -12 } | Out-Null
Invoke-Tool 'set_track' @{ track = 'Drum Bus'; volume_db = 4 } | Out-Null

# --- 4. Movement: filter sweeps, the break, ducking on the kicks, the fade -------------------------
Write-Host "4. Automating..."
$tone = 'insert A: Tone'
Invoke-Tool 'set_automation' @{ track = 'Melody A'; parameter = $tone; points = @(
    @{ seconds = 0; value = 900 }, @{ seconds = 2 * $bar; value = 900 }, @{ seconds = 4 * $bar; value = 8000 },   # the intro opens
    @{ seconds = 24 * $bar; value = 8000 }, @{ seconds = 28 * $bar; value = 700 }) } | Out-Null                # the outro closes
Invoke-Tool 'set_automation' @{ track = 'Melody B'; parameter = $tone; points = @(
    @{ seconds = 12 * $bar; value = 2200 }, @{ seconds = 16 * $bar - 0.01; value = 2200 }, @{ seconds = 16 * $bar; value = 8000 }) } | Out-Null

# Ducking ("sidechain" pump): 4 dB down on every kick of the drum loop, back over 250 ms.
$kickBeats = @(0, 2.5)
foreach ($m in @(@{ track = 'Melody A'; first = 4 }, @{ track = 'Melody B'; first = 16 })) {
    $points = @(@{ seconds = 0; value = 0 })
    for ($i = 0; $i -lt 8; $i++) {
        foreach ($k in $kickBeats) {
            $t = (($m.first + $i) * 4 + $k) * 60.0 / $bpm
            $points += @{ seconds = [Math]::Round($t - 0.004, 3); value = 0 }
            $points += @{ seconds = [Math]::Round($t + 0.01, 3); value = -4 }
            $points += @{ seconds = [Math]::Round($t + 0.26, 3); value = 0 }
        }
    }
    if ($m.track -eq 'Melody A') {   # the outro fades away
        $points += @{ seconds = 24 * $bar; value = 0 }
        $points += @{ seconds = 28 * $bar; value = -30 }
    }
    Invoke-Tool 'set_automation' @{ track = $m.track; parameter = 'volume'; points = $points } | Out-Null
}

# --- 5. Export -------------------------------------------------------------------------------
if (-not $NoBounce) {
    Write-Host "5. Bouncing..."
    Invoke-Tool 'bounce' @{ format = 'mp3'; normalize_lufs = -14; end_seconds = 28 * $bar + 2 } | Out-Null
    do { Start-Sleep -Milliseconds 500; $status = Invoke-Tool 'get_status' } while ($status.bounce.running)
    $status.bounce | ConvertTo-Json -Depth 4
}
Write-Host "Done."
