#requires -Version 7.0
#requires -PSEdition Core

function Initialize-RecorderSummaryNative {
    if('RecorderSummaryFrames' -as [type]){return}
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
public sealed class RecorderQualifiedRanges {
    readonly double[] begins,ends;readonly bool[] qualified;
    public RecorderQualifiedRanges(double[] begins,double[] ends,bool[] qualified){this.begins=begins;this.ends=ends;this.qualified=qualified;}
    public bool Contains(double begin,double end){
        if(end<=begin||begins.Length==0)return false;int lo=0,hi=begins.Length-1,index=-1;
        while(lo<=hi){int mid=lo+(hi-lo)/2;if(begins[mid]<=begin){index=mid;lo=mid+1;}else hi=mid-1;}
        if(index<0)return false;double cursor=begin;
        for(int i=index;i<begins.Length;++i){if(begins[i]>cursor||!qualified[i])return false;cursor=Math.Max(cursor,ends[i]);if(cursor>=end)return true;}return false;
    }
}
public static class RecorderSummaryFrames {
    public sealed class Bucket {public readonly List<double> Display=new List<double>(),Latency=new List<double>();public readonly Dictionary<string,int> Swapchains=new Dictionary<string,int>();}
    public sealed class Result {public Bucket[] Raw,Steady;public long InvalidRows;}
    static string[] Csv(string line){var result=new List<string>();var field=new StringBuilder();bool quoted=false;for(int i=0;i<line.Length;++i){char c=line[i];if(c=='"'){if(quoted&&i+1<line.Length&&line[i+1]=='"'){field.Append('"');++i;}else quoted=!quoted;}else if(c==','&&!quoted){result.Add(field.ToString());field.Length=0;}else field.Append(c);}if(quoted)throw new FormatException("Unclosed CSV quote.");result.Add(field.ToString());return result.ToArray();}
    static bool Number(string value,out double number){return Double.TryParse(value,NumberStyles.Float,CultureInfo.InvariantCulture,out number)&&!Double.IsNaN(number)&&!Double.IsInfinity(number);}
    static void Add(Bucket b,double interval,double latency,string chain){b.Display.Add(interval);b.Latency.Add(latency);int count;b.Swapchains.TryGetValue(chain,out count);b.Swapchains[chain]=count+1;}
    public static Result Collect(string path,long pid,double frequency,double[] partBegins,double[] partEnds,RecorderQualifiedRanges ranges){
        var r=new Result {Raw=new Bucket[partBegins.Length],Steady=new Bucket[partBegins.Length]};for(int i=0;i<r.Raw.Length;++i){r.Raw[i]=new Bucket();r.Steady[i]=new Bucket();}
        using(var reader=new StreamReader(path)){
            string header=reader.ReadLine();if(header==null)return r;string[] names=Csv(header);var columns=new Dictionary<string,int>(StringComparer.Ordinal);for(int i=0;i<names.Length;++i)columns[names[i]]=i;
            foreach(string name in new[]{"InitialSnapshot","Dropped","ProcessID","StreamEpoch","ProviderOrdinal","msBetweenDisplayChange","msUntilDisplayed","qpcTime","SwapChainAddress"})if(!columns.ContainsKey(name)){r.InvalidRows++;return r;}
            var seen=new HashSet<string>(StringComparer.Ordinal);string line;
            while((line=reader.ReadLine())!=null){if(String.IsNullOrWhiteSpace(line))continue;try{
                string[] fields=Csv(line);if(fields.Length!=names.Length){r.InvalidRows++;continue;}
                if(fields[columns["InitialSnapshot"]]!="0"||fields[columns["Dropped"]]!="0")continue;
                long target;if(!Int64.TryParse(fields[columns["ProcessID"]],NumberStyles.Integer,CultureInfo.InvariantCulture,out target)||(pid>0&&target!=pid))continue;
                if(!seen.Add(fields[columns["StreamEpoch"]]+"|"+fields[columns["ProviderOrdinal"]]))continue;
                double interval,latency,qpc;if(!Number(fields[columns["msBetweenDisplayChange"]],out interval)||interval<=0||!Number(fields[columns["msUntilDisplayed"]],out latency)||latency<0||!Number(fields[columns["qpcTime"]],out qpc)){r.InvalidRows++;continue;}
                double display=qpc+latency*frequency/1000,previous=display-interval*frequency/1000;
                if(previous<partBegins[0]||display>partEnds[0])continue;bool valid=ranges.Contains(previous,display);string chain=fields[columns["SwapChainAddress"]];
                for(int i=0;i<partBegins.Length;++i)if(previous>=partBegins[i]&&display<=partEnds[i]){Add(r.Raw[i],interval,latency,chain);if(valid)Add(r.Steady[i],interval,latency,chain);}
            }catch(FormatException){r.InvalidRows++;}}
        }return r;
    }
}
'@
}

function ConvertTo-RecorderCompactStatus([string]$Line) {
    try {
        $row=$Line|ConvertFrom-Json @recorderJsonOptions
        if($row.event -cne 'measurement_status'){return $null}
        $value=[ordered]@{event='measurement_status';compactSchema=1}
        foreach($name in @('pid','device','window','qpc_ns','writer_failures_total','attempt','engine_frame','sdk_id','sdk_query_result','sdk_framegen_result','sdk_frames','output_fresh','control_epoch','inflight_limit','async','copy_route','shared_copy_path','shared_color_direct','shared_auxiliary_direct','depth_snapshot_compute','shared_fallback_reason','input_captured','input_consumed','copy_failures','query_failures','query_unavailable','query_completed','submission_interval_resets',
            'status_begin_ns','status_end_ns','status_interval_state','status_interval_attempts','status_interval_steady_frames','status_interval_fg_off_frames','status_interval_invalid_frames','status_interval_transition_frames','status_interval_unknown_frames','status_interval_resets','status_interval_history_resets','status_reset_exclusion','status_interval_output_failures',
            'status_interval_sdk_warnings','status_interval_steady_sdk_warnings','status_interval_fallback_sdk_warnings','status_interval_unknown_sdk_warnings','status_interval_sdk_errors','status_interval_steady_sdk_errors',
            'menu_paired','menu_requested_enabled','menu_gates_consistent','input_matched','input_valid','input_validity','history_reset_known','history_reset_reasons','fg_enabled','reset','reset_count','steady_fg_input_valid_frames','input_matched_frames','input_invalid_frames','fg_off_frames','unknown_frames',
            'sr_guard_schema','sr_guard_mask_draws','sr_guard_mask_fallbacks','sr_guard_mask_refreshes','sr_guard_mask_configurations','sr_tail_diagnostics_enabled','game_frame_cap_30_to_40_requested','game_frame_cap_schema','game_frame_cap_requested','game_frame_cap_target_fps','game_frame_cap_invalid_config','game_frame_cap_installed','game_frame_cap_state','game_frame_cap_calls','game_frame_cap_remapped','game_frame_cap_last_requested','game_frame_cap_last_forwarded','game_frame_cap_readback','game_frame_cap_immediate_readback','game_frame_cap_readback_semantics','fg_settings_schema','fg_settings_available','fg_settings_policy','fg_threshold_requested_index','fg_settings_invalid','fg_threshold_attempted','fg_threshold_applied','fg_threshold_result','fg_threshold_requested','fg_debug_requested','fg_debug_attempted','fg_debug_applied','fg_debug_result','fg_totals_scope','fg_total_requested','fg_total_enabled','fg_total_multi_output','fg_total_single_output','fg_total_zero_output','fg_total_query_unavailable','fg_total_result_warnings','fg_total_result_errors','fg_total_history_resets','fg_multiplier_requested','fg_multiplier_actual','fg_multiplier_maximum','fg_multiplier_result','fg_multiplier_restart_required','fg_multiplier_change_policy','async_log_requested','async_log_active','async_log_start_failures','async_log_accepted','async_log_written','async_log_pending','async_log_sink_failures','async_log_max_write_ns','async_log_dropped_verbose','async_log_dropped_detail','async_log_dropped_summary','async_log_dropped_observation','sr_hot_path_schema','sr_guard_runtime_lock_contended','sr_guard_binding_lookup_scans','rp_facts_queries','rp_facts_lock_contended','rp_command_lookup_scans','sr_guard_command_checks','sr_guard_draw_checks','sr_guard_bind_checks','sr_guard_reference_queries','sr_guard_reference_batches','sr_guard_identity_queries','sr_guard_identity_batches','sr_guard_safe_cache_hits','sr_guard_explicit_omitted_lookups','sr_fault','sr_status_schema','sr_active_reason','sr_reason_category','sr_last_issue_reason','sr_last_issue_frame','sr_successful_recoveries','sr_last_recovery_frame','sr_currently_stopped','sr_recovery_blocked','sr_engine_native_request','sr_recording_scope','sr_recorded_frames','sr_xess_recordings','sr_transition_recordings','sr_unavailable_recordings','sr_submitted_batches','sr_failed_submit_batches','sr_gpu_completion_display_join_verified','sr_schema','sr_output_mode','sr_completed_frames','sr_xess_frames','sr_transition_frames','sr_unavailable_frames','sr_guard_suppressed_commands','sr_output_held','sr_degraded_since_frame','sr_degraded_since_qpc','sr_degraded_since_ns','sr_engine_native_updates','sr_engine_native_recoveries','sr_core_dispatch_forwarded','sr_native_resources_verified','sr_enabled','sr_runtime_connected','sr_state','sr_quality','sr_quality_name','sr_input_width','sr_input_height','sr_output_width','sr_output_height','sr_executes','sr_skips')) {
            if($row.PSObject.Properties.Name -ccontains $name){$value[$name]=$row.$name}
        }
        $value|ConvertTo-Json -Depth 6 -Compress
    }catch{return $null}
}

# Recording coverage is useful evidence, not a submit/completion/display join.
function Get-RecorderSrQualification([object[]]$Statuses,$Window) {
    $result=[ordered]@{schema=3;eligible=$false;recordingCoverageEligible=$false;proofScope='recording_only';state='unknown';reasons=@();recordedFrames=[long]0;xessRecordings=[long]0;coverageSeconds=0.0}
    $rows=@($Statuses|Where-Object {(-not $Window.gamePid -or $_.pid -eq $Window.gamePid) -and $_.sr_runtime_connected}|Sort-Object {[double]$_.qpc_ns} -Unique)
    if(-not $rows.Count){$result.state='not_observed';$result.reasons=@('No actual SR output counters; legacy/official recordings are not certified as pure XeSS.');return $result}
    $frequency=[double]$Window.qpcFrequency;$begin=[double]$Window.startQpcTicks*1e9/$frequency;$end=[double]$Window.endQpcTicks*1e9/$frequency
    if($end -le $begin){$result.reasons=@('empty_window');return $result}
    $required=@('sr_schema','sr_recording_scope','sr_failed_submit_batches','sr_output_mode','sr_recorded_frames','sr_xess_recordings','sr_transition_recordings','sr_unavailable_recordings','sr_degraded_since_qpc','sr_engine_native_updates','sr_core_dispatch_forwarded','sr_output_held')
    $cursor=$begin
    for($i=1;$i -lt $rows.Count;$i++){
        $a=$rows[$i-1];$b=$rows[$i];$left=[double]$a.qpc_ns;$right=[double]$b.qpc_ns
        if($right -le $begin -or $left -ge $end -or $right -le $left){continue}
        $missing=@(foreach($row in @($a,$b)){foreach($field in $required){if($row.PSObject.Properties.Name -cnotcontains $field -or $null -eq $row.$field){$field}}})
        if($missing.Count -or $a.sr_schema -ne 3 -or $b.sr_schema -ne 3){$result.reasons+='unsupported_or_incomplete_schema';continue}
        if($a.sr_recording_scope -cne 'application_command_recorded_not_gpu_complete' -or $b.sr_recording_scope -cne 'application_command_recorded_not_gpu_complete'){$result.reasons+='unsupported_recording_scope'}
        if([long]$a.sr_failed_submit_batches -ne 0 -or [long]$b.sr_failed_submit_batches -ne 0){$result.reasons+='submission_failed'}
        if($left -gt $cursor -or $right-$left -gt 2.5e9){$result.reasons+='counter_coverage_gap';continue}
        if($a.sr_output_mode -cne 'xess_replacement' -or $b.sr_output_mode -cne 'xess_replacement' -or $a.sr_output_held -or $b.sr_output_held){$result.reasons+='non_xess_output'}
        if([long]$a.sr_degraded_since_qpc -ne 0 -or [long]$b.sr_degraded_since_qpc -ne 0){$result.reasons+='session_degraded'}
        if([long]$a.sr_core_dispatch_forwarded -ne 0 -or [long]$b.sr_core_dispatch_forwarded -ne 0){$result.reasons+='fsr_core_forwarded'}
        if([long]$b.sr_engine_native_updates -ne [long]$a.sr_engine_native_updates){$result.reasons+='engine_native_update'}
        $recorded=[long]$b.sr_recorded_frames-[long]$a.sr_recorded_frames;$xess=[long]$b.sr_xess_recordings-[long]$a.sr_xess_recordings
        if($recorded -le 0 -or $recorded -ne $xess -or [long]$b.sr_transition_recordings -ne [long]$a.sr_transition_recordings -or [long]$b.sr_unavailable_recordings -ne [long]$a.sr_unavailable_recordings){$result.reasons+='incomplete_xess_frame_coverage'}
        $result.recordedFrames+=$recorded;$result.xessRecordings+=$xess
        $finish=[Math]::Min($end,$right);$result.coverageSeconds+=([Math]::Max(0,$finish-[Math]::Max($begin,$left)))/1e9;$cursor=$finish
    }
    if($cursor -lt $end){$result.reasons+='counter_coverage_incomplete'}
    $result.reasons=@($result.reasons|Select-Object -Unique);$result.recordingCoverageEligible=$result.reasons.Count -eq 0 -and $result.coverageSeconds -gt 0
    $result.state=$(if($result.recordingCoverageEligible){'recording_only_unverified'}else{'not_eligible'})
    $result.reasons+='submit_completion_display_join_unavailable'
    $result
}

function Get-RecorderMarkers([string]$Directory,$Window) {
    $result=[ordered]@{file='markers.jsonl';validCount=0;invalidRows=0;outsideWindowRows=0;partialTailRows=0;markers=@();keyMarkersAreStateEvidence=$false}
    $path=Join-Path $Directory $result.file
    if(-not [IO.File]::Exists($path)){return $result}
    $tail=[IO.File]::OpenRead($path);try{$hasCompleteTail=$tail.Length -eq 0;if($tail.Length -gt 0){$tail.Position=$tail.Length-1;$hasCompleteTail=$tail.ReadByte() -eq 10}}finally{$tail.Dispose()}
    $seen=[Collections.Generic.HashSet[long]]::new();$reader=[IO.StreamReader]::new($path)
    try {
        while($null -ne ($line=$reader.ReadLine())) {
            if($reader.EndOfStream -and -not $hasCompleteTail){$result.partialTailRows++;continue}
            if(-not $line.Trim()){continue}
            try {
                $row=$line|ConvertFrom-Json @recorderJsonOptions
                if($row.sequence -le 0 -or -not $seen.Add([long]$row.sequence) -or [long]$row.qpcFrequency -ne [long]$Window.qpcFrequency -or [long]$row.qpcTicks -le 0 -or $row.kind -cnotin @('manual','key','timer','screenshot')){throw 'Invalid marker identity or clock.'}
                [void][DateTimeOffset]::Parse($row.utc)
                if($row.kind -ceq 'key' -and $row.key -cnotin @('M','Esc')){throw 'Only M and Esc key markers are permitted.'}
                if([long]$row.qpcTicks -lt [long]$Window.startQpcTicks -or [long]$row.qpcTicks -gt [long]$Window.endQpcTicks){$result.outsideWindowRows++;continue}
                $result.markers+= $row
            }catch{$result.invalidRows++}
        }
    }finally{$reader.Dispose()}
    $result.markers=@($result.markers|Sort-Object {[long]$_.qpcTicks}, {[long]$_.sequence});$result.validCount=$result.markers.Count
    $result
}

function Get-RecorderJsonLines([string]$Directory,[string[]]$Patterns,[string]$Event) {
    $seen=[Collections.Generic.HashSet[string]]::new()
    foreach($pattern in $Patterns) {
        foreach($file in @(Get-ChildItem -LiteralPath $Directory -File -Filter $pattern|Sort-Object Name)) {
            foreach($line in [IO.File]::ReadLines($file.FullName)) {
                if(-not $line.Contains('"event":"'+$Event+'"') -and -not $line.Contains('"event": "'+$Event+'"')){continue}
                try {$row=$line|ConvertFrom-Json @recorderJsonOptions;if($row.event -cne $Event){continue};$identity=$(if($Event -ceq 'measurement_summary'){'{0}|{1}|{2}|{3}' -f $row.pid,$row.device,$row.begin_ns,$row.end_ns}else{'{0}|{1}|{2}' -f $row.pid,$row.device,$row.qpc_ns});if($seen.Add($identity)){$row}}catch{}
            }
        }
    }
}

function Get-RecorderStateIntervals([object[]]$Statuses,$Window) {
    $frequency=[double]$Window.qpcFrequency;$start=[double]$Window.startQpcTicks;$end=[double]$Window.endQpcTicks
    $intervals=[Collections.Generic.List[object]]::new();$cursor=$start
    foreach($row in @($Statuses|Sort-Object {[double]$_.qpc_ns})) {
        if($Window.gamePid -and $row.pid -ne $Window.gamePid){continue}
        $begin=[double]$row.status_begin_ns*$frequency/1e9;$finish=[double]$row.qpc_ns*$frequency/1e9
        if($begin -le 0 -or $finish -le $begin -or $finish -le $start -or $begin -ge $end){continue}
        $completeInterval=$row.status_reset_exclusion -ceq 'semantic_only_stall_remedies_retained' -and @(@('status_end_ns','status_interval_attempts','status_interval_steady_frames','status_interval_fg_off_frames','status_interval_invalid_frames','status_interval_transition_frames','status_interval_unknown_frames','status_interval_resets')|Where-Object {$row.PSObject.Properties.Name -cnotcontains $_}).Count -eq 0
        if($completeInterval){$classified=[long]$row.status_interval_steady_frames+[long]$row.status_interval_fg_off_frames+[long]$row.status_interval_invalid_frames+[long]$row.status_interval_transition_frames+[long]$row.status_interval_unknown_frames;$completeInterval=$classified -eq [long]$row.status_interval_attempts -and $classified -gt 0}
        # Fully counted native intervals retain long hitches; a missing sample alone cannot certify a long gap.
        $state=$(if(($finish-$begin -gt 2.5*$frequency -and -not $completeInterval) -or ($row.PSObject.Properties.Name -ccontains 'status_end_ns' -and [double]$row.status_end_ns -ne [double]$row.qpc_ns)){'unknown'}elseif($row.status_interval_state -ceq 'reset_transition'){'reset'}elseif($row.status_interval_state -cin @('steady_fg_input_valid','fg_off','input_invalid','map','menu','loading','reset')){[string]$row.status_interval_state}else{'unknown'})
        $begin=[Math]::Max($start,$begin);$finish=[Math]::Min($end,$finish)
        if($begin -lt $cursor){if($finish -le $cursor){continue};$begin=$cursor;$state='unknown'}
        if($begin -gt $cursor){$intervals.Add([ordered]@{begin=$cursor;end=$begin;state='unknown'})}
        # SDK warnings/errors remain in the eligible denominator; output success is not an admission gate.
        if($state -ceq 'steady_fg_input_valid' -and ([long]$row.status_interval_attempts -le 0 -or [long]$row.status_interval_steady_frames -ne [long]$row.status_interval_attempts -or [long]$row.status_interval_resets -gt 0)){$state='unknown'}
        $intervals.Add([ordered]@{begin=$begin;end=$finish;state=$state});$cursor=$finish
    }
    if($cursor -lt $end){$intervals.Add([ordered]@{begin=$cursor;end=$end;state='unknown'})}
    @($intervals.ToArray())
}

function Test-RecorderQualifiedRange([double]$Begin,[double]$End,[object[]]$Intervals) {
    if($script:recorderQualifiedRanges){return $script:recorderQualifiedRanges.Contains($Begin,$End)}
    if($End -le $Begin){return $false}
    $cursor=$Begin
    foreach($interval in $Intervals) {
        if($interval.end -le $cursor){continue}
        if($interval.begin -gt $cursor -or $interval.state -cne 'steady_fg_input_valid'){return $false}
        $cursor=[Math]::Max($cursor,[double]$interval.end);if($cursor -ge $End){return $true}
    }
    $false
}

function New-RecorderMetricBucket {
    [ordered]@{display=[Collections.Generic.List[double]]::new();latency=[Collections.Generic.List[double]]::new();sensors=@{};swapchains=@{}}
}

function ConvertTo-RecorderNumber($Value) {
    $number=0.0
    if($null -ne $Value -and [double]::TryParse([string]$Value,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$number) -and -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)){return $number}
    $null
}

function Get-RecorderDistribution($Values,[double]$ExposureSeconds) {
    $items=[double[]]$Values.ToArray();[Array]::Sort($items)
    if($items.Length -eq 0){return [ordered]@{count=0;available=$false;exposureSeconds=$ExposureSeconds;p50=$null;p90=$null;p95=$null;p99=$null;maximum=$null;over25ms=$null;over40ms=$null;over100ms=$null;over25PerMinute=$null;over40PerMinute=$null;over100PerMinute=$null}}
    $result=[ordered]@{count=$items.Length;available=$true;exposureSeconds=$ExposureSeconds;p50=$items[[Math]::Max(0,[int][Math]::Ceiling(.50*$items.Length)-1)];p90=$items[[Math]::Max(0,[int][Math]::Ceiling(.90*$items.Length)-1)];p95=$items[[Math]::Max(0,[int][Math]::Ceiling(.95*$items.Length)-1)];p99=$items[[Math]::Max(0,[int][Math]::Ceiling(.99*$items.Length)-1)];maximum=$items[-1];over25ms=0;over40ms=0;over100ms=0;over25PerMinute=$null;over40PerMinute=$null;over100PerMinute=$null}
    foreach($item in $items){if($item -gt 25){$result.over25ms++};if($item -gt 40){$result.over40ms++};if($item -gt 100){$result.over100ms++}}
    if($ExposureSeconds -gt 0){$result.over25PerMinute=$result.over25ms*60/$ExposureSeconds;$result.over40PerMinute=$result.over40ms*60/$ExposureSeconds;$result.over100PerMinute=$result.over100ms*60/$ExposureSeconds}
    $result
}

function Get-RecorderBucketSummary($Bucket,[double]$ExposureSeconds) {
    $sensors=[ordered]@{}
    foreach($key in @($Bucket.sensors.Keys|Sort-Object)){$sensor=$Bucket.sensors[$key];$sensors[$key]=[ordered]@{mean=$sensor.sum/$sensor.count;samples=$sensor.count}}
    [ordered]@{displayIntervalMs=(Get-RecorderDistribution $Bucket.display $ExposureSeconds);legacyMsUntilDisplayed=(Get-RecorderDistribution $Bucket.latency $ExposureSeconds);hardwareSampleMean=$sensors;swapchainRows=$Bucket.swapchains}
}

function Add-RecorderMeasurementTotals($Totals,$Row) {
    $Totals.windows++;$Totals.durationSeconds+=([double]$Row.end_ns-[double]$Row.begin_ns)/1e9
    foreach($counter in $Row.counters.PSObject.Properties){if(-not $Totals.counters.Contains($counter.Name)){$Totals.counters[$counter.Name]=[long]0};$Totals.counters[$counter.Name]+=[long]$counter.Value}
    foreach($role in $Row.copy_commands.PSObject.Properties){if($null -eq $role.Value){continue};foreach($command in $role.Value.PSObject.Properties){$name=$role.Name+'.'+$command.Name;if(-not $Totals.copyCommands.Contains($name)){$Totals.copyCommands[$name]=[long]0};$Totals.copyCommands[$name]+=[long]$command.Value}}
    foreach($name in @('cap_wait','copy_fence_wait','consumed_wait','dxgi_present','present_call')){$metric=$Row.metrics.$name;if($null -eq $metric){continue};if(-not $Totals.waits.Contains($name)){$Totals.waits[$name]=[ordered]@{count=[long]0;totalMs=0.0}};$Totals.waits[$name].count+=[long]$metric.count;$Totals.waits[$name].totalMs+=[double]$metric.sum/1e6}
}

function New-RecorderMeasurementTotals {[ordered]@{windows=0;durationSeconds=0.0;counters=[ordered]@{};copyCommands=[ordered]@{};waits=[ordered]@{}}}
function New-RecorderSdkTotals {[ordered]@{available=$false;wholeIntervals=0;missingCounterIntervals=0;boundaryIntervalsNotProrated=0;allWarnings=[long]0;steadyWarnings=[long]0;fallbackWarnings=[long]0;unknownWarnings=[long]0;allErrors=[long]0;steadyErrors=[long]0;outputFailures=$null;outputFailureIntervals=0}}

function Format-RecorderSummaryNumber($Value) {if($null -eq $Value){return 'unknown'};([double]$Value).ToString('0.###',[Globalization.CultureInfo]::InvariantCulture)}

function Get-RecorderRetainedPowerStateSummary([string]$Directory) {
    $result=[ordered]@{samples=0;unavailableSamples=0;invalidRows=0;groups=@();errors=@()};$groups=[ordered]@{};$path=Join-Path $Directory 'power-state.jsonl'
    if(-not [IO.File]::Exists($path)){return $result}
    foreach($line in [IO.File]::ReadLines($path)){try{$row=$line|ConvertFrom-Json @recorderJsonOptions;$result.samples++;if($row.available -ne $true){$result.unavailableSamples++};$identity=@($row.acSource,$row.powerPlanGuid,$row.effectiveOverlayGuid) -join '|';if(-not $groups.Contains($identity)){$groups[$identity]=[ordered]@{samples=0;acSource=$row.acSource;powerPlanGuid=$row.powerPlanGuid;powerPlanName=$row.powerPlanName;effectiveOverlayGuid=$row.effectiveOverlayGuid;batteryPercentMinimum=$null;batteryPercentMaximum=$null}};$group=$groups[$identity];$group.samples++;if($null -ne $row.batteryPercent){if($null -eq $group.batteryPercentMinimum -or $row.batteryPercent -lt $group.batteryPercentMinimum){$group.batteryPercentMinimum=$row.batteryPercent};if($null -eq $group.batteryPercentMaximum -or $row.batteryPercent -gt $group.batteryPercentMaximum){$group.batteryPercentMaximum=$row.batteryPercent}};$result.errors+=@($row.errors)}catch{$result.invalidRows++}}
    $result.groups=@($groups.Values);$result.errors=@($result.errors|Where-Object {$_}|Select-Object -Unique);$result
}

function Write-RecorderReadableSummary([string]$Directory,$Window,$Summary,$Context,[string]$SourceDirectory=$Directory) {
    Initialize-RecorderSummaryNative
    $markers=Get-RecorderMarkers $SourceDirectory $Window
    $markerIntegrity=[ordered]@{acknowledged=$false;accepted=$null;written=$null;dropped=$null;unwritten=$null;error=$null;inputError=$null;warnings=@()}
    $closedPath=Join-Path $SourceDirectory 'markers.closed.json'
    if([IO.File]::Exists($closedPath)){try{$closed=[IO.File]::ReadAllText($closedPath)|ConvertFrom-Json @recorderJsonOptions;$markerIntegrity.acknowledged=$closed.closed -eq $true;foreach($key in @('accepted','written','dropped','unwritten','error','inputError')){$markerIntegrity[$key]=$closed.$key}}catch{$markerIntegrity.error='Invalid markers.closed.json: '+$_.Exception.Message}}
    if($markerIntegrity.dropped -gt 0 -or $markerIntegrity.unwritten -gt 0 -or $markerIntegrity.error -or $markerIntegrity.inputError){$markerIntegrity.warnings+='Marker timing coverage is incomplete: dropped/unwritten markers, a writer error or input registration/cleanup error were reported.'}
    if($null -ne $markerIntegrity.written -and [long]$markerIntegrity.written -ne $markers.validCount+$markers.outsideWindowRows+$markers.invalidRows+$markers.partialTailRows){$markerIntegrity.warnings+='Marker writer row count differs from the retained marker file; inspect marker evidence before interpreting segment boundaries.'}
    if($markers.invalidRows -gt 0 -or $markers.partialTailRows -gt 0){$markerIntegrity.warnings+='Invalid or incomplete marker rows were retained and omitted from the valid marker count.'}
    if(($markers.validCount -gt 0 -or $Summary.markerClose.required) -and -not $markerIntegrity.acknowledged){$markerIntegrity.warnings+='Marker writer close acknowledgement is absent; unsaved markers are unknown.'}
    $powerContext=Get-RecorderRetainedPowerStateSummary $SourceDirectory
    $configuration=@(foreach($phase in @('start','stop')){$path=Join-Path $SourceDirectory ('configuration-'+$phase+'.json');if([IO.File]::Exists($path)){try{$snapshot=[IO.File]::ReadAllText($path)|ConvertFrom-Json @recorderJsonOptions;[ordered]@{phase=$phase;installation=$snapshot.installation;diagnosticFiles=$snapshot.diagnosticFiles;launcherProcesses=$snapshot.launcherProcesses;launcherQueryAvailable=$snapshot.launcherQueryAvailable;launcherQueryError=$snapshot.launcherQueryError;effectiveMeasurement=$snapshot.effectiveMeasurement}}catch{[ordered]@{phase=$phase;error=$_.Exception.Message}}}})
    $statuses=@(Get-RecorderJsonLines $SourceDirectory @('measurement-status-*.compact.jsonl','measurements-?.stream.jsonl','measurements-?.snapshot.jsonl') 'measurement_status')
    $intervals=@(Get-RecorderStateIntervals $statuses $Window)
    $frequency=[double]$Window.qpcFrequency;$start=[double]$Window.startQpcTicks;$end=[double]$Window.endQpcTicks
    if($start -le 0){$start=$end;$intervals=@()}
    $script:recorderQualifiedRanges=[RecorderQualifiedRanges]::new([double[]]@($intervals|ForEach-Object {$_.begin}),[double[]]@($intervals|ForEach-Object {$_.end}),[bool[]]@($intervals|ForEach-Object {$_.state -ceq 'steady_fg_input_valid'}))
    $duration=[Math]::Max(0.0,($end-$start)/$frequency)
    $parts=[Collections.Generic.List[object]]::new();$all=[ordered]@{label='全程';begin=$start;end=$end;raw=(New-RecorderMetricBucket);steady=(New-RecorderMetricBucket);rawMeasurement=(New-RecorderMeasurementTotals);steadyMeasurement=(New-RecorderMeasurementTotals);sdk=(New-RecorderSdkTotals);unqualifiedMeasurementWindows=0;markerBoundaryMeasurementWindows=0};$parts.Add($all)
    $cursor=$start;$name='开始 → 首个标记';$index=0
    foreach($marker in $markers.markers){$boundary=[double]$marker.qpcTicks;if($boundary -gt $cursor){$parts.Add([ordered]@{label=('区段 '+$index+': '+$name);begin=$cursor;end=$boundary;raw=(New-RecorderMetricBucket);steady=(New-RecorderMetricBucket);rawMeasurement=(New-RecorderMeasurementTotals);steadyMeasurement=(New-RecorderMeasurementTotals);sdk=(New-RecorderSdkTotals);unqualifiedMeasurementWindows=0;markerBoundaryMeasurementWindows=0})};$cursor=$boundary;$index++;$name=('#'+$marker.sequence+' '+$marker.kind+$(if($marker.key){' '+$marker.key}else{''})+' → 下个标记/结束')}
    if($cursor -lt $end -and $markers.validCount -gt 0){$parts.Add([ordered]@{label=('区段 '+$index+': '+$name);begin=$cursor;end=$end;raw=(New-RecorderMetricBucket);steady=(New-RecorderMetricBucket);rawMeasurement=(New-RecorderMeasurementTotals);steadyMeasurement=(New-RecorderMeasurementTotals);sdk=(New-RecorderSdkTotals);unqualifiedMeasurementWindows=0;markerBoundaryMeasurementWindows=0})}
    $sdkFields=[ordered]@{allWarnings='status_interval_sdk_warnings';steadyWarnings='status_interval_steady_sdk_warnings';fallbackWarnings='status_interval_fallback_sdk_warnings';unknownWarnings='status_interval_unknown_sdk_warnings';allErrors='status_interval_sdk_errors';steadyErrors='status_interval_steady_sdk_errors'}
    foreach($row in $statuses){if($Window.gamePid -and $row.pid -ne $Window.gamePid){continue};$begin=[double]$row.status_begin_ns*$frequency/1e9;$finish=[double]$row.qpc_ns*$frequency/1e9;if($begin -le 0 -or $finish -le $begin){continue};foreach($part in $parts){if($finish -le $part.begin -or $begin -ge $part.end){continue};if($begin -lt $part.begin -or $finish -gt $part.end){$part.sdk.boundaryIntervalsNotProrated++;continue};$part.sdk.wholeIntervals++;if($row.PSObject.Properties.Name -ccontains 'status_interval_output_failures'){if($null -eq $part.sdk.outputFailures){$part.sdk.outputFailures=[long]0};$part.sdk.outputFailures+=[long]$row.status_interval_output_failures;$part.sdk.outputFailureIntervals++};if(@($sdkFields.Values|Where-Object {$row.PSObject.Properties.Name -cnotcontains $_}).Count){$part.sdk.missingCounterIntervals++;continue};$part.sdk.available=$true;foreach($key in $sdkFields.Keys){$part.sdk[$key]+=[long]$row.($sdkFields[$key])}}}
    $frameFile=Join-Path $SourceDirectory 'frames.csv';$frameErrors=0
    if([IO.File]::Exists($frameFile)) {
        $frames=[RecorderSummaryFrames]::Collect($frameFile,[long]$Window.gamePid,$frequency,[double[]]@($parts|ForEach-Object {$_.begin}),[double[]]@($parts|ForEach-Object {$_.end}),$script:recorderQualifiedRanges)
        $frameErrors=$frames.InvalidRows
        for($i=0;$i -lt $parts.Count;$i++){foreach($kind in @('raw','steady')){$bucket=$(if($kind -ceq 'raw'){$frames.Raw[$i]}else{$frames.Steady[$i]});$parts[$i][$kind].display=$bucket.Display;$parts[$i][$kind].latency=$bucket.Latency;$parts[$i][$kind].swapchains=$bucket.Swapchains}}
    }
    $powerFile=Join-Path $SourceDirectory 'power.jsonl'
    if([IO.File]::Exists($powerFile)) {
        foreach($line in [IO.File]::ReadLines($powerFile)) {
            try {$row=$line|ConvertFrom-Json @recorderJsonOptions;if($row.error -or [double]$row.qpcTicks -lt $start -or [double]$row.qpcTicks -gt $end){continue};$point=[double]$row.qpcTicks;$qualified=$(if($point -eq $end){Test-RecorderQualifiedRange ($point-1) $point $intervals}else{Test-RecorderQualifiedRange $point ($point+1) $intervals})
                foreach($part in $parts){if($point -lt $part.begin -or $point -gt $part.end -or ($point -eq $part.end -and $point -ne $end)){continue};foreach($kind in @('raw','steady')){if($kind -ceq 'steady' -and -not $qualified){continue};foreach($reading in $row.readings){if($reading.finiteValid -ne $true -or $reading.name -cnotin @('CPU cores power','CPU GT power','CPU package power','CPU temperature','GPU temperature')){continue};$value=ConvertTo-RecorderNumber $reading.value;if($null -eq $value){continue};$sensors=$part[$kind].sensors;if(-not $sensors.ContainsKey($reading.name)){$sensors[$reading.name]=[ordered]@{sum=0.0;count=0}};$sensors[$reading.name].sum+=$value;$sensors[$reading.name].count++}}}
            }catch{}
        }
    }
    $measurementRows=@(Get-RecorderJsonLines $SourceDirectory @('measurements-summary-?.stream.jsonl','measurements-summary-?.snapshot.jsonl') 'measurement_summary')
    foreach($row in $measurementRows) {
        if($Window.gamePid -and $row.pid -ne $Window.gamePid){continue}
        $begin=[double]$row.begin_ns*$frequency/1e9;$finish=[double]$row.end_ns*$frequency/1e9
        if($finish -le $begin -or $finish -le $start -or $begin -ge $end){continue}
        foreach($part in $parts) {
            if($finish -le $part.begin -or $begin -ge $part.end){continue}
            if($begin -lt $part.begin -or $finish -gt $part.end){$part.markerBoundaryMeasurementWindows++;continue}
            Add-RecorderMeasurementTotals $part.rawMeasurement $row
            if(Test-RecorderQualifiedRange $begin $finish $intervals){Add-RecorderMeasurementTotals $part.steadyMeasurement $row}else{$part.unqualifiedMeasurementWindows++}
        }
    }
    $segments=@(foreach($part in $parts) {
        $states=[ordered]@{steady_fg_input_valid=0.0;fg_off=0.0;input_invalid=0.0;map=0.0;menu=0.0;loading=0.0;reset=0.0;unknown=0.0}
        foreach($interval in $intervals){$overlap=[Math]::Max(0.0,[Math]::Min([double]$part.end,[double]$interval.end)-[Math]::Max([double]$part.begin,[double]$interval.begin));$states[$interval.state]+=$overlap/$frequency}
        $seconds=($part.end-$part.begin)/$frequency
        if(-not $part.sdk.available){foreach($key in $sdkFields.Keys){$part.sdk[$key]=$null}}
        [ordered]@{label=$part.label;beginQpcTicks=$part.begin;endQpcTicks=$part.end;durationSeconds=$seconds;stateDurationSeconds=$states;raw=(Get-RecorderBucketSummary $part.raw $seconds);steady=(Get-RecorderBucketSummary $part.steady $states.steady_fg_input_valid);sdkObservations=$part.sdk;rawMeasurement=$part.rawMeasurement;steadyMeasurement=$part.steadyMeasurement;unqualifiedMeasurementWindows=$part.unqualifiedMeasurementWindows;boundaryMeasurementWindowsNotProrated=$part.markerBoundaryMeasurementWindows}
    })
    $data=[ordered]@{schema=1;label=$Context.label;labelCode=$Context.labelCode;notes=$Context.notes;xiaomiPerformanceMode=$Context.xiaomiPerformanceMode;xiaomiPerformanceModeSource='user-selected';measurementMode=$Context.measurementMode;detailIncluded=$Context.detailIncluded;markers=$markers;markerIntegrity=$markerIntegrity;powerState=$powerContext;configuration=$configuration;segments=$segments;frameSource='PMDP legacy event-time fields';invalidFrameRows=$frameErrors;steadyVerified=($segments.Count -gt 0 -and $segments[0].stateDurationSeconds.steady_fg_input_valid -gt 0);stateEvidence='Whole measurement_status intervals only; no state inferred from M/Esc key markers. Map/menu/loading labels require explicit source evidence; unavailable classifications remain unknown.';counterPolicy='Only complete measurement summary windows wholly contained in the selection and qualified intervals are summed. Boundary and mixed windows are never prorated.';latencyMeaning='legacy msUntilDisplayed is present-to-display timing, not input latency.';hardwareMeanPolicy='Arithmetic mean of finite valid sampled sensor readings within the selected state and QPC segment.';sizeBudget=$null}
    $data['pureXeSSQualification']=Get-RecorderSrQualification $statuses $Window
    $data['pureXeSSPowerEligible']=$data.pureXeSSQualification.eligible
    $data['copyCommandScope']='Application-injected commands in successful submissions; SDK-internal copies are unavailable.'
    $data['sdkInternalCopyCountAvailable']=$false
    $total=[long]0;foreach($file in @(Get-ChildItem -LiteralPath $SourceDirectory -File -Recurse|Where-Object {(Test-RecorderArtifact $_.Name) -and $_.Name -cnotin @('manifest.json','summary.json','summary.analysis.json','summary.md')})){$total+=$file.Length}
    $data.sizeBudget=[ordered]@{retainedDataBytes=$total;retainedDataMiB=$total/1MB;generatedMetadataAllowanceBytes=1MB;estimatedRetainedPayloadUpperBoundBytes=$total+1MB;target30MinuteBytes=100000000;zipCountedSeparately=$false;basis='Uncompressed retained data files, once each; summary.json, summary.analysis.json, summary.md and manifest.json have a separate 1 MiB allowance; ZIP is not counted twice.';projected30MinuteBytes=$(if($duration -gt 0){$total*1800/$duration+1MB}else{$null});extraRawSelected=($Summary.selectedCollectors.presentmonRaw -or $Summary.selectedCollectors.logs -or $Context.detailIncluded);note='Projection includes fixed context overhead; the actual 30-minute payload is the acceptance measure.'}
    Write-RecorderJson (Join-Path $Directory 'summary.analysis.json') $data
    $md=[Collections.Generic.List[string]]::new();$md.Add('# 录制摘要');$md.Add('');$md.Add('标签：'+$Context.label+' ('+$Context.labelCode+')；备注：'+(($Context.notes -replace '[\r\n]+',' ') -replace '\|','/'));$md.Add('小米性能模式：'+$Context.xiaomiPerformanceMode+'（用户自选）；测量明细：'+$Context.detailIncluded+'；标记：'+$markers.validCount+'。');$md.Add('');$md.Add('稳态仅采用完整测量区间证据。M/Esc 只表示按键时刻；不能区分地图、菜单或加载的时间归入 unknown。legacy msUntilDisplayed 是 present 到上屏时间，不是输入延迟。');$md.Add('');$md.Add('| 区段 / 统计范围 | 秒 | 上屏 p50 / p90 / p99 / max (ms) | >25 / >40 / >100 次数 | 对应每分钟发生率 | legacy msUntilDisplayed p50 / p95 (ms) |');$md.Add('|---|---:|---|---|---|---|')
    $md.Add('纯 XeSS 功耗对照有效：'+$data.pureXeSSPowerEligible+'；状态：'+$data.pureXeSSQualification.state+'；原因：'+($data.pureXeSSQualification.reasons -join ', ')+'。原始帧率与功耗仍保留，不能将无效样本解释为纯 XeSS。')
    if($markerIntegrity.warnings.Count){$md.Insert(5,'标记完整性警告：'+($markerIntegrity.warnings -join ' ')+' accepted='+(Format-RecorderSummaryNumber $markerIntegrity.accepted)+', written='+(Format-RecorderSummaryNumber $markerIntegrity.written)+', dropped='+(Format-RecorderSummaryNumber $markerIntegrity.dropped)+', unwritten='+(Format-RecorderSummaryNumber $markerIntegrity.unwritten)+', error='+$markerIntegrity.error+', inputError='+$markerIntegrity.inputError+'。')}
    foreach($part in $segments) {
        foreach($kind in @('steady','raw')){$bucket=$part[$kind];$display=$bucket.displayIntervalMs;$latency=$bucket.legacyMsUntilDisplayed;$label=$part.label+' / '+$(if($kind -ceq 'steady'){'稳态 FG ON 输入有效'}else{'全程原始（未筛选）'});$md.Add('| '+$label+' | '+(Format-RecorderSummaryNumber $display.exposureSeconds)+' | '+((@($display.p50,$display.p90,$display.p99,$display.maximum)|ForEach-Object {Format-RecorderSummaryNumber $_}) -join ' / ')+' | '+((@($display.over25ms,$display.over40ms,$display.over100ms)|ForEach-Object {Format-RecorderSummaryNumber $_}) -join ' / ')+' | '+((@($display.over25PerMinute,$display.over40PerMinute,$display.over100PerMinute)|ForEach-Object {Format-RecorderSummaryNumber $_}) -join ' / ')+' | '+(Format-RecorderSummaryNumber $latency.p50)+' / '+(Format-RecorderSummaryNumber $latency.p95)+' |')}
    }
    foreach($part in $segments) {
        $md.Add('');$md.Add($part.label+' 剔除/未知秒数：'+(($part.stateDurationSeconds.GetEnumerator()|Where-Object Key -cne 'steady_fg_input_valid'|ForEach-Object {$_.Key+'='+(Format-RecorderSummaryNumber $_.Value)}) -join '，')+'。');$md.Add('功耗/温度均值（稳态；括号内样本数）：'+$(if($part.steady.hardwareSampleMean.Count){($part.steady.hardwareSampleMean.GetEnumerator()|ForEach-Object {$_.Key+' '+(Format-RecorderSummaryNumber $_.Value.mean)+' ('+$_.Value.samples+')'}) -join '；'}else{'unknown'})+'。');$md.Add('等待与复制：稳态完整合格窗口 '+$part.steadyMeasurement.windows+'，原始完整窗口 '+$part.rawMeasurement.windows+'，混合/状态不明窗口 '+$part.unqualifiedMeasurementWindows+'，跨边界窗口 '+$part.boundaryMeasurementWindowsNotProrated+'；后两类不分摊计数。')
        $md.Add('稳态等待 count/总 ms：'+$(if($part.steadyMeasurement.waits.Count){($part.steadyMeasurement.waits.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value.count+'/'+(Format-RecorderSummaryNumber $_.Value.totalMs)}) -join '；'}else{'unknown'})+'。');$md.Add('稳态复制计数：'+$(if($part.steadyMeasurement.copyCommands.Count){($part.steadyMeasurement.copyCommands.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value}) -join '；'}else{'unknown'})+'。');$md.Add('稳态错误/回退等计数：'+$(if($part.steadyMeasurement.counters.Count){($part.steadyMeasurement.counters.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value}) -join '；'}else{'unknown'})+'。');$md.Add('')
        $md.Add('SDK 状态区间计数（不以返回码或输出帧数筛掉失败样本）：'+$(if($part.sdkObservations.available){'warnings 总/稳态/回退/unknown='+$part.sdkObservations.allWarnings+'/'+$part.sdkObservations.steadyWarnings+'/'+$part.sdkObservations.fallbackWarnings+'/'+$part.sdkObservations.unknownWarnings+'；负 errors 总/稳态='+$part.sdkObservations.allErrors+'/'+$part.sdkObservations.steadyErrors}else{'unknown'})+'；outputFailures='+(Format-RecorderSummaryNumber $part.sdkObservations.outputFailures)+'（'+$part.sdkObservations.outputFailureIntervals+' 个完整状态区间）；跨边界不分摊区间 '+$part.sdkObservations.boundaryIntervalsNotProrated+'，缺少分类计数区间 '+$part.sdkObservations.missingCounterIntervals+'。');$md.Add('')
        $md.Add('原始功耗/温度均值（未筛选）：'+$(if($part.raw.hardwareSampleMean.Count){($part.raw.hardwareSampleMean.GetEnumerator()|ForEach-Object {$_.Key+' '+(Format-RecorderSummaryNumber $_.Value.mean)+' ('+$_.Value.samples+')'}) -join '；'}else{'unknown'})+'。');$md.Add('原始完整窗口等待 count/总 ms（未筛选）：'+$(if($part.rawMeasurement.waits.Count){($part.rawMeasurement.waits.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value.count+'/'+(Format-RecorderSummaryNumber $_.Value.totalMs)}) -join '；'}else{'unknown'})+'。');$md.Add('原始完整窗口复制计数（未筛选）：'+$(if($part.rawMeasurement.copyCommands.Count){($part.rawMeasurement.copyCommands.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value}) -join '；'}else{'unknown'})+'。');$md.Add('')
    }
    if(-not $data.steadyVerified){$md.Add('本次没有可证明的稳态 FG ON 有效输入窗口；官方/无图层采集只展示被动原始数据，不能声称已验证稳态。');$md.Add('')}
    $md.Add('地图/菜单/加载的 0 秒仅表示没有被测量证据识别，不能证明没有发生；状态不足的时间已合并到 unknown。');$md.Add('')
    $md.Add('复制计数仅包括应用注入的复制命令；SDK 内部复制次数不可用。')
    $md.Add('reset 策略 semantic_only_stall_remedies_retained：仅语义 reset 排除稳态；LongFrame / Followup 长帧补救 reset 保留在稳态样本中，避免过滤卡顿后产生假稳定。全部 history reset 与语义 reset 分别留在 compact 状态；带 reset 原因的 SDK warning 仍计入 fallbackWarnings。')
    $md.Add('电源状态：'+$powerContext.samples+' 样本，不可用 '+$powerContext.unavailableSamples+'；各组 '+$(if($powerContext.groups.Count){($powerContext.groups|ForEach-Object {($_|ConvertTo-Json -Depth 4 -Compress)}) -join '；'}else{'unknown'})+'。')
    if($powerContext.errors.Count){$md.Add('电源字段读取说明：'+($powerContext.errors -join '；')+'。')}
    foreach($snapshot in $configuration){$md.Add('配置 '+$snapshot.phase+'：installationId='+$(if($snapshot.installation.installationId){$snapshot.installation.installationId}else{'unknown'})+'；observer SHA256='+$snapshot.installation.observerDllSha256+'；receipt SHA256='+$snapshot.installation.receiptSha256+'；实测路线/SR='+($snapshot.effectiveMeasurement|ConvertTo-Json -Depth 5 -Compress)+'；启动器 CLI='+$(if($snapshot.launcherProcesses.Count){($snapshot.launcherProcesses|ForEach-Object {$_.commandLine}) -join ' | '}else{'unknown（无可读的匹配启动器进程）'})+'。')}
    $md.Add('体积口径：采集数据保留一次，共 '+(Format-RecorderSummaryNumber $data.sizeBudget.retainedDataMiB)+' MiB，另为摘要/清单预留 1 MiB；ZIP 不重复计入。30 分钟目标约 100 MB；选择 raw/detail/logs 可超过目标。');$md.Add('完整机器可读统计：summary.analysis.json；配置起止快照：configuration-start.json / configuration-stop.json；电源状态：power-state.jsonl。')
    [IO.File]::WriteAllLines((Join-Path $Directory 'summary.md'),$md,[Text.UTF8Encoding]::new($false))
    $data
}
