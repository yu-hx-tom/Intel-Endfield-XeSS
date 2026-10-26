#requires -Version 7.0
#requires -PSEdition Core
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$fixture=Join-Path $project ('out/retention-fixture-'+[guid]::NewGuid().ToString('N'))
if(-not [IO.Path]::GetFullPath($fixture).StartsWith(([IO.Path]::GetFullPath($project)+'\out\'),[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe fixture root'}
[IO.Directory]::CreateDirectory($fixture)|Out-Null
$tokens=$null;$errors=$null
$managerJsonOptions=@{}
if((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')){$managerJsonOptions.DateKind='String'}
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'manage-observer-install.ps1'),[ref]$tokens,[ref]$errors)
if($errors){throw ($errors|Out-String)}
foreach($name in @('Assert-NoReparse','Get-SessionRetentionPlan','Invoke-SessionRetention')){
    $definition=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$true))
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
function Assert-ProtectedAcl { } # Fixtures only; no system ACL changes.
function Get-Process { return @() } # The fixture never inspects or touches game processes.
$checks=0
function Check($value,$why){if(-not $value){throw $why};$script:checks++}
function MakeSession([string]$Root,[int]$Number,[int]$Padding=0){
    $id=$Number.ToString('x32');$directory=Join-Path $Root $id
    [IO.Directory]::CreateDirectory($directory)|Out-Null
    [IO.File]::WriteAllText((Join-Path $directory 'launch.json'),([ordered]@{sessionId=$id;sessionDirectory=$directory;environmentForChildOnly=@{XESS_OBSERVER_LOG=(Join-Path $directory 'observer.jsonl')}}|ConvertTo-Json))
    [IO.File]::WriteAllText((Join-Path $directory 'observer.jsonl'),('x'*$Padding))
    [IO.Directory]::SetCreationTimeUtc($directory,([DateTime]::new(2020,1,1,0,0,0,[DateTimeKind]::Utc)).AddMinutes($Number))
    return $id
}
$sessionRoot=Join-Path $fixture 'count';$captureRoot=Join-Path $fixture 'captures';[IO.Directory]::CreateDirectory((Join-Path $captureRoot 'capture-a'))|Out-Null
1..23|ForEach-Object {MakeSession $sessionRoot $_|Out-Null}
[IO.File]::WriteAllText((Join-Path $captureRoot 'capture-a/session-discovery.json'),('{"state":"matched","match":{"sessionId":"'+(1).ToString('x32')+'"}}'))
$current=(23).ToString('x32');$plan=Get-SessionRetentionPlan $sessionRoot $current @($captureRoot)
Check ($plan.deleteCount -eq 3) 'Count retention boundary failed'
Check (@($plan.candidates|Where-Object {$_.id -eq $current -or $_.id -eq (1).ToString('x32')}).Count -eq 0) 'Protected session selected'
Check ($plan.candidates[0].id -eq (2).ToString('x32')) 'Oldest eligible session not selected'
$outside=Join-Path $fixture 'outside';[IO.Directory]::CreateDirectory($outside)|Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'valuable.txt'),'keep')
New-Item -ItemType Junction -Path (Join-Path $sessionRoot (24).ToString('x32')) -Target $outside|Out-Null
[IO.Directory]::CreateDirectory((Join-Path $sessionRoot 'not-a-session'))|Out-Null
$id=MakeSession $sessionRoot 25;[IO.File]::WriteAllText((Join-Path (Join-Path $sessionRoot $id) 'user-file.txt'),'keep')
$plan=Get-SessionRetentionPlan $sessionRoot $current @($captureRoot)
Check ($plan.skipped.Count -eq 3) 'Unknown/reparse session protection failed'
$result=Invoke-SessionRetention $plan
Check ($result.deleted.Count -eq 3 -and $result.failed.Count -eq 0) 'Fixture retention did not remove exactly planned sessions'
Check ([IO.File]::Exists((Join-Path $outside 'valuable.txt'))) 'Reparse target changed'
Check ([IO.File]::Exists((Join-Path (Join-Path $sessionRoot $id) 'user-file.txt'))) 'Unknown content changed'
$missing=Get-SessionRetentionPlan $sessionRoot $current @((Join-Path $fixture 'missing-captures')) 0 0
Check (-not $missing.referencesKnown -and $missing.deleteCount -eq 0) 'Unreadable references must fail closed'
$sessionRoot=Join-Path $fixture 'bytes';1..3|ForEach-Object {MakeSession $sessionRoot $_ 4096|Out-Null}
$all=Get-SessionRetentionPlan $sessionRoot (3).ToString('x32') @() 20 ([long]::MaxValue)
$total=[long](($all.sessions|Measure-Object bytes -Sum).Sum)
Check ((Get-SessionRetentionPlan $sessionRoot (3).ToString('x32') @() 3 $total).deleteCount -eq 0) 'Exact byte/count boundary deleted a session'
$small=Get-SessionRetentionPlan $sessionRoot (3).ToString('x32') @() 20 ($total-1)
Check ($small.deleteCount -eq 1) 'Byte retention boundary failed'
[IO.File]::AppendAllText((Join-Path $small.candidates[0].directory 'observer.jsonl'),'changed')
$changed=Invoke-SessionRetention $small
Check ($changed.deleted.Count -eq 0 -and $changed.failed.Count -eq 1) 'Changed session must survive stale plan'
$lockedPlan=Get-SessionRetentionPlan $sessionRoot (3).ToString('x32') @() 20 1
$lockedFile=Join-Path $lockedPlan.candidates[0].directory 'observer.jsonl'
$lease=[IO.File]::Open($lockedFile,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try{$lockedResult=Invoke-SessionRetention $lockedPlan}finally{$lease.Dispose()}
Check ($lockedResult.failed.Count -gt 0 -and $lockedResult.failed[0].removedFiles.Count -eq 0) 'Open writer must be detected before removing any files'
$wrong=Get-SessionRetentionPlan $sessionRoot (3).ToString('x32');$wrong.root=$outside;$rejected=$false
try{Invoke-SessionRetention $wrong|Out-Null}catch{$rejected=$true}
Check $rejected 'Out-of-root plan accepted'
$sessionRoot=Join-Path $fixture 'diagnostic-only'
1..3|ForEach-Object {MakeSession $sessionRoot $_|Out-Null}
foreach($number in 2..3){
    $path=Join-Path (Join-Path $sessionRoot $number.ToString('x32')) 'launch.json'
    $launch=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    $launch|Add-Member -NotePropertyName diagnosticMode -NotePropertyValue $true
    $launch|Add-Member -NotePropertyName runtimeStatusTransport -NotePropertyValue 'shared_memory_v1'
    $launch|ConvertTo-Json|Set-Content -LiteralPath $path -Encoding utf8
}
$only=Get-SessionRetentionPlan $sessionRoot (3).ToString('x32') @() 1 1MB -DiagnosticOnly
Check ($only.candidates.Count -eq 1 -and $only.candidates[0].id -ceq (2).ToString('x32')) 'RC5 retention selected a legacy session'
Check ($only.skipped.Count -eq 1 -and $only.skipped[0].reason -ceq 'not_rc5_diagnostic_session') 'Legacy diagnostic sessions must stay outside RC5 retention'
$result=Invoke-SessionRetention $only
Check ($result.deleted.Count -eq 1 -and [IO.Directory]::Exists((Join-Path $sessionRoot (1).ToString('x32')))) 'RC5 fresh recheck did not preserve legacy data'
# Exercise the recorder's actual pin helper with its ProgramData joins redirected
# into the verified fixture. No path below the real ProgramData is written.
& {
    $context=Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/recorder/RecorderContext.ps1'
    $tokens=$null;$errors=$null
    $contextAst=[Management.Automation.Language.Parser]::ParseFile($context,[ref]$tokens,[ref]$errors)
    $definition=@($contextAst.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Protect-RecorderSession'},$true))
    Check ($errors.Count -eq 0 -and $definition.Count -eq 1) 'Recorder pin helper could not be loaded'
    . ([scriptblock]::Create($definition[0].Extent.Text))
    $common=[Environment]::GetFolderPath('CommonApplicationData')
    $redirect=Microsoft.PowerShell.Management\Join-Path $fixture 'pin-fixture'
    function Join-Path {
        param([string]$Path,[string]$ChildPath)
        if($Path -ieq $common){$Path=$redirect}
        Microsoft.PowerShell.Management\Join-Path $Path $ChildPath
    }
    $pinId=(91).ToString('x32')
    $directory=Join-Path $common ('EndfieldXeSSObserver/Sessions/'+$pinId)
    $discovery=[pscustomobject]@{state='matched';match=[pscustomobject]@{sessionId=$pinId;directory=$directory}}
    Check (Protect-RecorderSession $discovery) 'Valid recorder session pin failed'
    Check ([IO.File]::Exists((Join-Path $common ('EndfieldXeSSDiag/SessionPins/'+$pinId+'.keep')))) 'Recorder pin was not written in fixture'
    $discovery.match.directory=$outside
    Check (-not (Protect-RecorderSession $discovery)) 'Out-of-root recorder session pinned'
    $discovery.match.sessionId='../outside'
    Check (-not (Protect-RecorderSession $discovery)) 'Invalid recorder session ID pinned'
    $redirect=Microsoft.PowerShell.Management\Join-Path $fixture 'pin-reparse'
    $parent=Join-Path $common 'EndfieldXeSSDiag';[IO.Directory]::CreateDirectory($parent)|Out-Null
    New-Item -ItemType Junction -Path (Join-Path $parent 'SessionPins') -Target $outside|Out-Null
    $discovery.match.sessionId=$pinId;$discovery.match.directory=Join-Path $common ('EndfieldXeSSObserver/Sessions/'+$pinId)
    Check (-not (Protect-RecorderSession $discovery) -and -not [IO.File]::Exists((Join-Path $outside ($pinId+'.keep')))) 'Recorder pin followed a reparse point'
}
[ordered]@{passed=$true;checks=$checks;fixture=$fixture;systemSessionsChanged=$false;gameProcessesTouched=$false}|ConvertTo-Json
