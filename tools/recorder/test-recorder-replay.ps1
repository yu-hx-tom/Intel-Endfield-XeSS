#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([string]$AuditDirectory=(Join-Path $PSScriptRoot '../../out/20261019-t3-default-game-ab'),[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$replayOutput=[IO.Path]::GetFullPath($OutputDirectory)
. (Join-Path $PSScriptRoot 'record.ps1')
if([IO.Directory]::Exists($replayOutput)){throw 'Use a new replay output folder.'}
[IO.Directory]::CreateDirectory($replayOutput)|Out-Null
$results=@();$checks=0
foreach($name in @('sync-capture-audit.json','k2-capture-audit.json','k1-capture-audit.json')) {
    $expectedRows=[IO.File]::ReadAllText((Join-Path $AuditDirectory $name))|ConvertFrom-Json @recorderJsonOptions
    foreach($expected in $expectedRows) {
        $source=[IO.Path]::GetFullPath($expected.original)
        $before=@(Get-ChildItem -LiteralPath $source -File -Recurse|ForEach-Object {[ordered]@{path=$_.FullName;sha256=Get-RecorderSha256 $_.FullName}})
        $summary=[IO.File]::ReadAllText((Join-Path $source 'summary.json'))|ConvertFrom-Json @recorderJsonOptions
        $window=[ordered]@{qpcFrequency=$expected.selection.qpcFrequency;startQpcTicks=$expected.selection.startQpcTicks;endQpcTicks=$expected.selection.endQpcTicks;gamePid=$summary.gamePid;recordingStarted=$true}
        $context=Resolve-RecorderRequestContext ([pscustomobject]@{label=$expected.label;labelCode=$expected.route;notes='Read-only replay of the existing audited 60-second selection.';measurementMode='full'})
        $destination=Join-Path $replayOutput $expected.label;[IO.Directory]::CreateDirectory($destination)|Out-Null
        $clock=[Diagnostics.Stopwatch]::StartNew();$analysis=Write-RecorderReadableSummary $destination $window $summary $context $source;$clock.Stop()
        foreach($key in @('count','p50','p90','p95','p99','maximum','over25ms','over40ms','over100ms')){if([Math]::Abs([double]$analysis.segments[0].raw.displayIntervalMs[$key]-[double]$expected.displayIntervalMs.$key) -gt 1e-8){throw ($expected.label+' raw display mismatch: '+$key)};$checks++}
        foreach($key in @('CPU cores power','CPU GT power','CPU package power','CPU temperature')){if($null -eq $expected.hardwareSampleMean.$key){continue};if([Math]::Abs([double]$analysis.segments[0].raw.hardwareSampleMean[$key].mean-[double]$expected.hardwareSampleMean.$key) -gt 1e-8){throw ($expected.label+' power mean mismatch: '+$key)};$checks++}
        if($analysis.steadyVerified -or [Math]::Abs($analysis.segments[0].stateDurationSeconds.unknown-[double]$expected.selection.durationSeconds) -gt 1e-8){throw 'Legacy status without whole-interval input evidence was presented as verified steady state.'};$checks++
        foreach($input in $before){if((Get-RecorderSha256 $input.path) -cne $input.sha256){throw ('Replay changed original input: '+$input.path)};$checks++}
        $results+=[ordered]@{label=$expected.label;source=$source;outputDirectory=$destination;rawFrameCount=$analysis.segments[0].raw.displayIntervalMs.count;rawP99=$analysis.segments[0].raw.displayIntervalMs.p99;steadyVerified=$analysis.steadyVerified;unknownSeconds=$analysis.segments[0].stateDurationSeconds.unknown;originalFilesUnchanged=$true;originalFileHashes=$before;summaryElapsedSeconds=$clock.Elapsed.TotalSeconds}
    }
}
$report=[ordered]@{passed=$true;checks=$checks;captures=$results;liveCollectorsStarted=$false;newEtwSessions=0;originalFilesModified=$false;powerShellRuntime=$powerShellRuntime;completedUtc=[DateTime]::UtcNow.ToString('o')}
Write-RecorderJson (Join-Path $replayOutput 'replay-test-result.json') $report
[ordered]@{passed=$true;checks=$checks;captureCount=$results.Count;outputDirectory=$replayOutput;originalFilesModified=$false;summaryElapsedSeconds=@($results|ForEach-Object {$_.summaryElapsedSeconds})}|ConvertTo-Json
