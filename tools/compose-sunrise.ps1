# Composes "Sunrise" entirely inside Meridian, through its AI tools: it creates
# instrument tracks, shapes their synth sounds, writes the notes, sets the mix
# and bounces. Every sound is made by Meridian's built-in synthesiser.
#
# Meridian must be running with a session open (an empty one is fine):
#   .\tools\compose-sunrise.ps1
#
# C major, 90 BPM, C - G - Am - F. 13 bars: intro (pad, bass, arpeggio), theme,
# variation, final chord.
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

# Harmony: C, G, Am, F (close voicings), and the bass roots.
$chords = @(@(60, 64, 67), @(59, 62, 67), @(57, 60, 64), @(57, 60, 65))
$roots  = @(36, 43, 45, 41)
$bars = 13

Invoke-Tool 'set_tempo' @{ bpm = 90 } | Out-Null

# --- The instruments ------------------------------------------------------------
$tracks = @(
    @{ name = 'Pad';  synth = @{ waveform = 'saw'; detune_cents = 12; attack = 0.6; decay = 2.0; sustain = 0.7; release = 1.5;
                                 cutoff_hz = 900; resonance = 0.1; filter_env_octaves = 1.0; gain_db = -16 }; volume = -5; pan = 0 },
    @{ name = 'Bass'; synth = @{ waveform = 'saw'; detune_cents = 0; attack = 0.003; decay = 0.4; sustain = 0.5; release = 0.08;
                                 cutoff_hz = 420; resonance = 0.25; filter_env_octaves = 2.0; gain_db = -10 }; volume = -3; pan = 0 },
    @{ name = 'Arp';  synth = @{ waveform = 'square'; detune_cents = 4; attack = 0.002; decay = 0.3; sustain = 0.0; release = 0.2;
                                 cutoff_hz = 2200; resonance = 0.2; filter_env_octaves = 2.5; gain_db = -16 }; volume = -9; pan = -0.35 },
    @{ name = 'Lead'; synth = @{ waveform = 'saw'; detune_cents = 6; attack = 0.03; decay = 0.6; sustain = 0.65; release = 0.35;
                                 cutoff_hz = 1600; resonance = 0.15; filter_env_octaves = 1.5; gain_db = -12 }; volume = -4; pan = 0.2 },
    @{ name = 'Kick'; synth = @{ waveform = 'sine'; detune_cents = 0; attack = 0.001; decay = 0.25; sustain = 0.0; release = 0.05;
                                 cutoff_hz = 20000; resonance = 0; filter_env_octaves = 0; gain_db = -4 }; volume = -6; pan = 0 }
)
foreach ($t in $tracks) {
    Invoke-Tool 'create_track' @{ kind = 'instrument'; name = $t.name } | Out-Null
    $synth = $t.synth.Clone(); $synth.track = $t.name
    Invoke-Tool 'set_synth' $synth | Out-Null
    Invoke-Tool 'set_track' @{ track = $t.name; volume_db = $t.volume; pan = $t.pan } | Out-Null
    Invoke-Tool 'add_midi_clip' @{ track = $t.name; start_beat = 0; length_beats = $bars * 4; name = $t.name } | Out-Null
}

# --- The notes ------------------------------------------------------------------
$pad = @(); $bass = @(); $arp = @(); $kick = @()
for ($bar = 0; $bar -lt $bars; $bar++) {
    $b0 = $bar * 4
    $c = $bar % 4
    if ($bar -eq 12) { $c = 0 }

    foreach ($p in $chords[$c]) { $pad += Note $p $b0 4 0.6 }
    if ($bar -eq 12) { $pad += Note 48 $b0 4 0.6; $bass += Note 36 $b0 4 0.9; $kick += Note 33 $b0 1 1.0; continue }

    # Bass: root, a pickup, root, and an octave lift into the next bar.
    $r = $roots[$c]
    $bass += Note $r ($b0) 1.5 0.9
    $bass += Note $r ($b0 + 1.5) 0.5 0.7
    $bass += Note $r ($b0 + 2) 1.5 0.85
    $bass += Note ($r + 12) ($b0 + 3.5) 0.5 0.7

    # Arpeggio: eighth notes through the chord, an octave up.
    $up = $chords[$c] | ForEach-Object { $_ + 12 }
    $order = @(0, 1, 2, 1, 0, 1, 2, 1)
    for ($e = 0; $e -lt 8; $e++) {
        $vel = 0.45; if ($e % 2 -eq 0) { $vel = 0.6 }
        $arp += Note $up[$order[$e]] ($b0 + $e * 0.5) 0.45 $vel
    }

    # A soft low thump on beats 1 and 3 once the theme starts.
    if ($bar -ge 4) { $kick += Note 33 $b0 1 1.0; $kick += Note 33 ($b0 + 2) 1 0.85 }
}

# The melody (original): theme over bars 5-8, variation over bars 9-12, home on bar 13.
$melody = @(
    @(@(76,1), @(79,1), @(76,0.5), @(74,0.5), @(72,1)),          # C
    @(@(74,1.5), @(71,0.5), @(74,1), @(79,1)),                   # G
    @(@(76,1.5), @(72,0.5), @(69,1), @(72,1)),                   # Am
    @(@(69,0.5), @(72,0.5), @(77,1), @(76,1), @(74,1)),          # F
    @(@(79,1), @(76,0.5), @(79,0.5), @(84,1), @(83,1)),          # C
    @(@(83,1), @(81,0.5), @(79,0.5), @(74,2)),                   # G
    @(@(84,1), @(83,0.5), @(81,0.5), @(76,1), @(81,1)),          # Am
    @(@(79,1), @(77,1), @(76,1), @(74,1)),                       # F
    @(,@(72,4))                                                  # C
)
$lead = @()
for ($i = 0; $i -lt $melody.Count; $i++) {
    $beat = (4 + $i) * 4
    foreach ($n in $melody[$i]) { $lead += Note $n[0] $beat ($n[1] * 0.95) 0.85; $beat += $n[1] }
}

foreach ($part in @(@('Pad', $pad), @('Bass', $bass), @('Arp', $arp), @('Lead', $lead), @('Kick', $kick))) {
    $r = Invoke-Tool 'add_notes' @{ track = $part[0]; clip = 1; notes = $part[1] }
    "{0,-5} {1,4} notes" -f $part[0], $r.total_notes
}

# A warm master, then the mixdown.
Invoke-Tool 'set_master' @{ console = $true; drive_db = 5; character = 0.35; curve = 'tanh'; volume_db = 0 } | Out-Null
Invoke-Tool 'save_session' @{} | Out-Null
$b = Invoke-Tool 'bounce' @{}
"bouncing to $($b.bouncing_to)"
try { $ws.CloseAsync('NormalClosure', 'done', $ct).Wait(2000) | Out-Null } catch { }
