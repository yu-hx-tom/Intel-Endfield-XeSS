[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'third_party/minhook.lock.json') -Raw | ConvertFrom-Json
$expectedCommit = 'c3fcafdc10146beb5919319d0683e44e3c30d537'
$expectedHash = 'CDCB160F734D81BD4D235DFEA79E3F5A661C8EF0AB74FA814272AA5449069034'
$expectedUrl = "https://codeload.github.com/TsudaKageyu/minhook/zip/$expectedCommit"
if ($lock.commit -ne $expectedCommit -or $lock.archiveSha256 -ne $expectedHash -or $lock.archiveUrl -ne $expectedUrl) {
    throw 'Unexpected MinHook lock identity; review the script and lock together before changing this pin.'
}
$cacheDir = Join-Path $projectRoot 'third_party/.cache'
$targetRoot = Join-Path $projectRoot 'third_party/minhook'
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
$archive = Join-Path $cacheDir "minhook-$expectedCommit.zip"
if (-not (Test-Path -LiteralPath $archive)) {
    # An access denial is terminal: no mirror, alternative release, or installer.
    Invoke-WebRequest -Uri $expectedUrl -OutFile $archive -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'MinHook archive hash mismatch; nothing is extracted or executed.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
$copied = 0
try {
    $prefix = "minhook-$expectedCommit/"
    $selected = @($zip.Entries | Where-Object {
        $_.FullName.StartsWith($prefix, [StringComparison]::Ordinal) -and $_.Name -ne '' -and
        ($_.FullName.StartsWith($prefix + 'src/', [StringComparison]::Ordinal) -or
         $_.FullName.StartsWith($prefix + 'include/', [StringComparison]::Ordinal) -or
         $_.FullName -eq ($prefix + 'LICENSE.txt') -or
         $_.FullName -eq ($prefix + 'README.md') -or
         $_.FullName -eq ($prefix + 'AUTHORS.txt'))
    })
    if ($selected.Count -ne 16) { throw "Unexpected source-file count: $($selected.Count)" }
    foreach ($entry in $selected) {
        $relative = $entry.FullName.Substring($prefix.Length)
        if ($relative -match '(^|/)\.\.(/|$)' -or $relative.Contains('\') -or $relative.Contains(':') -or $entry.Length -gt 128KB) {
            throw "Unexpected archive entry: $relative"
        }
        $destination = [IO.Path]::GetFullPath((Join-Path $targetRoot $relative))
        $boundary = [IO.Path]::GetFullPath($targetRoot).TrimEnd('\') + '\'
        if (-not $destination.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive path escaped source directory.' }
        $parent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        $pathCheck = $parent
        while ($pathCheck.Length -ge $targetRoot.Length) {
            if ((Get-Item -LiteralPath $pathCheck).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source directory contains a reparse point.' }
            if ($pathCheck -eq $targetRoot) { break }
            $pathCheck = Split-Path -Parent $pathCheck
        }
        if (Test-Path -LiteralPath $destination) {
            if ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source file is a reparse point.' }
            $stream = $entry.Open()
            $hasher = [Security.Cryptography.SHA256]::Create()
            try { $entryHash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
            finally { $hasher.Dispose(); $stream.Dispose() }
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $entryHash) {
                throw "Existing source differs from pinned archive: $relative; refusing to overwrite."
            }
        } else {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
        }
        ++$copied
    }
} finally { $zip.Dispose() }
[ordered]@{ verified = $true; commit = $expectedCommit; archiveSha256 = $expectedHash; sourceFiles = $copied; sourceModified = $false; systemInstalled = $false } | ConvertTo-Json -Compress
