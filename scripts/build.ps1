#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([switch]$ResearchDiagnostics,[string]$BuildDirectory,[switch]$WithoutCpuDiagnostics,[switch]$ReleaseMinimal)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'toolchain.lock.json') -Raw | ConvertFrom-Json
function ToolPath([string]$name, [string]$relative) {
    $package = $lock.packages | Where-Object name -EQ $name
    $path = Join-Path (Join-Path (Join-Path $projectRoot '.tools') $package.directory) $relative
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing $path; run scripts/fetch-toolchain.ps1 first." }
    return $path
}
$cmake = ToolPath 'cmake' 'bin/cmake.exe'
$compiler = ToolPath 'llvm-mingw' 'bin/x86_64-w64-mingw32-clang++.exe'
$cCompiler = ToolPath 'llvm-mingw' 'bin/x86_64-w64-mingw32-clang.exe'
$ninja = ToolPath 'ninja' 'ninja.exe'
if($ReleaseMinimal -and $ResearchDiagnostics){throw 'Minimal release cannot enable research diagnostics.'}
if($ReleaseMinimal){$WithoutCpuDiagnostics=$true}
if(-not $BuildDirectory){$BuildDirectory=if($ReleaseMinimal){'build-release'}elseif($ResearchDiagnostics){'build-research'}else{'build'}}
$buildDir = if([IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory}else{Join-Path $projectRoot $BuildDirectory}
$researchMode=if($ResearchDiagnostics){'ON'}else{'OFF'}
$cpuDiagnostics=if($WithoutCpuDiagnostics){'OFF'}else{'ON'}
$configuration=if($ReleaseMinimal){'Release'}else{'RelWithDebInfo'}
$minimal=if($ReleaseMinimal){'ON'}else{'OFF'}
$releaseFlags=if($ReleaseMinimal){@('-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-s','-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-s')}else{@()}
$originalPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $compiler) + [IO.Path]::PathSeparator + $originalPath
    & $cmake -S $projectRoot -B $buildDir -G Ninja "-DCMAKE_CXX_COMPILER=$compiler" "-DCMAKE_C_COMPILER=$cCompiler" "-DCMAKE_ASM_COMPILER=$cCompiler" "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_BUILD_TYPE=$configuration" "-DXESS_RELEASE_MINIMAL=$minimal" @releaseFlags "-DXESS_RESEARCH_DIAGNOSTICS=$researchMode" "-DXESS_DEVELOPMENT_CPU_DIAGNOSTICS=$cpuDiagnostics"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmake --build $buildDir --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
} finally { $env:PATH = $originalPath }
Get-ChildItem -LiteralPath $buildDir -Filter 'xess-vulkan-*.exe' | ForEach-Object { Write-Host "Built: $($_.FullName)" }

