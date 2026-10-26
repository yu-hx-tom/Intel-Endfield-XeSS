[CmdletBinding()]
param(
    [ValidateSet('interop', 'native', 'presenter')][string]$Test = 'interop',
    [ValidateRange(1, 1000000)][int]$Frames = 10000,
    [ValidateRange(1, 16384)][int]$Width = 127,
    [ValidateRange(1, 16384)][int]$Height = 73,
    [switch]$DisableFrameGeneration,
    [switch]$NegativeMismatch,
    [switch]$InjectSubmissionFailure,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$target = switch ($Test) {
    'interop' { 'xess-vulkan-interop-test.exe' }
    'native' { 'xess-vulkan-native-test.exe' }
    'presenter' { 'xess-vulkan-presenter.exe' }
}
$exe = Join-Path $projectRoot "build/$target"
if (-not (Test-Path -LiteralPath $exe)) { throw 'Run scripts/build.ps1 first.' }
if ($Test -eq 'presenter') {
    if ($Frames -lt 8 -or $Frames -gt 3600) { throw 'Presenter needs -Frames between 8 and 3600.' }
    & (Join-Path $PSScriptRoot 'fetch-sdks.ps1') -VerifyOnly
}
if ($Test -ne 'presenter' -and $InjectSubmissionFailure) { throw '-InjectSubmissionFailure applies to presenter only.' }
if ($Test -eq 'presenter' -and $NegativeMismatch) { throw '-NegativeMismatch applies to interop/native only.' }
if ($Test -ne 'presenter' -and $DisableFrameGeneration) { throw '-DisableFrameGeneration applies to presenter only.' }
$reportDir = Join-Path (Join-Path $projectRoot 'out') ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Test)
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
$stdout = Join-Path $reportDir 'stdout.json'
$stderr = Join-Path $reportDir 'stderr.txt'
$arguments = @('--frames', [string]$Frames)
if ($Test -in @('interop', 'native')) {
    $arguments += @('--width', [string]$Width, '--height', [string]$Height)
    if ($NegativeMismatch) { $arguments += '--negative-mismatch' }
} else {
    $arguments += @('--sdk-bin', ('"' + (Join-Path $projectRoot 'third_party/xess/bin') + '"'))
    if ($DisableFrameGeneration) { $arguments += '--off' }
    if ($InjectSubmissionFailure) { $arguments += '--fail-after-submit' }
}
$metadata = [ordered]@{
    capturedAt = (Get-Date).ToString('o')
    executable = $target
    arguments = $arguments
    timeoutSeconds = $TimeoutSeconds
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
    sourceHashes = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -File | Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash)
    binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    dependencyLockHash = (Get-FileHash -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Algorithm SHA256).Hash
    gameIntegrationTested = $false
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $reportDir 'metadata.json') -Encoding utf8
Write-Host "Running $Test. Report: $reportDir"
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$timer = [Diagnostics.Stopwatch]::StartNew()
while (-not $process.WaitForExit(1000)) {
    if ($timer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "Experiment timed out. Retained logs: $reportDir"
    }
}
$process.WaitForExit()
$nativeExitCode = $process.ExitCode
$metadata['exitCode'] = $nativeExitCode
$metadata['elapsedSeconds'] = $timer.Elapsed.TotalSeconds
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $reportDir 'metadata.json') -Encoding utf8
try {
    $result = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $reportDir 'result.json') -Encoding utf8
} catch { throw "Experiment returned invalid JSON (exit $nativeExitCode). Logs: $reportDir" }
Get-Content -LiteralPath (Join-Path $reportDir 'result.json')
if ($NegativeMismatch) {
    if ($nativeExitCode -eq 0 -or $result.passed -or $result.error -notmatch 'mismatch') { throw 'Expected an explicit pixel mismatch failure.' }
    Write-Host 'Negative control correctly detected corrupted expected pixels.'
} elseif ($InjectSubmissionFailure) {
    if ($nativeExitCode -ne 1 -or $result.testPassed -or $result.error -ne 'Injected failure after GPU submission') { throw 'Expected a cleanly handled post-submission failure.' }
    $cleanupLog = Get-Content -LiteralPath $stderr -Raw
    if ($cleanupLog -notmatch 'XeFG destroy=0' -or $cleanupLog -notmatch 'XeLL destroy=0') { throw 'SDK cleanup was not confirmed after the injected failure.' }
    Write-Host 'Post-submission fault correctly drained GPU work and destroyed SDK contexts.'
} elseif ($nativeExitCode -ne 0) { throw "Experiment failed (exit $nativeExitCode). Logs: $reportDir" }
elseif ($Test -eq 'native') {
    if (-not $result.passed -or $result.nativeImageFrames -ne $Frames -or $result.cpuUploadFrames -ne 0 -or
        $result.framesVerified -ne $Frames -or -not $result.everyPixelCompared -or
        -not $result.lateFenceNotificationRegressionPassed -or
        -not $result.sourcesReusedBeforeD3D12Readback -or -not $result.borrowedDeviceAliveAfterBridgeDestruction -or
        $result.negativeValidationChecks -lt 11 -or $result.bytesVerified -ne ([long]$Frames * $Width * $Height * 12)) {
        throw "Native GPU-copy evidence is incomplete. Logs: $reportDir"
    }
}
