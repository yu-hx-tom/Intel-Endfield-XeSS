#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'Run recorder tests with PowerShell 7 (pwsh.exe).'}
$powerShellRuntime=[ordered]@{executable=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName;version=$PSVersionTable.PSVersion.ToString();edition=$PSVersionTable.PSEdition;processId=$PID}
if(-not $OutputDirectory){$OutputDirectory=Join-Path ([IO.Path]::GetTempPath()) ('endfield-recorder-system-test-'+[guid]::NewGuid().ToString('N'))}
$testRoot=[IO.Path]::GetFullPath($OutputDirectory)
if([IO.Directory]::Exists($testRoot)){throw 'Use a new test output folder.'}
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$shell=$powerShellRuntime.executable
$capture=Join-Path $PSScriptRoot 'capture-system.ps1'
$encoding=[Text.UTF8Encoding]::new($false)
$workers=[Collections.Generic.List[object]]::new();$checks=0;$stopCosts=[Collections.Generic.List[double]]::new()
function Check([bool]$Good,[string]$Message){if(-not $Good){throw $Message};$script:checks++}
function Check-PowerShellRuntime($Runtime,[int]$ProcessId){
    Check ($Runtime.executable -ieq $shell -and $Runtime.version -like '7.*' -and $Runtime.edition -ceq 'Core' -and $Runtime.processId -eq $ProcessId) 'A collector did not report its actual PowerShell 7 runtime.'
}
function Quote-Argument([string]$Value){'"'+[regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'}
function New-Folder([string]$Name){$folder=Join-Path $testRoot $Name;[IO.Directory]::CreateDirectory($folder)|Out-Null;$folder}
function Start-Worker([string[]]$Arguments){
    $si=[Diagnostics.ProcessStartInfo]::new();$si.FileName=$shell;$si.Arguments=(@($Arguments|ForEach-Object {Quote-Argument $_}) -join ' ')
    $si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    $si.EnvironmentVariables.Remove('PSModulePath')
    $process=[Diagnostics.Process]::new();$process.StartInfo=$si
    if(-not $process.Start()){throw 'Could not start the owned test process.'}
    $worker=[pscustomobject]@{process=$process;stdout=$process.StandardOutput.ReadToEndAsync();stderr=$process.StandardError.ReadToEndAsync();completed=$false;output=$null;error=$null}
    $workers.Add($worker);$worker
}
function Start-Capture([string]$Folder,$Neutral,[uint64]$Creation,[string[]]$Switches,[int]$Duration=86400){
    Start-Worker (@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$capture,'-OutputDirectory',$Folder,
        '-TargetPid',$Neutral.process.Id.ToString(),'-TargetCreationFileTime',$Creation.ToString(),'-DurationSeconds',$Duration.ToString())+$Switches)
}
function Wait-Ready($Worker,[string]$Folder){
    $clock=[Diagnostics.Stopwatch]::StartNew();$path=Join-Path $Folder 'system.ready.json'
    while($clock.Elapsed.TotalSeconds -lt 12){
        if([IO.File]::Exists($path)){$ready=[IO.File]::ReadAllText($path)|ConvertFrom-Json;Check-PowerShellRuntime $ready.powerShellRuntime $Worker.process.Id;return $ready}
        if($Worker.process.HasExited){throw ('Capture exited before readiness: '+$Worker.stderr.GetAwaiter().GetResult())}
        Start-Sleep -Milliseconds 25
    }
    throw 'Timed out waiting for the system collector.'
}
function Complete-Worker($Worker,[int]$Milliseconds=5000){
    if(-not $Worker.process.WaitForExit($Milliseconds)){throw 'An owned test process did not exit within its time bound.'}
    if(-not $Worker.completed){$Worker.output=$Worker.stdout.GetAwaiter().GetResult();$Worker.error=$Worker.stderr.GetAwaiter().GetResult();$Worker.completed=$true}
}
function Stop-Capture($Worker,[string]$Folder){
    $clock=[Diagnostics.Stopwatch]::StartNew();[IO.File]::WriteAllText((Join-Path $Folder 'stop.request'),'self_test',$encoding)
    Complete-Worker $Worker 4000;$script:stopCosts.Add($clock.Elapsed.TotalMilliseconds)
    Check ($Worker.process.ExitCode -eq 0) ('Capture failed after stop: '+$Worker.error)
    Check ($clock.Elapsed.TotalMilliseconds -lt 2000) 'Stop marker response exceeded two seconds.'
    $summary=$Worker.output|ConvertFrom-Json;Check-PowerShellRuntime $summary.powerShellRuntime $Worker.process.Id;$summary
}
function Read-Json([string]$Folder,[string]$Name){[IO.File]::ReadAllText((Join-Path $Folder $Name))|ConvertFrom-Json}

try{
    $self=& $capture -SelfTest|ConvertFrom-Json
    Check ($self.passed -and $self.checks -ge 10 -and -not $self.samplingStarted -and $self.newEtwSessions -eq 0) 'Parser/identity self-test failed.'
    Check-PowerShellRuntime $self.powerShellRuntime $PID
    $checks+=$self.checks
    $neutral=Start-Worker @('-NoProfile','-NonInteractive','-Command','Start-Sleep -Seconds 90')
    $created=[RecorderSystemSampler]::ReadCreationFileTime($neutral.process.Id)
    Check ($created -gt 9007199254740992) 'FILETIME fixture must exercise integers above JSON double precision.'

    $folder=New-Folder 'identity-mismatch'
    $worker=Start-Capture $folder $neutral ($created+1) @('-CaptureThreads');Complete-Worker $worker
    Check ($worker.process.ExitCode -ne 0 -and $worker.error -like '*identity*') 'The script accepted a reused/mismatched process identity.'
    $summary=$worker.output|ConvertFrom-Json
    Check ($summary.samples -eq 0 -and $summary.threadSnapshots -eq 0 -and $summary.files.Count -eq 0) 'An identity mismatch started collection.'

    $folder=New-Folder 'stop-before-start'
    [IO.File]::WriteAllText((Join-Path $folder 'stop.request'),'self_test',$encoding)
    $worker=Start-Capture $folder $neutral $created @('-CaptureSystem','-CaptureThreads');Complete-Worker $worker
    $summary=$worker.output|ConvertFrom-Json
    Check ($worker.process.ExitCode -eq 0 -and $summary.stopReason -eq 'stop_file' -and $summary.stopMarkerObserved) 'A preexisting stop marker was not honored.'
    Check ($summary.samples -eq 0 -and $summary.threadSnapshots -eq 0 -and $summary.files.Count -eq 0) 'Stop-before-start ran a data source.'

    $folder=New-Folder ('threads-only space '+[char]0x6d4b)
    $worker=Start-Capture $folder $neutral $created @('-CaptureThreads');$ready=Wait-Ready $worker $folder
    Check ($ready.state -in @('ready','partial') -and $ready.sourceStatus.system.status -eq 'disabled') 'Thread-only readiness enabled the system source.'
    Check (-not [IO.File]::Exists((Join-Path $folder 'system.jsonl')) -and -not [IO.File]::Exists((Join-Path $folder 'services-start.json'))) 'Thread-only mode initialized PDH/service sampling.'
    $start=Read-Json $folder 'threads-start.json'
    Check ($start.snapshot.TargetLiveBefore -and $start.snapshot.TargetLiveAfter -and $start.snapshot.Threads.Count -gt 0) 'The owned neutral process has no readable thread boundary snapshot.'
    foreach($thread in $start.snapshot.Threads){
        Check (($null -ne $thread.CpuTime100ns -and $thread.CpuTime100ns -ge 0) -or [bool]$thread.CpuError) 'A missing thread CPU time lacks an error.'
        Check ([bool]$thread.StartAddress -or [bool]$thread.StartAddressError) 'A missing thread start address lacks an error.'
    }
    $summary=Stop-Capture $worker $folder
    Check ($summary.samples -eq 0 -and $summary.threadSnapshots -eq 2 -and $summary.coverage.threads.boundariesOnly) 'Thread-only coverage is incomplete.'
    Check ($summary.targetCreationFileTime -ceq $created.ToString()) 'Creation FILETIME lost precision in JSON.'
    Check ([IO.File]::Exists((Join-Path $folder 'threads-end.json'))) 'Manual stop did not preserve the end thread snapshot.'

    $folder=New-Folder 'system-only'
    $worker=Start-Capture $folder $neutral $created @('-CaptureSystem');$ready=Wait-Ready $worker $folder
    Check ($ready.state -in @('ready','partial') -and $ready.sourceStatus.threads.status -eq 'disabled') 'System-only readiness enabled thread snapshots.'
    $summary=Stop-Capture $worker $folder
    Check ($summary.samples -ge 1 -and $summary.threadSnapshots -eq 0 -and $summary.stopReason -eq 'stop_file') 'System-only collection did not preserve a sample.'
    Check (-not [IO.File]::Exists((Join-Path $folder 'threads-start.json'))) 'System-only mode queried thread start addresses.'
    $rows=@([IO.File]::ReadAllLines((Join-Path $folder 'system.jsonl'))|ForEach-Object {$_|ConvertFrom-Json})
    Check ($rows[0].type -eq 'metadata' -and $rows[-1].type -eq 'summary') 'System JSONL metadata/summary framing is missing.'
    $sample=@($rows|Where-Object {$_.type -eq 'sample'})[0]
    Check ($sample.qpcTicks -gt 0 -and $sample.utc -and $sample.sampleCostMs -ge 0 -and $sample.counterQueryCostMs -ge 0 -and $sample.processQueryCostMs -ge 0) 'Sample clocks or measured query costs are missing.'
    Check ($sample.counterIntervalSeconds -gt 0 -and $sample.processIntervalSeconds -gt 0) 'Measured rate intervals are missing.'
    foreach($name in @('cpuPercent','logicalCpu0Percent','dpcPercent','interruptPercent','processorQueueLength','availableMemoryMB','pagesInputPerSecond','pageReadsPerSecond',
        'diskReadBytesPerSecond','diskWriteBytesPerSecond','diskReadLatencyMs','diskWriteLatencyMs','diskCurrentQueueLength','diskAverageQueueLength',
        'c1Percent','c2Percent','c3Percent','processorPerformancePercent','processorInformationTimePercent','processorUtilityPercent','interruptsPerSecond','contextSwitchesPerSecond','systemThreadCount')){
        $property=$sample.system.PSObject.Properties[$name]
        Check ($null -ne $property) ('Required system counter missing from schema: '+$name)
        if($null -eq $property.Value){Check ([bool]$sample.counterErrors.PSObject.Properties[$name].Value) ('Missing counter has no source error: '+$name)}
    }
    Check (($null -ne $sample.targetThreadCount -and $sample.targetThreadCount -gt 0) -or [bool]$sample.targetThreadCountError) 'Target thread count has no value/error.'
    Check (($sample.targetProcess.Pid -eq $neutral.process.Id -and $sample.targetProcess.CreationFileTime -ceq $created.ToString()) -or [bool]$sample.targetProcessError) 'Target process rate has no exact identity or missing-source error.'
    foreach($name in @('services-start.json','services-end.json')){
        $services=Read-Json $folder $name
        Check ($services.snapshot.ReadOnly -and $services.snapshot.QpcTicks -gt 0 -and $services.snapshot.QueryCostMs -ge 0) 'Service snapshot is not a timed read-only boundary query.'
        Check ($services.snapshot.State -eq 'ready' -or [bool]$services.snapshot.Warning) 'Unavailable service mapping has no warning.'
    }

    $folder=New-Folder 'one-second-duration'
    $worker=Start-Capture $folder $neutral $created @('-CaptureSystem','-CaptureThreads') 1;Complete-Worker $worker 12000
    $summary=$worker.output|ConvertFrom-Json
    Check ($worker.process.ExitCode -eq 0 -and $summary.stopReason -eq 'duration_limit' -and $summary.samples -ge 1) 'A one-second capture omitted its first one-second sample.'
    Check ($summary.threadSnapshots -eq 2 -and $summary.coverage.system.enabled -and $summary.coverage.threads.enabled) 'Combined system/thread selection did not collect both boundaries.'

    $folder=New-Folder 'target-exit'
    $worker=Start-Capture $folder $neutral $created @('-CaptureThreads');$null=Wait-Ready $worker $folder
    $clock=[Diagnostics.Stopwatch]::StartNew();$neutral.process.Kill();Complete-Worker $neutral;Complete-Worker $worker 4000
    $summary=$worker.output|ConvertFrom-Json
    Check ($worker.process.ExitCode -eq 0 -and $summary.stopReason -eq 'target_exited' -and $clock.Elapsed.TotalMilliseconds -lt 2000) 'Exact target exit did not stop capture promptly.'
    $end=Read-Json $folder 'threads-end.json'
    Check ($end.snapshot.State -eq 'partial' -and -not $end.snapshot.TargetLiveAfter -and [bool]$end.snapshot.Warning) 'Target exit fabricated a successful end thread snapshot.'

    $report=[ordered]@{passed=$true;checks=$checks;powerShellRuntime=$powerShellRuntime;outputDirectory=$testRoot;stopLatencyMs=@($stopCosts.ToArray());host=$shell;newEtwSessions=0;configurationChanged=$false;gameStarted=$false}
    [IO.File]::WriteAllText((Join-Path $testRoot 'self-test.json'),($report|ConvertTo-Json -Depth 6),$encoding)
    $report|ConvertTo-Json -Depth 6
}finally{
    foreach($worker in $workers){
        if(-not $worker.process.HasExited){$worker.process.Kill();$worker.process.WaitForExit(4000)|Out-Null}
        $worker.process.Dispose()
    }
}
