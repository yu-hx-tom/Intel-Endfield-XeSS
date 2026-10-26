#requires -Version 7.0
#requires -PSEdition Core
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$original=Join-Path $project 'out/efu-preview-20261015/source-dither-attached'
$source=Join-Path $project 'out/efu-launcher-options-20261023/source'
$patch=Join-Path $project 'patches/efu-launcher-options.patch'
$zig=Join-Path $project '.tools/zig-x86_64-windows-0.16.0/zig.exe'
$utf8=[Text.UTF8Encoding]::new($false)
$base=Get-Content -LiteralPath (Join-Path $original 'LOCAL-SOURCE-PROVENANCE.json') -Raw | ConvertFrom-Json
if($base.commit -cne 'eff32bbc89baaf25fcb25f2a8903990e172f319e' -or $base.localPatchId -cne 'camera-minhook-attached-v2'){throw 'Unexpected EFU source base.'}
if(-not(Test-Path -LiteralPath $source)){
    foreach($entry in $base.files){
        if($entry.path -cnotmatch '^[A-Za-z0-9._/-]+$' -or @($entry.path.Split('/')|Where-Object {$_ -eq '..'}).Count){throw 'Unsafe source entry.'}
        $from=Join-Path $original $entry.path
        if((Get-FileHash -LiteralPath $from).Hash.ToLowerInvariant() -cne $entry.sha256){throw 'EFU source base changed.'}
        $to=Join-Path $source $entry.path
        [IO.Directory]::CreateDirectory((Split-Path -Parent $to))|Out-Null
        Copy-Item -LiteralPath $from -Destination $to
    }
    & git -C $project apply --directory=out/efu-launcher-options-20261023/source $patch
    if($LASTEXITCODE){throw 'EFU launcher option patch failed.'}
    Copy-Item -LiteralPath $patch -Destination (Join-Path $source 'local-patches/efu-launcher-options.patch')
    $entries=@($base.files|ForEach-Object {$_.path})+@('local-patches/efu-launcher-options.patch')
    $files=@($entries|Sort-Object|ForEach-Object {$item=Get-Item -LiteralPath (Join-Path $source $_);[ordered]@{path=$_;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $item.FullName).Hash.ToLowerInvariant()}})
    $provenance=[ordered]@{commit=$base.commit;localPatchId='camera-minhook-launcher-options-v3';sourceModified=$true;parentProvenanceSha256=(Get-FileHash -LiteralPath (Join-Path $original 'LOCAL-SOURCE-PROVENANCE.json')).Hash.ToLowerInvariant();launcherOptionsPatchSha256=(Get-FileHash -LiteralPath $patch).Hash.ToLowerInvariant();files=$files}
    [IO.File]::WriteAllText((Join-Path $source 'LOCAL-SOURCE-PROVENANCE.json'),($provenance|ConvertTo-Json -Depth 6),$utf8)
}
$prepared=Get-Content -LiteralPath (Join-Path $source 'LOCAL-SOURCE-PROVENANCE.json') -Raw|ConvertFrom-Json
if($prepared.launcherOptionsPatchSha256 -cne (Get-FileHash -LiteralPath $patch).Hash.ToLowerInvariant()){throw 'EFU patch changed; preserve the old build before rebuilding.'}
foreach($entry in $prepared.files){if((Get-FileHash -LiteralPath (Join-Path $source $entry.path)).Hash.ToLowerInvariant() -cne $entry.sha256){throw 'Prepared EFU source changed.'}}
Push-Location -LiteralPath $source
try {
    & $zig build -Doptimize=ReleaseSmall
    if($LASTEXITCODE){throw 'EFU option build failed.'}
    & $zig test src/dll.zig vendor/minhook/src/buffer.c vendor/minhook/src/hook.c vendor/minhook/src/trampoline.c vendor/minhook/src/hde/hde64.c -I vendor/minhook/include -lc -O ReleaseSafe
    if($LASTEXITCODE){throw 'EFU option CPU regressions failed.'}
} finally {Pop-Location}
[ordered]@{sourceDirectory=$source;localPatchId=$prepared.localPatchId;candidatePath=(Join-Path $source 'zig-out/bin/EFU.exe');candidateSha256=(Get-FileHash -LiteralPath (Join-Path $source 'zig-out/bin/EFU.exe')).Hash.ToLowerInvariant();cpuTestsPassed=$true;gameStarted=$false}|ConvertTo-Json
