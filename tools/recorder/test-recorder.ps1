#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([string]$OutputDirectory,[switch]$SaveRegressionOnly,[switch]$MockOnly)
$ErrorActionPreference='Stop'
$requestedOutput=$OutputDirectory
. (Join-Path $PSScriptRoot 'record.ps1')
Initialize-RecorderNative
$OutputDirectory=$requestedOutput
if(-not $OutputDirectory){$OutputDirectory=Join-Path ([IO.Path]::GetTempPath()) ('endfield-recorder-cpu-'+[guid]::NewGuid().ToString('N'))}
$testRoot=[IO.Path]::GetFullPath($OutputDirectory)
if([IO.Directory]::Exists($testRoot)){throw 'Use a new test output folder.'}
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$checks=0;$skippedChecks=@()
function Check([bool]$Good,[string]$Message) {if(-not $Good){throw $Message};$script:checks++}
function Check-PowerShellRuntime($Runtime,[int]$ProcessId) {
    Check ($Runtime.executable -ieq $powerShellRuntime.executable -and $Runtime.version -like '7.*' -and $Runtime.edition -ceq 'Core' -and $Runtime.processId -eq $ProcessId) 'A recorder child did not report its actual PowerShell 7 runtime.'
}
function Refuses([scriptblock]$Action,[string]$Message) {$rejected=$false;try{& $Action|Out-Null}catch{$rejected=$true};Check $rejected $Message}
function New-TestRequest([string]$Name,$Selected=$null) {
    $folder=Join-Path $testRoot $Name;[IO.Directory]::CreateDirectory($folder)|Out-Null
    $value=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency}
    if($null -ne $Selected){$value['collectors']=$Selected}
    Write-RecorderJson (Join-Path $folder 'request.json') $value
    $folder
}
function New-TestMask([string[]]$Keys) {$mask=[ordered]@{};foreach($key in @('presentmonRaw','pmdp','rtss','hardware','foreground','system','threads','measurements','logs')){$mask[$key]=$key -cin $Keys};$mask}
function Start-TestWorker([string]$Folder,[uint64]$Creation,[bool]$TestOnly) {
    $arguments=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'record.ps1'),'-OutputDirectory',$Folder,'-OwnerPid',$PID.ToString(),'-OwnerCreationFileTime',$Creation.ToString())
    if($TestOnly){$arguments+='-Simulate'}
    $modulePathBefore=$env:PSModulePath
    try {
        $env:PSModulePath=Join-Path $env:ProgramFiles 'PowerShell/7/Modules'
        Start-RecorderOwned 'test-worker' $powerShellRuntime.executable $arguments $Folder 'worker.stdout.txt' 'worker.stderr.txt'
    } finally {$env:PSModulePath=$modulePathBefore}
}
function Wait-TestState([string]$Folder,[string[]]$States,[int]$Seconds=15) {
    $until=[Diagnostics.Stopwatch]::GetTimestamp()+$Seconds*[Diagnostics.Stopwatch]::Frequency
    do {
        $path=Join-Path $Folder 'status.json'
        if([IO.File]::Exists($path)) {try {$value=[RecorderNative]::ReadAllText($path)|ConvertFrom-Json @recorderJsonOptions;if($value.state -in $States){return $value}}catch [IO.IOException] {}}
        Start-Sleep -Milliseconds 50
    }while([Diagnostics.Stopwatch]::GetTimestamp() -lt $until)
    throw ('Timed out waiting for '+($States -join ',')+' in '+$Folder)
}

$contextSource=Join-Path $testRoot 'context-source';$contextOutput=Join-Path $testRoot 'context-output'
[IO.Directory]::CreateDirectory($contextSource)|Out-Null;[IO.Directory]::CreateDirectory($contextOutput)|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $contextSource 'observer.jsonl.measurements-0.jsonl'),"complete`npartial",$encoding)
[IO.File]::WriteAllText((Join-Path $contextSource 'observer.jsonl'),"observer`npartial",$encoding)
[IO.File]::WriteAllText((Join-Path $contextSource 'efu-runtime.jsonl'),"efu`n",$encoding)
[IO.File]::WriteAllText((Join-Path $contextSource 'launch.json'),'{"TEST_ONLY":true}',$encoding)
$reader=[RecorderLineStream]::new((Join-Path $contextOutput 'measurements-0.stream.jsonl'))
try {$update=$reader.Poll((Join-Path $contextSource 'observer.jsonl.measurements-0.jsonl'))}finally{$reader.Dispose()}
$orderedStreams=@([ordered]@{file='measurements-0.stream.jsonl';copiedBytes=$update.CopiedBytes;remainingBytesAtLastPoll=$update.BacklogBytes;detectedRotations=0;lastError=$null})
$saved=Save-RecorderContextSnapshots (New-TestMask @('measurements','logs')) $contextSource $contextOutput
Check ($saved.snapshots.Count -eq 8 -and $saved.snapshots[0] -is [Collections.Specialized.OrderedDictionary] -and $orderedStreams[0] -is [Collections.Specialized.OrderedDictionary]) 'Actual snapshot/stream fixture lost ordered dictionary metadata.'
Check ([IO.File]::ReadAllText((Join-Path $contextOutput 'measurements-0.snapshot.jsonl')) -ceq "complete`n" -and $saved.snapshots[0].partialTailBytesOmitted -eq 7) 'Actual snapshot helper lost the complete-line boundary.'
Check ($saved.snapshots[1].state -eq 'missing' -and [IO.File]::ReadAllText((Join-Path $contextOutput 'observer-snapshot.jsonl')) -ceq "observer`n" -and [IO.File]::Exists((Join-Path $contextOutput 'launch.json'))) 'Actual final snapshot plan did not preserve missing/tail/launch evidence.'
$context=Get-RecorderContextSourceSummary 'measurements' $saved.snapshots $orderedStreams
Check ($context.coverage.streamBytes -eq 9 -and $context.coverage.bytes -eq 18 -and $context.coverage.availableSnapshots -eq 1 -and $context.state -eq 'partial' -and $context.files.Count -eq 2) 'Ordered snapshot plus stream metadata failed shared byte aggregation.'
$context=Get-RecorderContextSourceSummary 'logs' $saved.snapshots $orderedStreams
Check ($context.coverage.streamBytes -eq 0 -and $context.coverage.bytes -eq 13 -and $context.coverage.availableSnapshots -eq 2 -and $context.state -eq 'partial' -and $context.files.Count -eq 2) 'Actual log snapshot metadata failed shared byte aggregation or hid missing SR observation files.'
$context=Get-RecorderContextSourceSummary 'measurements' @() @()
Check ($context.state -eq 'unavailable' -and $context.coverage.bytes -eq 0 -and $context.files.Count -eq 0) 'Empty actual context became usable data.'
$context=Get-RecorderContextSourceSummary 'measurements' @([ordered]@{key='measurements';file='large.snapshot';bytes=4000000000L;state='copied'}) @([ordered]@{file='large.stream';copiedBytes=6000000000L;remainingBytesAtLastPoll=0;detectedRotations=0;lastError=$null})
Check ($context.coverage.bytes -eq 10000000000L -and $context.coverage.bytes -is [long] -and $context.state -eq 'collected') 'Context byte totals overflowed Int64 or failed ordered metadata.'
try {throw 'TEST ONLY diagnostic fixture'}catch{Write-RecorderError $contextOutput 'test_save' $_}
$diagnostic=[IO.File]::ReadAllText((Join-Path $contextOutput 'error-details.jsonl'))|ConvertFrom-Json @recorderJsonOptions
Check ($diagnostic.phase -eq 'test_save' -and $diagnostic.scriptLineNumber -gt 0 -and $diagnostic.scriptStackTrace -like '*test-recorder.ps1*' -and $diagnostic.positionMessage) 'Error sidecar lost ErrorRecord line or script stack.'

function Write-TestRecorderJsonLines([string]$Path,[string]$Event,[int]$First,[int]$Count,[int]$Padding=0,[string]$Prefix='') {
    $writer=[IO.StreamWriter]::new($Path,$false,$encoding);$writer.NewLine="`n";$pad='x'*$Padding
    try {if($Prefix){$writer.Write($Prefix)};for($index=0;$index -lt $Count;$index++){$writer.WriteLine('{"event":"'+$Event+'","sequence":'+($First+$index)+',"padding":"'+$pad+'"}')}}finally{$writer.Dispose()}
}
$rotationSource=Join-Path $testRoot 'rotation-source';$rotationOutput=Join-Path $testRoot 'rotation-output';$disabledOutput=Join-Path $testRoot 'disabled-streams'
foreach($directory in @($rotationSource,$rotationOutput,$disabledOutput)){[IO.Directory]::CreateDirectory($directory)|Out-Null}
$defaultSelection=Resolve-RecorderCollectors ([pscustomobject]@{})
Check ($defaultSelection.Keys.Count -eq 9 -and $defaultSelection.presentmonRaw -and $defaultSelection.pmdp -and $defaultSelection.rtss -and $defaultSelection.hardware -and $defaultSelection.foreground -and $defaultSelection.measurements -and $defaultSelection.logs -and -not $defaultSelection.system -and -not $defaultSelection.threads) 'Additional log files changed the nine collector defaults.'
foreach($keys in @(@(),@('measurements'),@('logs'),@('measurements','logs'))) {
    $mask=New-TestMask $keys;$plan=@(Get-RecorderSnapshotPlan $mask)
    Check (@($plan|Where-Object key -eq 'measurements').Count -eq $(if($mask.measurements){4}else{0}) -and @($plan|Where-Object key -eq 'logs').Count -eq $(if($mask.logs){4}else{0})) 'New context files bypassed their existing collector switches.'
}
$script:streams=@();$script:streamEvents=$null;$script:srStreamEvents=$null;$script:warnings=[Collections.Generic.List[string]]::new()
Start-RecorderContextStreams (New-TestMask @()) $rotationSource $disabledOutput
Check ($script:streams.Count -eq 0 -and $null -eq $script:streamEvents -and $null -eq $script:srStreamEvents -and @(Get-ChildItem -LiteralPath $disabledOutput -File).Count -eq 0) 'Disabled context collectors created streams.'
foreach($index in 0,1){Write-TestRecorderJsonLines (Join-Path $rotationSource ('observer.jsonl.measurements-'+$index+'.jsonl')) 'detail' (1000+$index*3) 3 5}
$summary0=Join-Path $rotationSource 'observer.jsonl.measurements-summary-0.jsonl';$summary1=Join-Path $rotationSource 'observer.jsonl.measurements-summary-1.jsonl'
Write-TestRecorderJsonLines $summary0 'measurement_summary' 0 100 20000;Write-TestRecorderJsonLines $summary1 'measurement_summary' 100 100 20000
Write-TestRecorderJsonLines (Join-Path $rotationSource 'observer.jsonl.sr-observation-0.jsonl') 'sr_tail_commands' 2000 120 20000
Write-TestRecorderJsonLines (Join-Path $rotationSource 'observer.jsonl.sr-observation-1.jsonl') 'sr_tail_commands' 2120 2 5
Write-TestRecorderJsonLines (Join-Path $rotationSource 'observer.jsonl') 'observer' 0 1
Write-TestRecorderJsonLines (Join-Path $rotationSource 'efu-runtime.jsonl') 'efu' 0 1
Write-RecorderJson (Join-Path $rotationSource 'launch.json') ([ordered]@{TEST_ONLY=$true})
$contextSelection=New-TestMask @('measurements','logs');$rotationStreams=@()
try {
    Start-RecorderContextStreams $contextSelection $rotationSource $rotationOutput
    Check ($script:streams.Count -eq 6 -and @($script:streams|Where-Object key -eq 'measurements').Count -eq 4 -and @($script:streams|Where-Object key -eq 'logs').Count -eq 2 -and @($script:streams.file|Sort-Object -Unique).Count -eq 6) 'Detail/summary and SR did not create four plus two independent streams.'
    Update-RecorderStreams -Drain
    Write-TestRecorderJsonLines $summary0 'measurement_summary' 200 100 20000;Update-RecorderStreams -Drain
    Write-TestRecorderJsonLines $summary1 'measurement_summary' 300 100 20000;Update-RecorderStreams -Drain
    $summaryStream0=Join-Path $rotationOutput 'measurements-summary-0.stream.jsonl'
    Write-TestRecorderJsonLines $summary0 'measurement_summary' 400 1;Update-RecorderStreams -Drain
    $beforePartial=([IO.FileInfo]$summaryStream0).Length
    [IO.File]::AppendAllText($summary0,'{"event":"measurement_summary","sequence":401',$encoding);Update-RecorderStreams
    Check (([IO.FileInfo]$summaryStream0).Length -eq $beforePartial) 'An unfinished summary line escaped the production stream helper.'
    [IO.File]::AppendAllText($summary0,"}`n",$encoding);Update-RecorderStreams -Drain
    Check (([IO.FileInfo]$summaryStream0).Length -gt $beforePartial) 'Completed summary line was not copied after its partial prefix.'
    [IO.File]::AppendAllText($summary0,'{"event":"unfinished"',$encoding);Update-RecorderStreams -Drain
    $rotationStreams=@($script:streams|ForEach-Object {[ordered]@{key=$_.key;source=$_.source;file=$_.file;copiedBytes=$_.bytes;remainingBytesAtLastPoll=$_.backlogBytes;detectedRotations=$_.rotations;lastError=$_.lastError}})
}finally{foreach($entry in $script:streams){$entry.reader.Dispose()};if($script:streamEvents){$script:streamEvents.Dispose()};if($script:srStreamEvents){$script:srStreamEvents.Dispose()};$script:streams=@();$script:streamEvents=$null;$script:srStreamEvents=$null}
$summaryRows=@(foreach($index in 0,1){Get-Content -LiteralPath (Join-Path $rotationOutput ('measurements-summary-'+$index+'.stream.jsonl'))|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions}})
Check ($summaryRows.Count -eq 402 -and (@($summaryRows.sequence|Sort-Object)-join ',') -ceq ((0..401)-join ',') -and @($summaryRows|Where-Object event -ne 'measurement_summary').Count -eq 0) 'Summary rotations lost, duplicated or mixed complete lines.'
$rotationEvents=@(Get-Content -LiteralPath (Join-Path $rotationOutput 'measurement-stream-events.jsonl')|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions})
Check (@($rotationEvents|Where-Object {$_.rotation -and $_.rotationReason -eq 'changed_first_complete_line'}).Count -eq 2 -and @($rotationEvents|Where-Object {$_.rotation -and $_.rotationReason -eq 'size_reset'}).Count -eq 1) 'Real summary file replacements did not record both rotation reasons.'
foreach($index in 0,1){$detailRows=@(Get-Content -LiteralPath (Join-Path $rotationOutput ('measurements-'+$index+'.stream.jsonl'))|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions});Check ($detailRows.Count -eq 3 -and @($detailRows|Where-Object event -ne 'detail').Count -eq 0) 'Summary streaming changed or mixed original detail streams.'}
$rotationSaved=Save-RecorderContextSnapshots $contextSelection $rotationSource $rotationOutput
$summarySnapshot0=@($rotationSaved.snapshots|Where-Object file -eq 'measurements-summary-0.snapshot.jsonl')[0]
$summarySnapshotRows=@(Get-Content -LiteralPath (Join-Path $rotationOutput $summarySnapshot0.file)|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions})
Check ($rotationSaved.snapshots.Count -eq 8 -and $summarySnapshotRows.Count -eq 2 -and $summarySnapshotRows[0].sequence -eq 400 -and $summarySnapshotRows[1].sequence -eq 401 -and $summarySnapshot0.partialTailBytesOmitted -gt 0) 'Summary final snapshot retained a partial line or missed the latest generation.'
foreach($index in 0,1){$original=Join-Path $rotationSource ('observer.jsonl.sr-observation-'+$index+'.jsonl');$copy=Join-Path $rotationOutput ('sr-observation-'+$index+'.snapshot.jsonl');Check ((Get-RecorderSha256 $original) -ceq (Get-RecorderSha256 $copy)) 'SR observation snapshot truncated or changed retained file bytes.'}
Check (([IO.FileInfo](Join-Path $rotationOutput 'sr-observation-0.snapshot.jsonl')).Length -gt 2097152) 'SR observation fixture did not exercise full retention beyond the ordinary 2 MiB tail.'
$logContext=Get-RecorderContextSourceSummary 'logs' $rotationSaved.snapshots $rotationStreams
Check ($logContext.state -eq 'collected' -and $logContext.files.Count -eq 6 -and $logContext.coverage.bytes -gt 2097152) 'SR observation snapshots or streams were omitted from final log coverage.'
Write-RecorderJson (Join-Path $rotationOutput 'test-context-result.json') ([ordered]@{TEST_ONLY=$true;selectedCollectors=$contextSelection;summaryRows=$summaryRows.Count;measurementStreams=@($rotationStreams|Where-Object key -eq 'measurements');srObservationStreams=@($rotationStreams|Where-Object key -eq 'logs');finalSnapshots=$rotationSaved.snapshots;logCoverage=$logContext;newEtwSessions=0;gameRead=$false})

$srSource=Join-Path $testRoot 'sr-only-source';$srOutput=Join-Path $testRoot 'sr-only-output'
foreach($directory in @($srSource,$srOutput)){[IO.Directory]::CreateDirectory($directory)|Out-Null}
$archiveRows=@(
    '{"event":"sr_shader_archive_begin","archive_id":"owned-early","chunks":2}',
    '{"event":"sr_shader_archive_chunk","archive_id":"owned-early","chunk":0,"bytes_hex":"03022307"}',
    '{"event":"sr_shader_archive_chunk","archive_id":"owned-early","chunk":1,"bytes_hex":"00000100"}',
    '{"event":"sr_shader_archive_end","archive_id":"owned-early","chunks":2,"complete":true}')
$archiveText=($archiveRows -join "`n")+"`n"
$modeLookingRow='{"event":"measurement_status","pid":123,"qpc_ns":100,"control_epoch":1,"sdk_frames":1,"inflight_limit":2,"async":true,"shared_copy_path":true}'+"`n"
$srSource0=Join-Path $srSource 'observer.jsonl.sr-observation-0.jsonl';$srSource1=Join-Path $srSource 'observer.jsonl.sr-observation-1.jsonl'
Write-TestRecorderJsonLines $srSource0 'sr_tail_commands' 0 400 20000 ($archiveText+$modeLookingRow)
Write-TestRecorderJsonLines $srSource1 'sr_tail_commands' 400 400 20000
Write-TestRecorderJsonLines (Join-Path $srSource 'observer.jsonl') 'observer' 0 1
Write-TestRecorderJsonLines (Join-Path $srSource 'efu-runtime.jsonl') 'efu' 0 1
Write-RecorderJson (Join-Path $srSource 'launch.json') ([ordered]@{TEST_ONLY=$true})
$previousSrStatus=$script:status;$previousSrGame=$script:gamePid;$previousSrDirectory=$script:OutputDirectory;$previousSrLatest=$script:latestMeasurement;$previousSrWarnings=$script:warnings
$script:status=[ordered]@{mode='SR_ONLY_SENTINEL'};$script:gamePid=123;$script:OutputDirectory=$srOutput;$script:latestMeasurement=$null;$script:warnings=[Collections.Generic.List[string]]::new()
$srStreams=@();$srWarnings=@();$srSelection=New-TestMask @('logs')
try {
    Start-RecorderContextStreams $srSelection $srSource $srOutput
    Check ($script:streams.Count -eq 2 -and @($script:streams|Where-Object key -ne 'logs').Count -eq 0 -and $null -eq $script:streamEvents -and -not [IO.File]::Exists((Join-Path $srOutput 'measurement-stream-events.jsonl'))) 'Logs-only collection enabled measurement streams or omitted an SR stream.'
    Check (@($script:streams|Where-Object {$_.source -notmatch 'observer\.jsonl\.sr-observation-[01]\.jsonl$'}).Count -eq 0) 'Ordinary Observer/EFU tails were incorrectly streamed in full.'
    Update-RecorderStreams -Drain
    $probe=[RecorderLineStream]::new((Join-Path $srOutput 'TEST-ONLY-status-probe.jsonl'))
    try {$probeUpdate=$probe.Poll($srSource0,$false);Check (-not $probeUpdate.LatestStatus) 'Native SR reader parsed measurement_status mode data.'}finally{$probe.Dispose()}
    Write-TestRecorderJsonLines $srSource0 'sr_tail_commands' 800 400 20000;Update-RecorderStreams -Drain
    Write-TestRecorderJsonLines $srSource1 'sr_tail_commands' 1200 400 20000;Update-RecorderStreams -Drain
    Write-TestRecorderJsonLines $srSource0 'sr_tail_commands' 1600 1;Update-RecorderStreams -Drain
    $srStream0=Join-Path $srOutput 'sr-observation-0.stream.jsonl';$beforeSrPartial=([IO.FileInfo]$srStream0).Length
    [IO.File]::AppendAllText($srSource0,'{"event":"sr_tail_commands","sequence":1601',$encoding);Update-RecorderStreams
    Check (([IO.FileInfo]$srStream0).Length -eq $beforeSrPartial) 'An unfinished SR row escaped the stream.'
    [IO.File]::AppendAllText($srSource0,"}`n",$encoding);Update-RecorderStreams -Drain
    [IO.File]::AppendAllText($srSource0,'{"event":"unfinished"',$encoding);Update-RecorderStreams -Drain
    Check ($script:status.mode -ceq 'SR_ONLY_SENTINEL' -and $null -eq $script:latestMeasurement -and -not [IO.File]::Exists((Join-Path $srOutput 'measurement-status-latest.json'))) 'SR data changed measurement_status mode state.'
    $srStreams=@($script:streams|ForEach-Object {[ordered]@{key=$_.key;source=$_.source;file=$_.file;copiedBytes=$_.bytes;remainingBytesAtLastPoll=$_.backlogBytes;detectedRotations=$_.rotations;lastError=$_.lastError}})
    $srWarnings=@($script:warnings)
}finally{
    foreach($entry in $script:streams){$entry.reader.Dispose()};if($script:srStreamEvents){$script:srStreamEvents.Dispose()};$script:streams=@();$script:streamEvents=$null;$script:srStreamEvents=$null
    $script:status=$previousSrStatus;$script:gamePid=$previousSrGame;$script:OutputDirectory=$previousSrDirectory;$script:latestMeasurement=$previousSrLatest;$script:warnings=$previousSrWarnings
}
$retainedArchive=@(foreach($index in 0,1){foreach($line in [IO.File]::ReadLines((Join-Path $srOutput ('sr-observation-'+$index+'.stream.jsonl')))){if($line.Contains('"archive_id":"owned-early"')){$line|ConvertFrom-Json @recorderJsonOptions}}})
Check ($retainedArchive.Count -eq 4 -and $retainedArchive[0].event -ceq 'sr_shader_archive_begin' -and $retainedArchive[-1].event -ceq 'sr_shader_archive_end' -and $retainedArchive[-1].complete -and (@($retainedArchive|Where-Object event -eq 'sr_shader_archive_chunk').bytes_hex -join '') -ceq '0302230700000100') 'Early once-only archive begin/chunks/end were lost or duplicated across SR rotations.'
$srSaved=Save-RecorderContextSnapshots $srSelection $srSource $srOutput
$srContext=Get-RecorderContextSourceSummary 'logs' $srSaved.snapshots $srStreams
Check ($srContext.state -eq 'partial' -and $srContext.files.Count -eq 6 -and $srContext.coverage.streamBytes -gt 16777216 -and $srContext.coverage.detectedRotations -eq 3 -and $srContext.coverage.remainingStreamBytes -gt 0 -and $srContext.coverage.lossNote -like '*rotations between polls*' -and $srWarnings.Count -eq 3) 'SR summary lost independent stream bytes, rotation counts or possible-loss notes.'
foreach($index in 0,1){Check (-not [IO.File]::ReadAllText((Join-Path $srOutput ('sr-observation-'+$index+'.snapshot.jsonl'))).Contains('owned-early')) 'SR overwrite fixture did not remove the early archive from the final snapshots.'}
$srLast=@(Get-Content -LiteralPath (Join-Path $srOutput 'sr-observation-0.snapshot.jsonl')|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions})
Check ($srLast.Count -eq 2 -and $srLast[1].sequence -eq 1601) 'Final SR snapshot did not retain only complete current-generation rows.'
Check ((Get-RecorderContextSourceSummary 'measurements' @() $srStreams).coverage.streamBytes -eq 0) 'SR bytes leaked into measurement source coverage.'
Write-RecorderJson (Join-Path $srOutput 'test-sr-stream-result.json') ([ordered]@{TEST_ONLY=$true;selectedCollectors=$srSelection;srObservationStreams=$srStreams;archiveRows=$retainedArchive.Count;finalSnapshots=$srSaved.snapshots;sourceStatus=[ordered]@{logs=$srContext};warnings=$srWarnings;newEtwSessions=0;gameRead=$false})

$operationFixture=& {
    $powerSource=Join-Path $PSScriptRoot 'capture-power.ps1'
    . $powerSource -TargetPid 123 -TargetCreationFileTime 1 -OutputDirectory $testRoot -SelfTest -CaptureForeground
    Check ([bool]$CaptureOperationKeys) 'Direct legacy power tool changed its default keyboard polling behavior.'
    $disabledLegacy=& {. $powerSource -TargetPid 123 -TargetCreationFileTime 1 -OutputDirectory $testRoot -SelfTest -CaptureForeground -CaptureOperationKeys:$false;[bool]$CaptureOperationKeys}
    Check (-not $disabledLegacy) 'Explicit keyboard polling disable was not accepted by the passive power tool.'
    $powerTokens=$null;$powerErrors=$null;$powerAst=[Management.Automation.Language.Parser]::ParseFile($powerSource,[ref]$powerTokens,[ref]$powerErrors)
    Check ($powerErrors.Count -eq 0) 'Power sampler syntax failed.'
    $nativeSource=@($powerAst.FindAll({param($node)$node -is [Management.Automation.Language.StringConstantExpressionAst] -and $node.Value.Contains('public static class RecorderForeground')},$true))[0].Value
    Check ([regex]::Matches($nativeSource,'GetAsyncKeyState\(0x(?:4D|1B)\)').Count -eq 2 -and [regex]::Matches($nativeSource,'GetAsyncKeyState').Count -eq 3) 'Native input reader is not limited to the two literal M/Esc virtual keys.'
    if(-not ('RecorderForeground' -as [type])){Add-Type -TypeDefinition $nativeSource}
    # Compile the native declaration, but invoke only this owned mock callback.
    $keyState=[ordered]@{known=$false;mask=0;qpcTicks=0L;utc=$null;polls=0;transitions=0}
    $keyRows=[Collections.Generic.List[object]]::new();$mock=[pscustomobject]@{reads=0;mask=0;ticks=0L;utc=$null;returnNull=$false}
    $baseUtc=[DateTime]::new(2026,1,1,0,0,0,[DateTimeKind]::Utc)
    $readKeys={
        $mock.reads++;if($mock.returnNull){return $null}
        [pscustomobject]@{Mask=$mock.mask;QpcTicks=$mock.ticks;Utc=$mock.utc;UnrelatedKey='MUST_NOT_RECORD_OTHER_KEYS'}
    }
    $disabledState=[ordered]@{known=$false;mask=0;qpcTicks=0L;utc=$null;polls=0;transitions=0}
    $disabledRows=@(Get-PowerOperationTransitions $disabledState ([ordered]@{targetForeground=$true}) $true $readKeys $false)
    Check ($disabledRows.Count -eq 0 -and $mock.reads -eq 0 -and $disabledState.polls -eq 0 -and -not (Get-PowerOperationKeyObservation $false $disabledState 'foreground.jsonl').enabled) 'Disabled foreground operation-key observation queried the keyboard or claimed enabled coverage.'
    function Invoke-TestKeySample([int]$Mask,[bool]$Foreground=$true,[bool]$Identity=$true,[bool]$Unavailable=$false) {
        $mock.mask=$Mask;$mock.ticks+=500;$mock.utc=$baseUtc.AddMilliseconds($mock.ticks).ToString('o');$mock.returnNull=$Unavailable
        $sample=[ordered]@{TEST_ONLY=$true;targetProcessId=123;targetForeground=$Foreground;qpcTicks=$mock.ticks;qpcFrequency=1000;utc=$mock.utc}
        $sample['operationKeyTransitions']=@(Get-PowerOperationTransitions $keyState $sample $Identity $readKeys)
        $keyRows.Add($sample);$sample.operationKeyTransitions
    }
    Check (@(Invoke-TestKeySample 0).Count -eq 0) 'First input sample fabricated an edge.'
    $edge=@(Invoke-TestKeySample 1);Check ($edge.Count -eq 1 -and $edge[0].key -ceq 'M' -and $edge[0].state -ceq 'down' -and $edge[0].qpcTicks -eq 1000 -and $edge[0].previousQpcTicks -eq 500 -and $edge[0].utc -ceq '2026-01-01T00:00:01.0000000Z') 'M down lost its sampled QPC/UTC or previous observation boundary.'
    Check (@(Invoke-TestKeySample 1).Count -eq 0) 'Held key generated repeated down events.'
    $edge=@(Invoke-TestKeySample 0);Check ($edge.Count -eq 1 -and $edge[0].key -ceq 'M' -and $edge[0].state -ceq 'up') 'M up was not retained.'
    $edge=@(Invoke-TestKeySample 2);Check ($edge.Count -eq 1 -and $edge[0].key -ceq 'Esc' -and $edge[0].state -ceq 'down') 'Esc down was not retained.'
    $edge=@(Invoke-TestKeySample 0);Check ($edge.Count -eq 1 -and $edge[0].key -ceq 'Esc' -and $edge[0].state -ceq 'up') 'Esc up was not retained.'
    Check (@(Invoke-TestKeySample 3).Count -eq 2) 'Two changed keys in one poll were not retained separately.'
    $before=$mock.reads;Check (@(Invoke-TestKeySample 0 $false).Count -eq 0 -and $mock.reads -eq $before -and -not $keyState.known) 'Background scope queried keys or fabricated releases.'
    Check (@(Invoke-TestKeySample 3).Count -eq 0) 'Focus regain fabricated new presses for already held keys.'
    $edge=@(Invoke-TestKeySample 0);Check ($edge.Count -eq 2 -and @($edge|Where-Object state -ne 'up').Count -eq 0) 'Foreground releases after a held baseline were lost.'
    $before=$mock.reads;Check (@(Invoke-TestKeySample 3 $true $false).Count -eq 0 -and $mock.reads -eq $before -and -not $keyState.known) 'Changed target identity queried keyboard input.'
    Check (@(Invoke-TestKeySample 1).Count -eq 0) 'Identity recovery fabricated a down edge.'
    Check (@(Invoke-TestKeySample 0 $true $true $true).Count -eq 0 -and -not $keyState.known) 'Unconfirmed native foreground sample retained an input baseline.'
    Check (@(Invoke-TestKeySample 0).Count -eq 0) 'Unreadable sample fabricated a later release.'
    Check (@(Invoke-TestKeySample 4).Count -eq 0 -and -not $keyState.known) 'Unexpected key bits escaped the M/Esc whitelist.'
    Check (@(Invoke-TestKeySample 0).Count -eq 0) 'Invalid input recovery fabricated an edge.'
    $allEdges=@($keyRows|ForEach-Object {$_.operationKeyTransitions});$text=$keyRows.ToArray()|ConvertTo-Json -Depth 8 -Compress
    Check ($allEdges.Count -eq 8 -and (@($allEdges.key|Sort-Object -Unique)-join ',') -ceq 'Esc,M' -and $text -notlike '*MUST_NOT_RECORD_OTHER_KEYS*' -and $text -notlike '*UnrelatedKey*') 'Operation log retained unrelated keyboard data.'
    $coverage=Get-PowerOperationKeyObservation $true $keyState 'foreground.jsonl'
    Check ($coverage.samplePeriodMs -eq 500 -and -not $coverage.exactEventTiming -and $coverage.shortPressesMayBeMissed -and $coverage.transitions -eq 8 -and $coverage.samePollKeyOrder -ceq 'unknown') 'Polling coverage claimed exact or complete key events.'
    [ordered]@{rows=$keyRows.ToArray();coverage=$coverage;mockReads=$mock.reads;nativeKeyboardReads=0}
}
Write-RecorderJson (Join-Path $testRoot 'operation-key-mock-result.json') $operationFixture

$recoverySource=New-TestRequest 'offline-recovery-fixture' (New-TestMask @('presentmonRaw','pmdp','foreground','measurements','logs'))
Write-RecorderJson (Join-Path $recoverySource 'request.json') ([ordered]@{utc='2026-01-01T00:00:00Z';qpcTicks=10000;qpcFrequency=1000;label='K2';labelCode='k2';notes='offline recovery fixture';xiaomiPerformanceMode='均衡';collectors=(New-TestMask @('presentmonRaw','pmdp','foreground','measurements','logs'))})
$fixtureStatus=[ordered]@{};foreach($key in (New-TestMask @()).Keys){$enabled=$key -in @('presentmonRaw','pmdp','foreground','measurements','logs');$fixtureStatus[$key]=[ordered]@{selected=$enabled;status=$(if($key -in @('pmdp','foreground')){'collected'}elseif($enabled){'collecting'}else{'disabled'});message='TEST ONLY';files=$(if($key -eq 'pmdp'){@('frames.csv')}elseif($key -eq 'foreground'){@('foreground.jsonl')}else{@()});coverage=$(if($key -eq 'foreground'){[ordered]@{operationKeyObservation=$operationFixture.coverage}}else{$null})}}
Write-RecorderJson (Join-Path $recoverySource 'status.json') ([ordered]@{state='failed';saveError='TEST ONLY original save failure';sourceStatus=$fixtureStatus;qpcFrequency=1000;startQpcTicks=11000;recordingReadyQpcTicks=11200;startedUtc='2026-01-01T00:00:01Z';updatedUtc='2026-01-01T00:00:03.5Z';durationSeconds=3;gamePid=123;sessionId='TEST-ONLY';mode='TEST ONLY';presentMonSource='existing_pmdp';independentPresentMonRaw=$false;presentMonFallbackReason='TEST ONLY unavailable raw'})
Write-RecorderJson (Join-Path $recoverySource 'stop.request') ([ordered]@{utc='2026-01-01T00:00:03.5Z';qpcTicks=13500;qpcFrequency=1000;reason='user'})
Write-RecorderJson (Join-Path $recoverySource 'game-identity.json') ([ordered]@{pid=123;path='TEST ONLY Endfield.exe';creationFileTime='1'})
Write-RecorderJson (Join-Path $recoverySource 'session-discovery.json') ([ordered]@{state='absent';match=$null})
Write-RecorderJson (Join-Path $recoverySource 'pmdp-run.json') ([ordered]@{rows=1;stopMarkerObserved=$true;newEtwSessions=0})
[IO.File]::WriteAllText((Join-Path $recoverySource 'frames.csv'),"time`n1`n",$encoding)
[IO.File]::Copy((Join-Path $contextOutput 'measurements-0.snapshot.jsonl'),(Join-Path $recoverySource 'measurements-0.snapshot.jsonl'))
foreach($name in @('measurements-summary-0.stream.jsonl','measurements-summary-1.stream.jsonl','measurements-summary-0.snapshot.jsonl','measurements-summary-1.snapshot.jsonl','measurement-stream-events.jsonl')){[IO.File]::Copy((Join-Path $rotationOutput $name),(Join-Path $recoverySource $name))}
foreach($name in @('sr-observation-0.stream.jsonl','sr-observation-1.stream.jsonl','sr-observation-0.snapshot.jsonl','sr-observation-1.snapshot.jsonl','sr-observation-stream-events.jsonl')){[IO.File]::Copy((Join-Path $srOutput $name),(Join-Path $recoverySource $name))}
$keyWriter=[IO.StreamWriter]::new((Join-Path $recoverySource 'foreground.jsonl'),$false,$encoding)
try {foreach($row in $operationFixture.rows){$keyWriter.WriteLine(($row|ConvertTo-Json -Depth 8 -Compress))}}finally{$keyWriter.Dispose()}
Write-RecorderJson (Join-Path $recoverySource 'power-run.json') ([ordered]@{TEST_ONLY=$true;samples=$operationFixture.rows.Count;captureHardware=$false;captureForeground=$true;files=@('foreground.jsonl');operationKeyObservation=$operationFixture.coverage})
[IO.File]::WriteAllText((Join-Path $recoverySource 'markers.jsonl'),"{`"sequence`":1,`"qpcTicks`":12000,`"qpcFrequency`":1000,`"utc`":`"2026-01-01T00:00:02Z`",`"kind`":`"manual`"}`n",$encoding)
Write-RecorderJson (Join-Path $recoverySource 'markers.closed.json') ([ordered]@{closed=$true;accepted=1;written=1;dropped=0})
foreach($phase in @('start','stop')){Write-RecorderJson (Join-Path $recoverySource ('configuration-'+$phase+'.json')) (Get-RecorderConfigurationSnapshot $phase $true $null)}
$shell=$powerShellRuntime.executable
$recoveryWorker=Start-RecorderOwned 'test-offline-recovery' $shell @('-NoProfile','-NonInteractive','-File',(Join-Path $PSScriptRoot 'recover-recording.ps1'),'-SourceDirectory',$recoverySource) $testRoot 'recovery.stdout.json' 'recovery.stderr.txt'
$recoveryResult=Complete-RecorderOwned $recoveryWorker 10000
Check ($recoveryResult.exitCode -eq 0 -and -not $recoveryResult.forcedTermination) ('Offline fixture recovery failed: '+[IO.File]::ReadAllText((Join-Path $testRoot 'recovery.stderr.txt')))
$recoveryRuntime=[IO.File]::ReadAllText((Join-Path ($recoverySource+'.recovered') 'recovery.json'))|ConvertFrom-Json @recorderJsonOptions
Check-PowerShellRuntime $recoveryRuntime.powerShellRuntime $recoveryResult.pid
$recovered=$recoverySource+'.recovered';$recoveryManifest=[IO.File]::ReadAllText((Join-Path $recovered 'manifest.json'))|ConvertFrom-Json @recorderJsonOptions
$recoveryWindow=[IO.File]::ReadAllText((Join-Path $recovered 'window.json'))|ConvertFrom-Json @recorderJsonOptions
$recoverySummary=[IO.File]::ReadAllText((Join-Path $recovered 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
Check ($recoveryManifest.recovered -and $recoveryManifest.allOriginalInputHashesMatch -and $recoveryWindow.startQpcTicks -eq 11000 -and $recoveryWindow.endQpcTicks -eq 13500 -and $recoveryWindow.durationSeconds -eq 2.5) 'Recovery did not preserve the original QPC boundaries.'
Check (-not $recoverySummary.independentPresentMonRaw -and $recoverySummary.presentMonSource -eq 'existing_pmdp' -and $null -eq $recoverySummary.collectors[0].exitCode -and $null -eq $recoverySummary.collectors[0].forcedTermination -and -not $recoverySummary.collectors[0].exitStatusKnown -and $recoverySummary.collectors[0].report.stopMarkerObserved) 'Recovery fabricated raw data or collector exit evidence.'
Check (-not [IO.File]::Exists((Join-Path $recovered 'presentmon.csv')) -and $recoverySummary.frameSources.pmdp.rows -eq 1 -and $recoverySummary.sourceStatus.measurements.status -eq 'partial') 'Recovery lost retained data or presented recovered context as fully known.'
Check ($recoveryWindow.measurementStreams.Count -eq 2 -and $recoveryWindow.srObservationStreams.Count -eq 2 -and $recoverySummary.srObservationStreams.Count -eq 2 -and @($recoverySummary.sourceStatus.measurements.files|Where-Object {$_ -like 'measurements-summary-*'}).Count -eq 4 -and @($recoverySummary.sourceStatus.logs.files|Where-Object {$_ -like 'sr-observation-*'}).Count -eq 4 -and $recoverySummary.sourceStatus.logs.coverage.detectedRotations -eq 3 -and $recoverySummary.sourceStatus.logs.coverage.lossNote) 'Recovery metadata omitted retained summary/SR streams or rotation/loss evidence.'
Check ($recoverySummary.sourceStatus.foreground.coverage.operationKeyObservation.transitions -eq 8 -and @($recoverySummary.collectors|Where-Object kind -eq 'power')[0].report.operationKeyObservation.samplePeriodMs -eq 500 -and $recoveryManifest.files.file -contains 'foreground.jsonl') 'Summary or ZIP manifest lost the polled M/Esc observation data or coverage limits.'
Check ($recoverySummary.markerCount -eq 1 -and $recoverySummary.label -ceq 'K2' -and $recoverySummary.notes -ceq 'offline recovery fixture' -and $recoverySummary.xiaomiPerformanceMode -ceq '均衡' -and $recoveryManifest.files.file -contains 'markers.jsonl' -and $recoveryManifest.files.file -contains 'markers.closed.json' -and $recoveryManifest.files.file -contains 'summary.md' -and $recoveryManifest.files.file -contains 'summary.analysis.json' -and $recoverySummary.configurationSnapshots.Count -eq 2) 'Offline recovery lost marker/context evidence or failed to regenerate its readable summary.'
foreach($input in $recoveryManifest.originalInputFiles){Check ((Get-RecorderSha256 (Join-Path $recoverySource $input.sourceFile)) -ceq $input.sha256 -and (Get-RecorderSha256 (Join-Path $recovered $input.copiedFile)) -ceq $input.sha256) ('Recovery changed input bytes: '+$input.sourceFile)}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::OpenRead($recovered+'.zip')
try {
    foreach($file in $recoveryManifest.files) {
        $entry=$archive.GetEntry($file.file);Check ($null -ne $entry) ('Recovered ZIP omitted '+$file.file)
        $inputStream=$entry.Open();$hash=[Security.Cryptography.SHA256]::Create()
        try {$actual=([BitConverter]::ToString($hash.ComputeHash($inputStream))).Replace('-','').ToLowerInvariant()}finally{$hash.Dispose();$inputStream.Dispose()}
        Check ($actual -ceq $file.sha256) ('Recovered ZIP hash mismatch: '+$file.file)
    }
    Check ($null -ne $archive.GetEntry('recovery-original/original-status.json') -and $null -ne $archive.GetEntry('manifest.json')) 'Recovered ZIP lost original failure status or manifest.'
}finally{$archive.Dispose()}
Refuses {& (Join-Path $PSScriptRoot 'recover-recording.ps1') -SourceDirectory $recoverySource} 'Offline recovery overwrote an existing recovered folder.'
. (Join-Path $PSScriptRoot 'test-recorder-summary.ps1')
if($SaveRegressionOnly) {
    $report=[ordered]@{passed=$true;checks=$checks;saveRegressionOnly=$true;powerShellRuntime=$powerShellRuntime;powerShellVersion=$PSVersionTable.PSVersion.ToString();newEtwSessions=0;gameStarted=$false;collectorsStarted=$false;outputDirectory=$testRoot;completedUtc=[DateTime]::UtcNow.ToString('o')}
    Write-RecorderJson (Join-Path $testRoot 'test-result.json') $report
    $report|ConvertTo-Json
    exit
}

$identity=[RecorderNative]::Read($PID)
$defaults=Resolve-RecorderCollectors ([pscustomobject]@{})
Check ($defaults.presentmonRaw -and $defaults.pmdp -and $defaults.rtss -and $defaults.hardware -and $defaults.foreground -and $defaults.measurements -and $defaults.logs -and -not $defaults.system -and -not $defaults.threads) 'Legacy requests changed their default source selection.'
$sparse=Resolve-RecorderCollectors ([pscustomobject]@{collectors=[pscustomobject]@{threads=$true}})
Check ($sparse.threads -and @($sparse.Values|Where-Object {$_}).Count -eq 1) 'Missing collector keys implicitly enabled another source.'
foreach($invalid in @([pscustomobject]@{},[pscustomobject]@{hardware='true'},[pscustomobject]@{hardware=1},[pscustomobject]@{hardware=$null},[pscustomobject]@{Hardware=$true},[pscustomobject]@{unknown=$true},$null,@($true))) {
    Refuses {Resolve-RecorderCollectors ([pscustomobject]@{collectors=$invalid})} 'Collector schema accepted empty, unknown, wrong-case or non-boolean input.'
}

$helperRoot=Join-Path $testRoot 'mock-helpers';[IO.Directory]::CreateDirectory($helperRoot)|Out-Null
foreach($file in @('capture-power.ps1','capture-system.ps1','pmdp-reader.exe','rtss-poll.exe')){[IO.File]::WriteAllText((Join-Path $helperRoot $file),'TEST ONLY')}
foreach($keys in @(@('presentmonRaw'),@('pmdp'),@('rtss'),@('hardware'),@('foreground'),@('system'),@('threads'),@('measurements'),@('logs'),@('hardware','threads','logs'),@('foreground','system','measurements'),@('presentmonRaw','pmdp','rtss','hardware','foreground','system','threads','measurements','logs'))) {
    $mask=New-TestMask $keys;$plan=@(Get-RecorderHelperPlan $mask $PID $identity.Created $testRoot $helperRoot 'TEST-ONLY-shell')
    $launches=@(& {
        function Start-RecorderOwned($Kind,$Exe,$Arguments,$Directory,$Stdout,$Stderr){[pscustomobject]@{kind=$Kind;exe=$Exe;arguments=$Arguments;stdout=$Stdout;stderr=$Stderr}}
        foreach($entry in $plan){Start-RecorderPlannedHelper $entry $testRoot}
    })
    $expected=@();if($mask.hardware -or $mask.foreground){$expected+='power'};if($mask.rtss){$expected+='rtss'};if($mask.pmdp){$expected+='pmdp'};if($mask.system -or $mask.threads){$expected+='system'}
    Check ((@($launches|ForEach-Object {$_.kind}) -join ',') -ceq ($expected -join ',')) ('Selected helper launch mismatch for '+($keys -join ','))
    foreach($launch in $launches) {
        Check ($launch.arguments -contains 'stop.request' -and $launch.arguments -contains ($launch.kind+'.ready.json')) 'Selected helper lost its owned Stop/ready markers.'
        if($launch.kind -eq 'power'){Check (($launch.arguments -contains '-CaptureHardware') -eq $mask.hardware -and ($launch.arguments -contains '-CaptureForeground') -eq $mask.foreground) 'Power helper flags enabled an unselected source.'}
        if($launch.kind -eq 'system'){Check (($launch.arguments -contains '-CaptureSystem') -eq $mask.system -and ($launch.arguments -contains '-CaptureThreads') -eq $mask.threads) 'System helper flags enabled an unselected source.'}
    }
    $snapshotPlan=@(Get-RecorderSnapshotPlan $mask)
    Check (@($snapshotPlan|Where-Object {$_.key -eq 'measurements'}).Count -eq $(if($mask.measurements){4}else{0}) -and @($snapshotPlan|Where-Object {$_.key -eq 'logs'}).Count -eq $(if($mask.logs){4}else{0})) 'Snapshot plan included an unselected measurement/log source.'
}

$hashFixture=Join-Path $testRoot 'hash-fixture.txt';[IO.File]::WriteAllText($hashFixture,'abc',[Text.Encoding]::ASCII)
Check ((Get-RecorderSha256 $hashFixture) -ceq 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') 'Module-independent SHA256 failed the known abc digest.'
Check ([RecorderNative]::Same($PID,$identity.Created)) 'Live exact owner identity failed.'
Check (-not [RecorderNative]::Same($PID,$identity.Created+1)) 'Creation identity mismatch was accepted.'
$own='EndfieldRecorder-'+[guid]::NewGuid().ToString('N')
Check (Test-RecorderOwnSessionName $own $own) 'Own GUID was rejected.'
foreach($name in @('PresentMon','ACE','EndfieldRecorder-abc','EndfieldRecorder-'+[guid]::NewGuid().ToString('N'))) {Check (-not (Test-RecorderOwnSessionName $name $own)) ('Cleanup accepted unowned session '+$name)}

$good=[pscustomobject]@{readOnly=$true;sessionsQuery=[pscustomobject]@{status=0;reportedCount=1;capacity=80};providers=@([pscustomobject]@{name='Microsoft-Windows-Kernel-Process';status=0;instances=@([pscustomobject]@{enables=@([pscustomobject]@{enabled=1;loggerId=9},[pscustomobject]@{enabled=1;loggerId=9})})})}
Check ((Get-RecorderInventorySummary $good).kernelProcessEnabledLoggerIds.Count -eq 1) 'ETW fanout must count unique logger IDs.'
foreach($kind in @('query','provider')) {
    $bad=$good|ConvertTo-Json -Depth 10|ConvertFrom-Json @recorderJsonOptions
    if($kind -eq 'query'){$bad.sessionsQuery.status=234}
    if($kind -eq 'provider'){$bad.providers[0].status=5}
    Refuses {Get-RecorderInventorySummary $bad} ('ETW '+$kind+' rejection failed.')
}
$full=$good|ConvertTo-Json -Depth 10|ConvertFrom-Json @recorderJsonOptions
$full.providers[0].instances[0].enables=@(1..8|ForEach-Object {[pscustomobject]@{enabled=1;loggerId=$_}})
Check (-not (Get-RecorderInventorySummary $full).kernelProcessHeadroomBelowEight) 'Eight existing ETW loggers were not marked for passive fallback.'

$streamSource=Join-Path $testRoot 'measurement-source.jsonl';$streamFile=Join-Path $testRoot 'measurement-stream.jsonl'
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($streamSource,"first`npartial",$encoding)
$reader=[RecorderLineStream]::new($streamFile)
try {
    $update=$reader.Poll($streamSource);Check ($update.CopiedBytes -eq 6 -and [RecorderNative]::ReadText($streamFile,1048576,$false) -ceq "first`n") 'Partial line escaped the stream.'
    [IO.File]::AppendAllText($streamSource,"-complete`n",$encoding);$update=$reader.Poll($streamSource)
    Check ([RecorderNative]::ReadText($streamFile,1048576,$false) -ceq "first`npartial-complete`n") 'Completed partial line was lost or duplicated.'
    [IO.File]::WriteAllText($streamSource,"replacement-first-is-longer`nsecond`n",$encoding);$update=$reader.Poll($streamSource)
    Check ($update.Rotated -and $update.RotationReason -eq 'changed_first_complete_line' -and $update.Generation -eq 1) 'Same-or-larger rotation was missed.'
    [IO.File]::WriteAllText($streamSource,"x`n",$encoding);$update=$reader.Poll($streamSource)
    Check ($update.Rotated -and $update.RotationReason -eq 'size_reset' -and $update.Generation -eq 2) 'Size reset was missed.'
    Check ([RecorderNative]::ReadText($streamFile,1048576,$false).EndsWith("second`nx`n")) 'Rotation content was not preserved.'
}finally{$reader.Dispose()}
$snapshot=Join-Path $testRoot 'freeze.jsonl';[IO.File]::WriteAllText($streamSource,"complete`ntail",$encoding)
$freeze=[RecorderNative]::Freeze($streamSource,$snapshot)
Check ($freeze.PartialTailBytes -eq 4 -and [IO.File]::ReadAllText($snapshot) -ceq "complete`n") 'Freeze did not retain only complete lines.'

$sessionRoot=Join-Path $testRoot 'sessions';[IO.Directory]::CreateDirectory($sessionRoot)|Out-Null
$sid=[guid]::NewGuid().ToString('N');$sessionDir=Join-Path $sessionRoot $sid;[IO.Directory]::CreateDirectory($sessionDir)|Out-Null
Write-RecorderJson (Join-Path $sessionDir 'launch.json') ([ordered]@{startedAt=[DateTime]::FromFileTimeUtc([long]$identity.Created).AddSeconds(-2).ToString('o');exactGamePath=$identity.ImagePath})
[IO.File]::WriteAllText((Join-Path $sessionDir 'observer.jsonl'),('{"event":"create_instance","pid":'+$PID+"}`n"),$encoding)
Check ((Find-RecorderSession $sessionRoot $PID $identity).match.sessionId -ceq $sid) 'Exact launch-time and observer PID session was not selected.'
Check ((Find-RecorderSession $sessionRoot ($PID+1) $identity).state -ceq 'absent') 'Different observer PID was guessed as current.'
$another=Join-Path $sessionRoot ([guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($another)|Out-Null
[IO.File]::Copy((Join-Path $sessionDir 'launch.json'),(Join-Path $another 'launch.json'));[IO.File]::Copy((Join-Path $sessionDir 'observer.jsonl'),(Join-Path $another 'observer.jsonl'))
Check ((Find-RecorderSession $sessionRoot $PID $identity).state -ceq 'ambiguous') 'Ambiguous sessions were guessed by recency.'
$stopFixture=New-TestRequest 'owner-exit-check';$autoStop=Get-RecorderStop (Join-Path $stopFixture 'stop.request') 0 $identity.Created 0 0 0
Check ($autoStop.reason -eq 'recorder_closed' -and [IO.File]::Exists((Join-Path $stopFixture 'stop.request'))) 'Owner exit did not signal owned helpers.'
$stopFixture=New-TestRequest 'game-exit-check';$autoStop=Get-RecorderStop (Join-Path $stopFixture 'stop.request') $PID $identity.Created 2147483647 1 0
Check ($autoStop.reason -eq 'game_exited') 'Exact game identity loss did not stop capture.'

$folder=New-TestRequest 'stop-before-record'
Write-RecorderJson (Join-Path $folder 'stop.request') ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;reason='cpu_test'})
$worker=Start-TestWorker $folder $identity.Created $false;$state=Wait-TestState $folder @('complete','failed');$result=Complete-RecorderOwned $worker
Check (@($result).Count -eq 1 -and $result -is [Collections.IDictionary] -and $result.kind -ceq 'test-worker') 'Owned process completion leaked output instead of one collector metadata object.'
Check ([object]::ReferenceEquals($result,(Complete-RecorderOwned $worker))) 'Cached owned process completion changed its metadata object.'
Write-RecorderJson (Join-Path $testRoot 'collector-metadata.json') ([ordered]@{collectors=@($result)})
$metadata=[IO.File]::ReadAllText((Join-Path $testRoot 'collector-metadata.json'))|ConvertFrom-Json @recorderJsonOptions
Check ($metadata.collectors.Count -eq 1 -and $metadata.collectors[0].kind -ceq 'test-worker') 'Collector metadata JSON contains leaked completion output.'
Check ($state.state -eq 'complete' -and $result.exitCode -eq 0) 'Stop-before-record did not complete cleanly.'
$summary=[IO.File]::ReadAllText((Join-Path $folder 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
Check-PowerShellRuntime $state.powerShellRuntime $result.pid
Check-PowerShellRuntime $summary.powerShellRuntime $result.pid
Check (-not $summary.recordingStarted -and $summary.collectors.Count -eq 0 -and -not [IO.File]::Exists((Join-Path $folder 'TEST-ONLY-synthetic.csv'))) 'Stop-before-record started a collector.'

$folder=New-TestRequest 'identity-mismatch';$worker=Start-TestWorker $folder ($identity.Created+1) $true;$state=Wait-TestState $folder @('failed');$result=Complete-RecorderOwned $worker
Check ($result.exitCode -ne 0 -and $state.message -like '*owner PID/creation identity*') 'Invalid owner was not rejected with saved failure status.'

$folder=New-TestRequest ('manual-stop space '+[char]0x6d4b);$worker=Start-TestWorker $folder $identity.Created $true;$state=Wait-TestState $folder @('recording','failed')
Check ($state.state -eq 'recording' -and $state.mode -like 'TEST ONLY*simulat*' -and $state.sessionId -cmatch '^simulated-[0-9a-f]{32}$') 'Simulation did not report explicit TEST ONLY simulated recording identity.'
$stopQpc=[Diagnostics.Stopwatch]::GetTimestamp();Write-RecorderJson (Join-Path $folder 'stop.request') ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=$stopQpc;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;reason='cpu_test'})
$state=Wait-TestState $folder @('complete','failed');$result=Complete-RecorderOwned $worker
Check ($result.exitCode -eq 0 -and $state.state -eq 'complete') 'Manual Stop simulation failed.'
$window=[IO.File]::ReadAllText((Join-Path $folder 'window.json'))|ConvertFrom-Json @recorderJsonOptions
Check ($window.endQpcTicks -eq $stopQpc -and $window.testOnly -and [IO.File]::Exists($state.zipPath)) 'Exact stop QPC or TEST ONLY ZIP was not saved.'
Check (([Diagnostics.Stopwatch]::GetTimestamp()-$stopQpc)/[double][Diagnostics.Stopwatch]::Frequency -lt 8) 'Manual Stop exceeded the bounded save latency in the CPU fixture.'

$folder=New-TestRequest 'metadata-markers-summary-worker' (New-TestMask @('pmdp','hardware','measurements'))
$request=[IO.File]::ReadAllText((Join-Path $folder 'request.json'))|ConvertFrom-Json @recorderJsonOptions
foreach($entry in ([ordered]@{label='同步+共享';labelCode='sync-shared';notes='metadata integration';xiaomiPerformanceMode='狂暴';measurementMode='summary';capturePowerState=$true;markerProtocol=[ordered]@{schema=1;writer='gui';closeBeforeStop=$true}}).GetEnumerator()){$request|Add-Member -NotePropertyName $entry.Key -NotePropertyValue $entry.Value}
Write-RecorderJson (Join-Path $folder 'request.json') $request
$worker=Start-TestWorker $folder $identity.Created $true;$state=Wait-TestState $folder @('recording','failed')
$workerIdentity=[RecorderNative]::Read($worker.process.Id)
Check ($state.state -eq 'recording' -and $state.workerPid -eq $worker.process.Id -and $state.backendPid -eq $worker.process.Id -and [uint64]$state.workerCreationFileTime -eq $workerIdentity.Created -and [uint64]$state.backendCreationFileTime -eq $workerIdentity.Created -and $state.recordingReadyQpcTicks -ge $state.startQpcTicks) 'GUI readiness identity fields did not match the exact worker process.'
Start-Sleep -Milliseconds 150
$mark=[ordered]@{sequence=1;qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;utc=[DateTime]::UtcNow.ToString('o');kind='manual';testOnly=$true}
[IO.File]::WriteAllText((Join-Path $folder 'markers.jsonl'),(($mark|ConvertTo-Json -Compress)+"`n"),$encoding)
Write-RecorderJson (Join-Path $folder 'markers.closed.json') ([ordered]@{closed=$true;accepted=1;written=1;dropped=0;error=$null})
Write-RecorderJson (Join-Path $folder 'stop.request') ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;reason='metadata_marker_test'})
$state=Wait-TestState $folder @('complete','failed');$result=Complete-RecorderOwned $worker
$summary=[IO.File]::ReadAllText((Join-Path $folder 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
$manifest=[IO.File]::ReadAllText((Join-Path $folder 'manifest.json'))|ConvertFrom-Json @recorderJsonOptions
Check ($result.exitCode -eq 0 -and $summary.markerCount -eq 1 -and $summary.markerClose.closed -and $summary.label -ceq '同步+共享' -and $summary.notes -ceq 'metadata integration' -and $summary.xiaomiPerformanceMode -ceq '狂暴' -and -not $summary.detailIncluded -and $summary.measurementMode -ceq 'summary') 'Worker save lost the request metadata, marker close state or summary-only detail selection.'
Check ($summary.powerState.samples -gt 0 -and $manifest.files.file -contains 'power-state.jsonl' -and $manifest.files.file -contains 'configuration-start.json' -and $manifest.files.file -contains 'configuration-stop.json' -and $manifest.files.file -contains 'markers.jsonl' -and $manifest.files.file -contains 'markers.closed.json' -and $manifest.files.file -contains 'summary.md' -and $manifest.files.file -contains 'summary.analysis.json') 'New retained evidence was not included in the SHA256 manifest.'
$mockPowerRows=@([IO.File]::ReadLines((Join-Path $folder 'power-state.jsonl'))|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions})
Check (@($mockPowerRows|Where-Object {$_.available -or $_.powerPlanGuid -or -not $_.testOnly}).Count -eq 0) 'Simulated worker called live power APIs or fabricated power state.'
$archive=[IO.Compression.ZipFile]::OpenRead($state.zipPath)
try {foreach($name in @('markers.jsonl','markers.closed.json','summary.md','configuration-start.json','power-state.jsonl')){Check ($null -ne $archive.GetEntry($name)) ('ZIP omitted new retained evidence '+$name)}}finally{$archive.Dispose()}

$folder=New-TestRequest 'existing-presentmon-fallback'
$request=[IO.File]::ReadAllText((Join-Path $folder 'request.json'))|ConvertFrom-Json @recorderJsonOptions
$request|Add-Member -NotePropertyName testOnlyPresentMonFallback -NotePropertyValue 'Kernel-Process has eight existing logger IDs'
Write-RecorderJson (Join-Path $folder 'request.json') $request
$worker=Start-TestWorker $folder $identity.Created $true;$state=Wait-TestState $folder @('recording','failed')
Check ($state.state -eq 'recording' -and $state.presentMonSource -eq 'existing_pmdp' -and $state.message -like '*复用已有PresentMon，缺少独立raw*') 'Passive fallback did not enter recording with its source warning.'
$stopQpc=[Diagnostics.Stopwatch]::GetTimestamp();Write-RecorderJson (Join-Path $folder 'stop.request') ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=$stopQpc;qpcFrequency=[Diagnostics.Stopwatch]::Frequency;reason='cpu_fallback_test'})
$state=Wait-TestState $folder @('complete','failed');$result=Complete-RecorderOwned $worker
$summary=[IO.File]::ReadAllText((Join-Path $folder 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
$window=[IO.File]::ReadAllText((Join-Path $folder 'window.json'))|ConvertFrom-Json @recorderJsonOptions
Check ($result.exitCode -eq 0 -and -not $result.forcedTermination -and $state.state -eq 'complete' -and [IO.File]::Exists($state.zipPath)) 'Passive fallback did not Stop and save a ZIP cleanly.'
Check ($summary.presentMonSource -eq 'existing_pmdp' -and -not $summary.independentPresentMonRaw -and $summary.presentMonFallbackReason -like '*eight*' -and $summary.recordingStarted -and $window.endQpcTicks -eq $stopQpc) 'Fallback source metadata or exact recording window was lost.'
Check ($null -eq $summary.presentMonSession -and $null -eq $summary.presentMonStop -and $summary.collectors.Count -eq 0 -and -not [IO.File]::Exists((Join-Path $folder 'presentmon.csv')) -and -not $summary.preexistingEtwSessionsStopped) 'No-ETW fallback claimed an independent session or fabricated raw output.'

foreach($keys in @(@('presentmonRaw'),@('pmdp'),@('rtss'),@('hardware'),@('foreground'),@('system'),@('threads'),@('measurements'),@('logs'),@('hardware','threads','logs'),@('foreground','system','measurements'))) {
    $mask=New-TestMask $keys;$folder=New-TestRequest ('mask-'+($keys -join '-')) $mask
    $worker=Start-TestWorker $folder $identity.Created $true;$state=Wait-TestState $folder @('recording','failed')
    Check ($state.state -eq 'recording') ('Selected source simulation did not become ready: '+($keys -join ','))
    Write-RecorderJson (Join-Path $folder 'stop.request') ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;reason='mask_test'})
    $state=Wait-TestState $folder @('complete','failed');$result=Complete-RecorderOwned $worker
    Check ($state.state -eq 'complete' -and $result.exitCode -eq 0) 'Selected source simulation failed to Stop/save.'
    $manifest=[IO.File]::ReadAllText((Join-Path $folder 'manifest.json'))|ConvertFrom-Json @recorderJsonOptions
    $summary=[IO.File]::ReadAllText((Join-Path $folder 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
    $window=[IO.File]::ReadAllText((Join-Path $folder 'window.json'))|ConvertFrom-Json @recorderJsonOptions
    $simulation=[IO.File]::ReadAllText((Join-Path $folder 'TEST-ONLY-helper-plan.json'))|ConvertFrom-Json @recorderJsonOptions
    Check ($simulation.inventoryWouldRun -eq $mask.presentmonRaw -and $simulation.newEtwSessions -eq 0 -and $summary.collectors.Count -eq 0) 'Simulation started a real helper/ETW or lost the raw inventory selection.'
    foreach($key in $mask.Keys) {
        $file='TEST-ONLY-'+$key+'.jsonl'
        Check ([IO.File]::Exists((Join-Path $folder $file)) -eq $mask[$key] -and ($manifest.files.file -contains $file) -eq $mask[$key]) ('Unselected artifact exclusion failed for '+$key)
        Check ($manifest.selectedCollectors.$key -eq $mask[$key] -and $summary.selectedCollectors.$key -eq $mask[$key] -and $state.selectedCollectors.$key -eq $mask[$key] -and $window.selectedCollectors.$key -eq $mask[$key]) ('Collector selection was not saved consistently for '+$key)
        Check ($summary.sourceStatus.$key.status -eq $(if($mask[$key]){'test_only'}else{'disabled'})) ('Selected/disabled source status was wrong for '+$key)
    }
    Check (-not [IO.File]::Exists((Join-Path $folder 'etw-before.json')) -and -not [IO.File]::Exists((Join-Path $folder 'presentmon.csv'))) 'CPU simulation produced real ETW or frame files.'
}

$folder=New-TestRequest 'raw-only-failure' (New-TestMask @('presentmonRaw'))
$request=[IO.File]::ReadAllText((Join-Path $folder 'request.json'))|ConvertFrom-Json @recorderJsonOptions;$request|Add-Member -NotePropertyName testOnlyPresentMonFallback -NotePropertyValue 'raw unavailable'
Write-RecorderJson (Join-Path $folder 'request.json') $request
$worker=Start-TestWorker $folder $identity.Created $true;$state=Wait-TestState $folder @('recording','failed')
Check ($state.state -eq 'recording' -and $state.presentMonSource -eq 'unavailable' -and -not $state.selectedCollectors.pmdp -and $state.sourceStatus.pmdp.status -eq 'disabled') 'Raw failure silently enabled PMDP or prevented other selected sources from proceeding.'
Write-RecorderJson (Join-Path $folder 'stop.request') ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');qpcTicks=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;reason='raw_failure_test'})
$state=Wait-TestState $folder @('complete','failed');$result=Complete-RecorderOwned $worker
$summary=[IO.File]::ReadAllText((Join-Path $folder 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
Check ($result.exitCode -eq 0 -and $state.state -eq 'complete' -and $summary.noData -and $state.noData -and $state.message -like 'No data:*' -and $summary.sourceStatus.presentmonRaw.status -eq 'unavailable') 'Raw-only failure was not saved as explicit no-data completion.'
Check (-not [IO.File]::Exists((Join-Path $folder 'TEST-ONLY-pmdp.jsonl')) -and $summary.collectors.Count -eq 0 -and $null -eq $summary.presentMonSession) 'Raw-only failure fabricated an unselected fallback.'

foreach($invalid in @((New-TestMask @()),[ordered]@{unknown=$true},[ordered]@{hardware='true'})) {
    $folder=New-TestRequest ('invalid-selection-'+[guid]::NewGuid().ToString('N')) $invalid
    $worker=Start-TestWorker $folder $identity.Created $true;$state=Wait-TestState $folder @('failed');$result=Complete-RecorderOwned $worker
    Check ($result.exitCode -ne 0 -and -not [IO.File]::Exists((Join-Path $folder 'TEST-ONLY-helper-plan.json')) -and -not [IO.File]::Exists((Join-Path $folder 'etw-before.json'))) 'Invalid collector selection started capture before rejecting the request.'
}

if($MockOnly){$skippedChecks+='Live hardware/foreground source integration: MockOnly mode; no live capture sources were opened.'}
if(-not $MockOnly){foreach($keys in @(@('hardware'),@('foreground'),@('hardware','foreground'))) {
    $folder=Join-Path $testRoot ('power-'+($keys -join '-'));[IO.Directory]::CreateDirectory($folder)|Out-Null
    $arguments=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'capture-power.ps1'),'-TargetPid',$PID.ToString(),'-TargetCreationFileTime',$identity.Created.ToString(),'-OutputDirectory',$folder,'-DurationSeconds','1')
    if($keys -contains 'hardware'){$arguments+='-CaptureHardware'};if($keys -contains 'foreground'){$arguments+='-CaptureForeground'}
    $capture=Start-RecorderOwned 'test-power' $powerShellRuntime.executable $arguments $folder 'power-run.json' 'power.stderr.txt'
    $result=Complete-RecorderOwned $capture 10000;Check ($result.exitCode -eq 0 -and -not $result.forcedTermination) 'Selected power/foreground fixture failed.'
    $powerReport=[IO.File]::ReadAllText((Join-Path $folder 'power-run.json'))|ConvertFrom-Json @recorderJsonOptions
    Check-PowerShellRuntime $powerReport.powerShellRuntime $result.pid
    $file=$(if($keys -contains 'hardware'){'power.jsonl'}else{'foreground.jsonl'});$samples=@([IO.File]::ReadAllLines((Join-Path $folder $file))|ForEach-Object {$_|ConvertFrom-Json @recorderJsonOptions})
    Check ($samples.Count -gt 0) 'Selected power/foreground fixture produced no samples.'
    foreach($sample in $samples) {
        Check (($sample.PSObject.Properties.Name -contains 'foregroundProcessId') -eq ($keys -contains 'foreground')) 'Foreground fields were read or emitted while disabled.'
        Check (($sample.PSObject.Properties.Name -contains 'readings') -eq ($keys -contains 'hardware')) 'MAHM fields were read or emitted while disabled.'
        if($keys -notcontains 'hardware'){Check (-not $sample.error) 'Foreground-only capture touched unavailable MAHM.'}
    }
}}

if(@(Get-Process -Name Endfield -ErrorAction SilentlyContinue).Count -eq 0) {
    $folder=New-TestRequest 'no-game';$worker=Start-TestWorker $folder $identity.Created $false;$state=Wait-TestState $folder @('failed');$result=Complete-RecorderOwned $worker
    Check ($result.exitCode -ne 0 -and $state.message -like '*Expected one live Endfield.exe; found 0*' -and -not [IO.File]::Exists((Join-Path $folder 'etw-before.json'))) 'No-game failure started inventory or failed unclearly.'
}else{$skippedChecks+='No-game rejection: Endfield is already running; the CPU test left it running.'}

$report=[ordered]@{passed=$true;checks=$checks;mockOnly=[bool]$MockOnly;liveSourceTestsRun=(-not $MockOnly);powerShellRuntime=$powerShellRuntime;skippedChecks=$skippedChecks;newEtwSessions=0;gameStarted=$false;outputDirectory=$testRoot;completedUtc=[DateTime]::UtcNow.ToString('o')}
Write-RecorderJson (Join-Path $testRoot 'test-result.json') $report
$report|ConvertTo-Json
