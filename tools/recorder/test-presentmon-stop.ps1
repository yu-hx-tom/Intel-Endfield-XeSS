#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$requestedOutput=$OutputDirectory
. (Join-Path $PSScriptRoot 'record.ps1')
Initialize-RecorderNative
$directory=[IO.Path]::GetFullPath($requestedOutput)
if([IO.Directory]::Exists($directory)){throw 'Use a new output folder for the neutral PresentMon stop smoke.'}
if(@(Get-Process -Name Endfield -ErrorAction SilentlyContinue).Count){throw 'This neutral smoke requires Endfield closed; no process was stopped.'}
[IO.Directory]::CreateDirectory($directory)|Out-Null
$exe='D:/Programfiles/RivaTuner Statistics Server/Plugins/Client/PresentMonDataProvider/PresentMon-2.3.1-x64.exe'
$inventory=Join-Path $PSScriptRoot 'etw-readonly-inventory.exe'
if(-not [IO.File]::Exists($inventory)){$inventory=Join-Path $PSScriptRoot '../../out/20261016-low-stutter/etw-readonly-inventory.exe'}
$sessionName='EndfieldRecorder-'+[guid]::NewGuid().ToString('N');$target=$null;$capture=$null;$stopResult=$null;$captureResult=$null;$errorMessage=$null
$report=[ordered]@{test='neutral-process-presentmon-session-stop';powerShellRuntime=$powerShellRuntime;gameClosed=$true;gameStarted=$false;sessionName=$sessionName;preexistingSessionsTouched=$false;success=$false}
try {
    $query=Start-RecorderOwned 'etw-readonly-inventory' $inventory @() $directory 'etw-before.json' 'inventory.stderr.txt';$qr=Complete-RecorderOwned $query
    if($qr.exitCode -ne 0 -or $qr.forcedTermination){throw 'Read-only ETW inventory failed.'}
    $headroom=Get-RecorderInventorySummary ([IO.File]::ReadAllText((Join-Path $directory 'etw-before.json'))|ConvertFrom-Json)
    if(-not $headroom.kernelProcessHeadroomBelowEight){throw 'Neutral smoke needs a free Kernel-Process logger; no ETW session was started.'}
    if((Get-RecorderEtwSession $sessionName).exists){throw 'Fresh GUID collision; no cleanup requested.'}
    $shell=$powerShellRuntime.executable
    [IO.File]::WriteAllText((Join-Path $directory 'neutral.ps1'),'$stop=Join-Path $PSScriptRoot ''neutral.stop'';while(-not [IO.File]::Exists($stop)){Start-Sleep -Milliseconds 50}',[Text.UTF8Encoding]::new($false))
    $target=Start-RecorderOwned 'owned-neutral-target' $shell @('-NoProfile','-NonInteractive','-File',(Join-Path $directory 'neutral.ps1')) $directory 'neutral.stdout.txt' 'neutral.stderr.txt'
    $report['targetPid']=$target.process.Id;$report['targetCreationFileTime']=([RecorderNative]::Read($target.process.Id)).Created.ToString()
    $capture=Start-RecorderOwned 'presentmon' $exe @('--process_id',$target.process.Id.ToString(),'--output_stdout','--timed','30','--terminate_after_timed','--v2_metrics','--track_frame_type','--qpc_time_ms','--no_console_stats','--session_name',$sessionName) $directory 'presentmon.csv' 'presentmon.stderr.txt'
    $deadline=[Diagnostics.Stopwatch]::GetTimestamp()+10L*[Diagnostics.Stopwatch]::Frequency;$ready=$false
    while(-not $capture.process.HasExited -and [Diagnostics.Stopwatch]::GetTimestamp() -lt $deadline) {
        if((Get-RecorderEtwSession $sessionName).exists){$ready=$true;break};Start-Sleep -Milliseconds 100
    }
    if(-not $ready){throw 'Neutral PresentMon session did not become ready.'}
    Start-Sleep -Milliseconds 250
    $beforeStop=[Diagnostics.Stopwatch]::GetTimestamp();$stopResult=Stop-RecorderPresentMon $exe $sessionName $sessionName $directory
    $captureResult=Complete-RecorderOwned $capture 5000
    if(@($captureResult).Count -ne 1 -or $captureResult -isnot [Collections.IDictionary]){throw 'PresentMon completion did not return one collector metadata object.'}
    $report['stopSeconds']=([Diagnostics.Stopwatch]::GetTimestamp()-$beforeStop)/[double][Diagnostics.Stopwatch]::Frequency
    $report['stop']=$stopResult;$report['capture']=$captureResult
    if(-not $stopResult.stopRequested -or $stopResult.stopExitCode -ne 0 -or -not $stopResult.sessionReleased -or $captureResult.forcedTermination -or $captureResult.exitCode -ne 0){throw 'PresentMon session stop or real process exit was not clean; inspect the recorded exit codes.'}
    $report.success=$true
}catch{$errorMessage=$_.Exception.Message;$report['error']=$errorMessage}
finally {
    if($capture) {
        if(-not $stopResult){try{$report['cleanup']=Stop-RecorderPresentMon $exe $sessionName $sessionName $directory}catch{$report['cleanupError']=$_.Exception.Message}}
        if(-not $captureResult){try{$report['capture']=Complete-RecorderOwned $capture 5000}catch{$report['captureError']=$_.Exception.Message}}
    }
    if($target){[IO.File]::WriteAllText((Join-Path $directory 'neutral.stop'),'stop');$report['neutralTarget']=Complete-RecorderOwned $target 5000
        if($report.neutralTarget.forcedTermination -or $report.neutralTarget.exitCode -ne 0){$report.success=$false;$errorMessage='Owned neutral target did not exit cleanly.';$report['error']=$errorMessage}}
    Write-RecorderJson (Join-Path $directory 'stop-smoke.json') $report
}
$report|ConvertTo-Json -Depth 12
if(-not $report.success){throw $errorMessage}
