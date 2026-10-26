#requires -Version 7.0
#requires -PSEdition Core
# Passive system/process rates and optional target-thread boundary snapshots.
[CmdletBinding()]
param(
    [ValidateRange(1,2147483647)][int]$TargetPid,
    [uint64]$TargetCreationFileTime,
    [string]$OutputDirectory,
    [ValidateRange(1,86400)][int]$DurationSeconds=86400,
    [string]$StopFile='stop.request',
    [string]$ReadyFile='system.ready.json',
    [switch]$CaptureSystem,
    [switch]$CaptureThreads,
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'The collector requires PowerShell 7 (pwsh.exe).'}
$powerShellRuntime=[ordered]@{executable=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName;version=$PSVersionTable.PSVersion.ToString();edition=$PSVersionTable.PSEdition;processId=$PID}
if(-not ('RecorderSystemSampler' -as [type])){Add-Type -LiteralPath (Join-Path $PSScriptRoot 'SystemSampler.cs')}
if($SelfTest){
    [ordered]@{passed=$true;powerShellRuntime=$powerShellRuntime;checks=[RecorderSystemSampler]::SelfTest();samplingStarted=$false;newEtwSessions=0;configurationChanged=$false}|ConvertTo-Json
    return
}
if(-not $CaptureSystem -and -not $CaptureThreads){throw 'Select CaptureSystem and/or CaptureThreads.'}
if(-not $TargetPid -or -not $TargetCreationFileTime){throw 'Exact target PID and creation FILETIME are required.'}
foreach($leaf in @($StopFile,$ReadyFile)){
    if(-not $leaf -or [IO.Path]::GetFileName($leaf) -cne $leaf -or $leaf -in @('.','..')){throw 'Use relative marker leaf names.'}
}
$reserved=@('system.jsonl','threads-start.json','threads-end.json','services-start.json','services-end.json')
if($StopFile -eq $ReadyFile -or $StopFile -in $reserved -or $ReadyFile -in $reserved){throw 'Marker names must be distinct from capture data files.'}
if(-not $OutputDirectory){$OutputDirectory=(Get-Location).Path}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory)|Out-Null
$stopPath=Join-Path $OutputDirectory $StopFile
$readyPath=Join-Path $OutputDirectory $ReadyFile
$encoding=[Text.UTF8Encoding]::new($false)
$qpcFrequency=[Diagnostics.Stopwatch]::Frequency
$startQpc=[Diagnostics.Stopwatch]::GetTimestamp()
$startedUtc=[DateTime]::UtcNow.ToString('o')
$files=[Collections.Generic.List[string]]::new()
$sourceStatus=[ordered]@{
    system=[ordered]@{status=$(if($CaptureSystem){'partial'}else{'disabled'});message=$(if($CaptureSystem){'Initializing one-second PDH/process sampling.'}else{'System sampling disabled.'})}
    threads=[ordered]@{status=$(if($CaptureThreads){'partial'}else{'disabled'});message=$(if($CaptureThreads){'Initializing start/end thread snapshots.'}else{'Thread snapshots disabled.'})}
}
$readyWritten=$false
function Write-CaptureJson([string]$Name,$Value){
    $stream=[IO.FileStream]::new((Join-Path $OutputDirectory $Name),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $jsonWriter=[IO.StreamWriter]::new($stream,$encoding)
    try{$jsonWriter.WriteLine(($Value|ConvertTo-Json -Depth 12 -Compress))}finally{$jsonWriter.Dispose()}
    $files.Add($Name)
}
function Write-CaptureReady {
    if($script:readyWritten){return}
    $partial=($CaptureSystem -and $sourceStatus.system.status -ne 'ready') -or ($CaptureThreads -and $sourceStatus.threads.status -ne 'ready')
    $value=[ordered]@{state=$(if($partial){'partial'}else{'ready'});powerShellRuntime=$powerShellRuntime;message=(@($sourceStatus.Values|Where-Object {$_.status -ne 'disabled'}|ForEach-Object {$_.message}) -join ' ')
        sourceStatus=$sourceStatus;targetPid=$TargetPid;targetCreationFileTime=$TargetCreationFileTime.ToString();utc=[DateTime]::UtcNow.ToString('o')
        qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=$qpcFrequency;readOnly=$true}
    $temporary=$readyPath+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try{[IO.File]::WriteAllText($temporary,($value|ConvertTo-Json -Depth 8),$encoding);[IO.File]::Move($temporary,$readyPath)}
    finally{if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
    $script:readyWritten=$true
}
function Get-CaptureStopReason {
    if([IO.File]::Exists($stopPath)){return 'stop_file'}
    if(-not $target.IsLive){return 'target_exited'}
    if($watch.Elapsed.TotalSeconds -ge $DurationSeconds -and (-not $CaptureSystem -or $nextSample -gt $DurationSeconds)){return 'duration_limit'}
    return $null
}
function Write-ThreadSnapshot([string]$Name,[string]$Boundary){
    $snapshot=$target.SnapshotThreads()
    $value=[ordered]@{type='thread_snapshot';schemaVersion=1;boundary=$Boundary;qpcFrequency=$qpcFrequency;readOnly=$true;snapshot=$snapshot}
    Write-CaptureJson $Name $value
    $script:threadSnapshots++
    if($snapshot.State -eq 'partial' -or ($threadSnapshots -gt 1 -and $sourceStatus.threads.status -eq 'partial')){$sourceStatus.threads.status='partial'}
    else{$sourceStatus.threads.status='ready'}
    $sourceStatus.threads.message=('{0} thread snapshot: {1} threads, {2} CPU / {3} start-address reads unavailable.' -f $Boundary,$snapshot.EnumeratedThreads,$snapshot.CpuReadFailures,$snapshot.StartAddressReadFailures)
    if($snapshot.Warning){$sourceStatus.threads.message+=' '+$snapshot.Warning}
    $snapshot
}
function Write-ServiceSnapshot([string]$Name,[string]$Boundary){
    $snapshot=[RecorderSystemSampler]::ReadServicesBounded(200)
    Write-CaptureJson $Name ([ordered]@{type='service_pid_snapshot';schemaVersion=1;boundary=$Boundary;qpcFrequency=$qpcFrequency;snapshot=$snapshot})
    if($snapshot.State -ne 'ready'){$script:serviceWarnings.Add($snapshot.Warning)}
}

$target=$null;$sampler=$null;$writer=$null;$watch=[Diagnostics.Stopwatch]::StartNew()
$samples=0;$threadSnapshots=0;$maxSampleCostMs=0.0;$lastSampleAt=0.0;$firstSampleQpc=$null;$lastSampleQpc=$null
$serviceWarnings=[Collections.Generic.List[string]]::new()
$stopReason=$null;$failure=$null;$systemMissing=[Collections.Generic.HashSet[string]]::new();$nextSample=1.0
try{
    if([IO.File]::Exists($stopPath)){
        $stopReason='stop_file'
        foreach($name in @('system','threads')){if($sourceStatus[$name].status -ne 'disabled'){$sourceStatus[$name].message='Stop marker was present before source startup.'}}
    }
    else{
        $target=[RecorderSystemSampler+Target]::new($TargetPid,$TargetCreationFileTime)
        if($CaptureSystem){
            $stream=[IO.FileStream]::new((Join-Path $OutputDirectory 'system.jsonl'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
            $writer=[IO.StreamWriter]::new($stream,$encoding);$writer.AutoFlush=$true;$files.Add('system.jsonl')
            $sampler=[RecorderSystemSampler]::new($TargetPid,$TargetCreationFileTime)
            $null=$sampler.ReadProcesses()
            $metadata=[ordered]@{type='metadata';schemaVersion=1;powerShellRuntime=$powerShellRuntime;utc=$startedUtc;qpcTicks=$startQpc;qpcFrequency=$qpcFrequency
                targetPid=$TargetPid;targetCreationFileTime=$TargetCreationFileTime.ToString();samplerPid=$PID;readOnly=$true;configurationChanged=$false;newEtwSessions=0
                intervalSeconds=1;durationSeconds=$DurationSeconds;logicalProcessors=$sampler.LogicalProcessors;logicalCpuLayout=$sampler.LogicalCpuLayout
                counterPaths=$sampler.CounterPaths;initializationErrors=$sampler.InitializationErrors;initializationCostMs=$sampler.InitializationCostMs
                notes=@('Process CPU percentages are normalized to all active logical processors. Process IO includes file/device/network IO; disk counters are physical-disk totals.',
                    'PDH English counters use their collection interval. Rates use measured QPC intervals; missing/unreadable sources are null with errors, never synthetic zero.',
                    'sampleCostMs covers collection and top-process selection, excluding serialization/writing. Query costs are recorded separately. Slow samples skip missed deadlines.',
                    'Services are read-only SCM Win32-service PID maps at start/end, bounded to 200 ms each. Thread handles are opened only when CaptureThreads is selected.',
                    'Thread snapshots contain cumulative CPU times and optional native start addresses; no module reads, injection, thread control, ETW or configuration changes.')}
            $writer.WriteLine(($metadata|ConvertTo-Json -Depth 10 -Compress))
            if(-not (Get-CaptureStopReason)){Write-ServiceSnapshot 'services-start.json' 'start'}
        }
        if($CaptureThreads -and -not (Get-CaptureStopReason)){$null=Write-ThreadSnapshot 'threads-start.json' 'start'}
        if(-not $CaptureSystem){Write-CaptureReady}
        while(-not ($stopReason=Get-CaptureStopReason)){
            if($CaptureSystem -and $watch.Elapsed.TotalSeconds -ge $nextSample){
                $sampleQpc=[Diagnostics.Stopwatch]::GetTimestamp();$sampleUtc=[DateTime]::UtcNow.ToString('o');$sampleAt=$watch.Elapsed.TotalSeconds
                $cost=[Diagnostics.Stopwatch]::StartNew()
                $values=$sampler.ReadCounters();$rates=@($sampler.ReadProcesses())
                $logicalCpuValues=@(foreach($cpu in $sampler.LogicalCpuLayout){
                    $number=$values['logicalCpu'+$cpu.Index+'Percent']
                    if($null -ne $number){[pscustomobject]@{index=$cpu.Index;percent=$number}}
                })
                $busiest=$logicalCpuValues|Sort-Object percent -Descending|Select-Object -First 1
                $topCpu=@($rates|Where-Object {$null -ne $_.CpuPercent}|Sort-Object CpuPercent -Descending|Select-Object -First 5)
                $topIo=@($rates|Where-Object {$null -ne $_.ReadBytesPerSecond}|Sort-Object {$_.ReadBytesPerSecond+$_.WriteBytesPerSecond} -Descending|Select-Object -First 5)
                $targetRate=$rates|Where-Object {$_.Pid -eq $TargetPid -and $_.CreationFileTime -ceq $TargetCreationFileTime.ToString()}|Select-Object -First 1
                foreach($name in $sampler.CounterErrors.Keys){[void]$systemMissing.Add($name)}
                $cost.Stop();$sampleCost=$cost.Elapsed.TotalMilliseconds;$maxSampleCostMs=[Math]::Max($maxSampleCostMs,$sampleCost)
                $samples++;if($null -eq $firstSampleQpc){$firstSampleQpc=$sampleQpc};$lastSampleQpc=$sampleQpc
                $sample=[ordered]@{type='sample';index=$samples;utc=$sampleUtc;qpcTicks=$sampleQpc;qpcFrequency=$qpcFrequency;endQpcTicks=[Diagnostics.Stopwatch]::GetTimestamp()
                    elapsedSeconds=$sampleAt;intervalSeconds=($sampleAt-$lastSampleAt);counterIntervalSeconds=$sampler.CounterIntervalSeconds;processIntervalSeconds=$sampler.ProcessIntervalSeconds
                    sampleCostMs=$sampleCost;counterQueryCostMs=$sampler.CounterQueryCostMs;processQueryCostMs=$sampler.ProcessQueryCostMs
                    system=$values;counterErrors=$sampler.CounterErrors;missingCounters=@($sampler.CounterErrors.Keys)
                    maxLogicalCpuIndex=$(if($busiest){$busiest.index}else{$null});maxLogicalCpuPercent=$(if($busiest){$busiest.percent}else{$null})
                    processesObserved=$sampler.ProcessesObserved;processCpuReadFailures=$sampler.CpuReadFailures;processIoReadFailures=$sampler.IoReadFailures
                    targetProcess=$targetRate;targetProcessError=$(if(-not $targetRate){'Target process rate unavailable (unreadable, exited, or no matching baseline).'}else{$null})
                    targetThreadCount=$sampler.TargetThreadCount;targetThreadCountError=$sampler.TargetThreadCountError;topCpu=$topCpu;topIo=$topIo}
                $writer.WriteLine(($sample|ConvertTo-Json -Depth 10 -Compress));$lastSampleAt=$sampleAt
                if($samples -eq 1){
                    $partial=$sampler.CounterErrors.Count -gt 0 -or $sampler.CpuReadFailures -gt 0 -or $sampler.IoReadFailures -gt 0 -or $serviceWarnings.Count -gt 0 -or -not $targetRate
                    $sourceStatus.system.status=$(if($partial){'partial'}else{'ready'})
                    $sourceStatus.system.message=('One-second sampling active: {0} missing counters, {1} CPU / {2} IO process reads unavailable, {3} service warnings.' -f $sampler.CounterErrors.Count,$sampler.CpuReadFailures,$sampler.IoReadFailures,$serviceWarnings.Count)
                    Write-CaptureReady
                }
                $nextSample=[Math]::Max(($nextSample+1),([Math]::Floor($watch.Elapsed.TotalSeconds)+1))
            }
            $remaining=$(if($CaptureSystem){($nextSample-$watch.Elapsed.TotalSeconds)*1000}else{100})
            if($remaining -gt 0){Start-Sleep -Milliseconds ([int][Math]::Max(1,[Math]::Min(100,[Math]::Ceiling($remaining))))}
        }
    }
}catch{
    $failure=$_;$stopReason='error'
    foreach($name in @('system','threads')){if($sourceStatus[$name].status -ne 'disabled'){$sourceStatus[$name].status='partial';$sourceStatus[$name].message=$_.Exception.Message}}
}finally{
    if($CaptureThreads -and $target){
        try{$null=Write-ThreadSnapshot 'threads-end.json' 'end'}catch{$sourceStatus.threads.status='partial';$sourceStatus.threads.message=$_.Exception.Message;if(-not $failure){$failure=$_;$stopReason='error'}}
    }
    if($CaptureSystem -and $sampler){
        try{Write-ServiceSnapshot 'services-end.json' 'end'}catch{$serviceWarnings.Add($_.Exception.Message)}
        if($serviceWarnings.Count -gt 0){$sourceStatus.system.status='partial';$sourceStatus.system.message+=' '+($serviceWarnings -join ' ')}
        if($systemMissing.Count -gt 0){$sourceStatus.system.status='partial'}
    }
    if($samples -eq 0 -and $CaptureSystem){$sourceStatus.system.status='partial';$sourceStatus.system.message+=' No one-second samples were collected.'}
    if($threadSnapshots -eq 0 -and $CaptureThreads){$sourceStatus.threads.status='partial';$sourceStatus.threads.message+=' No thread snapshots were collected.'}
    if(-not $readyWritten){try{Write-CaptureReady}catch{if(-not $failure){$failure=$_;$stopReason='error'}}}
    $endQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    $summary=[ordered]@{type='summary';schemaVersion=1;powerShellRuntime=$powerShellRuntime;samples=$samples;threadSnapshots=$threadSnapshots;files=@($files.ToArray());sourceStatus=$sourceStatus
        targetPid=$TargetPid;targetCreationFileTime=$TargetCreationFileTime.ToString();utc=[DateTime]::UtcNow.ToString('o');startQpcTicks=$startQpc;endQpcTicks=$endQpc;qpcFrequency=$qpcFrequency
        stopReason=$stopReason;stopMarkerObserved=[IO.File]::Exists($stopPath);actualElapsedSeconds=$watch.Elapsed.TotalSeconds;maxSampleCostMs=$maxSampleCostMs
        missingCounters=@($systemMissing);serviceWarnings=@($serviceWarnings.ToArray());readOnly=$true;configurationChanged=$false;newEtwSessions=0
        coverage=[ordered]@{system=[ordered]@{enabled=[bool]$CaptureSystem;samples=$samples;firstSampleQpcTicks=$firstSampleQpc;lastSampleQpcTicks=$lastSampleQpc;intervalSeconds=1}
            threads=[ordered]@{enabled=[bool]$CaptureThreads;snapshots=$threadSnapshots;boundariesOnly=$true}}
        error=$(if($failure){$failure.Exception.Message}else{$null})}
    if($writer){try{$writer.WriteLine(($summary|ConvertTo-Json -Depth 10 -Compress))}finally{$writer.Dispose()}}
    if($sampler){$sampler.Dispose()};if($target){$target.Dispose()}
}
$summary|ConvertTo-Json -Depth 10
if($failure){throw $failure}
