param(
    [Parameter(Mandatory)][ValidateSet('npm', 'rea', 'ghidra', 'jdk')][string]$Component
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'rea.dependencies.lock.json') -Raw | ConvertFrom-Json
$cache = Join-Path $projectRoot '.cache/rea-downloads'
$reaRoot = Join-Path $projectRoot '.tools/rea'
New-Item -ItemType Directory -Path $cache, $reaRoot -Force | Out-Null

function Assert-Artifact([string]$Path, $Spec) {
    if ($Spec.PSObject.Properties.Name -contains 'sha256') {
        $observed = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($observed -ne $Spec.sha256) { throw "SHA-256 mismatch: $Path" }
    } else {
        $stream = [IO.File]::OpenRead($Path)
        try { $hash = [Security.Cryptography.SHA512]::Create(); try { $observed = 'sha512-' + [Convert]::ToBase64String($hash.ComputeHash($stream)) } finally { $hash.Dispose() } } finally { $stream.Dispose() }
        if ($observed -ne $Spec.integrity) { throw "SHA-512 mismatch: $Path" }
    }
    if (($Spec.PSObject.Properties.Name -contains 'size') -and (Get-Item -LiteralPath $Path).Length -ne $Spec.size) { throw "Size mismatch: $Path" }
}

function Get-Artifact($Spec, [string]$Name) {
    $path = Join-Path $cache $Name
    if (Test-Path -LiteralPath $path) { Assert-Artifact $path $Spec; return $path }
    $partial = "$path.ranges.partial"
    Write-Host "Downloading $Name"
    $client = [Net.Http.HttpClient]::new()
    $client.Timeout = [TimeSpan]::FromSeconds(60)
    try {
        if (($Spec.PSObject.Properties.Name -contains 'size') -and $Spec.size -gt 16777216) {
            # Read bounded ranges. This connection closes large release streams
            # early; only append a complete response with the exact Content-Range.
            $offset = if (Test-Path -LiteralPath $partial) { (Get-Item -LiteralPath $partial).Length } else { 0L }
            if ($offset -gt $Spec.size) { throw 'Partial download exceeds pinned size' }
            $lastProgress = $offset
            while ($offset -lt $Spec.size) {
                $end = [Math]::Min($offset + 8388608L - 1, $Spec.size - 1)
                $complete = $false
                for ($attempt = 0; $attempt -lt 5 -and -not $complete; $attempt++) {
                    $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::Get, $Spec.url)
                    $request.Headers.Range = [Net.Http.Headers.RangeHeaderValue]::new($offset, $end)
                    $response = $null
                    try {
                        $response = $client.SendAsync($request).GetAwaiter().GetResult()
                        $range = $response.Content.Headers.ContentRange
                        if ([int]$response.StatusCode -ne 206 -or $null -eq $range -or $range.From -ne $offset -or $range.To -ne $end -or $range.Length -ne $Spec.size) { throw 'Server did not return the exact requested range' }
                        $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
                        if ($bytes.Length -ne ($end - $offset + 1)) { throw 'Incomplete range body' }
                        $stream = [IO.File]::Open($partial, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                        try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
                        $offset += $bytes.Length
                        $complete = $true
                    } catch {
                        if ($attempt -eq 4) { throw }
                        Start-Sleep -Seconds 2
                    } finally { if ($null -ne $response) { $response.Dispose() }; $request.Dispose() }
                }
                if (($offset - $lastProgress) -ge 67108864 -or $offset -eq $Spec.size) { Write-Host "$Name : $offset / $($Spec.size) bytes"; $lastProgress = $offset }
            }
        } else {
            if (Test-Path -LiteralPath $partial) { throw "Inspect existing partial file: $partial" }
            $bytes = $client.GetByteArrayAsync($Spec.url).GetAwaiter().GetResult()
            [IO.File]::WriteAllBytes($partial, $bytes)
        }
    } finally { $client.Dispose() }
    Assert-Artifact $partial $Spec
    Move-Item -LiteralPath $partial -Destination $path
    Write-Host "Checksum verified: $Name"
    return $path
}

function Expand-CheckedZip([string]$Archive, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { throw "Extraction destination already exists: $Destination" }
    $absoluteRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            $resolved = [IO.Path]::GetFullPath((Join-Path $absoluteRoot $entry.FullName))
            if (-not $resolved.StartsWith($absoluteRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "Archive entry escapes destination: $($entry.FullName)" }
        }
    } finally { $zip.Dispose() }
    Expand-Archive -LiteralPath $Archive -DestinationPath $Destination
}

switch ($Component) {
    'npm' {
        $archive = Get-Artifact $lock.npm 'npm-11.16.0.tgz'
        $destination = Join-Path $reaRoot 'npm-11.16.0'
        if (Test-Path -LiteralPath $destination) { throw "npm destination already exists: $destination" }
        $entries = & tar.exe -tf $archive
        if ($LASTEXITCODE -ne 0) { throw 'Cannot list npm tarball' }
        foreach ($entry in $entries) { if (-not $entry.StartsWith('package/') -or $entry -match '(^|/)\.\.(/|$)' -or $entry -match '\\') { throw "Unsafe npm entry: $entry" } }
        New-Item -ItemType Directory -Path $destination | Out-Null
        & tar.exe -xf $archive -C $destination --strip-components 1
        if ($LASTEXITCODE -ne 0) { throw 'Cannot extract npm tarball' }
        & $lock.node.reusedPath (Join-Path $destination 'bin/npm-cli.js') --version
        if ($LASTEXITCODE -ne 0) { throw 'npm version check failed' }
    }
    'rea' {
        $archive = Get-Artifact $lock.rea 'rea-agents-6.4.0.tgz'
        $npmCli = Join-Path $reaRoot 'npm-11.16.0/bin/npm-cli.js'
        if (-not (Test-Path -LiteralPath $npmCli)) { throw 'Install local npm first' }
        $packagePath = Join-Path $reaRoot 'package.json'
        $pinnedNpmLock = Join-Path $projectRoot $lock.npmDependencyLock.path
        if ((Get-FileHash -LiteralPath $pinnedNpmLock -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.npmDependencyLock.sha256) { throw 'Pinned npm dependency lock hash mismatch' }
        $pinnedPackages = Get-Content -LiteralPath $pinnedNpmLock -Raw | ConvertFrom-Json -AsHashtable
        if (-not (Test-Path -LiteralPath $packagePath)) {
            @{name='endfield-rea-tools'; version='1.0.0'; private=$true; dependencies=$pinnedPackages.packages[''].dependencies} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $packagePath -Encoding utf8
        }
        if (-not (Test-Path -LiteralPath (Join-Path $reaRoot 'package-lock.json'))) { Copy-Item -LiteralPath $pinnedNpmLock -Destination (Join-Path $reaRoot 'package-lock.json') }
        & $lock.node.reusedPath $npmCli install --prefix $reaRoot --cache (Join-Path $cache 'npm-cache') --ignore-scripts --no-audit --no-fund --save-exact 'rea-agents@6.4.0'
        if ($LASTEXITCODE -ne 0) { throw 'Local REA install failed' }
        $installed = Get-Content -LiteralPath (Join-Path $reaRoot 'package-lock.json') -Raw | ConvertFrom-Json -AsHashtable
        if ($installed.packages.'node_modules/rea-agents'.integrity -ne $lock.rea.integrity) { throw 'Installed REA integrity does not match reviewed release' }
        Copy-Item -LiteralPath (Join-Path $reaRoot 'package-lock.json') -Destination (Join-Path $cache 'rea-npm-package-lock.json')
        & $lock.node.reusedPath (Join-Path $reaRoot 'node_modules/rea-agents/scripts/rea.mjs') --version
        if ($LASTEXITCODE -ne 0) { throw 'REA version check failed' }
    }
    'ghidra' {
        $archive = Get-Artifact $lock.ghidra 'ghidra_12.1.4_PUBLIC_20260921.zip'
        Expand-CheckedZip $archive (Join-Path $projectRoot '.tools/ghidra')
    }
    'jdk' {
        $archive = Get-Artifact $lock.jdk 'OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip'
        Expand-CheckedZip $archive (Join-Path $projectRoot '.tools/jdk')
    }
}
Write-Host "Installed component: $Component"
