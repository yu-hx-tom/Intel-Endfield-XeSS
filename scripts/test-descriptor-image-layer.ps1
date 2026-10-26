[CmdletBinding()]
param([switch]$RawDriverOnly,[string]$RawDriverBaseline,[switch]$TemplateFixture)
$ErrorActionPreference='Stop'
if($RawDriverOnly -and $RawDriverBaseline){throw 'Choose either a new raw-only run or a saved raw baseline.'}
$project=Split-Path -Parent $PSScriptRoot
if(@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count){throw 'Normally exit the game before the self-owned descriptor GPU smoke.'}
$build=Join-Path $project 'build'
$exe=Join-Path $build $(if($TemplateFixture){'xess-descriptor-template-gpu-smoke.exe'}else{'xess-descriptor-image-gpu-smoke.exe'})
$expectedDispatches=if($TemplateFixture){4}else{8}
$expectedAccepted=if($TemplateFixture){3}else{5}
$expectedRefused=if($TemplateFixture){1}else{3}
$layer=(Resolve-Path -LiteralPath (Join-Path $build 'observer')).Path
$dll=Join-Path $layer 'xess-vulkan-observer.dll'
$report=Join-Path $project ('out/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+$(if($TemplateFixture){'-descriptor-template-gpu'}else{'-descriptor-image-gpu'}))
[IO.Directory]::CreateDirectory($report)|Out-Null

function Invoke-DescriptorFixture([string]$name,[bool]$withLayer){
    $directory=Join-Path $report $name
    [IO.Directory]::CreateDirectory($directory)|Out-Null
    $log=Join-Path $directory 'observer.jsonl'
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$exe;$start.WorkingDirectory=$directory
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($variable in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE',
        'XESS_OBSERVER_LOG','XESS_FSR_OBSERVER','XESS_SL_OBSERVER','XESS_COMPUTE_OBSERVER','XESS_BRIDGE_DEVICE','XESS_FSR_UPSCALE_OBSERVER')){
        $start.EnvironmentVariables.Remove($variable)
    }
    # Only this child changes loader selection; installed registrations stay intact.
    $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
    if($withLayer){
        $start.EnvironmentVariables['VK_LAYER_PATH']=$layer
        $start.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_XESS_bridge_observer'
        $start.EnvironmentVariables['XESS_OBSERVER_LOG']=$log
        $start.EnvironmentVariables['XESS_FSR_OBSERVER']='1'
    }
    $child=[Diagnostics.Process]::new();$child.StartInfo=$start
    if(-not $child.Start()){throw 'Could not start self-owned descriptor GPU fixture'}
    $output=$child.StandardOutput.ReadToEndAsync();$errors=$child.StandardError.ReadToEndAsync()
    $watch=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
    while(-not $child.WaitForExit(250)){
        if($watch.Elapsed.TotalSeconds -ge 30){$timedOut=$true;$child.Kill();break}
    }
    $child.WaitForExit()
    $stdout=$output.GetAwaiter().GetResult();$stderr=$errors.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $directory 'stdout.jsonl'),$stdout)
    [IO.File]::WriteAllText((Join-Path $directory 'stderr.txt'),$stderr)
    $problems=[Collections.Generic.List[string]]::new()
    $rows=@();$events=@();$test=$null
    try{$rows=@($stdout -split '\r?\n'|Where-Object {$_ -match '^\{'}|ForEach-Object {$_|ConvertFrom-Json})}
    catch{$problems.Add('Fixture stdout JSON could not be parsed')}
    $test=@($rows|Where-Object {$_.testPassed -eq $true}|Select-Object -Last 1)
    $expected=@($rows|Where-Object event -EQ 'fixture_dispatch')
    $module=@($rows|Where-Object event -EQ 'fixture_module_provenance')
    $actualModulePath=$null;$actualModuleSha256=$null
    if($module.Count -ne 1 -or $module[0].observer_loaded -ne $withLayer){$problems.Add('Raw/staged actual observer module presence mismatch')}
    if($withLayer -and $module.Count -eq 1 -and $module[0].actual_observer_path){
        $actualModulePath=$module[0].actual_observer_path
        try{
            $actualResolved=(Resolve-Path -LiteralPath $actualModulePath).Path
            $expectedResolved=(Resolve-Path -LiteralPath $dll).Path
            if(-not [string]::Equals($actualResolved,$expectedResolved,[StringComparison]::OrdinalIgnoreCase)){$problems.Add('Actually loaded observer path differs from staged build')}
            $actualModuleSha256=(Get-FileHash -LiteralPath $actualResolved -Algorithm SHA256).Hash.ToLowerInvariant()
        }catch{$problems.Add('Actually loaded module path/hash could not be verified')}
    }elseif($withLayer){$problems.Add('Loaded observer did not report an actual path')}
    if($timedOut -or $child.ExitCode -ne 0){$problems.Add('Fixture failed or timed out')}
    if($test.Count -ne 1 -or $expected.Count -ne $expectedDispatches){$problems.Add('Fixture dispatch count or successful completion missing')}
    if($test.Count -eq 1 -and (-not $test[0].cleanupSucceeded -or $test[0].submissions -ne 2 -or $test[0].shaderReadsImages -or $test[0].pixelsReadBack)){
        $problems.Add('Fixture tail cleanup or no-pixel contract failed')
    }
    if($TemplateFixture){
        $capability=@($rows|Where-Object event -EQ 'fixture_template_capability')
        if($capability.Count -ne 1 -or -not $capability[0].khrExtensionEnumerated -or -not $capability[0].khrExtensionEnabled -or
            -not $capability[0].khrAliasesAvailable -or $capability[0].coreFallbackUsed -or $test[0].coreTemplateUpdates -ne 2 -or $test[0].khrTemplateUpdates -ne 2){
            $problems.Add('Explicit KHR template extension/aliases or core/KHR call counts missing')
        }
    }
    $groups=@();$bindings=@();$summary=@()
    if($withLayer){
        if(Test-Path -LiteralPath $log){
            try{$events=@(Get-Content -LiteralPath $log|ForEach-Object {$_|ConvertFrom-Json})}
            catch{$problems.Add('Observer JSON could not be parsed')}
        }else{$problems.Add('Requested explicit observer did not produce a log')}
        $groups=@($events|Where-Object event -EQ 'descriptor_pre_dispatch')
        $bindings=@($events|Where-Object event -EQ 'descriptor_image_binding')
        $summary=@($events|Where-Object event -EQ 'descriptor_image_summary'|Select-Object -Last 1)
        if($groups.Count -ne $expectedDispatches -or $bindings.Count -ne ($expectedAccepted*3) -or $summary.Count -ne 1){$problems.Add('Observer dispatch/binding detail or summary counts differ')}
        foreach($entry in $expected){
            $actual=@($groups|Where-Object dispatch_call -EQ $entry.dispatch_call)
            if($actual.Count -ne 1){$problems.Add('Dispatch ordinal missing or duplicated');continue}
            $actual=$actual[0]
            if($actual.metadata_accepted -ne $entry.expected_accepted -or $actual.command -ne $entry.command -or $actual.pipeline -ne $entry.pipeline){
                $problems.Add('Dispatch acceptance or exact command/pipeline mismatch')
            }
            $groupExpected=if($entry.indirect){'0,0,0'}else{'1,1,1'}
            if(($actual.groups -join ',') -ne $groupExpected){$problems.Add('Dispatch group metadata mismatch')}
            if($TemplateFixture -and $actual.ignored_non_image_bindings -ne 3){$problems.Add('Mixed template dispatch did not separately ignore three non-image bindings')}
            if($entry.expected_accepted){
                if($actual.issues -ne 0 -or $actual.binding_count -ne 3 -or $actual.command_generation -le 0 -or $actual.pipeline_generation -le 0){$problems.Add('Accepted dispatch lacks complete three-binding identities')}
                $details=@($bindings|Where-Object dispatch_call -EQ $entry.dispatch_call)
                if($details.Count -ne 3){$problems.Add('Accepted dispatch binding details missing')}
                foreach($detail in $details){
                    $index=[int]$detail.array_element
                    if($index -lt 0 -or $index -ge 3){$problems.Add('Binding array element out of fixture range');continue}
                    if($detail.image -ne $entry.images[$index] -or $detail.view -ne $entry.views[$index] -or $detail.set -ne $entry.set -or
                        $detail.image_generation -le 0 -or $detail.view_generation -le 0 -or $detail.set_generation -le 0 -or
                        $detail.format -ne $entry.format -or $detail.view_format -ne $entry.format -or $detail.width -ne 64 -or $detail.height -ne 64 -or
                        $detail.declared_layout -ne $entry.declared_layout -or $detail.aspect_mask -ne 1 -or $detail.level_count -ne 1 -or $detail.layer_count -ne 1 -or
                        $detail.source_match_mask -ne 0 -or $detail.source_samples.Count -ne 0){$problems.Add('Binding exact image/view/generation/range/source metadata mismatch')}
                }
            }elseif($actual.issues -eq 0){$problems.Add('Rejected dispatch lacks an explicit reason')}
        }
        if($summary.Count -eq 1){
            $value=$summary[0]
            if($value.dispatch_calls -ne $expectedDispatches -or $value.accepted_dispatches -ne $expectedAccepted -or $value.refused_dispatches -ne $expectedRefused -or
                $value.failures -ne 0 -or $value.fatal_issues -ne 0 -or $value.capacity_refusals -ne 0 -or $value.source_observations -ne 0 -or
                @($value.object_usage|Where-Object live -NE 0).Count -ne 0){$problems.Add('Observer summary counts, source independence or final object cleanup mismatch')}
            if($TemplateFixture){
                $templateIssue=@($value.template_refused_issue_records|Where-Object issue_bit -EQ 64)
                $templateObjects=@($value.object_usage|Where-Object kind -EQ 'descriptor_update_templates')
                if($value.core_template_update_calls -ne 2 -or $value.khr_template_update_calls -ne 2 -or $value.ordinary_update_calls -ne 0 -or
                    $value.template_update_refusals -ne 1 -or $value.template_read_refusals -ne 0 -or $templateIssue.Count -ne 1 -or
                    $templateIssue[0].count -ne 1 -or $templateObjects.Count -ne 1 -or $templateObjects[0].live -ne 0){
                    $problems.Add('Template counters, local-value refusal or destroyed-template lifetime mismatch')
                }
            }
        }
        $first=@($bindings|Where-Object dispatch_call -EQ 1|Select-Object -First 1)
        $newOrdinal=if($TemplateFixture){4}else{6}
        $new=@($bindings|Where-Object dispatch_call -EQ $newOrdinal|Select-Object -First 1)
        if($first.Count -eq 1 -and $new.Count -eq 1 -and (($new[0].set_generation -le $first[0].set_generation -and -not $TemplateFixture) -or $new[0].command_generation -le $first[0].command_generation)){
            $problems.Add('Reset/reallocation did not advance command and descriptor-set generations')
        }
        $claims=@('complete_resource_set','snapshot_atomic','same_frame_verified','source_semantics_verified','undilated_verified','shader_semantics_verified',
            'layout_verified','submitted','gpu_complete','resource_lifetime_pinned','copy_permitted','pixels_captured','xefg_integrated')
        foreach($event in @($groups)+@($bindings)+@($summary)){
            foreach($claim in $claims){if($event.$claim -ne $false){$problems.Add('Observer raised or omitted prohibited claim: '+$claim)}}
        }
        if(-not ($events.event -contains 'destroy_device')){$problems.Add('Observer did not see normal device destruction')}
    }
    $record=[ordered]@{name=$name;passed=$problems.Count -eq 0;exitCode=$child.ExitCode;timedOut=$timedOut;
        elapsedSeconds=$watch.Elapsed.TotalSeconds;explicitObserverRequested=$withLayer;fixture=$test;observerSummary=$summary;
        actualObserverPath=$actualModulePath;actualObserverSha256=$actualModuleSha256;moduleProvenance=$module;
        requestedLayerDirectory=if($withLayer){$layer}else{$null};templateFixture=[bool]$TemplateFixture;
        expectedDispatches=$expectedDispatches;expectedAccepted=$expectedAccepted;expectedRefused=$expectedRefused;
        dispatchDetails=$groups;bindingDetails=$bindings;validationProblems=@($problems);stdout='stdout.jsonl';stderr='stderr.txt';
        gameTested=$false;installedPayloadChanged=$false;shaderReadsImages=$false;pixelsReadBack=$false;xefgIntegrated=$false}
    $record|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'result.json') -Encoding utf8
    return $record
}

$baselinePath=$null
if($RawDriverBaseline){
    $baselinePath=(Resolve-Path -LiteralPath $RawDriverBaseline).Path
    $savedBaseline=Get-Content -LiteralPath $baselinePath -Raw|ConvertFrom-Json
    $currentExecutableHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    if($savedBaseline.schema -ne 'descriptor-image-gpu-v1' -or -not $savedBaseline.rawDriver.passed -or
        $savedBaseline.rawDriver.explicitObserverRequested -or $savedBaseline.executableSha256 -ne $currentExecutableHash -or
        [bool]$savedBaseline.templateFixture -ne [bool]$TemplateFixture){
        throw 'Saved raw-driver baseline is not a passing run of this exact fixture binary.'
    }
    $raw=$savedBaseline.rawDriver
}else{$raw=Invoke-DescriptorFixture 'raw-driver' $false}
$staged=$null
if($raw.passed -and -not $RawDriverOnly){$staged=Invoke-DescriptorFixture 'staged-layer' $true}
$passed=$raw.passed -and ($RawDriverOnly -or ($null -ne $staged -and $staged.passed))
$sourceHashes=[ordered]@{}
foreach($name in @('src/descriptor_image_gpu_smoke.cpp','src/descriptor_template_gpu_smoke.cpp','src/vulkan_observer.cpp','src/descriptor_image_metadata.hpp','src/descriptor_image_metadata.cpp',
    'src/descriptor_image_observer.hpp','src/descriptor_image_observer.cpp','scripts/test-descriptor-image-layer.ps1','CMakeLists.txt')){
    $sourceHashes[$name]=(Get-FileHash -LiteralPath (Join-Path $project $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$result=[ordered]@{schema='descriptor-image-gpu-v1';capturedAt=[DateTimeOffset]::Now.ToString('o');passed=$passed;
    stage=if($RawDriverOnly){'raw_driver_baseline_only'}else{'raw_driver_and_staged_layer'};rawDriverBaselineReused=$baselinePath;
    executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();
    layerSha256=if($RawDriverOnly){$null}else{(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()};
    manifestSha256=if($RawDriverOnly){$null}else{(Get-FileHash -LiteralPath (Join-Path $layer 'VkLayer_xess_observer.json') -Algorithm SHA256).Hash.ToLowerInvariant()};
    requestedLayerDirectory=if($RawDriverOnly){$null}else{$layer};templateFixture=[bool]$TemplateFixture;
    expectedDispatches=$expectedDispatches;expectedAccepted=$expectedAccepted;expectedRefused=$expectedRefused;sourceSha256=$sourceHashes;
    rawDriver=$raw;stagedLayer=$staged;gameTested=$false;installedPayloadChanged=$false;validationLayersRequested=$false;
    shaderReadsImages=$false;pixelsReadBack=$false;xefgIntegrated=$false;copyPermitted=$false}
$result|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding utf8
Write-Output (Join-Path $report 'result.json')
if(-not $passed){throw 'Self-owned descriptor GPU/observer evidence did not pass; inspect saved results.'}
