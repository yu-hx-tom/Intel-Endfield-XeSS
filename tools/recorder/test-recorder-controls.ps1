#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'Use PowerShell 7.'}
if(-not $OutputDirectory){$OutputDirectory=Join-Path ([IO.Path]::GetTempPath()) ('endfield-recorder-controls-'+[guid]::NewGuid().ToString('N'))}
$testOutput=[IO.Path]::GetFullPath($OutputDirectory)
$programData=[Environment]::GetFolderPath('CommonApplicationData').TrimEnd('\')+'\'
if($testOutput.StartsWith($programData,[StringComparison]::OrdinalIgnoreCase)){throw 'Tests may not write ProgramData.'}
if(Test-Path -LiteralPath $testOutput){throw 'Use a new owned output folder.'}
[void][IO.Directory]::CreateDirectory($testOutput)
$fixtureRoot=Join-Path $testOutput 'fixtures'
[void][IO.Directory]::CreateDirectory((Join-Path $fixtureRoot 'switches'))
[void](New-Item -ItemType Junction -Path (Join-Path $fixtureRoot 'junction-switches') -Target (Join-Path $fixtureRoot 'switches'))
[void](New-Item -ItemType Junction -Path (Join-Path $fixtureRoot 'junction-parent') -Target $fixtureRoot)
[void][IO.Directory]::CreateDirectory((Join-Path $fixtureRoot 'numeric-unsafe-switches'))
[void](New-Item -ItemType Junction -Path (Join-Path $fixtureRoot 'numeric-unsafe-switches/AsyncCopyFrames') -Target (Join-Path $fixtureRoot 'switches'))
$compiler=Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$testExe=Join-Path $testOutput 'RecorderControlsTests.exe'
& $compiler /nologo /target:exe /platform:x64 /optimize+ /langversion:5 /codepage:65001 /r:System.Web.Extensions.dll ("/out:"+$testExe) (Join-Path $PSScriptRoot 'RecorderControls.cs') (Join-Path $PSScriptRoot 'RecorderControlsTests.cs')
if($LASTEXITCODE -ne 0){throw 'Recorder controls test build failed.'}
& $testExe $fixtureRoot
if($LASTEXITCODE -ne 0){throw 'Recorder controls tests failed.'}
Get-Content -Raw -LiteralPath (Join-Path $fixtureRoot 'controls-test-result.json')
