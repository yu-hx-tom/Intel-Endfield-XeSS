# Fetch the exact official SDK archives recorded in dependencies.lock.json.
# No installer, SDK executable, or system configuration change is performed.
[CmdletBinding()]
param([switch]$VerifyOnly)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$thirdPartyRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'third_party'))
$cacheRoot = Join-Path $thirdPartyRoot '.cache'
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
if ($lock.schemaVersion -ne 1) { throw 'Unsupported dependencies.lock.json schema.' }

function Get-ChildPath([string]$Root, [string]$RelativePath) {
    if ([System.IO.Path]::IsPathRooted($RelativePath)) { throw "Expected relative path: $RelativePath" }
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $fullPath = [System.IO.Path]::GetFullPath((Join-Path $fullRoot $RelativePath))
    if (-not $fullPath.StartsWith($fullRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes its intended directory: $RelativePath"
    }
    return $fullPath
}

function Assert-Hash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing dependency file: $Path" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw "SHA256 mismatch: $Path. Existing data is retained for inspection."
    }
}

function Assert-CriticalFiles([string]$Root, $Dependency) {
    foreach ($file in $Dependency.criticalFiles) {
        Assert-Hash (Get-ChildPath $Root $file.path) $file.sha256
    }
    foreach ($licensePath in $Dependency.licensePaths) {
        if (-not (Test-Path -LiteralPath (Get-ChildPath $Root $licensePath) -PathType Leaf)) {
            throw "Missing upstream license: $licensePath"
        }
    }
}

if (-not $VerifyOnly) { New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null }
Add-Type -AssemblyName System.IO.Compression.FileSystem

foreach ($dependency in $lock.dependencies) {
    $archivePath = Get-ChildPath $cacheRoot $dependency.archive
    $destination = Get-ChildPath $thirdPartyRoot $dependency.destination
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        if ($VerifyOnly) { throw "Archive not cached: $archivePath" }
        $uri = [System.Uri]$dependency.url
        if ($uri.Scheme -ne 'https' -or $uri.Host -notin @('github.com', 'codeload.github.com')) {
            throw "Unexpected SDK download origin: $uri"
        }
        $partialPath = Get-ChildPath $cacheRoot ($dependency.archive + '.' + [guid]::NewGuid().ToString('N') + '.part')
        Write-Host "Downloading $($dependency.name) $($dependency.version)..."
        Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $partialPath
        Assert-Hash $partialPath $dependency.sha256
        Move-Item -LiteralPath $partialPath -Destination $archivePath
    }
    # Even cached archives are verified before use or reporting success.
    Assert-Hash $archivePath $dependency.sha256

    if (-not (Test-Path -LiteralPath $destination)) {
        if ($VerifyOnly) { throw "SDK not extracted: $destination" }
        $stage = Get-ChildPath $thirdPartyRoot ('.staging-' + $dependency.name + '-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $stage | Out-Null
        $archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
        try {
            $prefix = if ($dependency.archiveRoot) { $dependency.archiveRoot.TrimEnd('/') + '/' } else { '' }
            foreach ($entry in $archive.Entries) {
                if ($prefix -and -not $entry.FullName.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
                    throw "Archive entry has unexpected root: $($entry.FullName)"
                }
                $relative = $entry.FullName.Substring($prefix.Length)
                if (-not $relative) { continue }
                $target = Get-ChildPath $stage $relative
                if ($entry.FullName.EndsWith('/')) {
                    New-Item -ItemType Directory -Force -Path $target | Out-Null
                } else {
                    New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($target)) | Out-Null
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
                }
            }
        } finally { $archive.Dispose() }
        Assert-CriticalFiles $stage $dependency
        # Both resolved paths are checked to remain under third_party before moving.
        $stage = Get-ChildPath $thirdPartyRoot ([System.IO.Path]::GetFileName($stage))
        $destination = Get-ChildPath $thirdPartyRoot $dependency.destination
        Move-Item -LiteralPath $stage -Destination $destination
    }
    # Do not overwrite user edits or silently trust a partial previous extraction.
    Assert-CriticalFiles $destination $dependency
    Write-Host "Verified $($dependency.name) $($dependency.version): $destination"
}
