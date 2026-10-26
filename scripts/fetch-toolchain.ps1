[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$toolsRoot = Join-Path $projectRoot '.tools'
$cacheRoot = Join-Path $projectRoot '.cache/toolchain'
$lockPath = Join-Path $projectRoot 'toolchain.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($lock.schemaVersion -ne 1 -or $lock.platform -ne 'windows-x86_64') {
    throw 'Unsupported toolchain lock schema or platform.'
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'This toolchain requires a 64-bit Windows host.'
}

function Get-SafeChild([string] $Root, [string] $Child) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $childFull = [IO.Path]::GetFullPath((Join-Path $rootFull $Child))
    if (-not $childFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes its expected root: $Child"
    }
    return $childFull
}

function Assert-Archive([string] $Path, $Package) {
    if ((Get-Item -LiteralPath $Path).Length -ne $Package.sizeBytes) {
        throw "Archive length mismatch: $Path"
    }
    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $Package.sha256) {
        throw "SHA256 mismatch for $Path. Expected $($Package.sha256); got $actualHash."
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Path $toolsRoot, $cacheRoot -Force | Out-Null
foreach ($package in $lock.packages) {
    $archivePath = Get-SafeChild $cacheRoot $package.archive
    $installPath = Get-SafeChild $toolsRoot $package.directory
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        $partialPath = $archivePath + '.partial'
        Write-Host "Downloading $($package.name) $($package.version)..."
        $curlCommand = Get-Command curl.exe -ErrorAction SilentlyContinue
        if ($null -ne $curlCommand) {
            & $curlCommand.Source --fail --location --retry 3 --retry-all-errors --connect-timeout 30 --max-time 600 --silent --show-error --output $partialPath $package.url
            if ($LASTEXITCODE -ne 0) { throw "Download failed for $($package.name): curl exit $LASTEXITCODE" }
        } else {
            Invoke-WebRequest -UseBasicParsing -Uri $package.url -OutFile $partialPath
        }
        Assert-Archive $partialPath $package
        Move-Item -LiteralPath $partialPath -Destination $archivePath -Force
    }
    Assert-Archive $archivePath $package

    # Compare installed entry points against bytes from the verified upstream archive.
    # A missing or changed entry point causes re-extraction, never a false success.
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $expectedHashes = @{}
        $needsExtraction = $false
        foreach ($relativeExe in $package.executables) {
            $entryName = $relativeExe
            if ($package.archiveContainsRoot) { $entryName = $package.directory + '/' + $relativeExe }
            $entry = $zip.GetEntry($entryName)
            if ($null -eq $entry) { throw "Required entry missing from verified archive: $entryName" }
            $entryStream = $entry.Open()
            $hashAlgorithm = [Security.Cryptography.SHA256]::Create()
            try {
                $expectedHash = [BitConverter]::ToString($hashAlgorithm.ComputeHash($entryStream)).Replace('-', '').ToLowerInvariant()
            } finally {
                $hashAlgorithm.Dispose()
                $entryStream.Dispose()
            }
            $expectedHashes[$relativeExe] = $expectedHash
            $installedExe = Get-SafeChild $installPath $relativeExe
            if (-not (Test-Path -LiteralPath $installedExe -PathType Leaf)) {
                $needsExtraction = $true
            } elseif ((Get-FileHash -LiteralPath $installedExe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
                $needsExtraction = $true
            }
        }
    } finally { $zip.Dispose() }

    if ($needsExtraction) {
        Write-Host "Extracting $($package.name) $($package.version)..."
        $destination = $installPath
        if ($package.archiveContainsRoot) { $destination = $toolsRoot }
        Expand-Archive -LiteralPath $archivePath -DestinationPath $destination -Force
    }
    foreach ($relativeExe in $package.executables) {
        $installedExe = Get-SafeChild $installPath $relativeExe
        $installedHash = (Get-FileHash -LiteralPath $installedExe -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($installedHash -ne $expectedHashes[$relativeExe]) { throw "Extracted executable hash mismatch: $installedExe" }
    }
    $versionExe = Get-SafeChild $installPath $package.executables[0]
    $versionOutput = & $versionExe --version 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Tool failed its version check: $versionExe (exit $LASTEXITCODE)" }
    Write-Host ($versionOutput | Select-Object -First 1)
    Write-Host "Ready: $versionExe"
}
