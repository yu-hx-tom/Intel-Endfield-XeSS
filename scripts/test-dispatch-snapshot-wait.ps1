[CmdletBinding()]
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if(@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count){throw 'Normally exit Endfield and EFU before the owned two-queue wait fixture.'}
if(-not $BuildDirectory){$BuildDirectory=Join-Path $project 'build'}
$build=(Resolve-Path -LiteralPath $BuildDirectory).Path
$exe=Join-Path $build 'xess-dispatch-snapshot-wait-gpu-test.exe'
if(-not (Test-Path -LiteralPath $exe)){throw ('Missing fixture executable: '+$exe)}
$report=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-dispatch-snapshot-wait')
[IO.Directory]::CreateDirectory($report)|Out-Null
function Invoke-WaitChild([string]$name,[string]$arguments){
    $directory=Join-Path $report $name;[IO.Directory]::CreateDirectory($directory)|Out-Null
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$exe;$start.Arguments=$arguments;$start.WorkingDirectory=$directory
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($variable in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE',
        'XESS_OBSERVER_LOG','XESS_FSR_OBSERVER','XESS_SL_OBSERVER','XESS_COMPUTE_OBSERVER','XESS_BRIDGE_DEVICE','XESS_FSR_UPSCALE_OBSERVER')){$start.EnvironmentVariables.Remove($variable)}
    $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
    $child=[Diagnostics.Process]::new();$child.StartInfo=$start;if(-not $child.Start()){throw ('Could not start '+$name)}
    $output=$child.StandardOutput.ReadToEndAsync();$errors=$child.StandardError.ReadToEndAsync();$watch=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
    while(-not $child.WaitForExit(250)){if($watch.Elapsed.TotalSeconds -ge 30){$timedOut=$true;$child.Kill();break}}
    $child.WaitForExit();$stdout=$output.GetAwaiter().GetResult();$stderr=$errors.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.jsonl'),$stdout);[IO.File]::WriteAllText((Join-Path $directory 'stderr.txt'),$stderr)
    $rows=@();$parseError=$null;try{$rows=@($stdout -split '\r?\n'|Where-Object {$_ -match '^\{'}|ForEach-Object {$_|ConvertFrom-Json})}catch{$parseError=$_.Exception.Message}
    $record=[ordered]@{name=$name;exitCode=$child.ExitCode;timedOut=$timedOut;elapsedSeconds=$watch.Elapsed.TotalSeconds;
        executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();rows=$rows;parseError=$parseError;
        fixture=@($rows|Where-Object stage -EQ 'dispatch-snapshot-wait'|Select-Object -Last 1);stdout='stdout.jsonl';stderr='stderr.txt'}
    $record|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $directory 'result.json') -Encoding utf8;return $record
}
$positive=Invoke-WaitChild 'raw-two-queue-positive' ''
$negative=Invoke-WaitChild 'contract-rejection-negative' '--reject-transfer-only-recipe'
$problems=[Collections.Generic.List[string]]::new()
foreach($run in @($positive,$negative)){if($run.timedOut -or $run.parseError -or $run.fixture.Count -ne 1){$problems.Add('Timeout, malformed JSON or absent result: '+$run.name)}}
$unsupported=$positive.fixture.Count -eq 1 -and $positive.fixture[0].unsupported -eq $true
if(-not $unsupported){
    if($positive.exitCode -ne 0 -or $positive.fixture.Count -ne 1 -or $positive.fixture[0].passed -ne $true){$problems.Add('Two-queue positive did not pass')}
    if($positive.fixture.Count -eq 1){$value=$positive.fixture[0]
        foreach($claim in @('selfOwnedPixelsCaptured','sourceGpuGenerated','distinctSameFamilyQueues','markerPendingVerified','markerCompleteVerified','commandTraceVerified','cleanupSucceeded')){
            if($value.$claim -ne $true){$problems.Add('Positive result missing '+$claim)}}
        if($value.sourceCpuUploaded -ne $false -or $value.submissions -ne 2 -or $value.captures -ne 1 -or $value.verifiedPixels -ne 11680 -or $value.sourceLayout -ne 5 -or ($value.formats -join ',') -ne '37,83,130,122'){
            $problems.Add('Positive pixel counts, layout or actual GPU generation differ')}
    }
    $runtime=@($positive.rows|Where-Object event -EQ 'wait_runtime');$pending=@($positive.rows|Where-Object event -EQ 'compute_wait_pending');$complete=@($positive.rows|Where-Object event -EQ 'compute_wait_completed')
    if($runtime.Count -ne 1 -or $runtime[0].enabledQueueCount -ne 2 -or $runtime[0].availableQueueCount -lt 2 -or $runtime[0].producerQueue -eq $runtime[0].consumerQueue -or
        $runtime[0].observerLoaded -ne $false -or $runtime[0].separateDepthStencilLayoutsEnabled -ne $false){$problems.Add('Actual distinct same-family queue/runtime evidence missing')}
    if($pending.Count -ne 1 -or $pending[0].hostGateValue -ne 0 -or $pending[0].producerSignalValue -ne 0 -or $pending[0].producerFenceStatus -ne 1 -or
        $pending[0].consumerFenceStatus -ne 1 -or $pending[0].finiteWaitResult -ne 2 -or $pending[0].markerStatus -ne 4 -or $pending[0].markerSamples -ne 8 -or
        $pending[0].consumerWaitStages -ne 2048 -or $pending[0].producerWaitStages -ne 65536 -or $pending[0].pendingHostRead -ne $false){$problems.Add('Actual gated transfer-marker/fence evidence missing')}
    if($complete.Count -ne 1 -or $complete[0].hostGateValue -ne 1 -or $complete[0].producerSignalValue -ne 1 -or $complete[0].markerStatus -ne 3 -or
        $complete[0].producerFenceStatus -ne 0 -or $complete[0].consumerFenceStatus -ne 0){$problems.Add('Actual released marker/semaphore/fence evidence missing')}
    $trace=@($positive.rows|Where-Object event -EQ 'recorder_chain_trace')
    if($trace.Count -ne 4){$problems.Add('Exact four-plane recorder trace missing')}
    foreach($row in $trace){if(($row.srcStages -band 65536) -eq 0 -or $row.dstStages -ne 4096 -or $row.sourceAccess -ne 98304 -or $row.destinationAccess -ne 2048 -or
        $row.targetDestinationAccess -ne 4096 -or $row.copySourceLayout -ne 6 -or $row.copyTargetLayout -ne 7 -or $row.sourceOldLayout -ne 5 -or
        $row.sourceRestoredLayout -ne 5 -or ($row.order -join ',') -ne 'barrier,copy,restore'){$problems.Add('Actual recorder wait/barrier/access recipe differs')}
        if($row.format -eq 130 -and ($row.barrierAspects -ne 6 -or $row.copyAspect -ne 2)){$problems.Add('D32S8 common layout/depth-only trace differs')}}
    $planes=@($positive.rows|Where-Object event -EQ 'plane_pixels_verified')
    if($planes.Count -ne 4 -or ($planes|Measure-Object verifiedPixelCount -Sum).Sum -ne 11680){$problems.Add('Exhaustive four-format pixel rows missing')}
}elseif($positive.exitCode -ne 2 -or $positive.fixture[0].passed -ne $false -or $positive.fixture[0].selfOwnedPixelsCaptured -ne $false -or $positive.fixture[0].cleanupSucceeded -ne $true){$problems.Add('Unsupported must be explicit with safe cleanup and no pixel-success claim')}
if($negative.exitCode -ne 3 -or $negative.fixture.Count -ne 1 -or $negative.fixture[0].passed -ne $false -or $negative.fixture[0].contractRejected -ne $true -or
    $negative.fixture[0].commandsIssued -ne 0 -or $negative.fixture[0].recorderCommandsIssued -ne 0 -or $negative.fixture[0].submissions -ne 0 -or
    $negative.fixture[0].cleanupSucceeded -ne $true -or $negative.fixture[0].illegalGpuWorkSubmitted -ne $false){$problems.Add('Negative did not reject its recipe before every command and submission')}
foreach($run in @($positive,$negative)){if($run.fixture.Count -eq 1){foreach($claim in @('gameTested','productionLinked','gamePixelsCaptured','xefgIntegrated','gameSameFrameVerified','motionSemanticsVerified','depthConverted')){
    if($run.fixture[0].$claim -ne $false){$problems.Add('Unsupported claim in '+$run.name+': '+$claim)}}}}
$hashes=[ordered]@{}
foreach($path in @('src/dispatch_snapshot.hpp','src/dispatch_snapshot.cpp','src/dispatch_snapshot_test.cpp','src/dispatch_snapshot_gpu_test.cpp','src/dispatch_snapshot_wait_gpu_test.cpp',
    'scripts/test-dispatch-snapshot-wait.ps1','docs/dispatch-snapshot-wait.md','CMakeLists.txt')){$hashes[$path]=(Get-FileHash -LiteralPath (Join-Path $project $path) -Algorithm SHA256).Hash.ToLowerInvariant()}
$result=[ordered]@{schema='dispatch-snapshot-wait-v1';capturedAt=[DateTimeOffset]::Now.ToString('o');passed=(-not $unsupported -and $problems.Count -eq 0);unsupported=$unsupported;
    rawTwoQueuePositive=$positive;contractRejectionNegative=$negative;sourceSha256=$hashes;validationProblems=@($problems);
    gameTested=$false;productionLinked=$false;gamePixelsCaptured=$false;xefgIntegrated=$false;installedPayloadChanged=$false;validationLayersRequested=$false}
$result|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding utf8;Write-Output (Join-Path $report 'result.json')
if($problems.Count){throw 'Two-queue wait evidence failed; inspect saved raw results.'}
if($unsupported){Write-Output 'Unsupported: no qualifying two-queue recipe was executed.';exit 2}
