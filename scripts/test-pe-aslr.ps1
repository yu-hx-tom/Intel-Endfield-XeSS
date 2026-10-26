[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root ('out\self-owned-aslr-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a fresh experiment directory so previous evidence is preserved.' }
[IO.Directory]::CreateDirectory($output) | Out-Null
$compiler = Join-Path $root '.tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe'
$reader = Join-Path $root '.tools\llvm-mingw-20261006-ucrt-x86_64\bin\llvm-readobj.exe'
$dllSource = Join-Path $root 'src\pe_aslr_fixture.cpp'
$exeSource = Join-Path $root 'src\pe_aslr_test.cpp'
$header = Join-Path $root 'src\fsr_pe_guard.hpp'
$dll = Join-Path $output 'xess-pe-aslr-fixture.dll'
$exe = Join-Path $output 'pe-aslr-test.exe'
& $compiler -shared -nostdlib -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic $dllSource -o $dll '-Wl,--entry,DllMain' '-Wl,--image-base,0x180000000' '-Wl,--dynamicbase' '-Wl,--nxcompat' '-Wl,--no-insert-timestamp'
if ($LASTEXITCODE -ne 0) { throw 'Self-owned DLL compilation failed.' }
& $compiler -std=c++20 -O2 -static -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Wall -Wextra -Wpedantic $exeSource -o $exe '-Wl,--no-insert-timestamp'
if ($LASTEXITCODE -ne 0) { throw 'Self-owned EXE compilation failed.' }
$records = @()
foreach ($mode in @('normal','preferred_range_reserved')) {
    $arguments = @()
    if ($mode -eq 'preferred_range_reserved') { $arguments = @('--force-rebase') }
    $raw = & $exe @arguments 2> (Join-Path $output ($mode + '.stderr.txt'))
    $exitCode = $LASTEXITCODE
    $raw | Set-Content -LiteralPath (Join-Path $output ($mode + '.json')) -Encoding UTF8
    $record = $raw | ConvertFrom-Json
    $records += [ordered]@{ mode=$mode; exitCode=$exitCode; result=$record }
    if ($exitCode -ne 0 -or -not $record.testPassed) { throw "Self-owned ASLR experiment failed: $mode; raw evidence retained in $output" }
}
& $reader --coff-basereloc $dll | Set-Content -LiteralPath (Join-Path $output 'fixture-relocations.txt') -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw 'Self-owned fixture relocation-table inspection failed.' }
$os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture
$hashes = @(Get-FileHash -Algorithm SHA256 -LiteralPath $dllSource,$exeSource,$header,$dll,$exe | Select-Object Path,Hash)
$summary = [ordered]@{ passed=$true; operatingSystem=$os; records=$records; hashes=$hashes
    sources=@('src/pe_aslr_fixture.cpp','src/pe_aslr_test.cpp'); gameModuleLoaded=$false; externalProcessOpened=$false
    limitation='Observed normal Windows loading on this machine with a newly compiled self-owned DLL. Does not establish behavior on every Windows version or validate any game hook.' }
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding UTF8
[ordered]@{passed=$true;outputDirectory=$output;operatingSystem=$os;cases=@($records | ForEach-Object {
    [ordered]@{mode=$_.mode;exitCode=$_.exitCode;diskPreferredImageBase=$_.result.diskPreferredImageBase
        liveHeaderImageBase=$_.result.liveHeaderImageBase;actualModuleBase=$_.result.actualModuleBase
        virtualQueryAllocationBase=$_.result.virtualQueryAllocationBase;normalizationApplied=$_.result.guard.image_base_normalization_applied
        wrongActualBaseRejected=$_.result.wrongActualBaseRejected;otherNtDifferences=$_.result.ntDifferencesOutsideImageBase}
})}|ConvertTo-Json -Depth 7
