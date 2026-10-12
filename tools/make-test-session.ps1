# Creates a small test session: "Engine Test" with a stereo track holding a 4-second,
# -40 dBFS, 440 Hz tone (with fades) and an empty mono track. Prints the session file path.
#
#   $s = .\tools\make-test-session.ps1
#   & .\build\app\src\Meridian_artefacts\Debug\Meridian.exe $s
param([string]$Root = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Meridian Sessions'))

$folder = Join-Path $Root 'Engine Test'
$audio = Join-Path $folder 'Audio Files'
New-Item -ItemType Directory -Force -Path $audio | Out-Null

# 16-bit stereo PCM WAV at 48 kHz
$sr = 48000; $seconds = 4; $frames = $sr * $seconds; $channels = 2
$amp = [math]::Pow(10, -40 / 20) * 32767
$dataBytes = $frames * $channels * 2
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
$w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $dataBytes)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
$w.Write([Text.Encoding]::ASCII.GetBytes('fmt ')); $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]$channels)
$w.Write([int]$sr); $w.Write([int]($sr * $channels * 2)); $w.Write([int16]($channels * 2)); $w.Write([int16]16)
$w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$dataBytes)
for ($i = 0; $i -lt $frames; $i++) {
    $s = [int16][math]::Round($amp * [math]::Sin(2 * [math]::PI * 440 * $i / $sr))
    $w.Write($s); $w.Write($s)
}
$w.Flush()
[IO.File]::WriteAllBytes((Join-Path $audio 'Tone.wav'), $ms.ToArray())

$xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<SESSION name="Engine Test" sampleRate="48000.0" bitDepth="24" fileFormat="wav" formatVersion="1" nextId="10">
  <TRACKS>
    <TRACK id="1" name="Tone" channels="2" colour="ff5a8dd6" volumeDb="0.0" pan="0.0" mute="0" solo="0" recordArm="0" inputChannel="0" automationMode="read">
      <CLIPS>
        <CLIP id="2" name="Tone" file="Audio Files/Tone.wav" start="24000" length="$frames" offset="0" sourceLength="$frames" gainDb="0.0" fadeIn="4800" fadeOut="4800"/>
      </CLIPS>
      <INSERTS/>
      <AUTOMATION/>
    </TRACK>
    <TRACK id="3" name="Vocal" channels="1" colour="ff4fb3a9" volumeDb="0.0" pan="0.0" mute="0" solo="0" recordArm="0" inputChannel="0" automationMode="read">
      <CLIPS/>
      <INSERTS/>
      <AUTOMATION/>
    </TRACK>
  </TRACKS>
</SESSION>
"@
$sessionFile = Join-Path $folder 'Engine Test.meridian'
[IO.File]::WriteAllText($sessionFile, $xml, (New-Object Text.UTF8Encoding($false)))
$sessionFile
