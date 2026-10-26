[CmdletBinding()]
param([string]$BuildDirectory,[string]$AnalyzeDirectory)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if(-not $BuildDirectory){$BuildDirectory=Join-Path $project 'build'}
$build=(Resolve-Path -LiteralPath $BuildDirectory).Path
$exe=Join-Path $build 'xess-rawcolor-capture-probe-gpu-test.exe'
if(-not $AnalyzeDirectory -and -not (Test-Path -LiteralPath $exe)){throw ('Missing rawcolor fixture executable: '+$exe)}
$sourcePaths=@('src/rawcolor_capture_probe_gpu_test.cpp','scripts/test-rawcolor-capture-probe.ps1','docs/rawcolor-capture-probe-gpu.md',
    'src/rawcolor_capture_probe.hpp','src/rawcolor_capture_probe.cpp','src/borrowed_capture_completion.hpp','src/borrowed_capture_completion.cpp',
    'src/descriptor_image_metadata.hpp','src/descriptor_image_metadata.cpp','src/descriptor_image_observer.hpp','src/descriptor_image_observer.cpp',
    'src/render_pass_metadata.hpp','src/render_pass_metadata.cpp','src/render_pass_observer.hpp','src/render_pass_observer.cpp',
    'src/resource_memory_facts.hpp','src/resource_memory_facts.cpp','src/resource_memory_observer.hpp','src/resource_memory_observer.cpp',
    'src/image_inventory.hpp','src/command_recording_facts.hpp','src/recorded_source_layout.hpp','CMakeLists.txt')
function SourceHashes {
    $hashes=[ordered]@{};foreach($path in $sourcePaths){$hashes[$path]=(Get-FileHash -LiteralPath (Join-Path $project $path) -Algorithm SHA256).Hash.ToLowerInvariant()};return $hashes
}
if($AnalyzeDirectory){
    $report=(Resolve-Path -LiteralPath $AnalyzeDirectory).Path
    $raw=Get-Content -LiteralPath (Join-Path $report 'raw-result.json') -Raw|ConvertFrom-Json
    $savedExe=Join-Path $report 'fixture.exe'
    if((Get-FileHash -LiteralPath $savedExe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $raw.executableSha256){throw 'Analyze-only saved executable differs from the originally run binary.'}
}else{
    $processes=@(Get-Process -Name Endfield,EFU,Games,Launcher -ErrorAction SilentlyContinue)
    if($processes.Count){throw 'Normally exit Endfield, EFU and game launcher processes before this self-owned GPU fixture.'}
    $report=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-rawcolor-capture-probe')
    [IO.Directory]::CreateDirectory($report)|Out-Null
    $savedExe=Join-Path $report 'fixture.exe';Copy-Item -LiteralPath $exe -Destination $savedExe
    $executableHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    if((Get-FileHash -LiteralPath $savedExe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $executableHash){throw 'Saved raw fixture executable hash differs from the build artifact.'}
    $beforeHashes=SourceHashes
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$savedExe;$start.WorkingDirectory=$report
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($variable in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE',
        'XESS_OBSERVER_LOG','XESS_FSR_OBSERVER','XESS_SL_OBSERVER','XESS_COMPUTE_OBSERVER','XESS_BRIDGE_DEVICE','XESS_FSR_UPSCALE_OBSERVER','XESS_RAWCOLOR_CAPTURE')){$start.EnvironmentVariables.Remove($variable)}
    $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
    $child=[Diagnostics.Process]::new();$child.StartInfo=$start
    if(-not $child.Start()){throw 'Could not start the new rawcolor replay fixture.'}
    $output=$child.StandardOutput.ReadToEndAsync();$errors=$child.StandardError.ReadToEndAsync();$watch=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
    while(-not $child.WaitForExit(250)){if($watch.Elapsed.TotalSeconds -ge 30){$timedOut=$true;$child.Kill();break}}
    $child.WaitForExit();$stdout=$output.GetAwaiter().GetResult();$stderr=$errors.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $report 'stdout.jsonl'),$stdout);[IO.File]::WriteAllText((Join-Path $report 'stderr.txt'),$stderr)
    $rows=@();$parseError=$null
    try{$rows=@($stdout -split '\r?\n'|Where-Object {$_ -match '^\{'}|ForEach-Object {$_|ConvertFrom-Json})}catch{$parseError=$_.Exception.Message}
    $raw=[ordered]@{schema='rawcolor-probe-replay-raw-v1';capturedAt=[DateTimeOffset]::Now.ToString('o');exitCode=$child.ExitCode;
        timedOut=$timedOut;elapsedSeconds=$watch.Elapsed.TotalSeconds;parseError=$parseError;rows=$rows;
        executable='fixture.exe';executableSha256=$executableHash;afterExecutableSha256=(Get-FileHash -LiteralPath $savedExe -Algorithm SHA256).Hash.ToLowerInvariant();sourceSha256=$beforeHashes;
        afterSourceSha256=(SourceHashes);stdoutSha256=(Get-FileHash -LiteralPath (Join-Path $report 'stdout.jsonl') -Algorithm SHA256).Hash.ToLowerInvariant();
        stderrSha256=(Get-FileHash -LiteralPath (Join-Path $report 'stderr.txt') -Algorithm SHA256).Hash.ToLowerInvariant();
        processesAtPreflight=@($processes|Select-Object Id,ProcessName);gameTested=$false;gamePixelsCaptured=$false;xefgIntegrated=$false;installedPayloadChanged=$false}
    # Preserve this raw record before the independent BMP validation. Subsequent
    # parser repairs can use -AnalyzeDirectory without repeating GPU execution.
    $raw|ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $report 'raw-result.json') -Encoding utf8
}
$problems=[Collections.Generic.List[string]]::new()
if($raw.executableSha256 -ne $raw.afterExecutableSha256){$problems.Add('Saved executable changed while the child ran.')}
foreach($path in $sourcePaths){if($raw.sourceSha256.$path -ne $raw.afterSourceSha256.$path){$problems.Add('Source changed while the child ran: '+$path)}}
$rows=@($raw.rows);$fixture=@($rows|Where-Object stage -EQ 'rawcolor-capture-probe')
if($raw.timedOut -or $raw.parseError -or $raw.exitCode -ne 0 -or $fixture.Count -ne 1 -or $fixture[0].passed -ne $true){$problems.Add('Actual GPU child did not produce a successful unique final result.')}
$runtime=@($rows|Where-Object event -EQ 'rawcolor_replay_runtime')
if($runtime.Count -ne 1 -or $runtime[0].queueFamily -ne 0 -or $runtime[0].enabledQueueCount -ne 1 -or ($runtime[0].queueFlags -band 7) -ne 7 -or
    $runtime[0].observerLoaded -ne $false -or $runtime[0].sourceFormat -ne 122 -or $runtime[0].width -ne 64 -or $runtime[0].height -ne 64 -or
    $runtime[0].instanceExtensionsEnabled -ne 0 -or $runtime[0].instanceCreatePNext -ne $false -or $runtime[0].applicationInstanceCallbacksAbsentKnown -ne $true){$problems.Add('Actual single family0 queue/source/runtime provenance differs.')}
$sourceQueries=@($rows|Where-Object event -EQ 'rawcolor_source_format_query')
if($sourceQueries.Count -ne 3 -or ($sourceQueries.format -join ',') -ne '130,122,83' -or ($sourceQueries.usage -join ',') -ne '39,23,23'){$problems.Add('Actual source full-usage format queries differ.')}
foreach($row in $sourceQueries){if($row.queryResult -ne 0 -or $row.maxExtent[0] -lt 64 -or $row.maxExtent[1] -lt 64 -or $row.maxExtent[2] -lt 1 -or ($row.sampleCounts -band 1) -ne 1){$problems.Add('Actual full-usage source format query failed.')}}
$query=@($rows|Where-Object event -EQ 'rawcolor_target_format_query')
if($query.Count -ne 1 -or $query[0].queryResult -ne 0 -or ($query[0].optimalFeatures -band 49152) -ne 49152 -or
    $query[0].maxExtent[0] -lt 64 -or $query[0].maxExtent[1] -lt 64 -or $query[0].maxExtent[2] -lt 1 -or $query[0].maxMipLevels -lt 1 -or
    $query[0].maxArrayLayers -lt 1 -or ($query[0].sampleCounts -band 1) -ne 1 -or $query[0].maxResourceSize -lt 16384){$problems.Add('Real format122 optimal transfer capability query failed or differs.')}
$clears=@($rows|Where-Object event -EQ 'rawcolor_source_clear_completed')
if($clears.Count -ne 2 -or ($clears.value -join ',') -ne 'A,B'){ $problems.Add('Two actual source GPU clear phases are missing.') }
foreach($row in $clears){if($row.actualFenceStatus -ne 0 -or $row.finalLayout -ne 7 -or $row.cpuUpload -ne $false){$problems.Add('Source GPU clear did not independently complete at literal transfer-destination layout.')}}
$recording=@($rows|Where-Object event -EQ 'rawcolor_borrowed_recording')
if($recording.Count -ne 1 -or $recording[0].contextIncarnation -le 0 -or $recording[0].rpRecordingGeneration -le 0 -or $recording[0].poolGeneration -le 0 -or
    $recording[0].beginFlags -ne 0 -or $recording[0].rawBindings -ne 3 -or $recording[0].sourceInserted -ne $true -or $recording[0].firstFence -eq $recording[0].drainFence -or
    $recording[0].shaderReadsSources -ne $false){$problems.Add('Actual flags0 recording / distinct owner fences evidence differs.')}
$pending=@($rows|Where-Object event -EQ 'rawcolor_host_gate_pending')
if($pending.Count -ne 1 -or $pending[0].hostGateValue -ne 0 -or $pending[0].ownFenceStatus -ne 1 -or $pending[0].finiteWaitResult -ne 2 -or $pending[0].pollDriver -ne 1 -or
    $pending[0].pixelsSpanEmpty -ne $true -or $pending[0].stagingReadPerformed -ne $false -or $pending[0].bmpExists -ne $false -or $pending[0].filesWritten -ne 0){$problems.Add('Actual pending fence/readback/output refusal differs.')}
$submits=@($rows|Where-Object event -EQ 'rawcolor_application_submit')
if($submits.Count -ne 2 -or ($submits.ordinal -join ',') -ne '1,2'){ $problems.Add('Two serial original application submissions are missing.') }
foreach($row in $submits){if($row.actualOriginalResult -ne 0 -or $row.originalCalls -ne 1 -or $row.unchangedArguments -ne $true -or $row.waitStages -ne 2048 -or
    $row.waitTimelineValue -ne 1 -or $row.privateReadbackSubmits -ne 1 -or ($recording.Count -eq 1 -and $row.recordingGeneration -ne $recording[0].rpRecordingGeneration)){$problems.Add('Original exact-once / unchanged submit / same-generation replay differs.')}}
$packed=@($rows|Where-Object event -EQ 'rawcolor_first_staging_verified')
if($packed.Count -ne 2 -or ($packed.phase -join ',') -ne 'first-readback,after-replay'){ $problems.Add('Both first-staging packed pixel comparisons are missing.') }
foreach($row in $packed){if($row.format -ne 122 -or $row.packedWord -ne 1966080 -or $row.pixels -ne 4096 -or $row.allPixelsMatch -ne $true -or $row.bmpConversionVerified -ne $false){$problems.Add('Packed first staging A comparison differs.')}}
$owned=@($rows|Where-Object event -EQ 'rawcolor_owned_replay_verified')
if($owned.Count -ne 1 -or $owned[0].format -ne 122 -or $owned[0].packedWord -ne 2013266880 -or $owned[0].pixels -ne 4096 -or $owned[0].allPixelsMatch -ne $true -or
    $owned[0].actualDrainSucceededBeforeRead -ne $true){$problems.Add('Owned image B after replay actual oracle differs.')}
$drain=@($rows|Where-Object event -EQ 'rawcolor_shutdown_drain_verified')
if($drain.Count -ne 1 -or $drain[0].distinctFences -ne $true -or $drain[0].actualDrainWaitResult -ne 0 -or $drain[0].oracleFenceStatus -ne 0 -or $drain[0].recordingRetired -ne $true -or
    $drain[0].normalReleaseStillRetainedBeforeDrain -ne $true -or $drain[0].queueIdleInvented -ne $false -or $drain[0].probeOwnedChildrenClearedBeforeDeviceDestroy -ne $true -or
    $drain[0].privateReadbackSubmits -ne 1 -or $drain[0].drainSubmits -ne 1){$problems.Add('Independent last-use shutdown drain and ownership cleanup differ.')}
if($fixture.Count -eq 1){$value=$fixture[0]
    foreach($claim in @('selfOwnedSourcePixelsCaptured','actualProbeApiUsed','flags0SerialReplayTested','firstPackedStagingA','firstStagingAAfterReplay','ownedImageBAfterReplay',
        'pendingHostReadRefused','pendingBmpAbsent','separateShutdownDrainSucceeded','normalReplayReleaseNotFaked','borrowedRecordingRetiredByFixture','cleanupSucceeded')){
        if($value.$claim -ne $true){$problems.Add('Final actual fixture evidence missing '+$claim)}}
    foreach($claim in @('gameTested','productionLinked','gamePixelsCaptured','xefgIntegrated','gameSameFrameVerified','motionSemanticsVerified','depthConverted','installedPayloadChanged','shutdownQuarantined','bmpConversionVerified')){
        if($value.$claim -ne $false){$problems.Add('Unsupported or misclassified fixture claim '+$claim)}}
    if($value.applicationSubmits -ne 2 -or $value.originalUnchangedCalls -ne 2 -or $value.privateReadbackSubmits -ne 1 -or $value.stagingCopyCommands -ne 1 -or
        $value.drainSubmits -ne 1 -or $value.mappedPackedPixelsVerified -ne 12288 -or $value.probeOwnImagesCreated -ne 1 -or $value.probeOwnImagesDestroyed -ne 1 -or
        $value.probeOwnBuffersCreated -ne 1 -or $value.probeOwnBuffersDestroyed -ne 1 -or $value.probeOwnMemoryAllocations -ne 2 -or $value.probeOwnMemoryFrees -ne 2 -or
        $value.probeOwnPoolsCreated -ne 1 -or $value.probeOwnPoolsDestroyed -ne 1 -or $value.probeOwnFencesCreated -ne 2 -or $value.probeOwnFencesDestroyed -ne 2){$problems.Add('Exact private readback / drain / resource / packed pixel counts differ.')}
}
# Independently parse the file produced by the real probe. The packed format122
# words already compared by the child do not establish this lossy RGB24 result.
$bmpPath=Join-Path $report 'rawcolor-first.bmp';$bmp=[ordered]@{path='rawcolor-first.bmp';verified=$false;format='top-down RGB24 BMP';expectedBgr=@(0,255,0);verifiedPixels=0}
try{
    $bytes=[IO.File]::ReadAllBytes($bmpPath)
    if($bytes.Length -ne 12342 -or $bytes[0] -ne 66 -or $bytes[1] -ne 77 -or [BitConverter]::ToUInt32($bytes,2) -ne $bytes.Length -or
        [BitConverter]::ToUInt32($bytes,10) -ne 54 -or [BitConverter]::ToUInt32($bytes,14) -ne 40 -or [BitConverter]::ToInt32($bytes,18) -ne 64 -or
        [BitConverter]::ToInt32($bytes,22) -ne -64 -or [BitConverter]::ToUInt16($bytes,26) -ne 1 -or [BitConverter]::ToUInt16($bytes,28) -ne 24 -or
        [BitConverter]::ToUInt32($bytes,30) -ne 0 -or [BitConverter]::ToUInt32($bytes,34) -ne 12288){throw 'BMP header, extent, orientation, size or encoding differs.'}
    for($i=54;$i -lt $bytes.Length;$i+=3){if($bytes[$i] -ne 0 -or $bytes[$i+1] -ne 255 -or $bytes[$i+2] -ne 0){throw ('BMP does not preserve first A RGB at pixel '+(($i-54)/3))}}
    $bmp.verified=$true;$bmp.verifiedPixels=4096;$bmp.sha256=(Get-FileHash -LiteralPath $bmpPath -Algorithm SHA256).Hash.ToLowerInvariant();$bmp.bytes=$bytes.Length
}catch{$bmp.error=$_.Exception.Message;$problems.Add('Independent BMP comparison failed: '+$_.Exception.Message)}
$result=[ordered]@{schema='rawcolor-probe-replay-v1';analyzedAt=[DateTimeOffset]::Now.ToString('o');passed=($problems.Count -eq 0);raw='raw-result.json';
    rawSha256=(Get-FileHash -LiteralPath (Join-Path $report 'raw-result.json') -Algorithm SHA256).Hash.ToLowerInvariant();fixture=$fixture;bmp=$bmp;validationProblems=@($problems);
    selfOwnedPackedPixelsVerified=($packed.Count -eq 2 -and $owned.Count -eq 1);bmpConversionVerified=$bmp.verified;pendingMappingForbidden=$false;pendingPixelReadsForbidden=$true;
    normalReplayCompletionAndShutdownDrainSeparate=$true;gameTested=$false;productionLinked=$false;gamePixelsCaptured=$false;xefgIntegrated=$false;installedPayloadChanged=$false;
    realTimeoutOrDeviceLostInjected=$false;exceptionalBranchesCoveredByFrozenCpuComponentTests=$true;validationLayersRequested=$false;oldGpuExperimentsRepeated=$false}
$result|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding utf8
Write-Output (Join-Path $report 'result.json')
if($problems.Count){throw 'Rawcolor replay evidence failed; inspect the preserved raw result and independent BMP diagnosis.'}
