# Composes "Amber Waltz" entirely inside Meridian, through its AI tools: an
# original harpsichord-style piece in D minor whose phrases alternate a bar of
# six eighth notes with a bar of seven, which gives it a lilting, off-kilter
# waltz feel. Every sound is Meridian's built-in synthesiser and drum machine;
# the reverb is the built-in Meridian Reverb on a bus.
#
# Meridian must be running with a session open (an empty one is fine):
#   .\tools\compose-amber-waltz.ps1
#
# Each cycle is 13 eighths: 6 over one chord, 7 over the next.
# Dm (6) C (7) | Bb (6) A (7), four times, then a final Dm.
[CmdletBinding()]
param([string]$Url = 'ws://127.0.0.1:47800/')

$ErrorActionPreference = 'Stop'

$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [Threading.CancellationToken]::None
$ws.ConnectAsync([Uri]$Url, $ct).Wait(5000) | Out-Null
if ($ws.State -ne 'Open') { throw "Could not connect to $Url (is Meridian running?)" }

function Invoke-Tool([string]$tool, [hashtable]$arguments) {
    $json = @{ tool = $tool; arguments = $arguments } | ConvertTo-Json -Depth 8 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $ws.SendAsync((New-Object ArraySegment[byte] (,$bytes)), 'Text', $true, $ct).Wait()
    $buffer = New-Object byte[] 262144
    $result = $ws.ReceiveAsync((New-Object ArraySegment[byte] (,$buffer)), $ct).Result
    $reply = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count) | ConvertFrom-Json
    if (-not $reply.ok) { throw "$tool failed: $($reply.error)" }
    $reply.result
}

function Note([int]$pitch, [double]$start, [double]$length, [double]$velocity = 0.8) {
    @{ pitch = $pitch; start_beats = $start; length_beats = $length; velocity = $velocity }
}

Invoke-Tool 'set_tempo' @{ bpm = 132 } | Out-Null

# Chords as (root, arpeggio tones from low to high); the arpeggio walks up and back.
$chords = @(
    @{ root = 38; tones = @(62, 69, 74, 77, 81) },   # Dm
    @{ root = 36; tones = @(60, 67, 72, 76, 79) },   # C
    @{ root = 34; tones = @(58, 65, 70, 74, 77) },   # Bb
    @{ root = 33; tones = @(57, 64, 69, 73, 76) }    # A (major: the leading tone pulls home)
)
$sixPattern   = @(0, 1, 2, 3, 2, 1)       # six eighths
$sevenPattern = @(0, 1, 2, 3, 4, 3, 2)    # seven eighths
$progressions = 5                         # an intro, then four with the melody (13 beats each)

# --- The instruments ------------------------------------------------------------
$tracks = @(
    @{ name = 'Harpsichord'; kind = 'instrument'; volume = -4; pan = -0.12;
       synth = @{ waveform = 'saw'; detune_cents = 7; attack = 0.001; decay = 0.9; sustain = 0.0; release = 0.25;
                  cutoff_hz = 3800; resonance = 0.12; filter_env_octaves = 2.5; gain_db = -15 } },
    @{ name = 'Bass'; kind = 'instrument'; volume = -5; pan = 0;
       synth = @{ waveform = 'saw'; detune_cents = 0; attack = 0.002; decay = 1.0; sustain = 0.25; release = 0.15;
                  cutoff_hz = 320; resonance = 0.2; filter_env_octaves = 1.5; gain_db = -11 } },
    @{ name = 'Melody'; kind = 'instrument'; volume = -6; pan = 0.15;
       synth = @{ waveform = 'sine'; detune_cents = 0; attack = 0.05; decay = 0.4; sustain = 0.75; release = 0.35;
                  cutoff_hz = 6000; resonance = 0.0; filter_env_octaves = 0.5; gain_db = -9 } },
    @{ name = 'Brushes'; kind = 'drums'; volume = -12; pan = 0 }
)

$totalBeats = ($progressions * 2 * 6.5) + 4
foreach ($t in $tracks) {
    Invoke-Tool 'create_track' @{ kind = $t.kind; name = $t.name } | Out-Null
    if ($t.synth) { $synth = $t.synth.Clone(); $synth.track = $t.name; Invoke-Tool 'set_synth' $synth | Out-Null }
    Invoke-Tool 'set_track' @{ track = $t.name; volume_db = $t.volume; pan = $t.pan } | Out-Null
    Invoke-Tool 'add_midi_clip' @{ track = $t.name; start_beat = 0; length_beats = $totalBeats; name = $t.name } | Out-Null
}

# --- The notes ------------------------------------------------------------------
$harp = @(); $bass = @(); $drums = @()
$beat = 0.0
for ($p = 0; $p -lt $progressions; $p++) {
    for ($half = 0; $half -lt 2; $half++) {
        foreach ($group in 0, 1) {
            $chord = $chords[$half * 2 + $group]
            $pattern = if ($group -eq 0) { $sixPattern } else { $sevenPattern }
            $length = $pattern.Count * 0.5
            # Harpsichord: every eighth, the first of each group a little stronger.
            for ($e = 0; $e -lt $pattern.Count; $e++) {
                $vel = if ($e -eq 0) { 0.85 } else { 0.62 }
                $harp += Note $chord.tones[$pattern[$e]] ($beat + $e * 0.5) 0.48 $vel
            }
            # Bass: the root for the whole group, with a fifth on the last eighth.
            $bass += Note $chord.root $beat ($length - 0.5) 0.85
            $bass += Note ($chord.root + 7) ($beat + $length - 0.5) 0.45 0.6
            # Brushes from the second progression: kick on the group, soft stick and hat.
            if ($p -ge 1) {
                $drums += Note 36 $beat 0.5 0.7
                $drums += Note 37 ($beat + 1.5) 0.5 0.45
                for ($e = 0; $e -lt $pattern.Count; $e++) { $drums += Note 42 ($beat + $e * 0.5) 0.25 (0.25 + 0.1 * (($e + 1) % 2)) }
            }
            $beat += $length
        }
    }
}
# Home: a held D minor.
$harp += Note 50 $beat 4 0.8; $harp += Note 57 $beat 4 0.7; $harp += Note 62 $beat 4 0.7; $harp += Note 65 $beat 4 0.7
$bass += Note 38 $beat 4 0.9

# The melody (original), from the second progression: two phrases, then their answer.
# Each line is one group (6 or 7 eighths = 3 or 3.5 beats) of (pitch, beats).
$phraseA = @(
    @(@(81,1.5), @(79,0.5), @(77,1)),             # Dm
    @(@(76,1.5), @(74,0.5), @(76,0.5), @(79,1)),  # C
    @(@(77,1.5), @(74,0.5), @(77,1)),             # Bb
    @(@(76,2), @(73,1), @(69,0.5))                # A
)
$phraseB = @(
    @(@(86,1), @(84,0.5), @(81,1.5)),             # Dm
    @(@(79,1), @(76,0.5), @(79,1), @(84,1)),      # C
    @(@(82,1.5), @(81,0.5), @(77,1)),             # Bb
    @(@(76,1.5), @(79,0.5), @(81,1.5))            # A
)
$phraseC = @(
    @(@(77,1), @(76,0.5), @(74,1.5)),             # Dm
    @(@(72,1), @(76,1), @(79,1.5)),               # C
    @(@(74,1.5), @(77,0.5), @(82,1)),             # Bb
    @(@(81,2), @(76,1), @(73,0.5))                # A
)
$melodyGroups = @($phraseA + $phraseB + $phraseC + $phraseA)
$lead = @()
$beat = 2 * 6.5   # the second progression
foreach ($group in $melodyGroups) {
    $t = $beat
    foreach ($n in $group) { $lead += Note $n[0] $t ($n[1] * 0.92) 0.8; $t += $n[1] }
    $groupLength = ($group | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum
    $beat += $groupLength
}
$lead += Note 74 $beat 4 0.75   # resolve on D with the final chord

foreach ($part in @(@('Harpsichord', $harp), @('Bass', $bass), @('Melody', $lead), @('Brushes', $drums))) {
    $r = Invoke-Tool 'add_notes' @{ track = $part[0]; clip = 1; notes = $part[1] }
    "{0,-12} {1,4} notes" -f $part[0], $r.total_notes
}

# --- A small hall: Meridian Reverb on a bus, fed by sends -------------------------
Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Hall' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Hall'; slot = 'A'; plugin = 'Meridian Reverb' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'mix'; value = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'decay'; value = 2.4 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'damping'; value = 0.45 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Harpsichord'; slot = 'A'; bus = 'Hall'; level_db = -12 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Melody'; slot = 'A'; bus = 'Hall'; level_db = -9 } | Out-Null

Invoke-Tool 'set_master' @{ console = $true; drive_db = 4; character = 0.3; curve = 'tanh'; volume_db = 0 } | Out-Null
Invoke-Tool 'save_session' @{} | Out-Null
$b = Invoke-Tool 'bounce' @{ normalize_lufs = -14; ceiling_dbtp = -1 }
"bouncing to $($b.bouncing_to)"
for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 500
    $s = Invoke-Tool 'get_status' @{}
    if (-not $s.bounce.running -and $s.bounce.last_file) {
        "done: {0}  ({1} LUFS, true peak {2} dBTP)" -f $s.bounce.last_file, $s.bounce.last_integrated_lufs, $s.bounce.last_true_peak_dbtp
        break
    }
}
try { $ws.CloseAsync('NormalClosure', 'done', $ct).Wait(2000) | Out-Null } catch { }
