[CmdletBinding()]
param([ValidateRange(10, 300)][int]$TimeoutSeconds = 60)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $projectRoot 'build/xess-vulkan-probe.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Run scripts/build.ps1 first.' }
& (Join-Path $PSScriptRoot 'fetch-sdks.ps1') -VerifyOnly
$reportDir = Join-Path (Join-Path $projectRoot 'out') (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
$sdkBin = Join-Path $projectRoot 'third_party/xess/bin'
$stdout = Join-Path $reportDir 'probe.stdout.json'
$stderr = Join-Path $reportDir 'probe.stderr.txt'
$os = Get-CimInstance Win32_OperatingSystem
$environment = [ordered]@{
    capturedAt = (Get-Date).ToString('o')
    stage = 'P0-capability-probe'
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate, PNPDeviceID)
    windows = [ordered]@{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber }
    sourceHashes = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -File | Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash)
    binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    dependencyLockHash = (Get-FileHash -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Algorithm SHA256).Hash
    toolchainLockHash = (Get-FileHash -LiteralPath (Join-Path $projectRoot 'toolchain.lock.json') -Algorithm SHA256).Hash
    dllSignatures = @('libxess_fg.dll', 'libxell.dll') | ForEach-Object {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $sdkBin $_)
        [ordered]@{ file = $_; status = [string]$signature.Status; signer = $signature.SignerCertificate.Subject }
    }
    validationLayersEnabled = $false
    d3d12DebugLayerEnabled = $false
    gameIntegrationTested = $false
}
$environment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $reportDir 'environment.json') -Encoding utf8
$process = Start-Process -FilePath $exe -ArgumentList @('--sdk-bin', ('"' + $sdkBin + '"')) -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    throw "Probe timed out after $TimeoutSeconds seconds. Logs: $reportDir"
}
$process.WaitForExit()
$probeExitCode = $process.ExitCode
try {
    $result = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $reportDir 'capabilities.json') -Encoding utf8
} catch { throw "Probe did not return valid JSON (exit $probeExitCode). Raw logs: $reportDir" }
Write-Host "Report: $reportDir"
Write-Host "Capability gate: $($result.capabilityGatePassed). Generated frames verified: $($result.generatedFramesVerified)."
if ($probeExitCode -ne 0) { throw "Capability probe failed (exit $probeExitCode); inspect capabilities.json." }

