[CmdletBinding()]
param(
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\EndfieldXeSSLauncher.exe'),
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) ('out\launcher-readonly-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -cne 'Core' -or $PSVersionTable.PSVersion.Major -ne 7) { throw 'This test requires PowerShell 7 Core.' }
$jsonOptions = @{}
if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $jsonOptions.DateKind = 'String' }
$executablePath = (Get-Item -LiteralPath $Executable).FullName
$protectedRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'EndfieldXeSSObserver'
$protectedFiles = @('EndfieldXeSSLauncher.exe','manage-observer-install.ps1','install-receipt.json')
$before = @{}
foreach ($name in $protectedFiles) { $before[$name] = (Get-FileHash -LiteralPath (Join-Path $protectedRoot $name) -Algorithm SHA256).Hash }
$parentModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Process')
$parentLowResOutput = [Environment]::GetEnvironmentVariable('XESS_XEFG_LOW_RES_OUTPUT', 'Process')
$parentAcquireTrace = [Environment]::GetEnvironmentVariable('XESS_WSI_ACQUIRE_OBSERVER', 'Process')
$parentFakeSwapchain = [Environment]::GetEnvironmentVariable('XESS_FAKE_SWAPCHAIN', 'Process')
$parentGpuTimestamps = [Environment]::GetEnvironmentVariable('XESS_GPU_TIMESTAMPS', 'Process')
$parentTrackingCpuTiming = [Environment]::GetEnvironmentVariable('XESS_TRACKING_CPU_TIMING', 'Process')
$systemModules = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\Modules'
$pwshModules = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'PowerShell\7\Modules'
$cases = [ordered]@{ legacy_modules_only = $systemModules; inherited_pwsh_modules_first = ($pwshModules + ';' + $systemModules); untrusted_modules_first = ('C:\untrusted-modules;' + $pwshModules) }
$records = @()
foreach ($name in $cases.Keys) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $executablePath
    $start.Arguments = '--check-helper-readonly'
    $start.WorkingDirectory = Split-Path -Parent $executablePath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # Deliberately vary only this child environment. The EXE then creates its
    # own locked, signature-verified PowerShell 7 helper and trusted inbox modules.
    $start.EnvironmentVariables['PSModulePath'] = $cases[$name]
    $start.EnvironmentVariables['XESS_XEFG_LOW_RES_OUTPUT'] = '1'
    $start.EnvironmentVariables['XESS_WSI_ACQUIRE_OBSERVER'] = '1'
    $start.EnvironmentVariables['XESS_FAKE_SWAPCHAIN'] = '1'
    $start.EnvironmentVariables['XESS_TRACKING_CPU_TIMING'] = '1'
    $child = [Diagnostics.Process]::Start($start)
    try {
        $stdoutTask = $child.StandardOutput.ReadToEndAsync()
        $stderrTask = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit(45000)) { throw 'Readonly frontend probe exceeded its 30-second helper limit.' }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($child.ExitCode -ne 0) { throw "Readonly probe $name failed ($($child.ExitCode)): $stderr" }
        $result = $stdout | ConvertFrom-Json @jsonOptions
        if (-not $result.passed -or $result.mode -cne 'installed-helper-readonly-status' -or
            $result.helperExit -ne 0 -or -not $result.fileLocksHeldUntilExit -or -not $result.powerShell7Verified -or
            $result.guiShown -or $result.gameStarted -or $result.uacRequested -or $result.installedFilesModified) {
            throw "Readonly probe $name did not establish the required boundary."
        }
        $records += [ordered]@{ case=$name; childModulePath=$cases[$name]; exitCode=$child.ExitCode; result=$result; stderr=$stderr }
    } finally { $child.Dispose() }
}
foreach ($name in $protectedFiles) {
    if ((Get-FileHash -LiteralPath (Join-Path $protectedRoot $name) -Algorithm SHA256).Hash -cne $before[$name]) {
        throw "Protected file changed during readonly test: $name"
    }
}
if ([Environment]::GetEnvironmentVariable('PSModulePath', 'Process') -cne $parentModulePath) { throw 'Parent module path changed.' }
if ([Environment]::GetEnvironmentVariable('XESS_XEFG_LOW_RES_OUTPUT', 'Process') -cne $parentLowResOutput) { throw 'Parent low-resolution request changed.' }
if ([Environment]::GetEnvironmentVariable('XESS_WSI_ACQUIRE_OBSERVER', 'Process') -cne $parentAcquireTrace) { throw 'Parent acquire observation request changed.' }
if ([Environment]::GetEnvironmentVariable('XESS_FAKE_SWAPCHAIN', 'Process') -cne $parentFakeSwapchain) { throw 'Parent fake swapchain request changed.' }
if ([Environment]::GetEnvironmentVariable('XESS_GPU_TIMESTAMPS', 'Process') -cne $parentGpuTimestamps) { throw 'Parent GPU timestamp request changed.' }
if ([Environment]::GetEnvironmentVariable('XESS_TRACKING_CPU_TIMING', 'Process') -cne $parentTrackingCpuTiming) { throw 'Parent tracking CPU timing request changed.' }
$result = [ordered]@{ passed=$true; cases=$records; executable=$executablePath
    executableSha256=(Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
    protectedFilesBeforeAndAfter=$before; parentEnvironmentUnchanged=$true; gameStarted=$false; uacRequested=$false
    limitation='Runs installed helper Status through the real frontend lock/CreateProcess path. Does not exercise LaunchMod, UAC, EFU, game hooks or XeFG.' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 8
