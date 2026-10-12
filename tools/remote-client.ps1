# Minimal stand-in for an AI agent: sends each command to Meridian's remote-control
# port and prints the JSON reply.
#
#   .\tools\remote-client.ps1 STATUS
#   .\tools\remote-client.ps1 PLAY SLEEP1500 STOP
#   .\tools\remote-client.ps1 -Url 'ws://127.0.0.1:47800/?token=secret' SET_VOLUME_TRACK_1_-6dB
#
# "SLEEP<ms>" pauses between commands.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Commands = @('STATUS'),
    [string]$Url = 'ws://127.0.0.1:47800/'
)

$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [Threading.CancellationToken]::None
try { $ws.ConnectAsync([Uri]$Url, $ct).Wait(5000) | Out-Null } catch { }
if ($ws.State -ne 'Open') { Write-Error "Could not connect to $Url (is Meridian running with AI Remote Control enabled?)"; exit 1 }

foreach ($cmd in $Commands) {
    if ($cmd -match '^SLEEP(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    $bytes = [Text.Encoding]::UTF8.GetBytes($cmd)
    $ws.SendAsync((New-Object ArraySegment[byte] (,$bytes)), 'Text', $true, $ct).Wait()
    $buffer = New-Object byte[] 65536
    $result = $ws.ReceiveAsync((New-Object ArraySegment[byte] (,$buffer)), $ct).Result
    "> $cmd"
    "< " + [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
}

try { $ws.CloseAsync('NormalClosure', 'done', $ct).Wait(2000) | Out-Null } catch { }
