[CmdletBinding()]
param([Parameter(Mandatory)][string]$SessionDirectory)
$ErrorActionPreference = 'Stop'
$session = (Get-Item -LiteralPath $SessionDirectory).FullName
$launch = Get-Content -LiteralPath (Join-Path $session 'launch.json') -Raw | ConvertFrom-Json
$since = [datetimeoffset]::Parse($launch.startedAt)
$playerLog = Join-Path $env:USERPROFILE 'AppData/LocalLow/Hypergryph/Endfield/Player.log'
$streamlineLog = Join-Path $session 'endfield-sl.log'
$records = @()
foreach ($path in @($playerLog, $streamlineLog)) {
    if (-not (Test-Path -LiteralPath $path)) {
        $records += [ordered]@{ file = $path; exists = $false; fresh = $false }
        continue
    }
    $file = Get-Item -LiteralPath $path
    $fresh = $file.LastWriteTimeUtc -ge $since.UtcDateTime
    # Read at most the tail 256 KiB of each explicit text log. Never scan asset files.
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $count = [int][Math]::Min($stream.Length, 262144)
        $start = $stream.Length - $count
        [void]$stream.Seek($start, [IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new($count)
        $read = $stream.Read($buffer, 0, $count)
        $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
        if ($start -gt 0) { $firstBreak = $text.IndexOf("`n"); if ($firstBreak -ge 0) { $text = $text.Substring($firstBreak + 1) } }
    } finally { $stream.Dispose() }
    $lines = @($text -split '\r?\n')
    $extensionLines = @($lines | Where-Object { $_ -match '(?i)^vulkan (instance|device) extension:' })
    # Extension availability dumps can otherwise crowd out actual enabled-device
    # configuration and later feature initialization in the bounded summary.
    $matches = @($lines | Where-Object {
        $_ -notmatch '(?i)^vulkan (instance|device) extension:' -and
        $_ -match '(?i)vulkan|device extention:|streamline|slInit|slSetTag|slSetConstants|DLSS|FSR|unsupported|feature.*support'
    } | Select-Object -First 160)
    $records += [ordered]@{ file = $path; exists = $true; fresh = $fresh; sizeBytes = $file.Length; bytesRead = $read; availableExtensionLinesOmitted = $extensionLines.Count; matchedLines = $matches }
}
$result = [ordered]@{
    collectedAt = (Get-Date).ToString('o')
    logs = $records
    gameFrameInputsVerified = $false
    note = 'Initialization logs cannot by themselves prove live color/depth/motion tags. A missing Streamline log is inconclusive.'
}
$output = Join-Path $session 'diagnostic-summary.json'
$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $output -Encoding utf8
Write-Host "Saved bounded diagnostic summary: $output"
$records | ForEach-Object { Write-Host "exists=$($_.exists) fresh=$($_.fresh) $($_.file)" }
