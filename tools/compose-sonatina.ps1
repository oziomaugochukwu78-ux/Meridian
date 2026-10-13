# Composes "Sonatina in C" inside Meridian, through its AI tools: an original
# piece in the classical style of Mozart's piano sonatinas (Allegro, about two
# minutes). Right hand: a periodic melody in 4-bar phrases; left hand: Alberti
# bass (low-high-middle-high eighths). Form:
#   A  (bars 1-8)   theme in C: question ending on G, answer ending on C
#   A2 (9-16)       the theme again, ornamented
#   B  (17-24)      a new tune in G major (the dominant)
#   R  (25-32)      sequence and scales over G7, preparing the return
#   A  (33-40)      the theme returns
#   M  (41-48)      an episode in A minor
#   A2 (49-56)      the ornamented theme
#   Coda (57-62)    cadences and a final chord
# Every sound is Meridian's built-in synthesiser; the hall is Meridian Reverb.
#
# Meridian must be running with a session open (an empty one is fine):
#   .\tools\compose-sonatina.ps1
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
    $buffer = New-Object byte[] 1048576
    $result = $ws.ReceiveAsync((New-Object ArraySegment[byte] (,$buffer)), $ct).Result
    $reply = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count) | ConvertFrom-Json
    if (-not $reply.ok) { throw "$tool failed: $($reply.error)" }
    $reply.result
}

function Note([int]$pitch, [double]$start, [double]$length, [double]$velocity) {
    @{ pitch = $pitch; start_beats = $start; length_beats = $length; velocity = [Math]::Round([Math]::Min(1.0, $velocity), 3) }
}

# --- The melody: one string per bar, "pitch:beats" (60 = middle C) -------------------
$A1 = @(
    '72:1 76:1 79:1.5 77:.25 76:.25',
    '74:1 77:.5 76:.5 74:1 71:1',
    '72:.5 74:.5 76:.5 77:.5 79:1 84:1',
    '83:.5 81:.5 79:.5 77:.5 74:2',
    '76:1 79:.5 76:.5 72:1 76:1',
    '77:1.5 81:.5 79:.5 77:.5 76:.5 74:.5',
    '79:1 76:1 74:.25 76:.25 74:.25 72:.25 74:1',
    '72:2 67:.5 72:.5 76:.5 79:.5'
)
$A2 = @(
    '72:.75 74:.25 76:.75 77:.25 79:1.5 77:.25 76:.25',
    '74:.5 79:.5 77:.5 76:.5 74:.5 77:.5 71:1',
    '72:.25 74:.25 76:.25 77:.25 79:.25 81:.25 83:.25 84:.25 88:1 84:1',
    $A1[3],
    '76:.5 79:.5 84:.5 79:.5 76:.5 72:.5 76:1',
    $A1[5],
    $A1[6]
)
$B = @(
    '79:1 83:.5 81:.5 79:.5 78:.5 79:1',
    '81:1 84:.5 83:.5 81:.5 79:.5 78:1',
    '79:.5 81:.5 83:.5 84:.5 86:1 83:1',
    '81:.5 79:.5 78:.5 76:.5 74:2',
    '79:1.5 76:.5 83:1 79:1',
    '76:1.5 72:.5 81:1 76:1',
    '74:.5 79:.5 83:.5 79:.5 81:.25 83:.25 81:.25 78:.25 81:1',
    '79:2 78:.5 79:.5 81:.5 83:.5'
)
$R = @(
    '84:1 81:.5 76:.5 72:1 76:1',
    '78:1 81:.5 84:.5 83:1 81:1',
    '79:.5 83:.5 86:.5 83:.5 79:.5 74:.5 71:.5 74:.5',
    '77:2 74:1 71:1',
    '67:.25 69:.25 71:.25 72:.25 74:.25 76:.25 77:.25 79:.25 77:.5 74:.5 71:1',
    '71:.25 72:.25 74:.25 76:.25 77:.25 79:.25 81:.25 83:.25 86:1 83:1',
    '79:.5 77:.5 74:.5 71:.5 67:.5 71:.5 74:.5 77:.5',
    '79:1 77:1 74:2'
)
$M = @(
    '76:1.5 72:.5 69:1 72:1',
    '71:1.5 74:.5 80:1 76:1',
    '81:1 84:.5 83:.5 81:.5 80:.5 81:1',
    '83:1 80:.5 76:.5 71:2',
    '77:1.5 74:.5 81:1 77:1',
    '76:1.5 72:.5 81:1 76:1',
    '74:.5 76:.5 77:.5 74:.5 71:.5 80:.5 83:1',
    '81:2 79:1 77:1'
)
$Coda = @(
    '84:1 76:1 77:1 81:1',
    '79:1 76:1 77:.5 74:.5 71:1',
    '76:1 79:1 81:1 84:1',
    '86:.25 84:.25 83:.25 81:.25 79:.25 77:.25 76:.25 74:.25 71:1 74:1',
    '84:1 79:.5 76:.5 74:1 71:1',
    'END'
)

# --- The harmony: two half-bar chords per bar, played as Alberti bass ----------------
# Each chord: low, high, middle (the pattern is low-high-middle-high in eighths).
$voicing = @{
    'C'  = @(48, 55, 52); 'G7' = @(47, 53, 50); 'G'  = @(47, 55, 50); 'F'  = @(45, 53, 48)
    'C64' = @(43, 52, 48); 'GG' = @(43, 50, 47); 'D7' = @(42, 48, 45); 'D'  = @(42, 50, 45)
    'Em' = @(40, 47, 43); 'Gd' = @(38, 47, 43); 'Am' = @(45, 52, 48); 'E7' = @(44, 50, 47)
    'E'  = @(44, 52, 47); 'Dm' = @(41, 50, 45)
}
$hA = @('C C', 'G7 G7', 'C C', 'G G', 'C C', 'F F', 'C64 G7', 'C C')
$hB = @('GG GG', 'D7 D7', 'GG GG', 'D D', 'Em Em', 'C C', 'Gd D7', 'GG GG')
$hR = @('Am Am', 'D7 D7', 'GG GG', 'G7 G7', 'G7 G7', 'G7 G7', 'G7 G7', 'G7 G7')
$hM = @('Am Am', 'E7 E7', 'Am Am', 'E E', 'Dm Dm', 'Am Am', 'E7 E7', 'Am G7')
$hCoda = @('C F', 'C64 G7', 'C F', 'G7 G7', 'C G7', 'END')

# The whole piece: (melody bar, harmony bar, loudness) in order.
$sections = @(
    @{ melody = $A1;                  harmony = $hA;    loud = 0.74 },
    @{ melody = ($A2 + @('72:2 71:.5 72:.5 74:.5 76:.5')); harmony = $hA; loud = 0.66 },   # rises into G major
    @{ melody = $B;                   harmony = $hB;    loud = 0.78 },
    @{ melody = $R;                   harmony = $hR;    loud = 0.80 },
    @{ melody = $A1;                  harmony = $hA;    loud = 0.76 },
    @{ melody = $M;                   harmony = $hM;    loud = 0.62 },
    @{ melody = ($A2 + @($A1[7]));    harmony = $hA;    loud = 0.74 },
    @{ melody = $Coda;                harmony = $hCoda; loud = 0.86 }
)

$right = @(); $left = @()
$bar = 0
foreach ($s in $sections) {
    if ($s.melody.Count -ne $s.harmony.Count) { throw "section length mismatch at bar $($bar + 1)" }
    for ($i = 0; $i -lt $s.melody.Count; $i++) {
        $t0 = $bar * 4.0
        $loud = $s.loud
        if ($s.melody[$i] -eq 'END') {
            # The final chord: C major in both hands, held three beats.
            foreach ($p in 72, 76, 79, 84) { $right += Note $p $t0 3 ($loud + 0.04) }
            foreach ($p in 36, 48, 55)     { $left  += Note $p $t0 3 ($loud * 0.8) }
            $bar++
            continue
        }

        # Right hand: the bar must add up to exactly four beats.
        $t = $t0; $first = $true
        foreach ($token in ($s.melody[$i] -split ' ')) {
            $pitch, $beats = $token -split ':'
            $beats = [double]$beats
            $length = if ($beats -le 0.25) { $beats * 0.8 } else { $beats * 0.94 }   # light, articulated
            $vel = $loud + $(if ($first) { 0.08 } else { 0.0 }) + ([int]$pitch - 76) * 0.006 # higher notes sing out
            $right += Note ([int]$pitch) $t $length $vel
            $t += $beats; $first = $false
        }
        if ([Math]::Abs($t - $t0 - 4.0) -gt 1e-9) { throw "bar $($bar + 1) has $($t - $t0) beats: $($s.melody[$i])" }

        # Left hand: Alberti bass, two half-bar chords.
        $halves = $s.harmony[$i] -split ' '
        for ($h = 0; $h -lt 2; $h++) {
            $v = $voicing[$halves[$h]]
            if (-not $v) { throw "unknown chord $($halves[$h]) in bar $($bar + 1)" }
            $order = @($v[0], $v[1], $v[2], $v[1])
            for ($e = 0; $e -lt 4; $e++) {
                $vel = ($loud * 0.68) + $(if ($e -eq 0) { 0.07 } else { 0.0 })
                $left += Note $order[$e] ($t0 + $h * 2 + $e * 0.5) 0.47 $vel
            }
        }
        $bar++
    }
}
"{0} bars, {1} right-hand and {2} left-hand notes" -f $bar, $right.Count, $left.Count

# --- The instrument: a fortepiano-like voice from the built-in synth -----------------
Invoke-Tool 'set_tempo' @{ bpm = 126; clear_changes = $true } | Out-Null
$totalBeats = $bar * 4 + 4
$piano = @{ waveform = 'saw'; detune_cents = 3; attack = 0.002; decay = 1.6; sustain = 0.0; release = 0.45;
            filter_env_octaves = 2.6; resonance = 0.05 }
foreach ($hand in @(@{ name = 'Right Hand'; pan = 0.12; cutoff = 1900; gain = -14; volume = -3 },
                    @{ name = 'Left Hand';  pan = -0.12; cutoff = 1100; gain = -14; volume = -5 })) {
    Invoke-Tool 'create_track' @{ kind = 'instrument'; name = $hand.name } | Out-Null
    $synth = $piano.Clone(); $synth.track = $hand.name; $synth.cutoff_hz = $hand.cutoff; $synth.gain_db = $hand.gain
    Invoke-Tool 'set_synth' $synth | Out-Null
    Invoke-Tool 'set_track' @{ track = $hand.name; volume_db = $hand.volume; pan = $hand.pan } | Out-Null
    Invoke-Tool 'add_midi_clip' @{ track = $hand.name; start_beat = 0; length_beats = $totalBeats; name = $hand.name } | Out-Null
}
foreach ($part in @(@('Right Hand', $right), @('Left Hand', $left))) {
    $r = Invoke-Tool 'add_notes' @{ track = $part[0]; clip = 1; notes = $part[1] }
    "{0,-11} {1,4} notes" -f $part[0], $r.total_notes
}

# --- A small hall ------------------------------------------------------------------
Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Hall' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Hall'; slot = 'A'; plugin = 'Meridian Reverb' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'mix'; value = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'decay'; value = 2.0 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'damping'; value = 0.5 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Right Hand'; slot = 'A'; bus = 'Hall'; level_db = -10 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Left Hand'; slot = 'A'; bus = 'Hall'; level_db = -12 } | Out-Null

Invoke-Tool 'set_master' @{ console = $true; drive_db = 2; character = 0.2; curve = 'tanh'; volume_db = 0 } | Out-Null
Invoke-Tool 'save_session' @{} | Out-Null
$b = Invoke-Tool 'bounce' @{ normalize_lufs = -16; ceiling_dbtp = -1 }
"bouncing to $($b.bouncing_to)"
for ($i = 0; $i -lt 240; $i++) {
    Start-Sleep -Milliseconds 500
    $s = Invoke-Tool 'get_status' @{}
    if (-not $s.bounce.running -and $s.bounce.last_file) {
        "done: {0}  ({1} LUFS, true peak {2} dBTP)" -f $s.bounce.last_file, $s.bounce.last_integrated_lufs, $s.bounce.last_true_peak_dbtp
        break
    }
}
try { $ws.CloseAsync('NormalClosure', 'done', $ct).Wait(2000) | Out-Null } catch { }
