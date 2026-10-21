# Connects Claude Desktop to Meridian: adds Meridian to Claude's MCP servers.
# Run it with "Connect to Claude.cmd". It works with both the installer and the
# Microsoft Store version of Claude Desktop, and keeps a backup of the old config.
$ErrorActionPreference = 'Stop'

function Say([string]$text, [string]$colour = 'Gray') { Write-Host $text -ForegroundColor $colour }

Say ''
Say 'Connect Claude Desktop to Meridian' White
Say '----------------------------------' White

# Claude Desktop reaches Meridian through "npx mcp-remote", which comes with Node.js.
if (-not (Get-Command npx -ErrorAction SilentlyContinue)) {
    Say 'Node.js is needed first. Install the LTS version from https://nodejs.org,' Yellow
    Say 'then run this again.' Yellow
    exit 1
}

# Where Claude Desktop keeps its config, newest layout first: recent versions in
# %LOCALAPPDATA%\Claude-Data\claude_<id>, older Store versions inside their package
# folder, older installer versions in %APPDATA%\Claude. Every config that exists is
# updated; when there is none yet, one is made in the newest folder that exists.
$candidates = @()
foreach ($root in @((Join-Path $env:LOCALAPPDATA 'Claude-Data'), (Join-Path $env:LOCALAPPDATA 'Packages'))) {
    if (-not (Test-Path $root)) { continue }
    Get-ChildItem $root -Directory -Filter 'claude_*' | ForEach-Object {
        if ($root -like '*\Packages') { $candidates += Join-Path $_.FullName 'LocalCache\Roaming\Claude' }
        else { $candidates += $_.FullName }
    }
}
$candidates += Join-Path $env:APPDATA 'Claude'
$existing = @($candidates | Where-Object { Test-Path $_ })
$dirs = @($existing | Where-Object { Test-Path (Join-Path $_ 'claude_desktop_config.json') })
if ($dirs.Count -eq 0) { $dirs = @($existing | Select-Object -First 1) }
if ($dirs.Count -eq 0) {
    Say 'Claude Desktop was not found. Install it from https://claude.ai/download,' Yellow
    Say 'open it once, then run this again.' Yellow
    exit 1
}

# Claude Desktop rewrites its config when it quits, which would undo the change,
# so it has to be closed first. (Claude Code is also called claude.exe; it is
# told apart by where it is installed.)
function Get-ClaudeDesktop {
    @(Get-Process -Name 'claude' -ErrorAction SilentlyContinue | Where-Object {
        -not $_.Path -or $_.Path -match '\\WindowsApps\\Claude_|\\AnthropicClaude\\'
    })
}
if ((Get-ClaudeDesktop).Count -gt 0) {
    Say ''
    Say 'Claude Desktop is open. Quit it fully: right-click the Claude icon by the' Yellow
    Say 'clock (click ^ if you cannot see it) and choose Quit. Closing the window' Yellow
    Say 'is not enough.' Yellow
    Say ''
    Say 'Waiting for Claude to quit... (press Ctrl+C to cancel)'
    while ((Get-ClaudeDesktop).Count -gt 0) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 2
}

$entry = [pscustomobject]@{ command = 'npx'; args = @('-y', 'mcp-remote', 'http://127.0.0.1:47800/mcp') }
$utf8 = New-Object Text.UTF8Encoding($false)
$updated = 0
foreach ($dir in $dirs) {
    $cfg = Join-Path $dir 'claude_desktop_config.json'
    $config = [pscustomobject]@{}
    if (Test-Path $cfg) {
        $text = [IO.File]::ReadAllText($cfg)
        if ($text.Trim()) {
            try { $config = $text | ConvertFrom-Json }
            catch {
                Say "Skipped $cfg" Yellow
                Say '  It is not valid JSON, so it was left alone. Fix or delete it, then run this again.' Yellow
                continue
            }
        }
        Copy-Item $cfg "$cfg.before-meridian" -Force
    }
    if (-not $config.PSObject.Properties['mcpServers'] -or $null -eq $config.mcpServers) {
        $config | Add-Member -NotePropertyName mcpServers -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    $config.mcpServers | Add-Member -NotePropertyName meridian -NotePropertyValue $entry -Force
    [IO.File]::WriteAllText($cfg, ($config | ConvertTo-Json -Depth 64), $utf8)
    Say "Updated $cfg"
    $updated++
}
if ($updated -eq 0) { exit 1 }

Say ''
Say 'Done: Claude Desktop is connected to Meridian.' Green
Say 'Start Meridian, then open Claude Desktop. Settings > Developer should list'
Say '"meridian". In a new chat, try: "What is in my Meridian session?"'
