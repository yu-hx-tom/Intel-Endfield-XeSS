[CmdletBinding()]
param([switch]$RunGpu, [switch]$FakeSwapchain, [switch]$GpuTimestamps, [switch]$AsyncCopyPath, [switch]$SharedCopyPath, [switch]$SharedCopySync, [switch]$DepthCompute, [switch]$AsyncCapacity, [switch]$GatedCopy, [switch]$ForceK1Reset,
    [ValidateSet(1,2)][int]$AsyncCopyFrames=2,
    [ValidateSet(0,1)][int]$FollowupReset=0, [ValidateSet(35,60,100,200)][int]$GateMs=100,
    [string]$SdkDirectory, [string]$OutputDirectory, [string]$ValidationLayerDirectory)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $project 'build/xess-presentation-lowres-gpu-test.exe'
$sharedRequested = $SharedCopyPath -or $SharedCopySync
if ($GatedCopy -and (-not $FakeSwapchain -or -not $GpuTimestamps)) { throw 'GatedCopy requires FakeSwapchain and GpuTimestamps.' }
if ($AsyncCopyPath -and -not $FakeSwapchain) { throw 'AsyncCopyPath requires FakeSwapchain.' }
if ($ValidationLayerDirectory -and -not $FakeSwapchain) { throw 'Validation requires FakeSwapchain for complete owned parent cleanup.' }
if ($ValidationLayerDirectory) {
    $ValidationLayerDirectory=(Get-Item -LiteralPath $ValidationLayerDirectory).FullName
    foreach($name in @('VkLayer_khronos_validation.json','VkLayer_khronos_validation.dll')) {
        if(-not (Test-Path -LiteralPath (Join-Path $ValidationLayerDirectory $name) -PathType Leaf)) {throw ('Missing local validation artifact '+$name)}
    }
}
if ($SharedCopyPath -and (-not $AsyncCopyPath -or $AsyncCapacity)) { throw 'SharedCopyPath requires AsyncCopyPath and keeps the separate capacity fixture unchanged.' }
if ($SharedCopySync -and (-not $FakeSwapchain -or $AsyncCopyPath -or $SharedCopyPath -or $AsyncCapacity -or $GatedCopy -or $FollowupReset -ne 0)) {
    throw 'SharedCopySync requires FakeSwapchain and excludes async admission and the separate gate/reset fixture.'
}
if ($DepthCompute -and -not $sharedRequested) { throw 'DepthCompute requires SharedCopyPath or SharedCopySync.' }
if ($AsyncCapacity -and (-not $AsyncCopyPath -or -not $GpuTimestamps)) { throw 'AsyncCapacity requires AsyncCopyPath and GpuTimestamps.' }
if ($AsyncCapacity -and $AsyncCopyFrames -ne 2) { throw 'AsyncCapacity requires a two-frame limit.' }
if ($AsyncCopyPath -and ($GatedCopy -or $FollowupReset -ne 0)) { throw 'P2 async checks keep the separate P1 gate/reset experiment disabled.' }
if ($GatedCopy -and $GateMs -eq 35 -and $FollowupReset -ne 0) { throw '35 ms reset experiment requires FollowupReset=0.' }
if ($ForceK1Reset -and (-not $GatedCopy -or $GateMs -ne 35)) { throw 'ForceK1Reset requires the gated 35 ms experiment.' }
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build target xess-presentation-lowres-gpu-test first.' }
if (-not $RunGpu) {
    [ordered]@{ executable=$executable; executableSha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        gpuRun=$false; fakeSwapchainRequested=[bool]$FakeSwapchain; gpuTimestampsRequested=[bool]$GpuTimestamps
        asyncCopyPathRequested=[bool]$AsyncCopyPath; sharedCopyPathRequested=[bool]$sharedRequested; sharedCopySyncRequested=[bool]$SharedCopySync; depthComputeRequested=[bool]$DepthCompute; asyncCapacityRequested=[bool]$AsyncCapacity
        asyncCopyFrames=$AsyncCopyFrames
        validationLayerDirectory=$ValidationLayerDirectory; validationRequested=[bool]$ValidationLayerDirectory
        gatedCopy=[bool]$GatedCopy; forceK1Reset=[bool]$ForceK1Reset; followupReset=$FollowupReset; gateMs=$GateMs; note='Compiled fixture only. Supply -RunGpu after normal game exit to run real owned WSI/SDK/readback checks.' } | ConvertTo-Json
    return
}
if (@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count) {
    throw 'Normally exit the game and EFU before this self-owned GPU fixture. This script never closes them.'
}
if (-not $SdkDirectory) { $SdkDirectory = Join-Path $project 'third_party/xess/bin' }
$sdk = (Get-Item -LiteralPath $SdkDirectory).FullName
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'libxess_fg.dll'))) { throw 'Real XeFG SDK directory required.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-presentation-lowres') }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$records = @()
$modes = if ($AsyncCapacity) { @('async_capacity') } elseif ($GatedCopy) { @('gated_copy') } elseif ($sharedRequested) { @('normal','linear_unsupported','storage_unsupported') } else { @('normal','linear_unsupported') }
foreach ($mode in $modes) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$executable; $start.WorkingDirectory=$project; $start.UseShellExecute=$false
    $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.Arguments='--sdk-bin "'+$sdk+'"'
    if ($FakeSwapchain) { $start.Arguments+=' --fake-swapchain' }
    if ($GpuTimestamps) { $start.Arguments+=' --gpu-timestamps' }
    if ($AsyncCopyPath) { $start.Arguments+=' --async-copy-path' }
    if ($SharedCopyPath) { $start.Arguments+=' --shared-copy-path' }
    if ($SharedCopySync) { $start.Arguments+=' --shared-copy-sync' }
    if ($DepthCompute) { $start.Arguments+=' --depth-compute' }
    if ($ValidationLayerDirectory) { $start.Arguments+=' --validation' }
    $start.Arguments+=' --async-copy-frames='+$AsyncCopyFrames
    if ($AsyncCapacity) { $start.Arguments+=' --async-capacity' }
    if ($GatedCopy) { $start.Arguments+=' --gated-copy --gate-ms '+$GateMs+' --followup-reset='+$FollowupReset }
    if ($ForceK1Reset) { $start.Arguments+=' --force-k1-reset' }
    if ($mode -ceq 'linear_unsupported') { $start.Arguments+=' --mask-linear' }
    if ($mode -ceq 'storage_unsupported') { $start.Arguments+=' --mask-depth-storage' }
    # All changes belong to this self-owned child; no installed/global state changes.
    foreach ($name in @($start.EnvironmentVariables.Keys | Where-Object { $_ -like 'XESS_*' -or $_ -like 'SL_*' }) +
        @('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_ADD_IMPLICIT_LAYER_PATH',
          'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG')) {
        $start.EnvironmentVariables.Remove($name)
    }
    if ($GpuTimestamps) { $start.EnvironmentVariables['XESS_GPU_TIMESTAMPS']='1' }
    if ($ValidationLayerDirectory) {
        $start.EnvironmentVariables['VK_LAYER_PATH']=$ValidationLayerDirectory
        $start.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
    }
    $start.EnvironmentVariables['XESS_XEFG_LOW_RES_OUTPUT']='1'
    $start.EnvironmentVariables['DISABLE_RTSS_LAYER']='1'
    $child=[Diagnostics.Process]::new(); $child.StartInfo=$start
    try {
        if (-not $child.Start()) { throw 'Could not start the self-owned low-resolution fixture.' }
        $stdoutTask=$child.StandardOutput.ReadToEndAsync(); $stderrTask=$child.StandardError.ReadToEndAsync()
        $watch=[Diagnostics.Stopwatch]::StartNew(); $timedOut=$false
        while (-not $child.WaitForExit(500)) {
            if ($watch.Elapsed.TotalSeconds -gt 130) { $timedOut=$true; $child.Kill(); break }
        }
        $child.WaitForExit(); $stdout=$stdoutTask.GetAwaiter().GetResult(); $stderr=$stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $OutputDirectory ($mode+'-stdout.jsonl')),$stdout)
        [IO.File]::WriteAllText((Join-Path $OutputDirectory ($mode+'-stderr.txt')),$stderr)
        $result=$null
        try { $result=($stdout -split '\r?\n' | Where-Object { $_ -match '^\{"testPassed":' } | Select-Object -Last 1) | ConvertFrom-Json } catch { }
        $validationResult=$null
        if($ValidationLayerDirectory) {
            $validationResult=($stdout -split '\r?\n' | Where-Object { $_ -match '^\{"event":"validation_cleanup"' } | Select-Object -Last 1) | ConvertFrom-Json
        }
        $gateResult=$null
        if ($GatedCopy) {
            $gateResult=($stdout -split '\r?\n' | Where-Object { $_ -match '^\{"event":"gated_copy_result"' } | Select-Object -Last 1) | ConvertFrom-Json
        }
        $records += [ordered]@{ mode=$mode; exitCode=$child.ExitCode; timedOut=$timedOut; result=$result; gatedCopyResult=$gateResult; validationCleanupResult=$validationResult }
        $summary=[ordered]@{ executable=$executable; executableSha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
            gpuRun=$true; fakeSwapchainRequested=[bool]$FakeSwapchain; gpuTimestampsRequested=[bool]$GpuTimestamps
            asyncCopyPathRequested=[bool]$AsyncCopyPath; sharedCopyPathRequested=[bool]$sharedRequested; sharedCopySyncRequested=[bool]$SharedCopySync; depthComputeRequested=[bool]$DepthCompute; asyncCapacityRequested=[bool]$AsyncCapacity
            asyncCopyFrames=$AsyncCopyFrames
            validationLayerDirectory=$ValidationLayerDirectory; validationRequested=[bool]$ValidationLayerDirectory
            gatedCopy=[bool]$GatedCopy; forceK1Reset=[bool]$ForceK1Reset; followupReset=$FollowupReset; gateMs=$GateMs; gameTouched=$false; installedFilesChanged=$false; runs=$records }
        if (-not $FakeSwapchain) { $summary['nativeParentsRetainedUntilProcessExit']=$true }
        $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding UTF8
        if ($timedOut -or $child.ExitCode -ne 0 -or -not $result.testPassed) { throw "Low-resolution GPU fixture failed ($mode); saved $OutputDirectory" }
        if ($ValidationLayerDirectory -and (-not $result.validationRequested -or -not $result.validationLayerLoaded -or
            -not $validationResult.layerActuallyLoaded -or -not $validationResult.parentCleanupCompleted -or $validationResult.errors -ne 0)) {
            throw "Actual validation layer load, callback errors or parent cleanup did not pass ($mode)."
        }
        if ($AsyncCopyPath -and (-not $result.asyncCopyPath -or $result.auxiliaryPixelFrames -lt 2)) { throw "Async path or real auxiliary pixel oracle was not exercised ($mode)." }
        if ($sharedRequested -and (-not $result.sharedCopyPath -or $result.sharedColorFrames -lt 5 -or $result.sharedAuxiliaryFrames -lt 5 -or $result.sharedWraps -lt 5)) { throw "Shared path/direct pixels/lease wrap coverage incomplete ($mode)." }
        if ($SharedCopySync -and (-not $result.sharedCopySync -or $result.asyncCopyPath -or $result.inflightHighWater -ne 0 -or $result.inflightCapWaits -ne 0)) { throw "Synchronous shared incorrectly entered async admission/capacity waiting ($mode)." }
        if ($sharedRequested -and ($result.computeRestoreChecks -ne $result.providerCaptured -or
            $result.depthComputeFrames+$result.depthTransferFrames -ne $result.providerCaptured -or
            $(if ($mode -ceq 'storage_unsupported' -or -not $DepthCompute) { ($mode -ceq 'storage_unsupported' -and -not $result.depthStorageCapabilityMasked) -or $result.depthComputeFrames -ne 0 } else { $result.depthComputeFrames -lt 5 }))) {
            throw "Depth compute/fallback or following game-dispatch restore oracle incomplete ($mode)."
        }
        if ($sharedRequested -and $GpuTimestamps -and $result.depthGpuSamples -ne $result.providerCaptured) {throw "Actual depth query coverage incomplete ($mode)."}
        if ($result.inflightLimit -ne $(if ($AsyncCopyPath) {$AsyncCopyFrames} else {0})) { throw "Effective frame limit differs from the selected route ($mode)." }
        if (($AsyncCopyPath -or $SharedCopySync) -and -not $AsyncCapacity -and $result.physicalResizeChecks -ne 2) { throw "Physical resize/recovery checks were not exercised ($mode)." }
        if ($AsyncCapacity -and ($result.asyncCapacityCases -ne 3 -or $result.asyncPendingAcquireCases -ne 3)) { throw 'The real pending K2/acquire/capacity cases were not reproduced.' }
        if ($GpuTimestamps -and (-not $result.gpuTimestampsRequested -or $result.vkGpuSamples -lt 2 -or $result.interopGpuSamples -lt 2 -or $result.d3dGpuSamples -lt 2)) {
            throw "GPU timestamp request produced insufficient explicit Vulkan/D3D samples ($mode)."
        }
    } finally { $child.Dispose() }
}
Write-Output (Join-Path $OutputDirectory 'summary.json')
