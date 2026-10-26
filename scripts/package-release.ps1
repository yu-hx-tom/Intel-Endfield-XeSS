#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([string]$BuildDirectory='build-release',[ValidatePattern('^[0-9A-Za-z._-]+$')][string]$Version='0.1.0')
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$build=Join-Path $project $BuildDirectory
$cache=Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
foreach($setting in @('CMAKE_BUILD_TYPE:STRING=Release','XESS_RELEASE_MINIMAL:BOOL=ON','XESS_RESEARCH_DIAGNOSTICS:BOOL=OFF','XESS_DEVELOPMENT_CPU_DIAGNOSTICS:BOOL=OFF')){
    if(-not $cache.Contains($setting)){throw "Release build setting missing: $setting"}
}
$library=Join-Path $build 'observer/xess-vulkan-observer.dll'
& (Join-Path $build 'xess-observer-interface-test.exe') --layer $library --release-minimal | Out-Host
if($LASTEXITCODE){throw 'Release capability check failed.'}
$stage=& (Join-Path $PSScriptRoot 'stage-mod-launcher.ps1') -WithXeFGSdk -BuildDirectory $BuildDirectory | ConvertFrom-Json
$stageRoot=$stage.packageDirectory
$name="EndfieldXeSS-$Version-win64"
$root=Join-Path $project "dist/$name"
$archive="$root.zip"
if((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath $archive)){throw 'Release output already exists; choose a new version.'}
[IO.Directory]::CreateDirectory($root)|Out-Null
foreach($item in @('payload','mod','efu-corresponding-source','manage-observer-install.ps1')){
    Copy-Item -LiteralPath (Join-Path $stageRoot $item) -Destination $root -Recurse
}
$utf8=[Text.UTF8Encoding]::new($false)
$bom=[Text.UTF8Encoding]::new($true)
[IO.File]::WriteAllText((Join-Path $root 'Release.ps1'),(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-entry.ps1') -Raw),$bom)
foreach($action in @('Install','Verify','Uninstall','Launch')){
    $batch=@"
@echo off
setlocal
set "PS7=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not exist "%PS7%" (
  echo Please install Microsoft PowerShell 7 x64, then try again.
  echo Manual install: winget install --id Microsoft.PowerShell --source winget --installer-type wix --architecture x64
  pause
  exit /b 1
)
"%PS7%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Release.ps1" -Action $action
set "RESULT=%ERRORLEVEL%"
if /i not "$action"=="Launch" pause
exit /b %RESULT%
"@
    [IO.File]::WriteAllText((Join-Path $root "$action.cmd"),$batch.Replace("`n","`r`n"),[Text.Encoding]::ASCII)
}
Copy-Item -LiteralPath (Join-Path $project 'docs/release-minimal-readme-2026-10-24.md') -Destination (Join-Path $root 'README.md')
Copy-Item -LiteralPath (Join-Path $project 'docs/release-game-update-policy-2026-10-25.md') -Destination (Join-Path $root 'GAME-UPDATE-POLICY.md')
Copy-Item -LiteralPath (Join-Path $project 'docs/release-no-runtime-monitoring-2026-10-25.md') -Destination (Join-Path $root 'LOGGING-CONTRACT.md')
Copy-Item -LiteralPath (Join-Path $project 'third_party/minhook/LICENSE.txt') -Destination (Join-Path $root 'MINHOOK-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $project 'docs/release-third-party-notices-2026-10-24.txt') -Destination (Join-Path $root 'THIRD-PARTY-NOTICES.txt')
$licenses=Join-Path $root 'licenses'
[IO.Directory]::CreateDirectory($licenses)|Out-Null
$notices=@{
    'third_party/xess/third-party-programs.txt'='Intel-third-party-programs.txt'
    'third_party/Vulkan-Headers/LICENSE.md'='Vulkan-Headers-LICENSE.md'
    'third_party/Vulkan-Headers/LICENSES/Apache-2.0.txt'='Apache-2.0.txt'
    'third_party/Vulkan-Headers/LICENSES/MIT.txt'='MIT.txt'
    'third_party/streamline_headers/license.txt'='Streamline-license.txt'
    '.tools/llvm-mingw-20261006-ucrt-x86_64/LICENSE.TXT'='LLVM-MinGW-LICENSE.TXT'
}
foreach($entry in $notices.GetEnumerator()){Copy-Item -LiteralPath (Join-Path $project $entry.Key) -Destination (Join-Path $licenses $entry.Value)}
$toolchain=Get-Content -LiteralPath (Join-Path $project 'toolchain.lock.json') -Raw|ConvertFrom-Json
$buildInfo=[ordered]@{schema=1;version=$Version;buildType='Release';minimalRelease=$true;researchDiagnostics=$false;cpuDiagnostics=$false
    sourceReviewArchive="EndfieldXeSS-$Version-source-review.zip";bitIdenticalRebuildGuaranteed=$false
    toolchain=@($toolchain.packages|ForEach-Object {[ordered]@{name=$_.name;version=$_.version;archiveSha256=$_.sha256}})}
[IO.File]::WriteAllText((Join-Path $root 'BUILD-INFO.json'),($buildInfo|ConvertTo-Json -Depth 6),$utf8)
$files=@(Get-ChildItem -LiteralPath $root -File -Recurse|Sort-Object FullName|ForEach-Object{
    [ordered]@{path=[IO.Path]::GetRelativePath($root,$_.FullName).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest=[ordered]@{schema=1;version=$Version;flavor='release-minimal';platform='windows-x86_64';buildType='Release';researchDiagnostics=$false;cpuDiagnostics=$false;gpuDiagnostics=$false
    defaultRoute='single-chain/synchronous-shared/T3.3';defaultLaunch='XeSS SR + menu-controlled XeSS FG';gameTested=$false;files=$files}
[IO.File]::WriteAllText((Join-Path $root 'release-manifest.json'),($manifest|ConvertTo-Json -Depth 6),$utf8)
& (Join-Path $root 'Release.ps1') -Action Verify | Out-Host
if($LASTEXITCODE){throw 'Release verification failed.'}
# Zip from this explicit allowlist-derived directory, never from the checkout.
Compress-Archive -LiteralPath $root -DestinationPath $archive -CompressionLevel Optimal
$hash=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256","$hash  $name.zip`n",$utf8)
[ordered]@{directory=$root;archive=$archive;bytes=(Get-Item -LiteralPath $archive).Length;sha256=$hash;stagingDirectory=$stageRoot;fileCount=$files.Count}|ConvertTo-Json
