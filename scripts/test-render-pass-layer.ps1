[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if(@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count){throw 'Normally exit the game before the self-owned GPU smoke.'}
$build=Join-Path $project 'build'
$exe=Join-Path $build 'xess-render-pass-gpu-smoke.exe'
$layer=Join-Path $build 'observer'
$dll=Join-Path $layer 'xess-vulkan-observer.dll'
$report=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-render-pass-gpu')
[IO.Directory]::CreateDirectory($report)|Out-Null
$log=Join-Path $report 'observer.jsonl'
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=$exe; $start.WorkingDirectory=$report
$start.UseShellExecute=$false; $start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
$start.EnvironmentVariables['VK_LAYER_PATH']=$layer
$start.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_XESS_bridge_observer'
$start.EnvironmentVariables['XESS_OBSERVER_LOG']=$log
$start.EnvironmentVariables['XESS_FSR_OBSERVER']='1'
foreach($name in @('XESS_SL_OBSERVER','XESS_COMPUTE_OBSERVER','XESS_BRIDGE_DEVICE','XESS_FSR_UPSCALE_OBSERVER')){$start.EnvironmentVariables.Remove($name)}
$child=[Diagnostics.Process]::new(); $child.StartInfo=$start
if(-not $child.Start()){throw 'Could not start self-owned RP smoke'}
$output=$child.StandardOutput.ReadToEndAsync(); $errors=$child.StandardError.ReadToEndAsync()
$watch=[Diagnostics.Stopwatch]::StartNew(); $timedOut=$false
while(-not $child.WaitForExit(500)){
    if($watch.Elapsed.TotalSeconds -ge 30){$timedOut=$true;$child.Kill();break}
}
$child.WaitForExit()
$stdout=$output.GetAwaiter().GetResult(); $stderr=$errors.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $report 'stdout.jsonl'),$stdout)
[IO.File]::WriteAllText((Join-Path $report 'stderr.txt'),$stderr)
$test=$null; $events=@()
try{$test=($stdout -split '\r?\n'|Where-Object {$_ -match '^\{'}|Select-Object -Last 1)|ConvertFrom-Json}catch{}
if(Test-Path -LiteralPath $log){$events=@(Get-Content -LiteralPath $log|ForEach-Object {$_|ConvertFrom-Json})}
$summary=@($events|Where-Object event -EQ 'rp_metadata_summary'|Select-Object -Last 1)
$passed= -not $timedOut -and $child.ExitCode -eq 0 -and $test.testPassed -and $summary.Count -eq 1
if($passed){
    $passed=$summary[0].accepted_layout_operations -eq 36 -and $summary[0].failed_driver_submissions -eq 0 -and
        $summary[0].failures -eq 0 -and $summary[0].fatal_issues -eq 0 -and ($events.event -contains 'destroy_device')
}
$record=[ordered]@{capturedAt=[DateTimeOffset]::Now.ToString('o');passed=$passed;exitCode=$child.ExitCode;timedOut=$timedOut;
    executableSha256=(Get-FileHash -LiteralPath $exe).Hash.ToLower();layerSha256=(Get-FileHash -LiteralPath $dll).Hash.ToLower();
    fixture=$test;observerSummary=$summary;gameTested=$false;installedPayloadChanged=$false;validationLayersRequested=$false}
$record|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding UTF8
Write-Output (Join-Path $report 'result.json')
if(-not $passed){throw 'Self-owned RP GPU/observer evidence did not pass; inspect saved results.'}
