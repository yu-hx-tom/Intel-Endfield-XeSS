#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([switch]$Disabled, [switch]$GameRoute)
$ErrorActionPreference='Stop'
if (@(Get-Process -Name Endfield,Games -ErrorAction SilentlyContinue).Count) { throw 'Close the game before this GPU self-test.' }
$project=Split-Path -Parent $PSScriptRoot
$output=Join-Path $project ('out\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-sr-loader-startup')
[void][IO.Directory]::CreateDirectory($output)
$dll=Join-Path $project 'build\observer\xess-vulkan-observer.dll'
$sdk=Join-Path $project 'third_party\xess\bin\libxess.dll'
if ((Get-FileHash -LiteralPath $sdk).Hash.ToLowerInvariant() -cne '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7') { throw 'Unexpected SR SDK binary.' }
Copy-Item -LiteralPath $dll,$sdk -Destination $output
$manifest=Get-Content -LiteralPath (Join-Path $project 'build\observer\VkLayer_xess_observer.json') -Raw | ConvertFrom-Json
$manifest.layer.library_path=Join-Path $output 'xess-vulkan-observer.dll'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'VkLayer_xess_observer.json') -Encoding utf8NoBOM
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=Join-Path $project 'build\xess-vulkan-wsi-smoke.exe'
$start.Arguments='--frames 24 --api-1.3 --compute-smoke'
$start.UseShellExecute=$false; $start.WorkingDirectory=$output
$start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
foreach ($name in @($start.Environment.Keys)) {
    if ($name -like 'XESS_*' -or $name -like 'VK_*') { [void]$start.Environment.Remove($name) }
}
$start.Environment['DISABLE_RTSS_LAYER']='1'
$start.Environment['VK_LAYER_PATH']=$output
$start.Environment['VK_INSTANCE_LAYERS']='VK_LAYER_XESS_bridge_observer'
$start.Environment['XESS_OBSERVER_LOG']=Join-Path $output 'observer.jsonl'
if (-not $Disabled) { $start.Environment['XESS_SR_REPLACE']='1' }
if ($GameRoute) {
    foreach ($name in @('XESS_CONTINUOUS_INPUTS','XESS_FSR_NATIVE_OFF','XESS_FSR_UPSCALE_OBSERVER','XESS_BRIDGE_DEVICE','XESS_PRESENTATION_OFF','XESS_FAKE_SWAPCHAIN','XESS_GPU_TIMESTAMPS')) { $start.Environment[$name]='1' }
    $start.Environment['DISABLE_RTSS_LAYER']='1'
}
$record=[ordered]@{passed=$false; srRequested=(-not $Disabled); gameRouteRequested=[bool]$GameRoute; output=$output; observerSha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant(); gameStarted=$false; systemConfigurationChanged=$false}
try {
    $child=[Diagnostics.Process]::Start($start)
    $stdout=$child.StandardOutput.ReadToEndAsync(); $stderr=$child.StandardError.ReadToEndAsync()
    if (-not $child.WaitForExit(30000)) { $child.Kill($true); $child.WaitForExit(); throw 'SR loader self-test timed out.' }
    $record.exitCode=$child.ExitCode
    [IO.File]::WriteAllText((Join-Path $output 'stdout.log'),$stdout.Result)
    [IO.File]::WriteAllText((Join-Path $output 'stderr.log'),$stderr.Result)
    if ($child.ExitCode -ne 0 -or $stdout.Result -notmatch 'WSI_SMOKE_PASS') { throw "Native loader self-test failed: $($child.ExitCode)" }
    $events=@(Get-Content -LiteralPath (Join-Path $output 'observer.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    $record.srInstancePlans=@($events | Where-Object event -CEQ 'sr_instance_plan').Count
    $record.srDevicePlans=@($events | Where-Object { $_.event -ceq 'sr_device_plan' -and $_.applied }).Count
    $record.privateProbeInstances=@($events | Where-Object { $_.event -ceq 'create_instance' -and $_.sr_sdk_private }).Count
    if ($Disabled) {
        if ($record.srInstancePlans -ne 0 -or $record.privateProbeInstances -ne 0) { throw 'SR OFF entered the SDK query path.' }
    } else {
        if (-not $GameRoute -and $record.srDevicePlans -ne 1) { throw 'SR loader-visible device requirements were not applied.' }
        if ($record.srInstancePlans -ne 1 -or $record.privateProbeInstances -lt 1) { throw 'The real SDK probe did not exercise exactly one outer admission.' }
    }
    $record.passed=$true
} catch { $record.error=$_.Exception.Message }
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8NoBOM
$record | ConvertTo-Json -Depth 4
if (-not $record.passed) { exit 1 }
