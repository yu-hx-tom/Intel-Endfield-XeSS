[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $root 'out\efu-pitch-prefix-test'
$minHookRoot = Join-Path $root 'third_party\minhook'
$minHookPin = Get-Content -LiteralPath (Join-Path $root 'third_party\minhook.lock.json') -Raw | ConvertFrom-Json
if ($minHookPin.commit -cne 'c3fcafdc10146beb5919319d0683e44e3c30d537') { throw 'Unexpected MinHook pin.' }
$toolchain = Get-Content -LiteralPath (Join-Path $root 'toolchain.lock.json') -Raw | ConvertFrom-Json
$llvmPackage = @($toolchain.packages | Where-Object name -EQ 'llvm-mingw')
if ($llvmPackage.Count -ne 1) { throw 'Expected one pinned llvm-mingw package.' }
$compilerDirectory = Join-Path (Join-Path $root '.tools') ($llvmPackage[0].directory + '\bin')
$clang = Join-Path $compilerDirectory 'clang.exe'
$clangCpp = Join-Path $compilerDirectory 'clang++.exe'
$zig = Join-Path $root '.tools\zig-x86_64-windows-0.16.0\zig.exe'
$zigArchive = Join-Path $root '.tools\zig-x86_64-windows-0.16.0.zip'
$decoderSourcePath = Join-Path $root 'out\efu-preview-20261015\source\src\dll.zig'
$fixtureSource = Join-Path $root 'src\efu_pitch_prefix_test.cpp'
foreach ($path in @($clang, $clangCpp, $zig, $zigArchive, $decoderSourcePath, $fixtureSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing test dependency: $path" }
}
$zigHash = (Get-FileHash -LiteralPath $zigArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($zigHash -cne '68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e') { throw 'Zig archive does not match the official 0.16.0 pin.' }
$decoderHash = (Get-FileHash -LiteralPath $decoderSourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($decoderHash -cne 'a7cfe9b7a99f45accd10d1ad4f1c1ba80658f0aa83653589390aa146d69b0f9d') { throw 'Original EFU decoder source differs from pinned eff32.' }
$zigVersion = (& $zig version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $zigVersion -cne '0.16.0') { throw 'Zig 0.16.0 is required.' }
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

# Extract only the unchanged pure decoder functions; never compile the original runtime DLL.
$decoderText = [IO.File]::ReadAllText($decoderSourcePath).Replace("`r`n", "`n")
$decoderParts = @(
    '// Original EFU decoder, eff32bbc89baaf25fcb25f2a8903990e172f319e; AGPL-3.0.',
    '// See the pinned EFU source LICENSE; this file is generated only for the offline test.',
    'const std = @import("std");',
    'const JUMP_SIZE: usize = 12;',
    'const MAX_STOLEN_BYTES: usize = 32;'
)
foreach ($name in @('hasModRm', 'modRmLen', 'instructionLen', 'patchLen')) {
    $start = $decoderText.IndexOf(('fn ' + $name + '('), [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing original decoder function: $name" }
    $end = $decoderText.IndexOf("`n}`n", $start, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Incomplete original decoder function: $name" }
    $decoderParts += $decoderText.Substring($start, $end + 3 - $start)
}
$prefix = [Convert]::FromHexString('40534881ec90000000488b057098100c488bd9488b48404885c90f8556f2cf01')
$prefixBytes = ($prefix | ForEach-Object { '0x{0:x2}' -f $_ }) -join ','
$decoderParts += @"
test "current pitch prologue fails the old twelve-byte overwrite boundary" {
    const prefix = [_]u8{$prefixBytes};
    const ptr: [*]const u8 = @ptrCast(&prefix);
    try std.testing.expectEqual(@as(?usize, 2), instructionLen(ptr, 0));
    try std.testing.expectEqual(@as(?usize, 7), instructionLen(ptr, 2));
    try std.testing.expectEqual(@as(?usize, null), instructionLen(ptr, 9));
    try std.testing.expectEqual(@as(?usize, null), patchLen(@ptrCast(&prefix)));
}
"@
$generatedDecoder = Join-Path $outputDirectory 'old_decoder_repro.zig'
[IO.File]::WriteAllText($generatedDecoder, ($decoderParts -join "`n`n"), [Text.UTF8Encoding]::new($false))

Push-Location $outputDirectory
try {
    & $clang -O2 -DNDEBUG -I (Join-Path $minHookRoot 'include') -I (Join-Path $minHookRoot 'src') -c `
        (Join-Path $minHookRoot 'src\buffer.c') (Join-Path $minHookRoot 'src\hook.c') `
        (Join-Path $minHookRoot 'src\trampoline.c') (Join-Path $minHookRoot 'src\hde\hde64.c')
    if ($LASTEXITCODE -ne 0) { throw 'Private MinHook object build failed.' }
    & $clangCpp -std=c++20 -O2 -Wall -Wextra -static -I (Join-Path $minHookRoot 'include') `
        $fixtureSource '.\buffer.o' '.\hook.o' '.\trampoline.o' '.\hde64.o' -o '.\efu_pitch_prefix_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Private fixture link failed.' }
    $nativeOutput = & '.\efu_pitch_prefix_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Owned native prefix fixture failed.' }
    $nativeResult = $nativeOutput | ConvertFrom-Json
    if (-not $nativeResult.passed -or $nativeResult.gameDllLoaded -or $nativeResult.minhookCommit -cne $minHookPin.commit) { throw 'Unexpected native fixture result.' }
    $nativeOutput | Set-Content -LiteralPath '.\owned-prefix-result.json' -Encoding utf8
    & $zig test $generatedDecoder --cache-dir '.\zig-cache' --global-cache-dir '.\zig-global-cache' 2>&1 |
        Tee-Object -FilePath '.\old-decoder-test.txt'
    if ($LASTEXITCODE -ne 0) { throw 'Original decoder regression failed.' }
    $report = [ordered]@{
        passed = $true
        minHookCommit = $minHookPin.commit
        originalDecoderSourceSha256 = $decoderHash
        originalDecoderRegressionPassed = $true
        sourceSha256 = (Get-FileHash -LiteralPath $fixtureSource -Algorithm SHA256).Hash.ToLowerInvariant()
        binarySha256 = (Get-FileHash -LiteralPath '.\efu_pitch_prefix_test.exe' -Algorithm SHA256).Hash.ToLowerInvariant()
        nativeFixture = $nativeResult
        limitation = 'Owned prologue and trampoline behavior only; no game DLL or production runtime was loaded. MinHook copies nine bytes, leaving the RIP-relative load in original code.'
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath '.\verification-report.json' -Encoding utf8
    Write-Output ('PASS: exact prefix, original decoder rejection, trampoline, feature toggle, enable/disable/remove. Report: ' + (Join-Path $outputDirectory 'verification-report.json'))
} finally {
    Pop-Location
}
