[CmdletBinding()]
param([ValidateRange(16,600)][int]$Frames=60,[switch]$RetireViaAcquire,[switch]$ResizeCycles)
$ErrorActionPreference='Stop'
if($ResizeCycles -and -not $RetireViaAcquire){throw '-ResizeCycles requires -RetireViaAcquire.'}
$project=Split-Path -Parent $PSScriptRoot
$running=@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue)
if($running.Count){throw 'Normally exit the game before this self-owned GPU experiment. Do not start it during the test.'}
$exe=Join-Path $project 'build/xess-wsi-native-presenter-lifecycle-test.exe'
$sdk=Join-Path $project 'third_party/xess/bin'
if(-not (Test-Path -LiteralPath $exe)){throw 'Build the lifecycle test first.'}
$report=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-wsi-native-presenter')
[IO.Directory]::CreateDirectory($report)|Out-Null
$records=@()
foreach($mode in @('on','off')){
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$exe
    $start.WorkingDirectory=$project
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    $start.Arguments='--sdk-bin "'+$sdk+'" --frames '+$Frames
    if($RetireViaAcquire){$start.Arguments+=' --retire-via-acquire'}
    if($ResizeCycles){$start.Arguments+=' --resize-cycles'}
    if($mode -eq 'off'){$start.Arguments+=' --off'}
    # Remove only our observer flags from this self-owned child. No global edits.
    foreach($name in @('XESS_OBSERVER_LOG','XESS_SL_OBSERVER','XESS_BRIDGE_DEVICE','XESS_COMPUTE_OBSERVER','XESS_FSR_OBSERVER','XESS_FSR_UPSCALE_OBSERVER')){
        $start.EnvironmentVariables.Remove($name)
    }
    $child=[Diagnostics.Process]::new()
    $child.StartInfo=$start
    $watch=[Diagnostics.Stopwatch]::StartNew()
    if(-not $child.Start()){throw 'Could not start the self-owned lifecycle experiment.'}
    $output=$child.StandardOutput.ReadToEndAsync()
    $errors=$child.StandardError.ReadToEndAsync()
    $timedOut=$false
    while(-not $child.WaitForExit(500)){
        if($watch.Elapsed.TotalSeconds -gt 100){$timedOut=$true;$child.Kill();break}
    }
    $child.WaitForExit()
    $stdout=$output.GetAwaiter().GetResult()
    $stderr=$errors.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $report ($mode+'-stdout.jsonl')),$stdout)
    [IO.File]::WriteAllText((Join-Path $report ($mode+'-stderr.txt')),$stderr)
    $parsed=$null
    try{$parsed=($stdout -split '\r?\n'|Where-Object {$_ -match '^\{'}|Select-Object -Last 1)|ConvertFrom-Json}catch{}
    $record=[ordered]@{mode=$mode;exitCode=$child.ExitCode;timedOut=$timedOut;seconds=$watch.Elapsed.TotalSeconds;result=$parsed}
    $records+=$record
    $summary=[ordered]@{capturedAt=[DateTimeOffset]::Now.ToString('o');executable=$exe;
        executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower();frames=$Frames;
        gameTouched=$false;selfOwnedVisibleTestWindows=$true;validationLayersRequested=$false;
        retirementMode=$(if($RetireViaAcquire){'acquire-fence-with-minimal-wsi-tail-retention'}else{'maintenance1-presentation-fences'});
        nativeResizeCyclesRequested=[bool]$ResizeCycles;
        inheritedVkInstanceLayers=$start.EnvironmentVariables['VK_INSTANCE_LAYERS'];
        inheritedVkLayerPath=$start.EnvironmentVariables['VK_LAYER_PATH'];runs=$records}
    $summary|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $report 'summary.json') -Encoding UTF8
    if($timedOut -or $child.ExitCode -ne 0 -or -not $parsed.testPassed){
        throw ('Lifecycle experiment failed ('+$mode+'); saved '+$report)
    }
    if($RetireViaAcquire -and (-not $parsed.renderResourcesCleanupSucceeded -or $parsed.cleanupSucceeded -or
        $parsed.portableWsiFinalRetirementVerified -or $parsed.retainedSourceImages -ne 0 -or
        $parsed.retainedRenderCommandPools -ne 0 -or $parsed.retainedNativeSwapchains -ne 1 -or
        $parsed.retainedPresentSemaphores -ne $parsed.pendingPresentsAtStop)){
        throw ('Lifecycle cleanup boundary mismatch ('+$mode+'); saved '+$report)
    }
    if($ResizeCycles -and ($parsed.nativeResizeCount -ne 2 -or $parsed.oldSwapchainGenerationsActuallyDestroyed -ne 2 -or
        $parsed.preflightVerifiedFrames -ne 6 -or -not $parsed.resizeRecoveryTested -or $parsed.xefgOutputResizeTested)){
        throw ('Lifecycle resize evidence mismatch ('+$mode+'); saved '+$report)
    }
}
Write-Output (Join-Path $report 'summary.json')
