[CmdletBinding()]
param(
    [ValidateSet('probe', 'interop', 'native', 'wsi', 'interface')][string]$Test = 'interop',
    [ValidateSet('absolute', 'none', 'relative', 'missing-directory')][string]$LogMode = 'absolute',
    [ValidateRange(1, 10000)][int]$Frames = 100,
    [switch]$TraceStreamline,
    [switch]$BridgeDevice,
    [switch]$TraceCompute,
    [switch]$TraceFsr,
    [switch]$TraceFsrUpscale,
    [string]$BuildDirectory,
    [ValidateRange(10, 120)][int]$TimeoutSeconds = 45
)
$ErrorActionPreference = 'Stop'
if ($TraceFsrUpscale -and -not $TraceFsr) { throw '-TraceFsrUpscale requires -TraceFsr.' }
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'build' }
$BuildDirectory = (Get-Item -LiteralPath $BuildDirectory).FullName
$layerDirectory = Join-Path $BuildDirectory 'observer'
$manifest = Join-Path $layerDirectory 'VkLayer_xess_observer.json'
$dll = Join-Path $layerDirectory 'xess-vulkan-observer.dll'
if (-not (Test-Path -LiteralPath $manifest) -or -not (Test-Path -LiteralPath $dll)) { throw 'Build the observer first.' }
if ($Test -eq 'wsi' -and ($Frames -lt 10 -or $Frames -gt 600)) { throw 'WSI smoke supports 10..600 frames.' }
if ($TraceCompute -and ($Test -ne 'wsi' -or $LogMode -ne 'absolute')) { throw '-TraceCompute requires -Test wsi and -LogMode absolute.' }
if ($TraceFsr -and ($Test -ne 'wsi' -or $LogMode -ne 'absolute')) { throw '-TraceFsr requires -Test wsi and -LogMode absolute.' }
if ($BridgeDevice -and ($Test -notin @('wsi', 'interop', 'native') -or $LogMode -ne 'absolute')) {
    throw '-BridgeDevice requires -Test wsi/interop/native and -LogMode absolute so effective device parameters can be checked.'
}
$target = switch ($Test) { 'probe' { 'xess-vulkan-probe.exe' }; 'interop' { 'xess-vulkan-interop-test.exe' }; 'native' { 'xess-vulkan-native-test.exe' }; 'wsi' { 'xess-vulkan-wsi-smoke.exe' }; 'interface' { 'xess-observer-interface-test.exe' } }
$exe = Join-Path $BuildDirectory $target
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $target; run scripts/build.ps1." }
$reportDir = Join-Path (Join-Path $projectRoot 'out') ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + "-observer-$Test-$LogMode")
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
$logPath = Join-Path $reportDir 'observer.jsonl'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exe
$start.WorkingDirectory = $reportDir
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.EnvironmentVariables['VK_LAYER_PATH'] = $layerDirectory
$start.EnvironmentVariables['VK_INSTANCE_LAYERS'] = 'VK_LAYER_XESS_bridge_observer'
$start.EnvironmentVariables['VK_LOADER_DEBUG'] = 'error,warn,layer'
$start.EnvironmentVariables.Remove('XESS_OBSERVER_LOG')
$start.EnvironmentVariables.Remove('XESS_SL_OBSERVER')
$start.EnvironmentVariables.Remove('XESS_BRIDGE_DEVICE')
$start.EnvironmentVariables.Remove('XESS_COMPUTE_OBSERVER')
$start.EnvironmentVariables.Remove('XESS_FSR_OBSERVER')
$start.EnvironmentVariables.Remove('XESS_FSR_UPSCALE_OBSERVER')
if ($TraceStreamline) { $start.EnvironmentVariables['XESS_SL_OBSERVER'] = '1' }
if ($BridgeDevice) { $start.EnvironmentVariables['XESS_BRIDGE_DEVICE'] = '1' }
if ($TraceCompute) { $start.EnvironmentVariables['XESS_COMPUTE_OBSERVER'] = '1' }
if ($TraceFsr) { $start.EnvironmentVariables['XESS_FSR_OBSERVER'] = '1' }
if ($TraceFsrUpscale) { $start.EnvironmentVariables['XESS_FSR_UPSCALE_OBSERVER'] = '1' }
switch ($LogMode) {
    'absolute' { $start.EnvironmentVariables['XESS_OBSERVER_LOG'] = $logPath }
    'relative' { $start.EnvironmentVariables['XESS_OBSERVER_LOG'] = 'relative-observer.jsonl' }
    'missing-directory' { $start.EnvironmentVariables['XESS_OBSERVER_LOG'] = Join-Path $reportDir 'missing-parent/observer.jsonl' }
}
if ($Test -eq 'probe') { $start.Arguments = '--sdk-bin "' + (Join-Path $projectRoot 'third_party/xess/bin') + '"' }
elseif ($Test -eq 'interface') { $start.Arguments = '--layer "' + $dll + '"' }
else { $start.Arguments = '--frames ' + $Frames }
if ($BridgeDevice -and $Test -eq 'wsi') { $start.Arguments += ' --api-1.3' }
if ($TraceCompute) { $start.Arguments += ' --compute-smoke' }
$child = [Diagnostics.Process]::new()
$child.StartInfo = $start
if (-not $child.Start()) { throw 'Could not start test process.' }
$stdoutTask = $child.StandardOutput.ReadToEndAsync()
$stderrTask = $child.StandardError.ReadToEndAsync()
$timer = [Diagnostics.Stopwatch]::StartNew()
while (-not $child.WaitForExit(500)) {
    if ($timer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
        $child.Kill()
        throw "Self-owned observer test timed out; retained logs: $reportDir"
    }
}
$child.WaitForExit()
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
$stdout | Set-Content -LiteralPath (Join-Path $reportDir 'stdout.txt') -Encoding utf8
$stderr | Set-Content -LiteralPath (Join-Path $reportDir 'stderr.txt') -Encoding utf8
$events = @()
$verified = $child.ExitCode -eq 0
if ($Test -eq 'interface') {
    $protocol = $stdout | ConvertFrom-Json
    $verified = $verified -and $protocol.passed -and $protocol.global_lookup_checks -eq 8
} elseif ($LogMode -eq 'absolute') {
    if (Test-Path -LiteralPath $logPath) {
        $events = @(Get-Content -LiteralPath $logPath | ForEach-Object { $_ | ConvertFrom-Json })
        foreach ($required in @('create_instance', 'destroy_instance')) { $verified = $verified -and ($events.event -contains $required) }
        if ($Test -ne 'probe') {
            foreach ($required in @('create_device', 'destroy_device')) { $verified = $verified -and ($events.event -contains $required) }
        }
        if ($Test -in @('interop', 'native')) {
            foreach ($required in @('create_image', 'get_device_queue', 'queue_submit_sample')) { $verified = $verified -and ($events.event -contains $required) }
            $device = @($events | Where-Object event -EQ 'create_device')[0]
            $verified = $verified -and $device.timeline_semaphore_requested -and ($device.enabled_extensions -contains 'VK_KHR_external_memory_win32')
        }
        if ($Test -eq 'wsi') {
            foreach ($required in @('create_win32_surface', 'destroy_surface', 'create_swapchain', 'destroy_swapchain', 'swapchain_images', 'queue_present_sample')) { $verified = $verified -and ($events.event -contains $required) }
            $deviceEnd = @($events | Where-Object event -EQ 'destroy_device')[0]
            $verified = $verified -and $deviceEnd.present_calls -eq $Frames -and $deviceEnd.present_negative_results -eq 0
            $surfaceStart = @($events | Where-Object event -EQ 'create_win32_surface')[0]
            $surfaceEnd = @($events | Where-Object event -EQ 'destroy_surface')[0]
            $swapchainStart = @($events | Where-Object event -EQ 'create_swapchain')[0]
            $verified = $verified -and $surfaceStart.result -eq 0 -and $surfaceStart.hwnd -match '^0x[1-9a-f][0-9a-f]*$' -and
                $surfaceStart.surface -eq $surfaceEnd.surface -and $surfaceStart.surface -eq $swapchainStart.surface
        }
        if ($TraceStreamline) {
            $refusal = @($events | Where-Object { $_.event -eq 'sl_observer_install' -and $_.state -eq 'refused_process_path' })
            $verified = $verified -and $refusal.Count -eq 1
        }
        if ($TraceFsr) {
            $refusal = @($events | Where-Object { $_.event -eq 'fsr_observer_install' -and $_.state -eq 'refused_process_path' })
            $verified = $verified -and $refusal.Count -eq 1 -and ($events.event -notcontains 'fsr_prepare_input_candidate')
        } else { $verified = $verified -and ($events.event -notcontains 'fsr_observer_install') }
        if ($TraceFsrUpscale) {
            $upscaleRefusal = @($events | Where-Object { $_.event -eq 'upscale_observer_install' -and $_.state -eq 'refused_process_path' })
            $verified = $verified -and $upscaleRefusal.Count -eq 1
        } else { $verified = $verified -and ($events.event -notcontains 'upscale_observer_install') }
        if ($TraceCompute) {
            foreach ($required in @('compute_observer_start','compute_shader_metadata','compute_pipeline','compute_dispatch_sample','compute_observer_summary')) {
                $verified = $verified -and ($events.event -contains $required)
            }
            $computeSummaries = @($events | Where-Object event -EQ 'compute_observer_summary')
            $dispatches = @($events | Where-Object event -EQ 'compute_dispatch_sample')
            $verified = $verified -and $computeSummaries.Count -gt 0 -and $dispatches.Count -ge 2
            if ($computeSummaries.Count -gt 0 -and $dispatches.Count -ge 2) {
                $computeEnd = $computeSummaries[-1]
                $verified = $verified -and $computeEnd.dispatch_recording_calls -eq $Frames -and $computeEnd.unknown_pipeline_dispatches -eq 0 -and
                    $computeEnd.trace_failures -eq 0 -and $computeEnd.live_command_buffers -eq 0 -and $computeEnd.live_pipelines -eq 0 -and
                    $dispatches[0].shader_generation -gt 0 -and $dispatches[0].pipeline_generation -gt 0
            }
        } else { $verified = $verified -and ($events.event -notcontains 'compute_observer_start') }
        $deviceCreates = @($events | Where-Object event -EQ 'create_device')
        if ($BridgeDevice) {
            $verified = $verified -and $deviceCreates.Count -gt 0
            foreach ($effectiveDevice in $deviceCreates) {
                $verified = $verified -and $effectiveDevice.result -eq 0 -and $effectiveDevice.bridge_device_opt_in -and
                    $effectiveDevice.bridge_device_plan_status -eq 'ready' -and $effectiveDevice.bridge_device_plan_applied -and
                    $effectiveDevice.bridge_device_create_attempts -eq 1 -and $effectiveDevice.timeline_semaphore_requested -and
                    ($effectiveDevice.enabled_extensions -contains 'VK_KHR_external_memory_win32') -and
                    ($effectiveDevice.enabled_extensions -contains 'VK_KHR_external_semaphore_win32')
                if ($Test -eq 'wsi') {
                    $verified = $verified -and -not $effectiveDevice.original_timeline_semaphore_requested -and
                        $effectiveDevice.bridge_device_timeline_changed -and $effectiveDevice.bridge_device_added_extensions.Count -eq 2 -and
                        @($effectiveDevice.original_enabled_extensions).Count -eq 1 -and
                        ($effectiveDevice.original_enabled_extensions -contains 'VK_KHR_swapchain')
                } elseif ($Test -in @('interop', 'native')) {
                    $verified = $verified -and $effectiveDevice.original_timeline_semaphore_requested -and
                        -not $effectiveDevice.bridge_device_timeline_changed -and $effectiveDevice.bridge_device_added_extensions.Count -eq 0
                }
            }
        } else {
            foreach ($effectiveDevice in $deviceCreates) {
                $verified = $verified -and -not $effectiveDevice.bridge_device_opt_in -and
                    $effectiveDevice.bridge_device_plan_status -eq 'not_requested' -and -not $effectiveDevice.bridge_device_plan_applied
            }
        }
    } else { $verified = $false }
} else {
    $verified = $verified -and -not (Test-Path -LiteralPath $logPath) -and -not (Test-Path -LiteralPath (Join-Path $reportDir 'relative-observer.jsonl'))
}
if ($Test -eq 'native') {
    $protocol = $stdout | ConvertFrom-Json
    $verified = $verified -and $protocol.passed -and $protocol.nativeImageFrames -eq $Frames -and
        $protocol.cpuUploadFrames -eq 0 -and $protocol.everyPixelCompared -and $protocol.borrowedDeviceAliveAfterBridgeDestruction
}
$result = [ordered]@{
    capturedAt = (Get-Date).ToString('o')
    test = $Test
    logMode = $LogMode
    streamlineOptIn = [bool]$TraceStreamline
    bridgeDeviceOptIn = [bool]$BridgeDevice
    computeObserverOptIn = [bool]$TraceCompute
    fsrCallbackObserverOptIn = [bool]$TraceFsr
    fsrUpscaleObserverOptIn = [bool]$TraceFsrUpscale
    exitCode = $child.ExitCode
    passed = [bool]$verified
    eventCount = $events.Count
    eventTypes = @($events.event | Select-Object -Unique)
    observerHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    manifestHash = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
    buildDirectory = $BuildDirectory
    testExecutableHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    gameTested = $false
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $reportDir 'result.json') -Encoding utf8
Write-Host "Report: $reportDir"
$result | ConvertTo-Json -Depth 6
if (-not $verified) { throw 'Observer verification failed; inspect retained child output and events.' }
