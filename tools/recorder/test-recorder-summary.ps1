#requires -Version 7.0
#requires -PSEdition Core
# Dot-sourced by test-recorder.ps1; all source files below are generated fixtures.

$absentSource=Get-RecorderMissingSessionStatus 'absent'
Check ($absentSource.expected -and $absentSource.message.Contains('诊断模式关闭')) 'Expected zero-log recording must not warn or fail for an absent session.'
Check (-not (Get-RecorderMissingSessionStatus 'ambiguous').expected) 'Ambiguous PID/session identity must remain a diagnostic warning.'

$metadata=Resolve-RecorderRequestContext ([pscustomobject]@{label='K2';labelCode='k2';notes='test note';xiaomiPerformanceMode='均衡';measurementMode='summary';capturePowerState=$true})
Check ($metadata.label -ceq 'K2' -and $metadata.notes -ceq 'test note' -and $metadata.xiaomiPerformanceModeSource -ceq 'user-selected' -and -not $metadata.detailIncluded) 'Recording request context was not retained truthfully.'
foreach($invalidContext in @([pscustomobject]@{measurementMode='sampled'},[pscustomobject]@{capturePowerState='true'},[pscustomobject]@{labelCode='../outside'},[pscustomobject]@{notes=42},[pscustomobject]@{xiaomiPerformanceMode='invented'})){Refuses {Resolve-RecorderRequestContext $invalidContext} 'Invalid request metadata was accepted.'}
$mockPower=Get-RecorderPowerState $true
$mockConfiguration=Get-RecorderConfigurationSnapshot 'start' $true $null
Check (-not $mockPower.available -and $null -eq $mockPower.batteryPercent -and $null -eq $mockPower.powerPlanGuid -and $mockPower.errors -contains 'test_only_no_live_api' -and -not ('RecorderPowerState' -as [type])) 'Mock power state called a live API or fabricated measurements.'
Check ($mockConfiguration.testOnly -and -not $mockConfiguration.launcherQueryAvailable -and $mockConfiguration.installation.installationId -ceq 'unknown' -and $mockConfiguration.diagnosticFiles.error -ceq 'test_only_no_live_read') 'Mock configuration performed a real configuration/launcher read.'
$configFixture=Join-Path $testRoot 'configuration-fixture';[IO.Directory]::CreateDirectory($configFixture)|Out-Null
[IO.File]::WriteAllText((Join-Path $configFixture 'small64'),('a'*64),$encoding);[IO.File]::WriteAllText((Join-Path $configFixture 'large65'),('b'*65),$encoding)
Write-RecorderJson (Join-Path $configFixture 'install-receipt.json') ([ordered]@{installationId='fixture-installation'})
[IO.File]::WriteAllText((Join-Path $configFixture 'xess-vulkan-observer.dll'),'fixture observer bytes',$encoding)
$configBefore=@{};foreach($file in Get-ChildItem -LiteralPath $configFixture -File){$configBefore[$file.Name]=Get-RecorderSha256 $file.FullName}
$config=Get-RecorderDiagnosticFilesSnapshot $configFixture;$installed=Get-RecorderInstallationSnapshot $configFixture
Check ($config.available -and @($config.files|Where-Object name -CEQ 'small64')[0].contentRead -and @($config.files|Where-Object name -CEQ 'small64')[0].content.Length -eq 64 -and -not @($config.files|Where-Object name -CEQ 'large65')[0].contentRead) 'Configuration snapshot violated the 64-byte content boundary.'
Check ($installed.installationId -ceq 'fixture-installation' -and $installed.receiptSha256 -ceq $configBefore['install-receipt.json'] -and $installed.observerDllSha256 -ceq $configBefore['xess-vulkan-observer.dll']) 'Installation receipt identity or DLL/receipt hash was lost.'
foreach($file in Get-ChildItem -LiteralPath $configFixture -File){Check ((Get-RecorderSha256 $file.FullName) -ceq $configBefore[$file.Name]) 'Read-only configuration snapshot modified a source.'}
$missingInstall=Get-RecorderInstallationSnapshot (Join-Path $configFixture 'missing')
Check ($missingInstall.installationId -ceq 'unknown' -and $null -eq $missingInstall.observerDllSha256) 'Missing installation evidence became a fabricated installation identity.'
$effective=Get-RecorderEffectiveMeasurement ([pscustomobject]@{copy_route='synchronous_shared';inflight_limit=0;shared_copy_path=$true;depth_snapshot_compute=$true;sr_runtime_connected=$false;sr_state='off_not_connected';qpc_ns=42})
Check ($effective.copy_route -ceq 'synchronous_shared' -and $effective.inflight_limit -eq 0 -and $effective.shared -and $effective.depth_snapshot_compute -and $effective.sr.sr_state -ceq 'off_not_connected') 'Configuration snapshot lost actual route/limit/shared/depth/SR facts.'
$liveSr=[pscustomobject]@{fg_multiplier_requested=4;fg_multiplier_actual=3;fg_multiplier_maximum=3;fg_multiplier_result=0;fg_multiplier_restart_required=$false;event='measurement_status';sr_status_schema=1;sr_active_reason='none';sr_reason_category='none';sr_last_issue_reason='graphics_bindings_unknown';sr_currently_stopped=$false;sr_recovery_blocked='none';sr_successful_recoveries=2;sr_guard_schema=2;sr_guard_mask_draws=12345678;sr_guard_mask_fallbacks=7;sr_guard_mask_refreshes=10;sr_tail_diagnostics_enabled=$false;sr_guard_draw_checks=12345678;sr_guard_reference_batches=120;sr_guard_safe_cache_hits=12345000;sr_degraded_since_frame=44}
$liveSr | Add-Member -NotePropertyMembers @{fg_multiplier_change_policy='next_process';async_log_requested=$true;async_log_active=$true;async_log_pending=3;async_log_dropped_detail=7;async_log_max_write_ns=1234567890}
$multiplierMetadata=Get-RecorderEffectiveMeasurement $liveSr
Check ($multiplierMetadata.sr.fg_multiplier_requested -eq 4 -and $multiplierMetadata.sr.fg_multiplier_actual -eq 3 -and $multiplierMetadata.sr.fg_multiplier_maximum -eq 3) 'Recorder lost requested/applied frame-generation multiplier.'
$liveSr|Add-Member -NotePropertyMembers @{fg_settings_schema=1;fg_settings_available=$true;fg_threshold_requested_index=2;fg_threshold_applied=$true;fg_debug_requested=0;fg_total_single_output=3;fg_total_multi_output=197;fg_total_history_resets=2;game_frame_cap_schema=2;game_frame_cap_requested=$true;game_frame_cap_target_fps=45;game_frame_cap_invalid_config=$false;game_frame_cap_30_to_40_requested=$true;game_frame_cap_installed=$true;game_frame_cap_readback=40;game_frame_cap_remapped=2}
$multiplierMetadata=Get-RecorderEffectiveMeasurement $liveSr
$liveCompact=ConvertTo-RecorderCompactStatus ($liveSr|ConvertTo-Json -Compress)|ConvertFrom-Json
Check ($liveCompact.fg_multiplier_change_policy -ceq 'next_process' -and $liveCompact.async_log_pending -eq 3 -and $liveCompact.async_log_dropped_detail -eq 7 -and $liveCompact.async_log_max_write_ns -eq 1234567890 -and $multiplierMetadata.sr.async_log_active) 'Startup multiplier policy and asynchronous log backpressure evidence were lost.'
Check ($multiplierMetadata.sr.fg_settings_schema -eq 1 -and $multiplierMetadata.sr.game_frame_cap_readback -eq 40) 'Actual context snapshot lost FG tuning or engine cap.'
Check ($liveCompact.game_frame_cap_schema -eq 2 -and $liveCompact.game_frame_cap_requested -and $liveCompact.game_frame_cap_target_fps -eq 45 -and -not $liveCompact.game_frame_cap_invalid_config) 'Configurable FPS target lost in compact stream.'
Check ($liveCompact.fg_settings_schema -eq 1 -and $liveCompact.fg_settings_available -and $liveCompact.fg_threshold_requested_index -eq 2 -and $liveCompact.fg_total_multi_output -eq 197 -and $liveCompact.game_frame_cap_readback -eq 40 -and $liveCompact.game_frame_cap_remapped -eq 2) 'FG tuning outcomes and engine cap were lost from compact measurement stream.'
$liveEffective=Get-RecorderEffectiveMeasurement $liveSr
Check ($liveCompact.sr_active_reason -ceq 'none' -and -not $liveCompact.sr_currently_stopped -and $liveCompact.sr_last_issue_reason -ceq 'graphics_bindings_unknown' -and $liveCompact.sr_degraded_since_frame -eq 44 -and $liveCompact.sr_successful_recoveries -eq 2) 'Current recovery state and historical degradation must survive compact retention independently.'
Check ($liveEffective.sr.sr_guard_draw_checks -eq 12345678 -and $liveCompact.sr_guard_reference_batches -eq 120 -and $liveCompact.sr_guard_safe_cache_hits -eq 12345000) 'Per-category guard workload counters were lost from recorder context or compact data.'
Check ($liveEffective.sr.sr_guard_schema -eq 2 -and $liveCompact.sr_guard_mask_draws -eq 12345678 -and $liveCompact.sr_guard_mask_fallbacks -eq 7 -and $liveCompact.sr_guard_mask_refreshes -eq 10 -and -not $liveCompact.sr_tail_diagnostics_enabled) 'Reference-mask fast and fallback counters or tail diagnostics state were lost.'
$intervalFixture=@([pscustomobject]@{pid=123;status_begin_ns=1000000000;status_end_ns=2000000000;qpc_ns=2000000000;status_interval_state='reset_transition'},[pscustomobject]@{pid=123;status_begin_ns=2000000000;status_end_ns=3000000000;qpc_ns=3000000000;status_interval_state='mixed'},[pscustomobject]@{pid=123;status_begin_ns=3000000000;status_end_ns=6000000000;qpc_ns=6000000000;status_interval_state='steady_fg_input_valid';status_interval_attempts=1;status_interval_steady_frames=1;status_interval_resets=0},[pscustomobject]@{pid=123;status_begin_ns=6000000000;status_end_ns=7000000001;qpc_ns=7000000000;status_interval_state='steady_fg_input_valid';status_interval_attempts=1;status_interval_steady_frames=1;status_interval_resets=0})
$stateFixture=@(Get-RecorderStateIntervals $intervalFixture ([ordered]@{gamePid=123;qpcFrequency=1000;startQpcTicks=1000;endQpcTicks=7000}))
Check ($stateFixture.Count -eq 4 -and $stateFixture[0].state -ceq 'reset' -and @($stateFixture|Where-Object state -CEQ 'steady_fg_input_valid').Count -eq 0) 'Mixed/reset/long-gap/inconsistent-clock intervals were promoted into qualified steady time.'
$hitchFixture=Join-Path $testRoot 'whole-interval-hitch-fixture';[IO.Directory]::CreateDirectory($hitchFixture)|Out-Null
$hitchWriter=[IO.StreamWriter]::new((Join-Path $hitchFixture 'measurement-status-0.compact.jsonl'),$false,$encoding)
try {foreach($range in @(@(1000,4000),@(4000,34000))){$hitchWriter.WriteLine((ConvertTo-RecorderCompactStatus (([ordered]@{event='measurement_status';pid=123;status_begin_ns=[long]$range[0]*1000000;status_end_ns=[long]$range[1]*1000000;qpc_ns=[long]$range[1]*1000000;status_interval_state='steady_fg_input_valid';status_interval_attempts=1;status_interval_steady_frames=1;status_interval_fg_off_frames=0;status_interval_invalid_frames=0;status_interval_transition_frames=0;status_interval_unknown_frames=0;status_interval_resets=0;status_interval_history_resets=1;status_reset_exclusion='semantic_only_stall_remedies_retained';status_interval_output_failures=1;sdk_frames=1;output_fresh=$false;sdk_query_result=-1}|ConvertTo-Json -Compress))))}}finally{$hitchWriter.Dispose()}
[IO.File]::WriteAllText((Join-Path $hitchFixture 'frames.csv'),"StreamEpoch,ProviderOrdinal,InitialSnapshot,ProcessID,Dropped,qpcTime,msUntilDisplayed,msBetweenDisplayChange,SwapChainAddress`n1,1,0,123,0,4000,0,3000,0x1`n1,2,0,123,0,34000,0,30000,0x1`n",$encoding)
$hitch=Write-RecorderReadableSummary $hitchFixture ([ordered]@{gamePid=123;qpcFrequency=1000;startQpcTicks=1000;endQpcTicks=34000}) ([ordered]@{selectedCollectors=(New-TestMask @('pmdp','measurements'))}) $metadata
Check ($hitch.steadyVerified -and $hitch.segments[0].stateDurationSeconds.steady_fg_input_valid -eq 33 -and $hitch.segments[0].steady.displayIntervalMs.count -eq 2 -and $hitch.segments[0].steady.displayIntervalMs.maximum -eq 30000 -and $hitch.segments[0].steady.displayIntervalMs.over100ms -eq 2 -and $hitch.segments[0].sdkObservations.outputFailures -eq 2) 'Complete native intervals dropped 3s/30s hitches, long-frame remedy resets or failed-output samples.'
$offPlan=@(Get-RecorderHelperPlan (New-TestMask @('foreground')) 123 1 $testRoot $PSScriptRoot 'TEST ONLY')
$onPlan=@(Get-RecorderHelperPlan (New-TestMask @('foreground')) 123 1 $testRoot $PSScriptRoot 'TEST ONLY' $true)
$hardwareOnlyPlan=@(Get-RecorderHelperPlan (New-TestMask @('hardware')) 123 1 $testRoot $PSScriptRoot 'TEST ONLY' $true)
Check ($offPlan[0].arguments -ccontains '-CaptureOperationKeys:$false' -and $onPlan[0].arguments -ccontains '-CaptureOperationKeys:$true' -and $hardwareOnlyPlan[0].arguments -ccontains '-CaptureOperationKeys:$false') 'Recorder helper plan enabled keyboard polling without explicit foreground/key selection.'
$flagScript=Join-Path $testRoot 'test-operation-flag-binding.ps1'
[IO.File]::WriteAllText($flagScript,'param([switch]$CaptureOperationKeys=$true) [ordered]@{capture=[bool]$CaptureOperationKeys}|ConvertTo-Json -Compress',$encoding)
foreach($flag in @('$false','$true')) {
    $child=Start-RecorderOwned 'test-operation-switch' $powerShellRuntime.executable @('-NoProfile','-NonInteractive','-File',$flagScript,('-CaptureOperationKeys:'+$flag)) $testRoot ('operation-switch-'+$flag.Substring(1)+'.json') ('operation-switch-'+$flag.Substring(1)+'.stderr.txt')
    $completed=Complete-RecorderOwned $child 5000;$bound=[IO.File]::ReadAllText((Join-Path $testRoot ('operation-switch-'+$flag.Substring(1)+'.json')))|ConvertFrom-Json @recorderJsonOptions
    Check ($completed.exitCode -eq 0 -and $bound.capture -eq ($flag -ceq '$true')) 'PowerShell 7 worker argument binding changed the explicit keyboard polling switch.'
}

$summaryFixture=Join-Path $testRoot 'summary-fixture';[IO.Directory]::CreateDirectory($summaryFixture)|Out-Null
$summaryWindow=[ordered]@{qpcFrequency=1000;startQpcTicks=1000;endQpcTicks=121000;gamePid=123;recordingStarted=$true}
$statusWriter=[IO.StreamWriter]::new((Join-Path $summaryFixture 'measurement-status-0.compact.jsonl'),$false,$encoding)
try {for($second=1;$second -le 120;$second++) {
    $state=$(if($second -le 60 -or $second -gt 90){'steady_fg_input_valid'}elseif($second -le 70){'fg_off'}elseif($second -le 80){'unknown'}else{'input_invalid'})
    $line=([ordered]@{event='measurement_status';pid=123;status_begin_ns=[long](1000+($second-1)*1000)*1000000;status_end_ns=[long](1000+$second*1000)*1000000;qpc_ns=[long](1000+$second*1000)*1000000;status_interval_state=$state;status_interval_attempts=60;status_interval_steady_frames=$(if($state -ceq 'steady_fg_input_valid'){60}else{0});status_interval_resets=0;
        status_interval_sdk_warnings=$(if($second -in @(3,65,75)){1}else{0});status_interval_steady_sdk_warnings=$(if($second -eq 3){1}else{0});status_interval_fallback_sdk_warnings=$(if($second -eq 65){1}else{0});status_interval_unknown_sdk_warnings=$(if($second -eq 75){1}else{0});status_interval_sdk_errors=$(if($second -eq 2){1}else{0});status_interval_steady_sdk_errors=$(if($second -eq 2){1}else{0});status_interval_history_resets=$(if($second -eq 2){1}else{0});status_reset_exclusion='semantic_only_stall_remedies_retained';status_interval_output_failures=$(if($second -eq 2){1}else{0});
        sdk_frames=$(if($state -ceq 'fg_off' -or $second -eq 2){1}else{2});output_fresh=($second -ne 2);sdk_query_result=$(if($second -eq 2){-5}else{0});sdk_framegen_result=$(if($second -eq 2){-6}else{0});control_epoch=1;async=$true;inflight_limit=2;shared_copy_path=$true;depth_snapshot_compute=$true;sr_enabled=$false;provider_depth_gpu=@{giant=('x'*1000)}}|ConvertTo-Json -Depth 5 -Compress)
    $compact=ConvertTo-RecorderCompactStatus $line
    Check (-not $compact.Contains('provider_depth_gpu') -and $compact.Contains('status_interval_state') -and $compact.Contains('status_interval_history_resets') -and $compact.Contains('semantic_only_stall_remedies_retained') -and $compact.Contains('status_interval_output_failures')) 'Compact status retained detail payload or lost the interval/reset/output evidence.'
    $statusWriter.WriteLine($compact)
}}finally{$statusWriter.Dispose()}
$markerWriter=[IO.StreamWriter]::new((Join-Path $summaryFixture 'markers.jsonl'),$false,$encoding)
try {foreach($marker in @([ordered]@{sequence=1;qpcTicks=61000;qpcFrequency=1000;utc='2026-01-01T00:01:00Z';kind='manual'},[ordered]@{sequence=2;qpcTicks=65000;qpcFrequency=1000;utc='2026-01-01T00:01:04Z';kind='key';key='M'},[ordered]@{sequence=3;qpcTicks=85000;qpcFrequency=1000;utc='2026-01-01T00:01:24Z';kind='key';key='Esc'},[ordered]@{sequence=4;qpcTicks=86000;qpcFrequency=1000;utc='2026-01-01T00:01:25Z';kind='key';key='A'})){$markerWriter.WriteLine(($marker|ConvertTo-Json -Compress))};$markerWriter.Write('{"sequence":5,') }finally{$markerWriter.Dispose()}
$csv=[Collections.Generic.List[string]]::new();$csv.Add('StreamEpoch,ProviderOrdinal,InitialSnapshot,ProcessID,Dropped,qpcTime,msUntilDisplayed,msBetweenDisplayChange,SwapChainAddress')
$positions=@(2000,3000,4000,5000,63000,73000,83000,93000,61010);$intervalValues=@(16,26,41,101,150,60,50,20,100)
for($i=0;$i -lt $positions.Count;$i++){$latency=$i+1;$csv.Add('1,'+($i+1)+',0,123,0,'+($positions[$i]-$latency)+','+$latency+','+$intervalValues[$i]+',0x1')}
$csv.Add($csv[1]);$csv.Add('1,100,1,123,0,12000,2,999,0x1');$csv.Add('1,101,0,124,0,12000,2,999,0x1');$csv.Add('1,102,0,123,1,12000,2,999,0x1');$csv.Add('1,103,0,123,0,12000,2,NaN,0x1')
[IO.File]::WriteAllLines((Join-Path $summaryFixture 'frames.csv'),$csv,$encoding)
$sampleWriter=[IO.StreamWriter]::new((Join-Path $summaryFixture 'power.jsonl'),$false,$encoding)
try {foreach($sample in @(@(11000,10,4,70),@(71000,20,6,80))){$sampleWriter.WriteLine(([ordered]@{qpcTicks=$sample[0];error=$null;readings=@(@{name='CPU cores power';value=$sample[1];finiteValid=$true},@{name='CPU GT power';value=$sample[2];finiteValid=$true},@{name='CPU package power';value=30;finiteValid=$true},@{name='CPU temperature';value=$sample[3];finiteValid=$true},@{name='GPU temperature';value=999;finiteValid=$false})}|ConvertTo-Json -Depth 5 -Compress))}}finally{$sampleWriter.Dispose()}
[IO.File]::WriteAllText((Join-Path $summaryFixture 'power-state.jsonl'),"{`"available`":true,`"acSource`":`"ac`",`"powerPlanGuid`":`"fixture-plan`",`"powerPlanName`":`"fixture name`",`"effectiveOverlayGuid`":`"fixture-overlay`",`"batteryPercent`":80}`n{`"available`":false,`"acSource`":`"unknown`",`"errors`":[`"fixture_unavailable`"]}`n",$encoding)
$measurementWriter=[IO.StreamWriter]::new((Join-Path $summaryFixture 'measurements-summary-0.stream.jsonl'),$false,$encoding)
try {foreach($entry in @(@(1000,61000,500),@(61000,121000,900))){$measurementWriter.WriteLine(([ordered]@{event='measurement_summary';pid=123;device='0x1';begin_ns=[long]$entry[0]*1000000;end_ns=[long]$entry[1]*1000000;counters=@{copy_failures=0;sdk_warnings=3};copy_commands=@{color=@{image_copy=$entry[2]};sdk=$null};metrics=@{cap_wait=@{count=10;sum=100000000}}}|ConvertTo-Json -Depth 7 -Compress))}}finally{$measurementWriter.Dispose()}
$summaryStub=[ordered]@{selectedCollectors=(New-TestMask @('pmdp','hardware','measurements'))}
$analysis=Write-RecorderReadableSummary $summaryFixture $summaryWindow $summaryStub $metadata
Check ($analysis.markers.validCount -eq 3 -and $analysis.markers.invalidRows -eq 1 -and $analysis.markers.partialTailRows -eq 1 -and -not $analysis.markers.keyMarkersAreStateEvidence) 'Marker validation, two-key limit or incomplete-tail handling failed.'
$whole=$analysis.segments[0]
Check ($analysis.steadyVerified -and $whole.stateDurationSeconds.steady_fg_input_valid -eq 90 -and $whole.stateDurationSeconds.fg_off -eq 10 -and $whole.stateDurationSeconds.input_invalid -eq 10 -and $whole.stateDurationSeconds.unknown -eq 10 -and $whole.stateDurationSeconds.map -eq 0 -and $whole.stateDurationSeconds.menu -eq 0) 'Steady/excluded durations were fabricated from keypresses or lost source interval evidence.'
Check ($analysis.powerState.samples -eq 2 -and $analysis.powerState.unavailableSamples -eq 1 -and $analysis.powerState.groups.Count -eq 2 -and $analysis.powerState.errors -contains 'fixture_unavailable') 'Power-state grouping silently discarded unavailable samples or changes.'
Check ($whole.raw.displayIntervalMs.count -eq 9 -and $whole.steady.displayIntervalMs.count -eq 5 -and $whole.steady.displayIntervalMs.p50 -eq 26 -and $whole.steady.displayIntervalMs.p90 -eq 101 -and $whole.steady.displayIntervalMs.p99 -eq 101 -and $whole.steady.displayIntervalMs.maximum -eq 101) 'Display event-time qualification, PMDP identity or nearest-rank percentiles failed.'
Check ($whole.steady.displayIntervalMs.over25ms -eq 3 -and $whole.steady.displayIntervalMs.over40ms -eq 2 -and $whole.steady.displayIntervalMs.over100ms -eq 1 -and $whole.steady.displayIntervalMs.over25PerMinute -eq 2 -and $whole.steady.legacyMsUntilDisplayed.p50 -eq 3 -and $whole.steady.legacyMsUntilDisplayed.p95 -eq 8) 'Display threshold rates or explicitly legacy latency metrics failed.'
Check ($whole.steady.hardwareSampleMean['CPU cores power'].mean -eq 10 -and $whole.raw.hardwareSampleMean['CPU cores power'].mean -eq 15 -and $whole.steady.hardwareSampleMean['CPU temperature'].mean -eq 70 -and -not $whole.raw.hardwareSampleMean.Contains('GPU temperature')) 'Power/temperature state selection or finite reading validation failed.'
Check ($whole.steadyMeasurement.windows -eq 1 -and $whole.steadyMeasurement.copyCommands['color.image_copy'] -eq 500 -and $whole.rawMeasurement.copyCommands['color.image_copy'] -eq 1400 -and $whole.unqualifiedMeasurementWindows -eq 1 -and $whole.steadyMeasurement.waits.cap_wait.totalMs -eq 100) 'Mixed summary windows were prorated or aggregate waits/copies were lost.'
Check ($whole.sdkObservations.available -and $whole.sdkObservations.allWarnings -eq 3 -and $whole.sdkObservations.steadyWarnings -eq 1 -and $whole.sdkObservations.fallbackWarnings -eq 1 -and $whole.sdkObservations.unknownWarnings -eq 1 -and $whole.sdkObservations.allErrors -eq 1 -and $whole.sdkObservations.steadyErrors -eq 1 -and $whole.steady.displayIntervalMs.count -eq 5) 'SDK warnings/negative errors were hidden by output-success gating or their source classification was lost.'
Check ($whole.sdkObservations.outputFailures -eq 1 -and $whole.sdkObservations.outputFailureIntervals -eq 120 -and $whole.stateDurationSeconds.steady_fg_input_valid -eq 90 -and [IO.File]::ReadAllText((Join-Path $summaryFixture 'summary.md')).Contains('semantic_only_stall_remedies_retained')) 'Long-frame remedy resets filtered eligible samples or outputFailures/reset policy was omitted.'
Check ($analysis.segments.Count -eq 5 -and $analysis.segments[1].steadyMeasurement.windows -eq 1 -and $analysis.segments[2].boundaryMeasurementWindowsNotProrated -eq 1 -and [IO.File]::Exists((Join-Path $summaryFixture 'summary.md'))) 'Marker segmentation or full-window-only aggregation failed.'
$officialFixture=Join-Path $testRoot 'official-summary-fixture';[IO.Directory]::CreateDirectory($officialFixture)|Out-Null;[IO.File]::Copy((Join-Path $summaryFixture 'frames.csv'),(Join-Path $officialFixture 'frames.csv'))
$official=Write-RecorderReadableSummary $officialFixture $summaryWindow $summaryStub $metadata
Check (-not $official.steadyVerified -and $official.segments[0].stateDurationSeconds.unknown -eq 120 -and $official.segments[0].raw.displayIntervalMs.count -eq 9 -and $official.segments[0].steady.displayIntervalMs.count -eq 0) 'Official passive capture claimed verified FG state or lost its raw display data.'
$longOfficialWindow=[ordered]@{gamePid=123;qpcFrequency=10000000;startQpcTicks=1400000000000L;endQpcTicks=1403001234567L}
$longOfficial=Write-RecorderReadableSummary $officialFixture $longOfficialWindow $summaryStub $metadata
Check (-not $longOfficial.steadyVerified -and [Math]::Abs($longOfficial.segments[0].durationSeconds-300.1234567) -lt 1e-8 -and [Math]::Abs($longOfficial.segments[0].stateDurationSeconds.unknown-300.1234567) -lt 1e-8) 'Passive capture over Int32 QPC ticks overflowed or rounded unknown exposure.'

$markerProtocol=[pscustomobject]@{markerProtocol=[pscustomobject]@{schema=1;writer='gui';closeBeforeStop=$true}}
$fixtureIdentity=[RecorderNative]::Read($PID)
Write-RecorderJson (Join-Path $summaryFixture 'markers.closed.json') ([ordered]@{closed=$true;accepted=4;written=4;dropped=0;error=$null})
$markerAck=Wait-RecorderMarkersClosed $summaryFixture $markerProtocol $PID $fixtureIdentity.Created 10
Check ($markerAck.closed -and $markerAck.exclusiveReadConfirmed -and -not $markerAck.ownerExited) 'Marker close acknowledgement was not checked before packaging.'
$lockedMarker=[IO.File]::Open((Join-Path $summaryFixture 'markers.jsonl'),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
try {Refuses {Wait-RecorderMarkersClosed $summaryFixture $markerProtocol $PID $fixtureIdentity.Created 10} 'An active marker writer was hashed/packaged despite close acknowledgement.'}finally{$lockedMarker.Dispose()}
Write-RecorderJson (Join-Path $summaryFixture 'markers.closed.json') ([ordered]@{closed=$true;accepted=7;written=4;dropped=1;unwritten=2;error='TEST_WRITER_ERROR';inputError='TEST_INPUT_ERROR'})
$lossAnalysis=Write-RecorderReadableSummary $summaryFixture $summaryWindow $summaryStub $metadata
Check ($lossAnalysis.markerIntegrity.dropped -eq 1 -and $lossAnalysis.markerIntegrity.unwritten -eq 2 -and $lossAnalysis.markerIntegrity.inputError -ceq 'TEST_INPUT_ERROR' -and $lossAnalysis.markerIntegrity.warnings.Count -gt 0 -and [IO.File]::ReadAllText((Join-Path $summaryFixture 'summary.md')).Contains('TEST_INPUT_ERROR')) 'Marker writer/input failures were silently omitted from the shareable summary.'

# A 30-minute fixture exercises the real reader and summary, without live capture sources.
$longSource=Join-Path $testRoot 'long-summary-source';$longOutput=Join-Path $testRoot 'long-summary-output'
foreach($folder in @($longSource,$longOutput)){[IO.Directory]::CreateDirectory($folder)|Out-Null}
$longWriter=[IO.StreamWriter]::new((Join-Path $longSource 'observer.jsonl.measurements-0.jsonl'),$false,$encoding);$padding='x'*16000
try {for($second=1;$second -le 1800;$second++){$longWriter.WriteLine('{"event":"measurement_window","values_key_owner":"'+$padding+'"}');$longWriter.WriteLine(([ordered]@{event='measurement_status';pid=123;qpc_ns=[long](1000+$second*1000)*1000000;status_begin_ns=[long](1000+($second-1)*1000)*1000000;status_interval_state='steady_fg_input_valid';status_interval_attempts=60;status_interval_steady_frames=60;status_interval_resets=0;sdk_frames=2;output_fresh=$true;input_captured=$second*60;provider_depth_gpu=@{detail=$padding}}|ConvertTo-Json -Depth 4 -Compress))}}finally{$longWriter.Dispose()}
[IO.File]::WriteAllText((Join-Path $longSource 'observer.jsonl.measurements-1.jsonl'),'',$encoding)
foreach($index in 0,1){[IO.File]::WriteAllText((Join-Path $longSource ('observer.jsonl.measurements-summary-'+$index+'.jsonl')),'',$encoding)}
[IO.File]::WriteAllText((Join-Path $longSource 'launch.json'),'{"testOnly":true}',$encoding)
$previousContext=$script:recordingContext;$previousGamePid=$script:gamePid;$previousStatus=$script:status;$previousOutputDirectory=$script:OutputDirectory
$script:recordingContext=$metadata;$script:gamePid=123;$script:status=[ordered]@{mode='TEST ONLY'};$script:OutputDirectory=$longOutput;$script:streams=@();$script:streamEvents=$null;$script:srStreamEvents=$null;$script:latestMeasurement=$null
try {
    Start-RecorderContextStreams (New-TestMask @('measurements')) $longSource $longOutput;Update-RecorderStreams -Drain
    foreach($entry in $script:streams){$entry.reader.Dispose()};$script:streamEvents.Dispose();$script:streamEvents=$null
    $longSaved=Save-RecorderContextSnapshots (New-TestMask @('measurements')) $longSource $longOutput
    Check ($longSaved.snapshots.Count -eq 0 -and @(Get-ChildItem -LiteralPath $longOutput -File -Filter '*.snapshot.jsonl').Count -eq 0 -and -not [IO.File]::Exists((Join-Path $longOutput 'measurements-0.stream.jsonl'))) 'Summary-only mode retained full detail or duplicated history snapshots.'
    $compactBytes=([IO.FileInfo](Join-Path $longOutput 'measurement-status-0.compact.jsonl')).Length
    $statusRows=@([IO.File]::ReadLines((Join-Path $longOutput 'measurement-status-0.compact.jsonl')))
    Check ($statusRows.Count -eq 1800 -and $compactBytes -lt 2MB -and -not ($statusRows -join '').Contains('provider_depth_gpu')) 'Thirty-minute compact status retention lost seconds or exceeded its detail budget.'
    # The measured PMDP baseline is about 65 MB per 30 minutes; count it once.
    $payload=@(Get-ChildItem -LiteralPath $longOutput -File|Measure-Object -Property Length -Sum).Sum
    Check ($payload+65000000 -lt 100000000) 'Thirty-minute summary/status payload plus measured PMDP baseline exceeds 100 MB.'
    Write-RecorderJson (Join-Path $longOutput 'volume-test.json') ([ordered]@{testOnly=$true;durationSeconds=1800;sourceDetailBytes=([IO.FileInfo](Join-Path $longSource 'observer.jsonl.measurements-0.jsonl')).Length;retainedCompactBytes=$compactBytes;retainedMetadataBytes=$payload;measuredPmdpBaselineBytes=65000000;projectedCombinedBytes=$payload+65000000;powerState180SamplesBudgetBytes=180000;hardwareForeground1800SamplesBudgetBytes=6000000;combinedWithSampleBudgetsBytes=$payload+65000000+180000+6000000;liveSourcesRead=$false})
    Check ($payload+65000000+180000+6000000 -lt 100000000) 'Thirty-minute volume budget does not leave room for power/hardware samples.'
}finally{foreach($entry in $script:streams){try{$entry.reader.Dispose()}catch{}};if($script:streamEvents){$script:streamEvents.Dispose()};$script:streams=@();$script:streamEvents=$null;$script:recordingContext=$previousContext;$script:gamePid=$previousGamePid;$script:status=$previousStatus;$script:OutputDirectory=$previousOutputDirectory;$script:recorderQualifiedRanges=$null}

# Recording-only evidence never certifies actual GPU completion/display.
$srWindow=[pscustomobject]@{gamePid=123;qpcFrequency=1000;startQpcTicks=1000;endQpcTicks=3000}
$srRows=@(foreach($i in 0..3){[pscustomobject]@{event='measurement_status';pid=123;qpc_ns=[long]$i*1000000000;sr_runtime_connected=$true;sr_enabled=$true;sr_schema=3;sr_recording_scope='application_command_recorded_not_gpu_complete';sr_output_mode='xess_replacement';sr_recorded_frames=$i*60;sr_xess_recordings=$i*60;sr_transition_recordings=0;sr_unavailable_recordings=0;sr_degraded_since_qpc=0;sr_engine_native_updates=0;sr_core_dispatch_forwarded=0;sr_output_held=$false;sr_failed_submit_batches=0}})
$qualified=Get-RecorderSrQualification $srRows $srWindow
Check ($qualified.recordingCoverageEligible -and -not $qualified.eligible -and $qualified.xessRecordings -eq 120 -and $qualified.coverageSeconds -eq 2 -and $qualified.state -ceq 'recording_only_unverified') 'Complete recording evidence must remain explicitly unverified for pure XeSS power.'
foreach($field in @('sr_schema','sr_recording_scope','sr_output_mode','sr_degraded_since_qpc','sr_engine_native_updates','sr_core_dispatch_forwarded','sr_output_held','sr_xess_recordings','sr_failed_submit_batches')){
    $badRows=@($srRows|ForEach-Object {$_|ConvertTo-Json -Compress|ConvertFrom-Json})
    switch($field){
        'sr_schema' {$badRows[2].sr_schema=99}
        'sr_recording_scope' {$badRows[2].sr_recording_scope='gpu_completed'}
        'sr_output_mode' {$badRows[2].sr_output_mode='engine_native_unverified'}
        'sr_degraded_since_qpc' {$badRows[2].sr_degraded_since_qpc=1}
        'sr_engine_native_updates' {$badRows[2].sr_engine_native_updates=1}
        'sr_core_dispatch_forwarded' {$badRows[2].sr_core_dispatch_forwarded=1}
        'sr_output_held' {$badRows[2].sr_output_held=$true}
        'sr_xess_recordings' {$badRows[2].sr_xess_recordings--}
        'sr_failed_submit_batches' {$badRows[2].sr_failed_submit_batches=1}
    }
    $bad=Get-RecorderSrQualification $badRows $srWindow
    Check (-not $bad.recordingCoverageEligible -and -not $bad.eligible) ('Recording coverage silently accepted: '+$field)
}
$missing=@($srRows|ForEach-Object {$_|ConvertTo-Json -Compress|ConvertFrom-Json});$missing[2].PSObject.Properties.Remove('sr_recorded_frames')
Check (-not (Get-RecorderSrQualification $missing $srWindow).recordingCoverageEligible) 'Missing counters were treated as zero.'
Check (-not (Get-RecorderSrQualification @($srRows[2],$srRows[3]) $srWindow).recordingCoverageEligible) 'No baseline before the window still qualified.'
Check (-not (Get-RecorderSrQualification @($srRows[0],$srRows[1],$srRows[2]) $srWindow).recordingCoverageEligible) 'Missing trailing coverage still qualified.'
$legacy=@($srRows|ForEach-Object {$_|ConvertTo-Json -Compress|ConvertFrom-Json});foreach($row in $legacy){$row.sr_schema=2}
$old=Get-RecorderSrQualification $legacy $srWindow
Check (-not $old.eligible -and -not $old.recordingCoverageEligible -and $old.reasons -contains 'unsupported_or_incomplete_schema') 'Legacy schema must never inherit recording or pure-output certification.'
$compact=ConvertTo-RecorderCompactStatus ($srRows[2]|ConvertTo-Json -Compress)
Check ($compact.Contains('sr_recording_scope') -and $compact.Contains('sr_xess_recordings') -and $compact.Contains('sr_degraded_since_qpc') -and $compact.Contains('sr_failed_submit_batches')) 'Compact retention dropped the recording-only evidence.'
