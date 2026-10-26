[CmdletBinding()]
param(
    [string]$LauncherPath = 'D:\games\Hypergryph Launcher\Launcher.exe',
    [string]$GameDirectory = 'D:\games\Hypergryph Launcher\games\Arknights Endfield',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$launcher = Get-Item -LiteralPath $LauncherPath
$gameRoot = (Get-Item -LiteralPath $GameDirectory).FullName
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'Endfield.exe'))) { throw 'Endfield.exe is missing from the supplied game directory.' }
$running = @(Get-Process -Name Launcher,Games,Endfield -ErrorAction SilentlyContinue | Where-Object {
    # The bootstrapper launches Games.exe; elevated child paths may be unreadable.
    -not $_.Path -or $_.Path -eq $launcher.FullName -or $_.Path -eq (Join-Path $gameRoot 'Endfield.exe') -or
    $_.Path.StartsWith($launcher.DirectoryName + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
})
if ($running.Count -and -not $PrepareOnly) {
    throw 'The launcher or game is already running. Close it normally first so the new launch can inherit diagnostic environment variables. No process was stopped.'
}
$sessionDir = Join-Path (Join-Path $projectRoot 'out') ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-game-diagnostic')
New-Item -ItemType Directory -Path $sessionDir -Force | Out-Null
$settings = [ordered]@{
    SL_LOG_LEVEL = '2'
    SL_LOG_PATH = $sessionDir
    SL_LOG_NAME = 'endfield-sl.log'
    SL_ENABLE_CONSOLE_LOGGING = '0'
}
$manifest = [ordered]@{
    startedAt = (Get-Date).ToString('o')
    launcher = $launcher.FullName
    gameDirectory = $gameRoot
    environmentForChildOnly = $settings
    prepareOnly = [bool]$PrepareOnly
    modifiedGameFiles = $false
    injectedModules = $false
    tagCallsVerified = $false
    note = 'Uses the official launcher. Logging environment inheritance and tag visibility are not guaranteed.'
}
if (-not $PrepareOnly) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $launcher.FullName
    $startInfo.WorkingDirectory = $launcher.DirectoryName
    $startInfo.UseShellExecute = $false
    # This is the normal interactive game launcher; the user needs its window.
    foreach ($key in $settings.Keys) { $startInfo.EnvironmentVariables[$key] = $settings[$key] }
    $process = [Diagnostics.Process]::Start($startInfo)
    $manifest['launcherProcessId'] = $process.Id
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $sessionDir 'launch.json') -Encoding utf8
Write-Host "Diagnostic session: $sessionDir"
Write-Host 'After a normal short Vulkan game session, run collect-game-diagnostic.ps1 with this -SessionDirectory.'

