#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceDirectory)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'Offline recovery requires PowerShell 7. Run this script with pwsh.exe.'}
. (Join-Path $PSScriptRoot 'record.ps1')

$source=[IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
$destination=$source+'.recovered';$zip=$destination+'.zip'
if(-not [IO.Directory]::Exists($source)){throw 'The original recording directory is missing.'}
if([IO.Directory]::Exists($destination) -or [IO.File]::Exists($zip)){throw 'Recovery output already exists; no files were overwritten.'}
foreach($name in @('status.json','request.json','stop.request','game-identity.json','session-discovery.json')) {
    if(-not [IO.File]::Exists((Join-Path $source $name))){throw ('Required retained evidence is missing: '+$name)}
}
$originalStatus=[IO.File]::ReadAllText((Join-Path $source 'status.json'))|ConvertFrom-Json @recorderJsonOptions
$request=[IO.File]::ReadAllText((Join-Path $source 'request.json'))|ConvertFrom-Json @recorderJsonOptions
$stop=[IO.File]::ReadAllText((Join-Path $source 'stop.request'))|ConvertFrom-Json @recorderJsonOptions
$game=[IO.File]::ReadAllText((Join-Path $source 'game-identity.json'))|ConvertFrom-Json @recorderJsonOptions
$session=[IO.File]::ReadAllText((Join-Path $source 'session-discovery.json'))|ConvertFrom-Json @recorderJsonOptions
if($originalStatus.state -ne 'failed'){throw 'Recovery requires a retained failed recording.'}
$frequency=[long]$originalStatus.qpcFrequency;$start=[long]$originalStatus.startQpcTicks;$end=[long]$stop.qpcTicks
if($frequency -le 0 -or [long]$request.qpcFrequency -ne $frequency -or [long]$stop.qpcFrequency -ne $frequency -or $start -le 0 -or $end -lt $start){throw 'The retained recording QPC window is inconsistent.'}
$selected=Resolve-RecorderCollectors $request
$script:recordingContext=Resolve-RecorderRequestContext $request
$inputs=@(Get-ChildItem -LiteralPath $source -File -Recurse)
if(@($inputs|Where-Object {$_.FullName.Substring($source.Length+1) -like 'recovery-original/*' -or $_.FullName.Substring($source.Length+1) -like 'recovery-original\*'}).Count){throw 'Reserved recovery-original directory is already in the input.'}
[IO.Directory]::CreateDirectory($destination)|Out-Null
$inputHashes=@();$recoveredUtc=[DateTime]::UtcNow.ToString('o')
try {
    foreach($input in $inputs) {
        $relative=$input.FullName.Substring($source.Length+1).Replace('\','/')
        $copyRelative=$(if($relative -in @('status.json','summary.json','summary.md','summary.analysis.json','window.json','manifest.json','recovery.json') -or -not (Test-RecorderArtifact $input.Name)){'recovery-original/original-'+$relative}else{$relative})
        $copyPath=Join-Path $destination $copyRelative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copyPath))|Out-Null
        $hash=Get-RecorderSha256 $input.FullName
        [IO.File]::Copy($input.FullName,$copyPath,$false)
        $copyHash=Get-RecorderSha256 $copyPath
        if($copyHash -cne $hash){throw ('Copied evidence hash mismatch: '+$relative)}
        $inputHashes+=[ordered]@{sourceFile=$relative;copiedFile=$copyRelative;bytes=$input.Length;sha256=$hash;copiedSha256=$copyHash;match=$true}
    }
    $duration=($end-$start)/[double]$frequency
    $streams=@();$events=@()
    foreach($name in @('measurement-stream-events.jsonl','sr-observation-stream-events.jsonl')){$eventFile=Join-Path $destination $name;if([IO.File]::Exists($eventFile)){foreach($line in [IO.File]::ReadLines($eventFile)){if($line.Trim()){$events+=($line|ConvertFrom-Json @recorderJsonOptions)}}}}
    foreach($file in @(Get-ChildItem -LiteralPath $destination -File|Where-Object {$_.Name -like 'measurements-*.stream.jsonl' -or $_.Name -like 'measurement-status-*.compact.jsonl' -or $_.Name -like 'sr-observation-*.stream.jsonl'})) {
        $recorded=@($events|Where-Object {$_.file -ceq $file.Name})
        $last=$(if($recorded.Count){$recorded[-1]}else{$null})
        $streams+=[ordered]@{key=$(if($file.Name -like 'sr-observation-*'){'logs'}else{'measurements'});source=$last.source;file=$file.Name;copiedBytes=$file.Length;remainingBytesAtLastPoll=$last.backlogBytes;detectedRotations=@($recorded|Where-Object {$_.rotation -eq $true}).Count
            lastError=$null;lastErrorKnown=$false;finalInMemoryStateKnown=$false;recovered=$true;copiedBytesBasis='retained file length';lastRecordedEvent=$last}
    }
    $snapshots=@()
    foreach($plan in @(Get-RecorderSnapshotPlan $selected $script:recordingContext.measurementMode)) {
        $file=Join-Path $destination $plan.file;$exists=[IO.File]::Exists($file)
        $snapshots+=[ordered]@{key=$plan.key;source=$(if($session.match.directory){Join-Path $session.match.directory $plan.source}else{$null});file=$plan.file
            state=$(if($exists){'retained_before_save_failure'}else{'not_retained'});bytes=$(if($exists){([IO.FileInfo]$file).Length}else{0});tailOnly=$plan.tail;tailByteLimit=$(if($plan.tail){2097152}else{$null})
            sourceBytes=$null;partialTailBytesOmitted=$null;sourceChangedDuringSnapshot=$null;recovered=$true;note='Only the already retained copy was inspected; original live source was not opened.'}
    }
    $sourceStatus=$originalStatus.sourceStatus
    foreach($key in @('measurements','logs')) {
        if(-not $selected[$key]){continue}
        $context=Get-RecorderContextSourceSummary $key $snapshots $streams
        $sourceStatus.$key.files=$context.files;$sourceStatus.$key.status=$context.state
        $sourceStatus.$key.message=$context.message+' Recovered from retained files; unsaved final source state is unknown.'
        $sourceStatus.$key.coverage=$context.coverage
    }
    $frameSources=[ordered]@{}
    foreach($entry in @(@('pmdp','frames.csv','pmdp-run.json'),@('rtss','rtss-present-intervals.csv','rtss-run.json'),@('presentmonRaw','presentmon.csv',$null))) {
        if(-not $selected[$entry[0]]){continue}
        $rows=$null
        if($entry[2] -and [IO.File]::Exists((Join-Path $destination $entry[2]))){$stats=[IO.File]::ReadAllText((Join-Path $destination $entry[2]))|ConvertFrom-Json @recorderJsonOptions;if($null -ne $stats.rows){$rows=[long]$stats.rows}}
        $frameSources[$entry[0]]=[ordered]@{file=$entry[1];rows=$rows;state=$(if(-not [IO.File]::Exists((Join-Path $destination $entry[1]))){'missing'}elseif($null -eq $rows){'retained_rows_unknown'}elseif($rows -gt 0){'collected'}else{'zero_rows'});recovered=$true}
    }
    $collectors=@()
    foreach($entry in @(@('pmdp','pmdp-run.json'),@('rtss','rtss-run.json'),@('power','power-run.json'),@('system','system-run.json'),@('etw-readonly-inventory','etw-before.json'))) {
        $path=Join-Path $destination $entry[1]
        if([IO.File]::Exists($path)){$collectors+=[ordered]@{kind=$entry[0];runFile=$entry[1];report=([IO.File]::ReadAllText($path)|ConvertFrom-Json @recorderJsonOptions);exitCode=$null;forcedTermination=$null;exitStatusKnown=$false;note='Collector report retained verbatim; worker process exit code and forced-termination metadata were not persisted.'}}
    }
    $latest=$null;$latestPath=Join-Path $destination 'measurement-status-latest.json'
    if([IO.File]::Exists($latestPath)){$latest=[IO.File]::ReadAllText($latestPath)|ConvertFrom-Json @recorderJsonOptions}
    $unknown=@('Original worker and collector process exit codes and forced-termination metadata were not persisted.','Final in-memory observer stream errors and idle polls are unknown.','Snapshot original source length, omitted partial tail size, and concurrent-source-change result are unknown.','Original tool source hashes and original in-memory warning text are unavailable.')
    $warnings=@('Recovered offline after the original save failed. Original raw files were copied without modification.','Independent PresentMon ETW raw is unavailable; recovery does not reconstruct it.',$originalStatus.presentMonFallbackReason)+$unknown
    $hasData=@($sourceStatus.PSObject.Properties.Value|Where-Object {$_.status -in @('collected','partial') -and $_.files.Count -gt 0}).Count -gt 0
    $window=[ordered]@{recovered=$true;testOnly=$false;recordingStarted=$true;selectedCollectors=$selected;sourceStatus=$sourceStatus;requestedStartUtc=$request.utc;requestedStartQpcTicks=$request.qpcTicks
        startQpcTicks=$start;recordingReadyQpcTicks=$originalStatus.recordingReadyQpcTicks;endQpcTicks=$end;qpcFrequency=$frequency;stop=$stop;gamePid=$game.pid;gameCreationFileTime=$game.creationFileTime;sessionId=$originalStatus.sessionId;mode=$originalStatus.mode
        durationSeconds=$duration;originalReportedDurationSeconds=$originalStatus.durationSeconds;measurementStreams=@($streams|Where-Object key -eq 'measurements');srObservationStreams=@($streams|Where-Object key -eq 'logs');finalSnapshots=$snapshots
        coverageNote='QPC window is reconstructed from retained status.json and stop.request. Retained measurement/SR context may precede this window; filter by QPC. Rotations between polls and overwritten tails cannot be recovered. No live source was read and no missing frames, timing data, or ETW raw was reconstructed.'}
    $window['recordingContext']=$script:recordingContext;$window['measurementMode']=$script:recordingContext.measurementMode;$window['detailIncluded']=$script:recordingContext.detailIncluded
    Write-RecorderJson (Join-Path $destination 'window.json') $window
    $summary=[ordered]@{state='complete';recovered=$true;testOnly=$false;recordingStarted=$true;selectedCollectors=$selected;sourceStatus=$sourceStatus;noData=(-not $hasData);error=$null;originalSaveError=$originalStatus.saveError;originalState=$originalStatus.state
        stopReason=$stop.reason;outputDirectory=$destination;originalOutputDirectory=$source;gamePid=$game.pid;gameIdentity=$game;sessionId=$originalStatus.sessionId;mode=$originalStatus.mode;latestMeasurementStatus=$latest;measurementStreams=@($streams|Where-Object key -eq 'measurements');srObservationStreams=@($streams|Where-Object key -eq 'logs')
        startedUtc=$originalStatus.startedUtc;durationSeconds=$duration;originalReportedDurationSeconds=$originalStatus.durationSeconds;collectors=$collectors;presentMonSession=$null;presentMonStop=$null;presentMonSource=$originalStatus.presentMonSource
        independentPresentMonRaw=[bool]$originalStatus.independentPresentMonRaw;presentMonFallbackReason=$originalStatus.presentMonFallbackReason;frameSources=$frameSources;warnings=$warnings;unknownFields=$unknown
        gameLaunched=$false;gameOrInstallModified=$false;rtssConfigurationModified=$false;preexistingEtwSessionsStopped=$false;finishedUtc=$originalStatus.updatedUtc;recoveredUtc=$recoveredUtc}
    foreach($key in @('label','labelCode','notes','xiaomiPerformanceMode','xiaomiPerformanceModeSource','measurementMode','detailIncluded')){$summary[$key]=$script:recordingContext[$key]}
    $summary['markerClose']=[ordered]@{recovered=$true;writerClosedKnown=[IO.File]::Exists((Join-Path $destination 'markers.closed.json'));note='Only already retained marker files were inspected; unsaved markers and original writer state are unknown.'}
    $summary['configurationSnapshots']=@('configuration-start.json','configuration-stop.json'|Where-Object {[IO.File]::Exists((Join-Path $destination $_))})
    $analysis=Write-RecorderReadableSummary $destination $window $summary $script:recordingContext
    $summary['markerCount']=$analysis.markers.validCount;$summary['markerInvalidRows']=$analysis.markers.invalidRows;$summary['markerIntegrity']=$analysis.markerIntegrity;$summary['steadyVerified']=$analysis.steadyVerified;$summary['analysisFile']='summary.analysis.json';$summary['readableSummaryFile']='summary.md';$summary['sizeBudget']=$analysis.sizeBudget
    $warnings+=@($analysis.markerIntegrity.warnings);$summary.warnings=$warnings
    Write-RecorderJson (Join-Path $destination 'summary.json') $summary
    foreach($entry in $inputHashes){if((Get-RecorderSha256 (Join-Path $source $entry.sourceFile)) -cne $entry.sha256){throw ('Original input changed during recovery: '+$entry.sourceFile)}}
    $recovery=[ordered]@{recovered=$true;powerShellRuntime=$powerShellRuntime;sourceDirectory=$source;outputDirectory=$destination;recoveredUtc=$recoveredUtc;originalRecordingFinishedUtc=$originalStatus.updatedUtc;originalSaveError=$originalStatus.saveError
        originalInputFiles=$inputHashes;allOriginalInputHashesMatch=$true;liveSourcesRead=$false;collectorsStarted=$false;newEtwSessions=0;unknownFields=$unknown;recoveryTool=[ordered]@{path=$PSCommandPath;sha256=Get-RecorderSha256 $PSCommandPath}}
    Write-RecorderJson (Join-Path $destination 'recovery.json') $recovery
    $files=@(Get-ChildItem -LiteralPath $destination -File -Recurse|Where-Object {$_.Name -ne 'manifest.json' -and (Test-RecorderArtifact $_.Name)}|ForEach-Object {[ordered]@{file=$_.FullName.Substring($destination.Length+1).Replace('\','/');bytes=$_.Length;sha256=Get-RecorderSha256 $_.FullName}})
    Write-RecorderJson (Join-Path $destination 'manifest.json') ([ordered]@{schema=2;state='complete';recovered=$true;testOnly=$false;selectedCollectors=$selected;sourceStatus=$sourceStatus;noData=(-not $hasData);files=$files;originalInputFiles=$inputHashes;allOriginalInputHashesMatch=$true
        collectorSources=@();collectorSourceHashesKnown=$false;mutableFilesExcludedFromHashesAndZip=@('status.json','*.tmp','worker.*','backend.*','recorder-worker.*');hashAlgorithm='SHA256';createdUtc=$recoveredUtc})
    New-RecorderZip $destination $zip
    Write-RecorderJson (Join-Path $destination 'status.json') ([ordered]@{state='complete';recovered=$true;message='Recovered from retained original data. Independent PresentMon raw remains unavailable; see summary.json.';outputDirectory=$destination;zipPath=$zip
        startedUtc=$originalStatus.startedUtc;startQpcTicks=$start;qpcFrequency=$frequency;durationSeconds=$duration;gamePid=$game.pid;sessionId=$originalStatus.sessionId;mode=$originalStatus.mode;selectedCollectors=$selected;sourceStatus=$sourceStatus
        presentMonSource=$originalStatus.presentMonSource;independentPresentMonRaw=[bool]$originalStatus.independentPresentMonRaw;presentMonFallbackReason=$originalStatus.presentMonFallbackReason;noData=(-not $hasData);warningCount=$warnings.Count;updatedUtc=$recoveredUtc})
    [ordered]@{recovered=$true;outputDirectory=$destination;zipPath=$zip;originalInputFiles=$inputHashes.Count;allOriginalInputHashesMatch=$true;durationSeconds=$duration;independentPresentMonRaw=[bool]$originalStatus.independentPresentMonRaw}|ConvertTo-Json
} catch {Write-RecorderError $destination 'offline_recovery' $_;throw}
