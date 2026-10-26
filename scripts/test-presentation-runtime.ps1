[CmdletBinding()]
param([switch]$FakeSwapchain, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count) {
    throw 'Normally exit Endfield.exe and EFU before this self-owned GPU fixture. This script never closes them.'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('out/presentation-runtime-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$fixture = Join-Path $projectRoot 'build/xess-presentation-runtime-gpu-test.exe'
$observer = Join-Path $projectRoot 'build/observer/xess-vulkan-observer.dll'
$observerHash = (Get-FileHash -LiteralPath $observer -Algorithm SHA256).Hash
$sdk = Join-Path $projectRoot 'third_party/xess/bin'
$observerLog = Join-Path $OutputDirectory 'observer.jsonl'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName=$fixture; $start.WorkingDirectory=$projectRoot; $start.UseShellExecute=$false
$start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
if ($FakeSwapchain) { $start.Arguments='--fake-swapchain' }
$remove = @($start.EnvironmentVariables.Keys | Where-Object { $_ -like 'XESS_*' -or $_ -like 'SL_*' }) +
    @('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_ADD_IMPLICIT_LAYER_PATH',
      'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG')
foreach ($name in $remove) { $start.EnvironmentVariables.Remove($name) }
# Every setting belongs to this owned child; installed files and user settings stay untouched.
$start.EnvironmentVariables['VK_LAYER_PATH']=Join-Path $projectRoot 'build/observer'
$start.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_XESS_bridge_observer'
$start.EnvironmentVariables['XESS_BRIDGE_DEVICE']='1'
$start.EnvironmentVariables['XESS_PRESENTATION_OFF']='1'
$start.EnvironmentVariables['XESS_SDK_BIN']=$sdk
$start.EnvironmentVariables['XESS_OBSERVER_LOG']=$observerLog
$start.EnvironmentVariables['DISABLE_RTSS_LAYER']='1'
if ($FakeSwapchain) { $start.EnvironmentVariables['XESS_FAKE_SWAPCHAIN']='1' }
$child=[Diagnostics.Process]::new(); $child.StartInfo=$start
try {
    if (-not $child.Start()) { throw 'Could not start the self-owned presentation runtime fixture.' }
    $stdoutTask=$child.StandardOutput.ReadToEndAsync(); $stderrTask=$child.StandardError.ReadToEndAsync()
    $watch=[Diagnostics.Stopwatch]::StartNew(); $timedOut=$false
    while (-not $child.WaitForExit(500)) {
        if ($watch.Elapsed.TotalSeconds -gt 180) { $timedOut=$true; $child.Kill(); break }
    }
    $child.WaitForExit(); $stdout=$stdoutTask.GetAwaiter().GetResult(); $stderr=$stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'stdout.txt'),$stdout)
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'stderr.txt'),$stderr)
    $result=$null
    try { $result=($stdout -split '\r?\n' | Where-Object { $_ -match '^\{' } | Select-Object -Last 1) | ConvertFrom-Json } catch { }
    $events=@(); $logParseError=$null
    try { if (Test-Path -LiteralPath $observerLog) { $events=@(Get-Content -LiteralPath $observerLog | ForEach-Object { $_ | ConvertFrom-Json }) } } catch { $logParseError=$_.Exception.Message }
    $shutdown=@($events | Where-Object event -CEQ 'presentation_runtime_shutdown')
    $devices=@($events | Where-Object event -CEQ 'create_device')
    $fakeVerified=$false
    if ($FakeSwapchain) {
        $fakeVerified=$null -eq $logParseError -and $result.fixtureCallsCompleted -eq $true -and
            $result.fakeSwapchainRequested -eq $true -and $result.minimizedAcquireOutOfDate -eq $true -and
            $result.positiveExtentReplacements -eq 12 -and $result.minimizedFrameCalls -eq 0 -and
            $devices.Count -eq 1 -and $devices[0].fake_swapchain_enabled -eq $true -and
            $shutdown.Count -eq 1 -and $shutdown[0].result -eq 0 -and
            $shutdown[0].privateCleanupCompleted -eq $true -and $shutdown[0].SDKReleased -eq $true -and
            $shutdown[0].BridgeReleased -eq $true -and $shutdown[0].nativeTailRetained -eq $false -and
            $shutdown[0].failedQuarantined -eq $false -and
            @($events | Where-Object event -CEQ 'destroy_device').Count -eq 1 -and
            @($events | Where-Object event -CEQ 'destroy_instance').Count -eq 1
    }
    $passed=-not $timedOut -and $child.ExitCode -eq 0 -and $result.fixtureCallsCompleted -eq $true -and
        $result.fakeSwapchainRequested -eq [bool]$FakeSwapchain -and (-not $FakeSwapchain -or $fakeVerified)
    $checkpoint=[ordered]@{passed=[bool]$passed;exitCode=$child.ExitCode;timedOut=$timedOut
        observerSha256=$observerHash;fixtureSha256=(Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
        sdkBin=$sdk;fakeSwapchainRequested=[bool]$FakeSwapchain;fakeLifecycleLogVerified=[bool]$fakeVerified
        fixtureResult=$result;shutdown=$shutdown;logParseError=$logParseError;observerLog=$observerLog
        gpuTouched=$true;gameTouched=$false;installedFilesChanged=$false;parentEnvironmentChanged=$false;outputDirectory=$OutputDirectory}
    $checkpoint | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'checkpoint.json') -Encoding utf8
    $checkpoint | ConvertTo-Json -Depth 8
    if (-not $passed) { throw "Presentation GPU fixture failed; saved $OutputDirectory" }
} finally { $child.Dispose() }