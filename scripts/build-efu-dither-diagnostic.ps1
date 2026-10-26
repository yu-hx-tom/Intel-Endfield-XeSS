[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$base = Join-Path $project 'out\efu-preview-20261015'
$upstream = Join-Path $base 'source'
$source = Join-Path $base 'source-dither-attached'
$provenancePath = Join-Path $base 'source-provenance.json'
$patch = Join-Path $project 'patches\efu-dither-minhook.patch'
$diagnostic = Join-Path $project 'patches\efu-runtime-diagnostics.zig'
$zig = Join-Path $project '.tools\zig-x86_64-windows-0.16.0\zig.exe'
$utf8 = [Text.UTF8Encoding]::new($false)
$provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
if ($provenance.commit -cne 'eff32bbc89baaf25fcb25f2a8903990e172f319e' -or $provenance.files.Count -ne 27) { throw 'Unexpected upstream EFU source identity.' }
foreach ($entry in $provenance.files) {
    if ($entry.path -cnotmatch '^[A-Za-z0-9._/-]+$' -or $entry.path.StartsWith('/') -or @($entry.path.Split('/') | Where-Object { $_ -eq '..' }).Count) { throw 'Unsafe source path.' }
    if ((Get-FileHash -LiteralPath (Join-Path $upstream $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) { throw 'Original reviewed EFU source changed.' }
}
& (Join-Path $PSScriptRoot 'fetch-minhook.ps1') | Out-Null
$patchHash = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash.ToLowerInvariant()
$diagnosticHash = (Get-FileHash -LiteralPath $diagnostic -Algorithm SHA256).Hash.ToLowerInvariant()
$manifestPath = Join-Path $source 'LOCAL-SOURCE-PROVENANCE.json'
if (Test-Path -LiteralPath $source) {
    $prepared = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($prepared.patchSha256 -cne $patchHash -or $prepared.diagnosticModuleSha256 -cne $diagnosticHash) { throw 'Existing derived source belongs to another patch; preserve it before preparing a new build.' }
    foreach ($entry in $prepared.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $source $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) { throw 'Prepared derived source changed.' }
    }
} else {
    [IO.Directory]::CreateDirectory($source) | Out-Null
    foreach ($entry in $provenance.files) {
        $destination = Join-Path $source $entry.path
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $upstream $entry.path) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $project 'third_party\minhook') -Destination (Join-Path $source 'vendor\minhook') -Recurse
    Copy-Item -LiteralPath $diagnostic -Destination (Join-Path $source 'src\runtime_diagnostics.zig')
    & git -C $project apply --ignore-space-change --directory=out/efu-preview-20261015/source-dither-attached $patch
    if ($LASTEXITCODE -ne 0) { throw 'Local EFU source patch did not apply exactly.' }
    $localPatches = Join-Path $source 'local-patches'
    [IO.Directory]::CreateDirectory($localPatches) | Out-Null
    Copy-Item -LiteralPath $patch -Destination (Join-Path $localPatches 'efu-dither-minhook.patch')
    Copy-Item -LiteralPath $provenancePath -Destination (Join-Path $localPatches 'UPSTREAM-SOURCE-PROVENANCE.json')
    Copy-Item -LiteralPath (Join-Path $project 'third_party\minhook.lock.json') -Destination (Join-Path $localPatches 'minhook.lock.json')
    $files = @(Get-ChildItem -LiteralPath $source -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path=$_.FullName.Substring($source.Length + 1).Replace('\','/'); bytes=$_.Length
            sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $prepared = [ordered]@{ commit=$provenance.commit; localPatchId='camera-minhook-attached-v2'; sourceModified=$true
        patchSha256=$patchHash; diagnosticModuleSha256=$diagnosticHash
        upstreamProvenanceSha256=(Get-FileHash -LiteralPath $provenancePath -Algorithm SHA256).Hash.ToLowerInvariant()
        minhookCommit='c3fcafdc10146beb5919319d0683e44e3c30d537'; files=$files }
    [IO.File]::WriteAllText($manifestPath, ($prepared | ConvertTo-Json -Depth 6), $utf8)
}
if ((& $zig version) -cne '0.16.0') { throw 'This source build requires Zig 0.16.0.' }
Push-Location -LiteralPath $source
try {
    & $zig build -Doptimize=ReleaseSmall
    if ($LASTEXITCODE -ne 0) { throw 'EFU diagnostic build failed.' }
    # Save the same DLL embedded by the full EXE build, without rebuilding it
    # using the separate DLL-only resource configuration.
    $dlls = @(Get-ChildItem -LiteralPath (Join-Path $source '.zig-cache') -Filter EFUHook.dll -Recurse -File)
    if ($dlls.Count -ne 1) { throw 'Could not identify the exact embedded EFU DLL.' }
    Copy-Item -LiteralPath $dlls[0].FullName -Destination (Join-Path $source 'zig-out\bin\EFU.dll') -Force
    & $zig test src/dll.zig vendor/minhook/src/buffer.c vendor/minhook/src/hook.c vendor/minhook/src/trampoline.c vendor/minhook/src/hde/hde64.c -I vendor/minhook/include -lc -O ReleaseSafe
    if ($LASTEXITCODE -ne 0) { throw 'EFU source callback/decoder CPU regressions failed.' }
} finally { Pop-Location }
$record = [ordered]@{ sourceDirectory=$source; sourceCommit=$provenance.commit; localPatchId='camera-minhook-attached-v2'
    sourceModified=$true; patchSha256=$patchHash; sourceProvenancePath=$manifestPath
    compilerVersion='0.16.0'; buildExitCode=0; selfOwnedCpuTestsPassed=$true
    candidatePath=(Join-Path $source 'zig-out\bin\EFU.exe')
    candidateSha256=(Get-FileHash -LiteralPath (Join-Path $source 'zig-out\bin\EFU.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    embeddedDllPath=(Join-Path $source 'zig-out\bin\EFU.dll')
    embeddedDllSha256=(Get-FileHash -LiteralPath (Join-Path $source 'zig-out\bin\EFU.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    candidateGameExecutionPerformed=$false; gameLaunched=$false; installedFilesModified=$false }
[IO.File]::WriteAllText((Join-Path $base 'dither-build-result.json'), ($record | ConvertTo-Json -Depth 6), $utf8)
$record | ConvertTo-Json -Depth 6
