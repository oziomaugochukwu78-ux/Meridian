# Composes "Menuetto in F, with Trio" inside Meridian, through its AI tools: an
# original piece in the classical style of Mozart's minuets (3/4, about three
# minutes). Form, as a classical minuet is played:
#   Menuetto  ||: A (8 bars, F major -> C major) :||: B (8, sequence through
#             D minor and G minor) + A' (8, cadence home in F) :||
#   Trio      ||: (8, B-flat major -> F) :||: (16, through C minor, home to B-flat) :||
#             softer and legato, Alberti bass, with the sustain pedal
#   Menuetto da capo, without repeats, ending on a held F major chord
# Left hand in the Menuetto: bass on beat 1, chord on beats 2 and 3.
# Every sound is Meridian's built-in synthesiser; the hall is Meridian Reverb.
#
# Meridian must be running with a session open (an empty one is fine):
#   .\tools\compose-menuetto.ps1
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
    @{ pitch = $pitch; start_beats = $start; length_beats = $length; velocity = [Math]::Round([Math]::Max(0.05, [Math]::Min(1.0, $velocity)), 3) }
}

$beatsPerBar = 3

# --- The melody: one string per bar, "pitch:beats" (60 = middle C, R = rest) ---------
# Menuetto, first reprise: F major, the answer turns to C major.
$MA = @(
    '77:1.5 81:.5 79:.5 77:.5',      # F    F (A G F)
    '76:1 79:.5 82:.5 81:.5 79:.5',  # C7   E  G Bb A G
    '81:1 84:.5 82:.5 81:.5 79:.5',  # F    A  C Bb A G
    '79:2 R:1',                      # C    half cadence
    '81:1.5 82:.5 84:.5 81:.5',      # F    A. Bb C A
    '79:.5 83:.5 86:1 77:1',         # G7   G B D F  (to C major)
    '76:1 79:.5 77:.5 76:.5 74:.5',  # C G7 E  G F E D
    '72:2 R:1'                       # C    cadence in the dominant
)
# Second reprise: a sequence through D minor and G minor, then the theme returns.
$MB = @(
    '79:1 76:.5 73:.5 76:1',         # A7   G E C# E
    '77:1.5 76:.5 74:1',             # Dm   F. E D
    '84:1 81:.5 78:.5 81:1',         # D7   C A F# A
    '82:1.5 81:.5 79:1',             # Gm   Bb. A G
    '76:.5 79:.5 82:.5 79:.5 76:.5 72:.5',  # C7  arpeggio
    '82:1 81:.5 79:.5 77:.5 76:.5',  # Gm   Bb A G F E
    '79:1 76:1 72:1',                # C7   G E C
    '72:.5 74:.5 76:.5 77:.5 79:1'   # C7   lead-in back to the theme
)
$MA2 = @(
    $MA[0], $MA[1], $MA[2], $MA[3],
    '81:1.5 82:.5 84:.5 81:.5',      # F
    '86:1 82:.5 77:.5 82:1',         # Bb   D Bb F Bb
    '81:1 79:.5 77:.5 76:.5 74:.5',  # F64 C7
    '77:2 R:1'                       # F    cadence home
)
$hMA  = @('F', 'C7', 'F', 'C', 'F', 'G7', 'C64 G7c', 'Cc')
$hMB  = @('A7', 'Dm', 'D7', 'Gm', 'C7', 'Gm', 'C7', 'C7')
$hMA2 = @('F', 'C7', 'F', 'C', 'F', 'Bb', 'F64 C7', 'F')

# Trio: B-flat major, cantabile.
$TA = @(
    '74:2 77:1',                     # Bb   D (half) F
    '75:2 72:1',                     # F7   Eb C
    '74:1 77:1 82:1',                # Bb   D F Bb
    '81:2 R:1',                      # F    half cadence
    '82:2 79:1',                     # Gm   Bb G
    '79:1 76:1 72:1',                # C7   G E C   (to F major)
    '77:1 81:.5 79:.5 76:1',         # F C7
    '77:2 R:1'                       # F    cadence in the dominant
)
$TB = @(
    '75:1 77:.5 75:.5 72:1',         # F7   Eb F Eb C
    '74:1.5 77:.5 82:1',             # Bb   D. F Bb
    '83:1 79:.5 77:.5 74:1',         # G7   B G F D   (to C minor)
    '75:1.5 74:.5 72:1',             # Cm   Eb. D C
    '72:1 75:1 81:1',                # F7   C Eb A
    '82:1 77:1 74:1',                # Bb   Bb F D
    '79:1 75:.5 79:.5 77:1',         # Eb F7
    '77:1.5 75:.5 72:1',             # F7   F. Eb C  (Eb falls to D)
    $TA[0], $TA[1], $TA[2], $TA[3],
    '82:2 79:1',                     # Gm   Bb G
    '75:1 79:1 82:1',                # Eb   Eb G Bb
    '77:1 74:.5 75:.5 72:1',         # Bb64 F7
    '70:2 R:1'                       # Bb   cadence home
)
$hTA = @('Bb', 'F7', 'Bb', 'F', 'Gm', 'C7', 'F C7', 'F')
$hTB = @('F7', 'Bb', 'G7', 'Cm', 'F7', 'Bb', 'Eb F7', 'F7',
         'Bb', 'F7', 'Bb', 'F', 'Gm', 'Eb', 'Bb64 F7', 'Bb')

# Menuetto voicings: bass, then the two-note chord on beats 2 and 3.
$minuetVoicing = @{
    'F'   = @(41, 57, 60); 'C7'  = @(48, 52, 58); 'C'   = @(48, 52, 55); 'Cc'  = @(36, 52, 55)
    'G7'  = @(43, 53, 59); 'G7c' = @(43, 53, 59); 'C64' = @(43, 52, 60); 'F64' = @(48, 53, 57)
    'A7'  = @(45, 55, 61); 'Dm'  = @(50, 53, 57); 'D7'  = @(42, 57, 60); 'Gm'  = @(43, 58, 62)
    'Bb'  = @(46, 53, 62)
}
# Trio voicings for Alberti bass: low, high, middle.
$albertiVoicing = @{
    'Bb'   = @(46, 53, 50); 'F7'  = @(45, 51, 48); 'Gm' = @(43, 50, 46); 'C7' = @(48, 58, 52)
    'F'    = @(45, 53, 48); 'G7'  = @(47, 53, 50); 'Cm' = @(48, 55, 51); 'Eb' = @(46, 55, 51)
    'Bb64' = @(41, 50, 46)
}

# The whole piece in performance order.
$sections = @(
    @{ melody = $MA;          harmony = $hMA;         style = 'minuet'; loud = 0.76 },
    @{ melody = $MA;          harmony = $hMA;         style = 'minuet'; loud = 0.70 },   # repeat, a shade softer
    @{ melody = $MB + $MA2;   harmony = $hMB + $hMA2; style = 'minuet'; loud = 0.74 },
    @{ melody = $MB + $MA2;   harmony = $hMB + $hMA2; style = 'minuet'; loud = 0.70 },
    @{ melody = $TA;          harmony = $hTA;         style = 'trio';   loud = 0.58 },
    @{ melody = $TA;          harmony = $hTA;         style = 'trio';   loud = 0.54 },
    @{ melody = $TB;          harmony = $hTB;         style = 'trio';   loud = 0.60 },
    @{ melody = $TB;          harmony = $hTB;         style = 'trio';   loud = 0.56 },
    @{ melody = $MA;          harmony = $hMA;         style = 'minuet'; loud = 0.76 },   # da capo, no repeats
    @{ melody = $MB + $MA2;   harmony = $hMB + $hMA2; style = 'minuet'; loud = 0.76; final = $true }
)

$right = @(); $left = @(); $pedal = @()
$bar = 0; $trioStartBar = 0; $trioEndBar = 0
foreach ($s in $sections) {
    if ($s.melody.Count -ne $s.harmony.Count) { throw "section length mismatch at bar $($bar + 1)" }
    if ($s.style -eq 'trio' -and $trioStartBar -eq 0) { $trioStartBar = $bar + 1 }
    if ($s.style -eq 'minuet' -and $trioStartBar -ne 0 -and $trioEndBar -eq 0) { $trioEndBar = $bar }

    for ($i = 0; $i -lt $s.melody.Count; $i++) {
        $t0 = $bar * $beatsPerBar
        $loud = $s.loud
        $isLast = $s.final -and ($i -eq $s.melody.Count - 1)
        $legato = $s.style -eq 'trio'

        if ($isLast) {
            # The final chord: F major in both hands, held.
            foreach ($p in 69, 72, 77) { $right += Note $p $t0 3 ($loud + 0.02) }
            foreach ($p in 29, 41, 53, 57, 60) { $left += Note $p $t0 3 ($loud * 0.78) }
            $bar++
            continue
        }

        # Right hand: every bar must add up to exactly three beats.
        $t = $t0; $first = $true
        foreach ($token in ($s.melody[$i] -split ' ')) {
            $pitch, $beats = $token -split ':'
            $beats = [double]$beats
            if ($pitch -ne 'R') {
                $length = if ($legato) { $beats * 0.98 } elseif ($beats -le 0.5) { $beats * 0.82 } else { $beats * 0.92 }
                $vel = $loud + $(if ($first) { 0.07 } else { 0.0 }) + ([int]$pitch - 76) * 0.006
                $right += Note ([int]$pitch) $t $length $vel
            }
            $t += $beats; $first = $false
        }
        if ([Math]::Abs($t - $t0 - $beatsPerBar) -gt 1e-9) { throw "bar $($bar + 1) has $($t - $t0) beats: $($s.melody[$i])" }

        # Left hand. A bar may name two chords: the first for beats 1-2, the second for beat 3.
        $chords = $s.harmony[$i] -split ' '
        if ($s.style -eq 'minuet') {
            $v1 = $minuetVoicing[$chords[0]]
            if (-not $v1) { throw "unknown chord $($chords[0]) in bar $($bar + 1)" }
            $left += Note $v1[0] $t0 0.9 ($loud * 0.72 + 0.06)                       # bass, beat 1
            for ($b = 1; $b -lt 3; $b++) {
                $v = if ($b -eq 2 -and $chords.Count -gt 1) { $minuetVoicing[$chords[1]] } else { $v1 }
                if (-not $v) { throw "unknown chord $($chords[1]) in bar $($bar + 1)" }
                foreach ($p in $v[1], $v[2]) { $left += Note $p ($t0 + $b) 0.55 ($loud * 0.6) }   # chord, beats 2-3
            }
        }
        else {
            # Alberti bass in eighths: low-high-middle-high-middle-high.
            $order = @(0, 1, 2, 1, 2, 1)
            for ($e = 0; $e -lt 6; $e++) {
                $name = if ($e -ge 4 -and $chords.Count -gt 1) { $chords[1] } else { $chords[0] }
                $v = $albertiVoicing[$name]
                if (-not $v) { throw "unknown chord $name in bar $($bar + 1)" }
                $vel = ($loud * 0.62) + $(if ($e -eq 0) { 0.06 } else { 0.0 })
                $left += Note $v[$order[$e]] ($t0 + $e * 0.5) 0.48 $vel
            }
            # Pedal each harmony: down just after the bar starts, up just before the next change.
            $pedalLength = if ($chords.Count -gt 1) { 1.9 } else { 2.85 }
            $pedal += @{ start_beats = $t0 + 0.05; length_beats = $pedalLength }
            if ($chords.Count -gt 1) { $pedal += @{ start_beats = $t0 + 2.05; length_beats = 0.85 } }
        }
        $bar++
    }
}
"{0} bars, {1} right-hand and {2} left-hand notes" -f $bar, $right.Count, $left.Count

# --- Tempo and metre: 3/4; the Trio a little more relaxed; a broadening at the end ----
Invoke-Tool 'set_tempo' @{ bpm = 124; clear_changes = $true } | Out-Null
Invoke-Tool 'set_meter' @{ numerator = 3; denominator = 4 } | Out-Null
Invoke-Tool 'set_tempo' @{ bpm = 112; at_bar = $trioStartBar } | Out-Null
Invoke-Tool 'set_tempo' @{ bpm = 124; at_bar = $trioEndBar + 1 } | Out-Null
Invoke-Tool 'set_tempo' @{ bpm = 108; at_bar = $bar - 1 } | Out-Null
Invoke-Tool 'set_tempo' @{ bpm = 92;  at_bar = $bar } | Out-Null

# --- The instrument: a fortepiano-like voice from the built-in synth -----------------
$totalBeats = $bar * $beatsPerBar + 6
$piano = @{ waveform = 'saw'; detune_cents = 3; attack = 0.002; decay = 1.8; sustain = 0.0; release = 0.5;
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
    $r = Invoke-Tool 'add_notes' @{ track = $part[0]; clip = 1; notes = $part[1]; sustain = $pedal }
    "{0,-11} {1,4} notes" -f $part[0], $r.total_notes
}

# --- A small hall ------------------------------------------------------------------
Invoke-Tool 'create_track' @{ kind = 'aux'; name = 'Hall' } | Out-Null
Invoke-Tool 'insert_plugin' @{ track = 'Hall'; slot = 'A'; plugin = 'Meridian Reverb' } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'mix'; value = 1 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'decay'; value = 2.2 } | Out-Null
Invoke-Tool 'set_plugin_parameter' @{ track = 'Hall'; slot = 'A'; parameter = 'damping'; value = 0.5 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Right Hand'; slot = 'A'; bus = 'Hall'; level_db = -10 } | Out-Null
Invoke-Tool 'set_send' @{ track = 'Left Hand'; slot = 'A'; bus = 'Hall'; level_db = -12 } | Out-Null

Invoke-Tool 'set_master' @{ console = $true; drive_db = 2; character = 0.2; curve = 'tanh'; volume_db = 0 } | Out-Null
Invoke-Tool 'save_session' @{} | Out-Null
$b = Invoke-Tool 'bounce' @{ normalize_lufs = -16; ceiling_dbtp = -1 }
"bouncing to $($b.bouncing_to)"
for ($i = 0; $i -lt 360; $i++) {
    Start-Sleep -Milliseconds 500
    $s = Invoke-Tool 'get_status' @{}
    if (-not $s.bounce.running -and $s.bounce.last_file) {
        "done: {0}  ({1} LUFS, true peak {2} dBTP)" -f $s.bounce.last_file, $s.bounce.last_integrated_lufs, $s.bounce.last_true_peak_dbtp
        break
    }
}
try { $ws.CloseAsync('NormalClosure', 'done', $ct).Wait(2000) | Out-Null } catch { }
