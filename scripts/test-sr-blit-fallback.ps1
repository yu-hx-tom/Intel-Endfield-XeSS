#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
if (@(Get-CimInstance Win32_Process -Filter "Name='Endfield.exe' OR Name='Games.exe'").Count) { throw 'Close the game and official launcher before this standalone GPU test.' }
$project=Split-Path -Parent $PSScriptRoot
$output=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-sr-blit-fallback')
[void][IO.Directory]::CreateDirectory($output)
$sdk=Join-Path $project 'third_party/xess/bin'
$executable=Join-Path $project 'build/xess-sr-vulkan-test.exe'
$validation=Join-Path $project '.tools/vvl-aea761f4'
if ((Get-FileHash -LiteralPath (Join-Path $sdk 'libxess.dll')).Hash.ToLowerInvariant() -cne '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7') { throw 'Unexpected SDK binary.' }
if (-not (Test-Path -LiteralPath (Join-Path $validation 'VkLayer_khronos_validation.dll'))) { throw 'Validation layer is unavailable; preserve the test for later.' }
$result=[ordered]@{passed=$false;output=$output;gameStarted=$false;systemConfigurationChanged=$false;executableSha256=(Get-FileHash -LiteralPath $executable).Hash.ToLowerInvariant()}
try {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$executable;$start.WorkingDirectory=$output;$start.UseShellExecute=$false
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($name in @($start.Environment.Keys)){if($name -like 'XESS_*' -or $name -like 'VK_*'){[void]$start.Environment.Remove($name)}}
    $start.Environment['DISABLE_RTSS_LAYER']='1'
    foreach($argument in @('--sdk-bin',$sdk,'--test-blit-fallback','--blocking-build','--validation-layer-dir',$validation,'--timeout-ms','5000')){$start.ArgumentList.Add($argument)}
    if (@(Get-CimInstance Win32_Process -Filter "Name='Endfield.exe' OR Name='Games.exe'").Count) { throw 'Game started before the GPU test; test not launched.' }
    $child=[Diagnostics.Process]::Start($start)
    $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
    $timedOut=-not $child.WaitForExit(45000)
    if($timedOut){$child.Kill($true);$child.WaitForExit()}
    [IO.File]::WriteAllText((Join-Path $output 'stdout.jsonl'),$stdout.Result)
    [IO.File]::WriteAllText((Join-Path $output 'stderr.log'),$stderr.Result)
    if($timedOut){throw 'Owned GPU test exceeded its timeout.'}
    $result.exitCode=$child.ExitCode
    $events=@($stdout.Result -split '\r?\n' | Where-Object {$_} | ForEach-Object {$_|ConvertFrom-Json})
    $trials=@($events|Where-Object event -CEQ 'sr_blit_fallback_trial')
    $summaries=@($events|Where-Object event -CEQ 'sr_blit_fallback_summary')
    if($child.ExitCode -ne 0 -or $summaries.Count -ne 1 -or -not $summaries[0].passed){throw 'Standalone fallback test failed; inspect retained output.'}
    if($trials.Count -ne 3 -or @($trials|Where-Object {-not $_.everyOutputComponentChecked -or -not $_.fenceComplete}).Count){throw 'Missing complete output readback.'}
    if($trials[1].sdkResult -ne -3 -or -not $trials[1].realSdkCall -or -not $trials[2].simulatedWrittenPrefix){throw 'Failure cases were not exercised.'}
    if(-not $summaries[0].validationEnabled -or $summaries[0].validationErrors -ne 0){throw 'GPU validation did not pass.'}
    $result.trials=$trials;$result.summary=$summaries[0];$result.passed=$true
} catch {$result.error=$_.Exception.Message}
$result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8NoBOM
$result|ConvertTo-Json -Depth 6
if(-not $result.passed){exit 1}
