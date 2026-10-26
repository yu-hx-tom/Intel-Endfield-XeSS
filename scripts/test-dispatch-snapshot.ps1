[CmdletBinding()]
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if(@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count){throw 'Normally exit Endfield and EFU before this self-owned GPU fixture.'}
if(-not $BuildDirectory){$BuildDirectory=Join-Path $project 'build'}
$build=(Resolve-Path -LiteralPath $BuildDirectory).Path
$cpu=Join-Path $build 'xess-dispatch-snapshot-test.exe'
$gpu=Join-Path $build 'xess-dispatch-snapshot-gpu-test.exe'
foreach($executable in @($cpu,$gpu)){if(-not (Test-Path -LiteralPath $executable)){throw ('Missing fixture executable: '+$executable)}}
$report=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-dispatch-snapshot')
[IO.Directory]::CreateDirectory($report)|Out-Null

function Invoke-SnapshotChild([string]$name,[string]$executable,[string]$arguments){
    $directory=Join-Path $report $name
    [IO.Directory]::CreateDirectory($directory)|Out-Null
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$executable;$start.Arguments=$arguments
    $start.WorkingDirectory=$directory;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($variable in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE',
        'XESS_OBSERVER_LOG','XESS_FSR_OBSERVER','XESS_SL_OBSERVER','XESS_COMPUTE_OBSERVER','XESS_BRIDGE_DEVICE','XESS_FSR_UPSCALE_OBSERVER')){
        $start.EnvironmentVariables.Remove($variable)
    }
    # Child-local raw-loader settings; installation and registrations are intact.
    $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
    $child=[Diagnostics.Process]::new();$child.StartInfo=$start
    if(-not $child.Start()){throw ('Could not start '+$name)}
    $output=$child.StandardOutput.ReadToEndAsync();$errors=$child.StandardError.ReadToEndAsync()
    $watch=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
    while(-not $child.WaitForExit(250)){if($watch.Elapsed.TotalSeconds -ge 30){$timedOut=$true;$child.Kill();break}}
    $child.WaitForExit();$stdout=$output.GetAwaiter().GetResult();$stderr=$errors.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.jsonl'),$stdout)
    [IO.File]::WriteAllText((Join-Path $directory 'stderr.txt'),$stderr)
    $rows=@();$parseError=$null
    try{$rows=@($stdout -split '\r?\n'|Where-Object {$_ -match '^\{'}|ForEach-Object {$_|ConvertFrom-Json})}
    catch{$parseError=$_.Exception.Message}
    $record=[ordered]@{name=$name;exitCode=$child.ExitCode;timedOut=$timedOut;elapsedSeconds=$watch.Elapsed.TotalSeconds;
        executableSha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant();
        fixture=@($rows|Where-Object {$_.stage -like 'dispatch-snapshot-*'}|Select-Object -Last 1);
        rows=$rows;parseError=$parseError;stdout='stdout.jsonl';stderr='stderr.txt'}
    $record|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $directory 'result.json') -Encoding utf8
    return $record
}

$cpuRun=Invoke-SnapshotChild 'cpu' $cpu ''
$positive=Invoke-SnapshotChild 'raw-gpu-positive' $gpu ''
$negative=Invoke-SnapshotChild 'raw-gpu-mismatch-negative' $gpu '--inject-pixel-mismatch'
$problems=[Collections.Generic.List[string]]::new()
foreach($run in @($cpuRun,$positive,$negative)){if($run.timedOut -or $run.parseError -or $run.fixture.Count -ne 1){$problems.Add('Timeout, malformed output or missing result in '+$run.name)}}
if($cpuRun.exitCode -ne 0 -or $cpuRun.fixture.Count -ne 1 -or -not $cpuRun.fixture[0].passed -or $cpuRun.fixture[0].hotPathAllocations -ne 0){$problems.Add('CPU lifecycle/preflight fixture failed')}
if($positive.exitCode -ne 0 -or $positive.fixture.Count -ne 1 -or -not $positive.fixture[0].passed){$problems.Add('Raw GPU pixel fixture failed')}
if($positive.fixture.Count -eq 1){
    $value=$positive.fixture[0]
    foreach($claim in @('selfOwnedPixelsCaptured','sourceGpuGenerated','pendingTimelineGateVerified','pendingReuseRefused','ticketGenerationAdvanced','cleanupSucceeded')){
        if($value.$claim -ne $true){$problems.Add('Positive evidence missing '+$claim)}
    }
    if($value.submissions -ne 2 -or $value.captures -ne 3 -or $value.verifiedPixels -ne 29200 -or ($value.formats -join ',') -ne '37,83,130,122'){
        $problems.Add('Positive capture, format or exhaustive pixel counts differ')
    }
}
$runtime=@($positive.rows|Where-Object event -EQ 'snapshot_runtime')
if($runtime.Count -ne 1 -or $runtime[0].observerLoaded -ne $false){$problems.Add('Raw run did not confirm observer absence')}
$pending=@($positive.rows|Where-Object event -EQ 'timeline_pending')
if($pending.Count -ne 1 -or $pending[0].actualFenceStatus -ne 1 -or $pending[0].finiteWaitResult -ne 2 -or
    $pending[0].reuseRefusal -ne 'capacity' -or $pending[0].refusedCommandsIssued -ne 0){$problems.Add('Actual pending fence/timeout and zero-command reuse refusal were not reported')}
$preflight=@($positive.rows|Where-Object event -EQ 'all_plane_preflight_refusal')
if($preflight.Count -ne 1 -or $preflight[0].planeCount -ne 4 -or $preflight[0].badPlane -ne 3 -or $preflight[0].commandsIssued -ne 0){$problems.Add('Whole-source preflight was not rejected with zero commands')}
$tickets=@($positive.rows|Where-Object event -EQ 'capture_recorded')
if($tickets.Count -ne 3 -or $tickets[2].slot -ne $tickets[0].slot -or $tickets[2].ticketGeneration -le $tickets[0].ticketGeneration){$problems.Add('Actual ticket generation advancement was not reported')}
$planes=@($positive.rows|Where-Object event -EQ 'plane_pixels_verified')
if($planes.Count -ne 8 -or ($planes|Measure-Object -Property verifiedPixelCount -Sum).Sum -ne 29200){$problems.Add('Per-format exhaustive pixel verification counts are missing')}
$mismatch=@($negative.rows|Where-Object event -EQ 'pixel_mismatch')
if($negative.exitCode -ne 1 -or $negative.fixture.Count -ne 1 -or $negative.fixture[0].passed -ne $false -or
    $negative.fixture[0].selfOwnedPixelsCaptured -ne $false -or $negative.fixture[0].cleanupSucceeded -ne $true -or $mismatch.Count -ne 1){
    $problems.Add('Injected mismatch did not report false/exit1 with completed cleanup')
}
if($mismatch.Count -eq 1 -and ($mismatch[0].plane -ne 0 -or $mismatch[0].format -ne 37 -or $mismatch[0].group -ne 1 -or
    $mismatch[0].x -ne 31 -or $mismatch[0].y -ne 18 -or $mismatch[0].expectedMismatchInjected -ne $true -or
    $mismatch[0].expectedBits -eq $mismatch[0].actualBits)){$problems.Add('Mismatch location or expected/actual bits differ from selected negative pixel')}
foreach($run in @($positive,$negative)){if($run.fixture.Count -eq 1){foreach($claim in @('gamePixelsCaptured','xefgIntegrated','motionSemanticsVerified','gameSameFrameVerified','depthConverted','sourceCpuUploaded')){
    if($run.fixture[0].$claim -ne $false){$problems.Add('Unsupported claim in '+$run.name+': '+$claim)}
}}}
$hashes=[ordered]@{}
foreach($path in @('src/dispatch_snapshot.hpp','src/dispatch_snapshot.cpp','src/dispatch_snapshot_test.cpp','src/dispatch_snapshot_gpu_test.cpp',
    'scripts/test-dispatch-snapshot.ps1','docs/dispatch-snapshot.md','CMakeLists.txt')){
    $hashes[$path]=(Get-FileHash -LiteralPath (Join-Path $project $path) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$result=[ordered]@{schema='dispatch-snapshot-v1';capturedAt=[DateTimeOffset]::Now.ToString('o');passed=$problems.Count -eq 0;
    cpu=$cpuRun;rawGpuPositive=$positive;rawGpuMismatchNegative=$negative;sourceSha256=$hashes;validationProblems=@($problems);
    selfOwnedPixelsCaptured=($positive.fixture.Count -eq 1 -and $positive.fixture[0].selfOwnedPixelsCaptured -eq $true);
    gamePixelsCaptured=$false;xefgIntegrated=$false;motionSemanticsVerified=$false;gameSameFrameVerified=$false;depthConverted=$false;
    gameTested=$false;installedPayloadChanged=$false;productionObserverLinked=$false;validationLayersRequested=$false}
$result|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding utf8
Write-Output (Join-Path $report 'result.json')
if($problems.Count){throw 'Dispatch snapshot evidence failed; inspect saved result/stdout/stderr.'}
