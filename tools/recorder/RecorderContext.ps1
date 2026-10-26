#requires -Version 7.0
#requires -PSEdition Core

function Protect-RecorderSession($Discovery) {
    if($Discovery.state -cne 'matched' -or $Discovery.match.sessionId -cnotmatch '^[0-9a-fA-F]{32}$'){return $false}
    try{
        $common=[Environment]::GetFolderPath('CommonApplicationData')
        $sessions=[IO.Path]::GetFullPath((Join-Path $common 'EndfieldXeSSObserver/Sessions'))
        if([IO.Path]::GetFullPath($Discovery.match.directory) -ine (Join-Path $sessions $Discovery.match.sessionId)){return $false}
        $pins=Join-Path $common 'EndfieldXeSSDiag/SessionPins'
        for($path=$pins;$path;$path=[IO.Path]::GetDirectoryName($path)){
            if((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){return $false}
        }
        [IO.Directory]::CreateDirectory($pins)|Out-Null
        $file=Join-Path $pins ($Discovery.match.sessionId+'.keep')
        if(-not [IO.File]::Exists($file)){$stream=[IO.File]::Open($file,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read);$stream.Dispose()}
        return $true
    }catch{return $false}
}

function Get-RecorderMissingSessionStatus([string]$State) {
    # Absence alone cannot distinguish zero-log independent play from official
    # play. Do not attach an old session or infer SR/FG state from installation.
    if($State -ceq 'absent'){
        return [ordered]@{expected=$true;message='不可用（诊断模式关闭／未找到匹配的诊断会话）；PMDP和硬件采集照常。'}
    }
    [ordered]@{expected=$false;message=('Observer session '+$State+'; no exact launch-time and PID match.')}
}
function Resolve-RecorderRequestContext($Request) {
    $value=[ordered]@{label='自由游玩';labelCode='free';notes='';xiaomiPerformanceMode='不清楚';xiaomiPerformanceModeSource='user-selected';measurementMode='full';capturePowerState=$true;detailIncluded=$true}
    foreach($name in @('label','labelCode','notes','xiaomiPerformanceMode','measurementMode')) {
        if($Request.PSObject.Properties.Name -ccontains $name) {
            if($Request.$name -isnot [string]){throw ('request.json '+$name+' must be a string.')}
            $value[$name]=[string]$Request.$name
        }
    }
    if($value.label.Length -gt 128 -or $value.notes.Length -gt 8192 -or $value.labelCode -cnotmatch '^[a-z0-9][a-z0-9-]{0,31}$'){throw 'Invalid recording label, labelCode or notes length.'}
    if($value.xiaomiPerformanceMode -cnotin @('狂暴','均衡','安静','智能','不清楚')){throw 'Unknown user-selected Xiaomi performance mode.'}
    if($value.measurementMode -cnotin @('summary','full')){throw 'measurementMode must be summary or full.'}
    if($Request.PSObject.Properties.Name -ccontains 'capturePowerState') {
        if($Request.capturePowerState -isnot [bool]){throw 'capturePowerState must be a JSON boolean.'}
        $value.capturePowerState=$Request.capturePowerState
    }
    if($Request.PSObject.Properties.Name -ccontains 'recordMenuKeys' -and $Request.recordMenuKeys -isnot [bool]){throw 'recordMenuKeys must be a JSON boolean.'}
    if($Request.PSObject.Properties.Name -ccontains 'markerProtocol') {
        if($Request.markerProtocol -isnot [pscustomobject] -or $Request.markerProtocol.schema -ne 1 -or $Request.markerProtocol.writer -cne 'gui' -or $Request.markerProtocol.closeBeforeStop -isnot [bool] -or $Request.markerProtocol.closeBeforeStop -ne $true){throw 'Unsupported GUI marker close protocol.'}
    }
    $value.detailIncluded=$value.measurementMode -ceq 'full'
    $value
}

function Get-RecorderDiagnosticFilesSnapshot([string]$Directory) {
    $result=[ordered]@{directory=$Directory;readOnly=$true;available=$false;files=@();error=$null}
    try {
        if(-not [IO.Directory]::Exists($Directory)){$result.error='directory_absent';return $result}
        $result.available=$true
        $result.files=@(Get-ChildItem -LiteralPath $Directory -File|Sort-Object Name|ForEach-Object {
            $item=[ordered]@{name=$_.Name;bytes=$_.Length;lastWriteUtc=$_.LastWriteTimeUtc.ToString('o');content=$null;contentRead=$false;error=$null}
            if($_.Length -le 64){try{$stream=[IO.File]::Open($_.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite);try{if($stream.Length -le 64){$buffer=[byte[]]::new(65);$count=$stream.Read($buffer,0,65);if($count -le 64 -and $stream.Length -le 64){$item.content=[Text.Encoding]::UTF8.GetString($buffer,0,$count).TrimStart([char]0xfeff);$item.contentRead=$true}else{$item.error='grew_beyond_64_bytes'}}else{$item.error='grew_beyond_64_bytes'}}finally{$stream.Dispose()}}catch{$item.error=$_.Exception.Message}}
            $item
        })
    }catch{$result.error=$_.Exception.Message}
    $result
}

function Get-RecorderInstallationSnapshot([string]$Root) {
    $receipt=Join-Path $Root 'install-receipt.json';$dll=Join-Path $Root 'xess-vulkan-observer.dll'
    $result=[ordered]@{installRoot=$Root;installationId='unknown';receiptPath=$receipt;receiptSha256=$null;observerDllPath=$dll;observerDllSha256=$null;readOnly=$true;errors=@()}
    if([IO.File]::Exists($receipt)){try{$result.receiptSha256=Get-RecorderSha256 $receipt;$data=[IO.File]::ReadAllText($receipt)|ConvertFrom-Json @recorderJsonOptions;if($data.installationId){$result.installationId=[string]$data.installationId}}catch{$result.errors+=('receipt: '+$_.Exception.Message)}}else{$result.errors+='receipt_absent'}
    if([IO.File]::Exists($dll)){try{$result.observerDllSha256=Get-RecorderSha256 $dll}catch{$result.errors+=('observer DLL: '+$_.Exception.Message)}}else{$result.errors+='observer_dll_absent'}
    $result
}

function Get-RecorderEffectiveMeasurement($Status) {
    $result=[ordered]@{available=($null -ne $Status);copy_route=$null;inflight_limit=$null;shared=$null;depth_snapshot_compute=$null;sr=$null;qpc_ns=$null}
    if($null -eq $Status){return $result}
    $result.qpc_ns=$Status.qpc_ns;$result.inflight_limit=$Status.inflight_limit;$result.shared=$Status.shared_copy_path;$result.depth_snapshot_compute=$Status.depth_snapshot_compute
    if($Status.copy_route){$result.copy_route=$Status.copy_route}elseif($Status.PSObject.Properties.Name -ccontains 'async'){$result.copy_route=$(if($Status.async){'async'}else{'sync'})}
    $sr=[ordered]@{};foreach($name in @('game_frame_cap_30_to_40_requested','game_frame_cap_schema','game_frame_cap_requested','game_frame_cap_target_fps','game_frame_cap_invalid_config','game_frame_cap_installed','game_frame_cap_state','game_frame_cap_calls','game_frame_cap_remapped','game_frame_cap_last_requested','game_frame_cap_last_forwarded','game_frame_cap_readback','game_frame_cap_immediate_readback','game_frame_cap_readback_semantics','fg_settings_schema','fg_settings_available','fg_settings_policy','fg_threshold_requested_index','fg_settings_invalid','fg_threshold_attempted','fg_threshold_applied','fg_threshold_result','fg_threshold_requested','fg_debug_requested','fg_debug_attempted','fg_debug_applied','fg_debug_result','fg_totals_scope','fg_total_requested','fg_total_enabled','fg_total_multi_output','fg_total_single_output','fg_total_zero_output','fg_total_query_unavailable','fg_total_result_warnings','fg_total_result_errors','fg_total_history_resets','sr_guard_schema','sr_guard_mask_draws','sr_guard_mask_fallbacks','sr_guard_mask_refreshes','sr_guard_mask_configurations','sr_tail_diagnostics_enabled','fg_multiplier_requested','fg_multiplier_actual','fg_multiplier_maximum','fg_multiplier_result','fg_multiplier_restart_required','fg_multiplier_change_policy','async_log_requested','async_log_active','async_log_start_failures','async_log_accepted','async_log_written','async_log_pending','async_log_sink_failures','async_log_max_write_ns','async_log_dropped_verbose','async_log_dropped_detail','async_log_dropped_summary','async_log_dropped_observation','sr_hot_path_schema','sr_guard_runtime_lock_contended','sr_guard_binding_lookup_scans','rp_facts_queries','rp_facts_lock_contended','rp_command_lookup_scans','sr_guard_command_checks','sr_guard_draw_checks','sr_guard_bind_checks','sr_guard_reference_queries','sr_guard_reference_batches','sr_guard_identity_queries','sr_guard_identity_batches','sr_guard_safe_cache_hits','sr_guard_explicit_omitted_lookups','sr_fault','sr_status_schema','sr_active_reason','sr_reason_category','sr_last_issue_reason','sr_last_issue_frame','sr_successful_recoveries','sr_last_recovery_frame','sr_currently_stopped','sr_recovery_blocked','sr_engine_native_request','sr_recording_scope','sr_recorded_frames','sr_xess_recordings','sr_transition_recordings','sr_unavailable_recordings','sr_submitted_batches','sr_failed_submit_batches','sr_gpu_completion_display_join_verified','sr_schema','sr_output_mode','sr_completed_frames','sr_xess_frames','sr_transition_frames','sr_unavailable_frames','sr_guard_suppressed_commands','sr_output_held','sr_degraded_since_frame','sr_degraded_since_qpc','sr_degraded_since_ns','sr_engine_native_updates','sr_engine_native_recoveries','sr_core_dispatch_forwarded','sr_native_resources_verified','sr_enabled','sr_runtime_connected','sr_state','sr_quality','sr_quality_name','sr_input_width','sr_input_height','sr_output_width','sr_output_height','sr_executes','sr_skips')){if($Status.PSObject.Properties.Name -ccontains $name){$sr[$name]=$Status.$name}}
    if($sr.Count){$result.sr=$sr}
    $result
}

function Get-RecorderConfigurationSnapshot([string]$Phase,[bool]$TestOnly,$Measurement) {
    $result=[ordered]@{schema=1;phase=$Phase;utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;readOnly=$true;testOnly=$TestOnly;diagnosticFiles=$null;installation=$null;launcherProcesses=@();launcherQueryAvailable=$false;launcherQueryError=$null;effectiveMeasurement=(Get-RecorderEffectiveMeasurement $Measurement)}
    if($TestOnly){$result.diagnosticFiles=[ordered]@{available=$false;error='test_only_no_live_read';files=@()};$result.installation=[ordered]@{installationId='unknown';error='test_only_no_live_read'};$result.launcherQueryError='test_only_no_live_read';return $result}
    $result.diagnosticFiles=Get-RecorderDiagnosticFilesSnapshot (Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'EndfieldXeSSDiag')
    $result.installation=Get-RecorderInstallationSnapshot (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'EndfieldXeSSObserver')
    try{$result.launcherProcesses=@(Get-CimInstance Win32_Process -Filter "Name='EndfieldXeSSLauncher.exe'"|ForEach-Object {[ordered]@{pid=$_.ProcessId;creationUtc=$_.CreationDate.ToUniversalTime().ToString('o');path=$_.ExecutablePath;commandLine=$_.CommandLine;parentPid=$_.ParentProcessId}});$result.launcherQueryAvailable=$true}catch{$result.launcherQueryError=$_.Exception.Message}
    $result
}

function Initialize-RecorderPowerStateNative {
    if('RecorderPowerState' -as [type]){return}
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class RecorderPowerState {
    [StructLayout(LayoutKind.Sequential)] struct Status {public byte ACLineStatus,BatteryFlag,BatteryLifePercent,SystemStatusFlag;public uint BatteryLifeTime,BatteryFullLifeTime;}
    public sealed class Result {public string AcSource,PlanGuid,PlanName,EffectiveOverlayGuid,StatusError,PlanError,OverlayError;public int? BatteryPercent;public byte? BatteryFlag;}
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetSystemPowerStatus(out Status status);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("powrprof.dll")] static extern uint PowerGetActiveScheme(IntPtr root,out IntPtr guid);
    [DllImport("powrprof.dll")] static extern uint PowerGetEffectiveOverlayScheme(out Guid guid);
    [DllImport("powrprof.dll")] static extern uint PowerReadFriendlyName(IntPtr root,ref Guid scheme,IntPtr subgroup,IntPtr setting,byte[] buffer,ref uint size);
    public static Result Read() {
        var r=new Result();Status status;
        if(GetSystemPowerStatus(out status)){r.AcSource=status.ACLineStatus==0?"battery":status.ACLineStatus==1?"ac":"unknown";r.BatteryFlag=status.BatteryFlag;if(status.BatteryLifePercent<=100)r.BatteryPercent=status.BatteryLifePercent;}else r.StatusError=new Win32Exception(Marshal.GetLastWin32Error()).Message;
        IntPtr ptr=IntPtr.Zero;
        try {uint code=PowerGetActiveScheme(IntPtr.Zero,out ptr);if(code!=0)throw new Win32Exception((int)code);Guid guid=(Guid)Marshal.PtrToStructure(ptr,typeof(Guid));r.PlanGuid=guid.ToString();uint size=0;code=PowerReadFriendlyName(IntPtr.Zero,ref guid,IntPtr.Zero,IntPtr.Zero,null,ref size);if(code!=0&&code!=234)throw new Win32Exception((int)code);if(size>0&&size<65536){byte[] bytes=new byte[size];code=PowerReadFriendlyName(IntPtr.Zero,ref guid,IntPtr.Zero,IntPtr.Zero,bytes,ref size);if(code!=0)throw new Win32Exception((int)code);r.PlanName=Encoding.Unicode.GetString(bytes).TrimEnd('\0');}}catch(Exception e){r.PlanError=e.Message;}finally{if(ptr!=IntPtr.Zero)LocalFree(ptr);}
        try {Guid guid;uint code=PowerGetEffectiveOverlayScheme(out guid);if(code!=0)throw new Win32Exception((int)code);r.EffectiveOverlayGuid=guid.ToString();}catch(Exception e){r.OverlayError=e.Message;}return r;
    }
}
'@
}

function Get-RecorderPowerState([bool]$TestOnly) {
    $sample=[ordered]@{schema=1;utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;testOnly=$TestOnly;available=$false;acSource='unknown';batteryPercent=$null;batteryFlag=$null;powerPlanGuid=$null;powerPlanName=$null;effectiveOverlayGuid=$null;errors=@()}
    if($TestOnly){$sample.errors=@('test_only_no_live_api');return $sample}
    try {Initialize-RecorderPowerStateNative;$data=[RecorderPowerState]::Read();$sample.acSource=$data.AcSource;$sample.batteryPercent=$data.BatteryPercent;$sample.batteryFlag=$data.BatteryFlag;$sample.powerPlanGuid=$data.PlanGuid;$sample.powerPlanName=$data.PlanName;$sample.effectiveOverlayGuid=$data.EffectiveOverlayGuid;$sample.errors=@($data.StatusError,$data.PlanError,$data.OverlayError|Where-Object {$_});$sample.available=-not $data.StatusError}catch{$sample.errors+= $_.Exception.Message}
    $sample
}

function Wait-RecorderMarkersClosed([string]$Directory,$Request,[int]$GuiPid,[uint64]$GuiCreation,[int]$TimeoutMilliseconds=5000) {
    if(-not $Request.markerProtocol){return [ordered]@{required=$false;closed=$null;exclusiveReadConfirmed=$null}}
    if($Request.markerProtocol.schema -ne 1 -or $Request.markerProtocol.writer -cne 'gui' -or $Request.markerProtocol.closeBeforeStop -ne $true){throw 'Unsupported GUI marker close protocol.'}
    $deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long]($TimeoutMilliseconds*[Diagnostics.Stopwatch]::Frequency/1000)
    do {
        $closed=$null;$path=Join-Path $Directory 'markers.closed.json';$exclusive=$false
        try {if([IO.File]::Exists($path)){$closed=[IO.File]::ReadAllText($path)|ConvertFrom-Json @recorderJsonOptions};$marker=Join-Path $Directory 'markers.jsonl';if([IO.File]::Exists($marker)){$handle=[IO.File]::Open($marker,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None);$handle.Dispose();$exclusive=$true}else{$exclusive=$true}}catch{}
        if($exclusive -and ($closed.closed -eq $true -or -not [RecorderNative]::Same($GuiPid,$GuiCreation))){return [ordered]@{required=$true;closed=($closed.closed -eq $true);exclusiveReadConfirmed=$true;writerReport=$closed;ownerExited=(-not [RecorderNative]::Same($GuiPid,$GuiCreation));note=$(if($closed.closed -eq $true){$null}else{'GUI exited without close acknowledgement; complete retained marker rows only.'})}}
        if([Diagnostics.Stopwatch]::GetTimestamp() -ge $deadline){throw 'GUI marker writer did not close in time; retained files were left for offline recovery.'}
        Start-Sleep -Milliseconds 50
    }while($true)
}
