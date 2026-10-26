[CmdletBinding()]
param([switch]$RunGpu, [string]$Executable, [string]$ValidationLayerDirectory, [string]$OutputDirectory)
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 required.' }
$project=Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable=Join-Path $project 'build/xess-sr-gpu-timestamps-gpu-test.exe' }
if (-not $ValidationLayerDirectory) { $ValidationLayerDirectory=Join-Path $project '.tools/vvl-aea761f4' }
$Executable=(Get-Item -LiteralPath $Executable).FullName
$ValidationLayerDirectory=(Get-Item -LiteralPath $ValidationLayerDirectory).FullName
foreach ($name in @('VkLayer_khronos_validation.json','VkLayer_khronos_validation.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ValidationLayerDirectory $name) -PathType Leaf)) { throw ('Missing existing local validation artifact '+$name) }
}
$sourcePaths=@('src/sr_gpu_timestamps_gpu_test.cpp','src/sr_gpu_timestamps.cpp','src/sr_gpu_timestamps.hpp',
    'src/render_pass_observer.cpp','src/render_pass_metadata.cpp','src/render_tail_diagnostics.cpp','scripts/test-sr-gpu-timestamps.ps1')
$sourceHashes=@($sourcePaths | ForEach-Object { [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $project $_) -Algorithm SHA256).Hash.ToLowerInvariant()} })
$provenance=[ordered]@{executable=$Executable;executableSha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant();
    validationLayerDirectory=$ValidationLayerDirectory;validationDllSha256=(Get-FileHash -LiteralPath (Join-Path $ValidationLayerDirectory 'VkLayer_khronos_validation.dll') -Algorithm SHA256).Hash.ToLowerInvariant();
    sourceSha256=$sourceHashes;gameTested=$false;sdkUsed=$false;productionFilesChanged=$false;installedPayloadChanged=$false}
if (-not $RunGpu) { $provenance['gpuRun']=$false;$provenance['note']='Compiled fixture only. -RunGpu starts only this owned offscreen query fixture after game process checks.'; $provenance | ConvertTo-Json -Depth 8; return }
# Read-only checks immediately before launching; never close or manipulate these processes.
$running=@(Get-Process | Where-Object { $_.ProcessName -match '^(Endfield|Games|EFU)$' })
if ($running.Count) { throw ('Normally exit Endfield/Games/EFU before this fixture: '+(($running | ForEach-Object { $_.ProcessName+':'+$_.Id }) -join ', ')) }
$processCheck=[ordered]@{checkedUtc=[DateTime]::UtcNow.ToString('o');names=@('Endfield','Games','EFU');allAbsent=$true}
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-sr-timestamps-owned-gpu') }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$OutputDirectory=(Get-Item -LiteralPath $OutputDirectory).FullName
$start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$Executable;$start.ArgumentList.Add('--run-owned-gpu');$start.WorkingDirectory=$project
$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
foreach ($name in @($start.EnvironmentVariables.Keys | Where-Object { $_ -like 'XESS_*' -or $_ -like 'SL_*' }) +
    @('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_ADD_IMPLICIT_LAYER_PATH','VK_LAYER_SETTINGS_PATH',
      'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG','VK_ICD_FILENAMES','VK_DRIVER_FILES','VK_ADD_DRIVER_FILES')) {
    $start.EnvironmentVariables.Remove($name)
}
# All settings affect only this new child. No registry, installed layer, RTSS or ETW changes.
$start.EnvironmentVariables['VK_LAYER_PATH']=$ValidationLayerDirectory
$start.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
$start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
$start.EnvironmentVariables['DISABLE_RTSS_LAYER']='1'
$child=[Diagnostics.Process]::new();$child.StartInfo=$start
try {
    if (-not $child.Start()) { throw 'Could not start owned timestamp fixture.' }
    $stdoutTask=$child.StandardOutput.ReadToEndAsync();$stderrTask=$child.StandardError.ReadToEndAsync();$clock=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
    while (-not $child.WaitForExit(250)) { if ($clock.Elapsed.TotalSeconds -gt 32) { $timedOut=$true;$child.Kill();break } }
    $child.WaitForExit();$stdout=$stdoutTask.GetAwaiter().GetResult();$stderr=$stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'stdout.jsonl'),$stdout,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'stderr.txt'),$stderr,[Text.UTF8Encoding]::new($false))
    $rows=@($stdout -split '\r?\n' | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
    $result=@($rows | Where-Object event -EQ 'sr_timestamp_gpu_result');$runtime=@($rows | Where-Object event -EQ 'sr_timestamp_owned_runtime');$samples=@($rows | Where-Object event -EQ 'sr_timestamp_owned_sample')
    $problems=[Collections.Generic.List[string]]::new()
    if ($timedOut -or $child.ExitCode -ne 0) { $problems.Add('Owned child timed out or failed.') }
    if ($result.Count -ne 1) { $problems.Add('Exactly one final result required.') }
    else {
        $r=$result[0]
        if (-not $r.passed -or -not $r.validation_layer_loaded -or $r.validation_errors -ne 0 -or -not $r.parent_cleanup_completed -or $r.quarantined_until_process_exit) { $problems.Add('Validation load/errors or actual parent cleanup did not pass.') }
        if ($r.query_pools_created -ne 2 -or $r.query_pools_destroyed -ne 2 -or $r.query_reads -ne 1 -or -not $r.stop_retained_submitted -or -not $r.stop_retained_unsubmitted) { $problems.Add('Real query ownership, stop retention or unsubmitted retirement differs.') }
        if ($r.fixture_fence_timeout_ns -ne 5000000000 -or $r.device_wait_idle_calls -ne 0 -or $r.game_fsr_verified -or $r.sdk_used) { $problems.Add('Bounded own-fence/no-SDK/no-game scope differs.') }
    }
    if ($runtime.Count -ne 1 -or $runtime[0].actual_pipeline -eq 0 -or $runtime[0].timestamp_valid_bits -le 0 -or $runtime[0].timestamp_period_ns -le 0 -or $runtime[0].observer_layer_loaded) { $problems.Add('Actual hardware/pipeline/timestamp provenance missing.') }
    if ($samples.Count -ne 1 -or -not $samples[0].available -or -not $samples[0].duration_known -or -not $samples[0].native_submit_success -or -not $samples[0].native_fence_completed -or -not $samples[0].native_reset_before_retire -or $samples[0].game_fsr_verified) { $problems.Add('One actual completed submitted query interval required.') }
    $provenance['gpuRun']=$true;$provenance['processCheck']=$processCheck;$provenance['exitCode']=$child.ExitCode;$provenance['timedOut']=$timedOut
    $provenance['result']=$result;$provenance['runtime']=$runtime;$provenance['samples']=$samples;$provenance['problems']=@($problems)
    $provenance | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8NoBOM
    if ($problems.Count) { throw ('Owned timestamp fixture did not pass: '+($problems -join ' ')+' Saved '+$OutputDirectory) }
    [ordered]@{passed=$true;outputDirectory=$OutputDirectory;validationErrors=$result[0].validation_errors;parentCleanupCompleted=$true;querySamples=$samples.Count;gameFsrVerified=$false} | ConvertTo-Json
} finally { $child.Dispose() }
