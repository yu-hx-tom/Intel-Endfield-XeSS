[CmdletBinding()]
param(
    [ValidateSet('ReviewPlan', 'Install', 'Status', 'SetGamePath', 'Launch', 'LaunchMod', 'Smoke', 'Uninstall', 'RetentionPlan', 'SetRetentionPolicy')][string]$Action = 'ReviewPlan',
    [string]$SourceDirectory,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedDllSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedManifestSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedSmokeSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedScriptSha256,
    [switch]$WithModLauncher,
    [string]$ModPayloadDirectory,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedModLauncherSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedEfuSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedEfuLicenseSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedEfuNoticeSha256,
    [switch]$WithXeFGSdk,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedXeFGDllSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedXellDllSha256,
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedIntelLicenseSha256,
    [switch]$TraceStreamline,
    [switch]$BridgeDevice,
    [switch]$TraceCompute,
    [switch]$TraceFsr,
    [switch]$TraceFsrUpscale,
    [switch]$CaptureRawColor,
    [switch]$PresentationOff,
    [switch]$XeFG,
    [switch]$XessSr,
    [switch]$DisableNoDither,
    [switch]$TraceRenderTail,
    [switch]$WsiAcquireTrace,
    [switch]$FakeSwapchain,
    [switch]$GpuTimestamps,
    [switch]$TrackingCpuTiming,
    [switch]$ReleaseSessionRetention,
    [switch]$ReleaseMinimal,
    [switch]$DiagnosticMode,
    [ValidateSet('Preview','Auto')][string]$SessionRetentionMode='Preview',
    [string[]]$RecorderCaptureRoots=@(),
    [switch]$ApproveRetention,
    [switch]$ThreadCpuSampling,
    [Alias('FrameCap30To40')][switch]$Override30Fps,
    [ValidateRange(30,120)][int]$FrameCapTargetFps=40,
    [ValidateRange(0,3)][int]$FgDebugMode = 0,
    [ValidateRange(-1,6)][int]$FgSceneIndex = -1,
    [switch]$LowResOutput,
    [switch]$OfficialFallback,
    [switch]$ObserveInputs,
    [string]$LauncherPath = '',
    [string]$GameDirectory = '',
    [ValidatePattern('^[0-9a-fA-F]{32}$')][string]$SessionId = ([guid]::NewGuid().ToString('N'))
)
$ErrorActionPreference = 'Stop'
if($ReleaseMinimal -and ($Action -ne 'LaunchMod' -or -not $XeFG -or -not $FakeSwapchain -or -not $XessSr -or $OfficialFallback)){throw 'Minimal release requires the independent SR single-chain route.'}
if($DiagnosticMode -and -not $ReleaseMinimal){throw 'DiagnosticMode requires ReleaseMinimal.'}
if($ReleaseSessionRetention -and ($Action -ne 'LaunchMod' -or -not $XeFG -or -not $FakeSwapchain -or $OfficialFallback)){throw 'Session retention requires the independent release single-chain route.'}
if ($DisableNoDither -and ($Action -ne 'LaunchMod' -or $OfficialFallback)) { throw 'DisableNoDither requires independent LaunchMod.' }
if(($Override30Fps -or $FgSceneIndex -ge 0) -and ($Action -ne 'LaunchMod' -or -not $XeFG -or $OfficialFallback)){throw 'FG settings require the independent XeFG route.'}
if($FgDebugMode -and ($Action -ne 'LaunchMod' -or -not $XeFG -or $OfficialFallback)){throw 'FG debug requires the independent XeFG route.'}
if ($XessSr -and ($Action -ne 'LaunchMod' -or -not $XeFG -or -not $FakeSwapchain)) { throw 'XessSr requires LaunchMod with XeFG and FakeSwapchain.' }
if ($PSVersionTable.PSEdition -cne 'Core' -or $PSVersionTable.PSVersion.Major -ne 7) { throw 'Use PowerShell 7 Core (pwsh.exe); Windows PowerShell fallback is not supported.' }
$managerJsonOptions = @{}
if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $managerJsonOptions.DateKind = 'String' }
if ($TraceRenderTail -and ($Action -ne 'LaunchMod' -or -not $XeFG)) { throw 'TraceRenderTail requires LaunchMod with the explicit XeFG route.' }
if ($WsiAcquireTrace -and ($Action -ne 'LaunchMod' -or -not $XeFG -or $TraceRenderTail)) { throw 'WsiAcquireTrace requires LaunchMod with XeFG and without TraceRenderTail.' }
if ($FakeSwapchain -and ($Action -ne 'LaunchMod' -or -not $XeFG -or $TraceRenderTail -or $WsiAcquireTrace)) { throw 'FakeSwapchain requires LaunchMod with XeFG and without trace diagnostics.' }
if ($GpuTimestamps -and ($Action -ne 'LaunchMod' -or -not $XeFG)) { throw 'GpuTimestamps requires LaunchMod with the explicit XeFG route.' }
if ($TrackingCpuTiming -and ($Action -ne 'LaunchMod' -or -not $XeFG)) { throw 'TrackingCpuTiming requires LaunchMod with the explicit XeFG route.' }
if ($ThreadCpuSampling -and ($Action -ne 'LaunchMod' -or -not $XeFG)) { throw 'ThreadCpuSampling requires LaunchMod with the explicit XeFG route.' }
if ($LowResOutput -and ($Action -ne 'LaunchMod' -or -not $XeFG -or $TraceRenderTail)) { throw 'LowResOutput requires LaunchMod with XeFG and without TraceRenderTail.' }
if ($ObserveInputs -and ($Action -ne 'LaunchMod' -or $OfficialFallback -or $TraceStreamline -or $BridgeDevice -or $TraceCompute -or $TraceFsr -or $TraceFsrUpscale -or $CaptureRawColor -or $PresentationOff -or $XeFG)) { throw 'ObserveInputs requires LaunchMod alone, without official fallback or other experimental switches.' }
if ($TraceFsrUpscale -and -not $TraceFsr) { throw 'TraceFsrUpscale requires TraceFsr; the two entries share one installation transaction.' }
if ($CaptureRawColor -and ($Action -ne 'LaunchMod' -or -not $TraceFsr -or -not $TraceFsrUpscale)) { throw 'CaptureRawColor requires LaunchMod with both TraceFsr and TraceFsrUpscale.' }
if ($PresentationOff -and ($Action -ne 'LaunchMod' -or -not $BridgeDevice -or $CaptureRawColor)) { throw 'PresentationOff requires LaunchMod with BridgeDevice and rawcolor capture disabled.' }
if ($XeFG -and ($Action -ne 'LaunchMod' -or $OfficialFallback -or $ObserveInputs -or $TraceStreamline -or $BridgeDevice -or $TraceCompute -or $TraceFsr -or $TraceFsrUpscale -or $CaptureRawColor -or $PresentationOff)) { throw 'XeFG selects the experimental menu-driven XeSS 2x candidate through LaunchMod alone; actual ON requires frame admission.' }
if ($WithXeFGSdk -and (-not $WithModLauncher -or $Action -notin @('ReviewPlan','Install'))) { throw 'WithXeFGSdk requires a standalone launcher ReviewPlan or Install.' }
if ($OfficialFallback -and $Action -ne 'LaunchMod') { throw 'OfficialFallback is an explicit LaunchMod alternative only.' }
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell 7.' }
$productId = 'endfield-xess-vulkan-observer-v1'
$layerName = 'VK_LAYER_XESS_bridge_observer'
$registrySubKey = 'SOFTWARE\Khronos\Vulkan\ExplicitLayers'
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'EndfieldXeSSObserver'
$sessionRoot = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'EndfieldXeSSObserver\Sessions'
$dllPath = Join-Path $installRoot 'xess-vulkan-observer.dll'
$manifestPath = Join-Path $installRoot 'VkLayer_xess_observer.json'
$receiptPath = Join-Path $installRoot 'install-receipt.json'
$gameConfigurationPath = Join-Path $installRoot 'game-path.txt'
$installedScript = Join-Path $installRoot 'manage-observer-install.ps1'
$smokePath = Join-Path $installRoot 'xess-vulkan-wsi-smoke.exe'
$baseOwnedNames = @('xess-vulkan-observer.dll', 'VkLayer_xess_observer.json', 'manage-observer-install.ps1', 'xess-vulkan-wsi-smoke.exe')
$modOwnedNames = @('EndfieldXeSSLauncher.exe', 'EFU.exe', 'EFU-LICENSE.txt', 'EFU-SOURCE-NOTICE.txt')
$legacySdkOwnedNames = @('libxess_fg.dll', 'libxell.dll', 'LICENSE.txt')
$sdkOwnedNames = @('libxess_fg.dll', 'libxell.dll', 'LICENSE.txt', 'libxess.dll')
$xessSrDllSha256 = '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7'
$sdkVersion = '3.0.2'
$xeFGDllSha256 = 'ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'
$xellDllSha256 = 'd2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7'
$intelLicenseSha256 = '784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df'
$ownedNames = @($baseOwnedNames)
if ($WithModLauncher) { $ownedNames += $modOwnedNames }
if ($WithXeFGSdk) { $ownedNames += $sdkOwnedNames }
$efuSha256 = '65d34a67ae1e82b03e22ea7f38baffd5d75bcf5706a4f896e4633b55e2597e94'
$previousDitherEfuSha256 = '2ef1cd8066c62ebe42b41d098dfe8c36a559e5845626f7e6d98ddd78f4dd21fa'
$previousPreviewEfuSha256 = 'fe27f71582726fb21cb51076a9ed3e16c6c6eae32588364daff3c5a8078d24f9'
$legacyEfuSha256 = 'a686b5b7704341e7bfb016794df074a39f6195b8d80a4a99ca4f8d8de9eca5ae'
$legacyEfuSourceCommit = '1165a377f7915528b1ae9d2c3e41631bfd59b957'
$efuLicenseSha256 = '8486a10c4393cee1c25392769ddd3b2d6c242d6ec7928e1414efff7dfb2f07ef'
$efuSourceCommit = 'eff32bbc89baaf25fcb25f2a8903990e172f319e'
$officialLauncherPublisher = 'CN="Shanghai Hypergryph Network Technology Co., Ltd.", O="Shanghai Hypergryph Network Technology Co., Ltd.", S=shanghai, C=CN'
$previousUnitySha256 = 'bee7be52370adddd67ba61e4937ca51b7f272656841d187e95e505496da798d1'
$reviewedGamePins = [ordered]@{
    'Endfield.exe' = '09f516d7c5c61933697e974a9c15ce38873fd031ba2a1d1575fdc358c3f046b5'
    'UnityPlayer.dll' = '9aa9397a6c5ee4fa083ea379b81ff14019eeba26a76d15446318d5947fb3d2d0'
    'OneChannelLC.dll' = 'd0dd44b8a88d694b2f02f7145c9efc4bac0c5eeda7829d5d5d90985ec204b8b1'
}
$efuPath = Join-Path $installRoot 'EFU.exe'
$utf8 = [Text.UTF8Encoding]::new($false)
$adminSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
$systemSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
$usersSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-545')
function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Assert-NoReparse([string]$Path) {
    $itemPath = [IO.Path]::GetFullPath($Path)
    while ($itemPath) {
        if (Test-Path -LiteralPath $itemPath) {
            if ((Get-Item -LiteralPath $itemPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing reparse point in protected path: $itemPath"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($itemPath)
        if ($parent -eq $itemPath) { break }
        $itemPath = $parent
    }
}
function Get-TrustedPowerShell7 {
    $path = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'PowerShell\7\pwsh.exe'
    Assert-NoReparse $path
    $file = Get-Item -LiteralPath $path
    if ($file.VersionInfo.FileMajorPart -ne 7) { throw 'The trusted pwsh.exe must be PowerShell 7.' }
    $signature = Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate -or
        $signature.SignerCertificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -cne 'Microsoft Corporation') {
        throw 'PowerShell 7 requires a valid Microsoft Authenticode signature.'
    }
    return $file.FullName
}
$powerShell7Path = Get-TrustedPowerShell7
if ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName -ine $powerShell7Path) { throw 'Run this script with the trusted Program Files PowerShell 7 pwsh.exe.' }
function Set-ProtectedAcl([string]$Path, [bool]$Directory) {
    if ($Directory) { $acl = [Security.AccessControl.DirectorySecurity]::new() }
    else { $acl = [Security.AccessControl.FileSecurity]::new() }
    $acl.SetOwner($adminSid)
    $acl.SetAccessRuleProtection($true, $false)
    $inherit = [Security.AccessControl.InheritanceFlags]::None
    if ($Directory) { $inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit' }
    foreach ($sid in @($adminSid, $systemSid)) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', $inherit, 'None', 'Allow'))
    }
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($usersSid, 'ReadAndExecute', $inherit, 'None', 'Allow'))
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Assert-ProtectedAcl([string]$Path) {
    Assert-NoReparse $Path
    $acl = Get-Acl -LiteralPath $Path
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $adminSid.Value -or -not $acl.AreAccessRulesProtected) {
        throw "Installed path is not protected and owned by Administrators: $Path"
    }
    $write = [Security.AccessControl.FileSystemRights]'WriteData,AppendData,WriteExtendedAttributes,WriteAttributes,ChangePermissions,TakeOwnership,Delete,DeleteSubdirectoriesAndFiles'
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $write) -and
            $rule.IdentityReference.Value -notin @($adminSid.Value, $systemSid.Value)) {
            throw "Unexpected writable ACL on installed path: $Path"
        }
    }
}
function Open-LayerKey([bool]$Writable, [bool]$Create = $false) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        if ($Create) { return $base.CreateSubKey($registrySubKey, $true) }
        return $base.OpenSubKey($registrySubKey, $Writable)
    } finally { $base.Dispose() }
}
function Get-RegistryState {
    $key = Open-LayerKey $false
    try {
        if (-not $key -or $key.GetValueNames() -notcontains $manifestPath) { return @{ exists = $false; data = $null; kind = $null } }
        return @{ exists = $true; data = $key.GetValue($manifestPath); kind = [string]$key.GetValueKind($manifestPath) }
    } finally { if ($key) { $key.Dispose() } }
}
function Assert-PeDll([string]$Path, [bool]$RequireDll = $true, [switch]$RequireRetention) {
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -gt 25165824 -or $file.Length -lt 512) { throw 'Unexpected observer DLL size.' }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    function U16([int]$Offset) { if ($Offset -lt 0 -or $Offset + 2 -gt $bytes.Length) { throw 'Malformed PE offset.' }; [BitConverter]::ToUInt16($bytes, $Offset) }
    function U32([int]$Offset) { if ($Offset -lt 0 -or $Offset + 4 -gt $bytes.Length) { throw 'Malformed PE offset.' }; [BitConverter]::ToUInt32($bytes, $Offset) }
    $pe = [int](U32 60)
    if ((U16 0) -ne 0x5a4d -or (U32 $pe) -ne 0x4550 -or (U16 ($pe + 4)) -ne 0x8664 -or
        (U16 ($pe + 24)) -ne 0x20b) { throw 'Payload must be an x64 PE32+ binary.' }
    $isDll = ((U16 ($pe + 22)) -band 0x2000) -ne 0
    if ($isDll -ne $RequireDll) { throw 'Payload DLL/executable type does not match its intended role.' }
    if (-not $RequireDll) { return }
    $sectionCount = U16 ($pe + 6)
    $sectionStart = $pe + 24 + (U16 ($pe + 20))
    if ($sectionCount -gt 96) { throw 'Unexpected PE section count.' }
    function RvaOffset([uint32]$Rva) {
        for ($s = 0; $s -lt $sectionCount; $s++) {
            $section = $sectionStart + 40 * $s
            $virtual = U32 ($section + 12); $rawSize = U32 ($section + 16)
            if ($Rva -ge $virtual -and ([uint64]$Rva - $virtual) -lt $rawSize) { return [int]((U32 ($section + 20)) + $Rva - $virtual) }
        }
        throw 'Unmapped PE export RVA.'
    }
    $export = RvaOffset (U32 ($pe + 24 + 112))
    $count = U32 ($export + 24); $names = RvaOffset (U32 ($export + 32))
    if ($count -gt 4096) { throw 'Unexpected export count.' }
    $exports = @()
    for ($n = 0; $n -lt $count; $n++) {
        $start = RvaOffset (U32 ($names + 4 * $n)); $end = $start
        while ($end -lt $bytes.Length -and $end -lt $start + 256 -and $bytes[$end] -ne 0) { $end++ }
        if ($end -ge $bytes.Length -or $end -ge $start + 256) { throw 'Malformed export name.' }
        $exports += [Text.Encoding]::ASCII.GetString($bytes, $start, $end - $start)
    }
    if($RequireRetention -and $exports -cnotcontains 'xessObserverReleaseRetentionV1'){throw 'Session retention is unavailable in this development/older build.'}
    foreach ($required in @('vkGetInstanceProcAddr', 'vkGetDeviceProcAddr', 'vkNegotiateLoaderLayerInterfaceVersion', 'vk_layerGetPhysicalDeviceProcAddr', 'xessObserverSingleChainLocalFiOffV1')) {
        if ($exports -cnotcontains $required) { throw "Missing observer export: $required" }
    }
}
function Assert-SingleChainPolicy($Receipt) {
    if (($Receipt.singleChainPolicyVersion -isnot [int] -and $Receipt.singleChainPolicyVersion -isnot [long]) -or
        $Receipt.singleChainPolicyVersion -ne 1) { throw 'Install the complete single-chain LocalFiOff package before using FakeSwapchain.' }
}
function Get-ReceiptOwnedNames($Receipt) {
    # Legacy schema-1 receipts own exactly four files. The optional extension
    # has an exact, closed list: arbitrary receipt paths never reach removal.
    $names = @($baseOwnedNames)
    if ($null -ne $Receipt.modLauncher) {
        $extension = $Receipt.modLauncher
        $legacyIdentity = ($extension.efuSha256 -ceq $legacyEfuSha256 -and $extension.efuSourceCommit -ceq $legacyEfuSourceCommit) -or
            ($extension.efuSha256 -ceq $previousPreviewEfuSha256 -and $extension.efuSourceCommit -ceq $efuSourceCommit) -or
            ($extension.efuSha256 -ceq $previousDitherEfuSha256 -and $extension.efuSourceCommit -ceq $efuSourceCommit)
        $currentIdentity = $efuSha256 -cmatch '^[0-9a-f]{64}$' -and
            $extension.efuSha256 -ceq $efuSha256 -and $extension.efuSourceCommit -ceq $efuSourceCommit
        if ($extension.version -ne 1 -or $extension.efuVersion -cne '5.1.0' -or -not ($legacyIdentity -or $currentIdentity)) {
            throw 'Unexpected standalone-launcher receipt extension.'
        }
        foreach ($field in @('launcherSha256','efuSha256','licenseSha256','noticeSha256')) {
            if ($extension.sourceHashes.$field -cnotmatch '^[0-9a-f]{64}$') { throw 'Malformed standalone-launcher source hashes.' }
        }
        if ($extension.sourceHashes.efuSha256 -cne $extension.efuSha256 -or $extension.sourceHashes.licenseSha256 -cne $efuLicenseSha256) {
            throw 'EFU release or license hash in receipt does not match the reviewed pin.'
        }
        $names += $modOwnedNames
    }
    if ($null -ne $Receipt.xeFGSdk) {
        $sdk = $Receipt.xeFGSdk
        if ($null -eq $Receipt.modLauncher -or $sdk.version -ne 1 -or $sdk.sdkVersion -cne $sdkVersion -or
            $sdk.sourceHashes.xeFGDllSha256 -cne $xeFGDllSha256 -or
            $sdk.sourceHashes.xellDllSha256 -cne $xellDllSha256 -or
            $sdk.sourceHashes.intelLicenseSha256 -cne $intelLicenseSha256) { throw 'Unexpected XeFG SDK receipt extension or release hashes.' }
        $names += $legacySdkOwnedNames
        if ($null -ne $sdk.sourceHashes.xessSrDllSha256) {
            if ($sdk.sourceHashes.xessSrDllSha256 -cne $xessSrDllSha256) { throw 'Unreviewed XeSS SR SDK pin.' }
            $names += 'libxess.dll'
        }
    }
    if (@($Receipt.files).Count -ne $names.Count) { throw 'Unexpected owned-file receipt.' }
    foreach ($name in $names) {
        $entry = @($Receipt.files | Where-Object name -CEQ $name)
        if ($entry.Count -ne 1 -or $entry[0].sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Malformed file ownership receipt.' }
    }
    if ($null -ne $Receipt.modLauncher) {
        $mapping = [ordered]@{ 'EndfieldXeSSLauncher.exe'='launcherSha256'; 'EFU.exe'='efuSha256'; 'EFU-LICENSE.txt'='licenseSha256'; 'EFU-SOURCE-NOTICE.txt'='noticeSha256' }
        foreach ($name in $mapping.Keys) {
            $entry = @($Receipt.files | Where-Object name -CEQ $name)[0]
            if ($entry.sha256 -cne $Receipt.modLauncher.sourceHashes.($mapping[$name])) { throw 'Standalone payload receipt hashes disagree.' }
        }
    }
    if ($null -ne $Receipt.xeFGSdk) {
        $mapping = [ordered]@{ 'libxess_fg.dll'='xeFGDllSha256'; 'libxell.dll'='xellDllSha256'; 'LICENSE.txt'='intelLicenseSha256'; 'libxess.dll'='xessSrDllSha256' }
        foreach ($name in $mapping.Keys) {
            if ($name -ceq 'libxess.dll' -and $null -eq $Receipt.xeFGSdk.sourceHashes.xessSrDllSha256) { continue }
            $entry = @($Receipt.files | Where-Object name -CEQ $name)[0]
            if ($entry.sha256 -cne $Receipt.xeFGSdk.sourceHashes.($mapping[$name])) { throw 'SDK payload receipt hashes disagree.' }
        }
    }
    return $names
}
function Read-Receipt {
    if (-not (Test-Path -LiteralPath $receiptPath)) { throw "No ownership receipt at $receiptPath; nothing will be overwritten or removed." }
    Assert-ProtectedAcl $installRoot
    Assert-ProtectedAcl $receiptPath
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json @managerJsonOptions
    if ($receipt.productId -ne $productId -or $receipt.schemaVersion -ne 1 -or $receipt.installRoot -ne $installRoot -or
        $receipt.registrySubKey -ne $registrySubKey -or $receipt.registryValueName -ne $manifestPath -or $receipt.layerName -ne $layerName) {
        throw 'Receipt identity/path mismatch; refusing changes.'
    }
    Get-ReceiptOwnedNames $receipt | Out-Null
    return $receipt
}
function Assert-OwnedFiles($Receipt, [bool]$AllowMissing = $false) {
    foreach ($entry in $Receipt.files) {
        $path = Join-Path $installRoot $entry.name
        if (-not (Test-Path -LiteralPath $path)) {
            if ($AllowMissing) { continue }
            throw "Missing installed file: $path"
        }
        Assert-ProtectedAcl $path
        if ((Get-Sha256 $path) -ne $entry.sha256) { throw "Owned file was modified; refusing overwrite/removal: $path" }
    }
}
function Assert-GameClosed {
    $running = @(Get-Process -Name Launcher,Games,HypergryphLauncher,Endfield,EFU -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Close the official launcher (including tray process), game and EFU normally first. Matching basenames are refused even when their path is ambiguous. This script never terminates them.' }
}
function Assert-FrontendClosed {
    # Installation changes may otherwise remove the layer and receipt before
    # reaching an executable still mapped by the running native frontend.
    $running = @(Get-Process -Name EndfieldXeSSLauncher -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Close EndfieldXeSSLauncher normally (including its tray icon) before installing or uninstalling. Ambiguous matching basenames are refused; this script never terminates them.' }
}
function Normalize-GameDirectory([string]$Directory) {
    if (-not $Directory -or $Directory -notmatch '^[a-zA-Z]:[\\/]' -or $Directory.IndexOfAny([char[]]@('"',"`r","`n",[char]0)) -ge 0) {
        throw 'Choose a complete local game directory; network and argument paths are refused.'
    }
    $full=[IO.Path]::GetFullPath($Directory).TrimEnd('\')
    if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($full)).DriveType -eq [IO.DriveType]::Network) { throw 'Network game paths are not supported.' }
    Assert-NoReparse $full
    return $full
}
function Read-GameDirectory {
    if (-not (Test-Path -LiteralPath $gameConfigurationPath)) { return '' }
    Assert-ProtectedAcl $installRoot; Assert-ProtectedAcl $gameConfigurationPath
    if ((Get-Item -LiteralPath $gameConfigurationPath).Length -gt 32768) { throw 'Game path configuration is too large.' }
    return Normalize-GameDirectory ([IO.File]::ReadAllText($gameConfigurationPath,$utf8).TrimEnd([char[]]"`r`n"))
}
function Write-GameDirectory([string]$Directory) {
    $directory=Normalize-GameDirectory $Directory
    $locks=@{}
    try {
        $compatibility=Get-ModGameCompatibility $directory $locks
        if (-not $compatibility.efuLaunchCompatible) { throw "游戏路径未保存：文件缺失或当前版本尚未适配（$($compatibility.state)）。请检查目录，或等待兼容版本；不会跳过身份校验。" }
        Assert-ProtectedAcl $installRoot; Assert-NoReparse $gameConfigurationPath
        if(Test-Path -LiteralPath $gameConfigurationPath){Assert-ProtectedAcl $gameConfigurationPath}
        $temporary=Join-Path $installRoot ('game-path.'+[guid]::NewGuid().ToString('N')+'.tmp')
        try {
            $file=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            try {$bytes=$utf8.GetBytes($directory+"`n");$file.Write($bytes,0,$bytes.Length);$file.Flush($true)}finally{$file.Dispose()}
            Set-ProtectedAcl $temporary $false
            [IO.File]::Move($temporary,$gameConfigurationPath,$true)
        }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
    }finally{foreach($stream in $locks.Values){$stream.Dispose()}}
    return $directory
}
function Get-ModLaunchSpecification([string]$Directory, [bool]$Official = $false, [bool]$Observe = $false, [bool]$ContinuousOff = $false) {
    if (($Official -and ($Observe -or $ContinuousOff)) -or ($Observe -and $ContinuousOff)) { throw 'Select one independent launch mode or official fallback.' }
    $directory=Normalize-GameDirectory $Directory
    $gamePath = Join-Path $directory 'Endfield.exe'
    if ($Official) {
        $officialPath=Join-Path (Split-Path -Parent (Split-Path -Parent $directory)) 'Launcher.exe'
        return [ordered]@{ executable = $officialPath
            workingDirectory = (Split-Path -Parent $officialPath); gamePath = $gamePath
            arguments = ''; backend = 'OfficialLauncher'; launchPolicy = 'official_fallback'
            officialGameStartRequired = $true; researchFeaturesAvailable = $false }
    }
    return [ordered]@{ executable = $efuPath; workingDirectory = $directory; gamePath = $gamePath
        arguments = ('--silent --gp "' + $gamePath + '" --EFMI false'); backend = 'EFU-preview-eff32bbc'
        launchPolicy = $(if ($ContinuousOff) { 'independent_efu_continuous_off' } elseif ($Observe) { 'independent_efu_observe_inputs' } else { 'independent_efu_preview' }); officialGameStartRequired = $false; researchFeaturesAvailable = $false }
}
function Assert-ModGameClosed {
    # Official Launcher/Games instances are allowed: their own single-instance
    # protocol handles this new request. Never attach to or inject into them.
    if (@(Get-Process -Name Endfield,EFU -ErrorAction SilentlyContinue).Count) {
        throw 'Close the game and EFU normally before requesting another launch. The official launcher may remain open; no process is terminated by this helper.'
    }
}
function Assert-OfficialLauncherIdentity([string]$Path, $Signature) {
    if ([IO.Path]::GetFileName($Path) -ine 'Launcher.exe' -or
        $Signature.Status -ne 'Valid' -or $null -eq $Signature.SignerCertificate -or
        $Signature.SignerCertificate.Subject -cne $officialLauncherPublisher) {
        throw 'LaunchMod requires the fixed official Launcher.exe with a valid Hypergryph publisher signature.'
    }
}
function Get-GameSignature([string]$Path) { Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $Path }
function Test-GameSignature($Signature) {
    return $null -ne $Signature -and ($Signature.Status -eq 'NotSigned' -or
        ($Signature.Status -eq 'Valid' -and $null -ne $Signature.SignerCertificate -and $Signature.SignerCertificate.Subject -ceq $officialLauncherPublisher))
}
function Get-ModGameCompatibility([string]$Directory, [Collections.IDictionary]$HeldLocks = $null) {
    $specification = Get-ModLaunchSpecification $Directory
    $result = [ordered]@{ files = @(); matchesCurrentBuild = $false; efuLaunchCompatible = $false
        state = 'unavailable'; researchFeaturesAvailable = $false
        reason = 'Independent preview requires the complete reviewed Endfield/UnityPlayer/OneChannelLC identity. The old graphics hooks remain disabled.' }
    $stream = $null
    try {
        $allMatch = $true
        $signature=Get-GameSignature $specification.gamePath
        if(-not (Test-GameSignature $signature)){
            $result.state='unverified_publisher';return $result
        }
        foreach ($name in $reviewedGamePins.Keys) {
            $path = Join-Path (Split-Path -Parent $specification.gamePath) $name
            Assert-NoReparse $path
            $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            if ($stream.Length -le 0 -or $stream.Length -gt 64MB) { $result.state = 'unreviewed_size'; return $result }
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
            finally { $sha.Dispose() }
            $matches = $actual -ceq $reviewedGamePins[$name]
            $result.files += [ordered]@{ name=$name; bytes=$stream.Length; sha256=$actual; matchesReviewedPin=[bool]$matches }
            $allMatch = $allMatch -and $matches
            if ($null -ne $HeldLocks) { $HeldLocks[$name] = $stream; $stream = $null }
            else { $stream.Dispose(); $stream = $null }
        }
        $result.matchesCurrentBuild = [bool]$allMatch; $result.efuLaunchCompatible = [bool]$allMatch
        $result.state = if ($allMatch) { 'reviewed_preview_launch_build' } else { 'unreviewed_game_build' }
    } catch {
        # Any absent/changed/unreadable module refuses the independent route.
        $result.state = 'unavailable'
    } finally { if ($stream) { $stream.Dispose() } }
    return $result
}
function Set-OfficialCompatibilityEnvironment($StartInfo) {
    # Only this child changes. Explicit layer registration by itself does not
    # activate our observer. Remove inherited requests before official handoff.
    $remove = @('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH',
        'VK_ADD_IMPLICIT_LAYER_PATH','VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE',
        'VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG','SL_LOG_LEVEL','SL_LOG_PATH','SL_LOG_NAME',
        'SL_ENABLE_CONSOLE_LOGGING','EFU_RUNTIME_LOG','EFU_NO_DITHER')
    $remove += @($StartInfo.EnvironmentVariables.Keys | Where-Object { $_ -like 'XESS_*' -or $_ -like 'SL_*' })
    foreach ($name in $remove) { $StartInfo.EnvironmentVariables.Remove($name) }
}
function Set-NoDitherEnvironment($StartInfo, [bool]$Disabled) {
    $StartInfo.EnvironmentVariables['EFU_NO_DITHER'] = $(if($Disabled){'0'}else{'1'})
}
function Set-InputObservationEnvironment($StartInfo, [string]$Session) {
    Set-OfficialCompatibilityEnvironment $StartInfo
    $settings = [ordered]@{VK_INSTANCE_LAYERS=$layerName;XESS_FSR_UPSCALE_OBSERVER='1'}
    if($Session){
        $settings['VK_LOADER_DEBUG']='error,warn,layer';$settings['XESS_OBSERVER_LOG']=Join-Path $Session 'observer.jsonl'
        $settings['EFU_RUNTIME_LOG']=Join-Path $Session 'efu-runtime.jsonl';$settings['XESS_DIAGNOSTIC_MODE']='1'
    }else{$settings['XESS_DIAGNOSTIC_MODE']='0'}
    foreach ($name in $settings.Keys) { $StartInfo.EnvironmentVariables[$name] = $settings[$name] }
    return $settings
}
function Set-ContinuousOffEnvironment($StartInfo, [string]$Session, [bool]$TraceRenderTail = $false, [bool]$LowResOutput = $false, [bool]$WsiAcquireTrace = $false, [bool]$FakeSwapchain = $false, [bool]$GpuTimestamps = $false, [bool]$TrackingCpuTiming = $false, [bool]$XessSr = $false, [bool]$ThreadCpuSampling = $false) {
    if ($TraceRenderTail -and $LowResOutput) { throw 'LowResOutput cannot combine with TraceRenderTail.' }
    if ($TraceRenderTail -and $WsiAcquireTrace) { throw 'WsiAcquireTrace cannot combine with TraceRenderTail.' }
    if ($FakeSwapchain -and ($TraceRenderTail -or $WsiAcquireTrace)) { throw 'FakeSwapchain cannot combine with trace diagnostics.' }
    $settings = Set-InputObservationEnvironment $StartInfo $Session
    $settings['XESS_CONTINUOUS_INPUTS'] = '1'; $settings['XESS_FSR_NATIVE_OFF'] = '1'
    $settings['XESS_BRIDGE_DEVICE'] = '1'; $settings['XESS_PRESENTATION_OFF'] = '1'
    $settings['XESS_SDK_BIN'] = $installRoot
    $settings['DISABLE_RTSS_LAYER'] = '1'
    if ($TraceRenderTail) { $settings['XESS_RENDER_TAIL_DIAGNOSTIC'] = '1' }
    if ($WsiAcquireTrace) { $settings['XESS_WSI_ACQUIRE_OBSERVER'] = '1' }
    if ($FakeSwapchain) { $settings['XESS_FAKE_SWAPCHAIN'] = '1' }
    if ($GpuTimestamps) { $settings['XESS_GPU_TIMESTAMPS'] = '1' }
    if ($XessSr) { $settings['XESS_SR_REPLACE'] = '1' }
    if ($TrackingCpuTiming) { $settings['XESS_TRACKING_CPU_TIMING'] = '1' }
    if ($ThreadCpuSampling) { $settings['XESS_THREAD_CPU_SAMPLING'] = '1' }
    if ($Override30Fps) { $settings['XESS_FRAME_CAP_OVERRIDE'] = '1'; $settings['XESS_FRAME_CAP_TARGET_FPS'] = [string]$FrameCapTargetFps }
    if ($FgDebugMode) { $settings['XESS_FG_DEBUG_MODE'] = [string]$FgDebugMode }
    if ($null -ne $FgSceneIndex -and $FgSceneIndex -ge 0) { $settings['XESS_FG_SCENE_THRESHOLD'] = [string]$FgSceneIndex }
    if ($LowResOutput) { $settings['XESS_XEFG_LOW_RES_OUTPUT'] = '1' }
    foreach ($name in $settings.Keys) { $StartInfo.EnvironmentVariables[$name] = $settings[$name] }
    return $settings
}
function Get-SessionRetentionPlan([string]$Root,[string]$Current,[string[]]$CaptureRoots=@(),[int]$Limit=20,[long]$ByteLimit=524288000,[switch]$DiagnosticOnly) {
    $rootFull=[IO.Path]::GetFullPath($Root).TrimEnd('\');Assert-NoReparse $rootFull
    $protected=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $referencesKnown=$true;$referenceErrors=@()
    foreach($captureRoot in $CaptureRoots){
        try{
            Assert-NoReparse $captureRoot
            if(-not [IO.Directory]::Exists($captureRoot)){throw 'Capture directory unavailable'}
            foreach($capture in Get-ChildItem -LiteralPath $captureRoot -Directory -Force){
                Assert-NoReparse $capture.FullName
                $discovery=Join-Path $capture.FullName 'session-discovery.json'
                if(-not [IO.File]::Exists($discovery)){continue};Assert-NoReparse $discovery
                if((Get-Item -LiteralPath $discovery).Length -gt 1MB){throw 'Oversized capture reference'}
                $record=Get-Content -LiteralPath $discovery -Raw|ConvertFrom-Json @managerJsonOptions
                if($record.state -cnotin @('matched','ambiguous','absent') -or ($record.state -ceq 'matched' -and $record.match.sessionId -cnotmatch '^[0-9a-fA-F]{32}$')){throw 'Unrecognized capture reference schema'}
                foreach($entry in @($record.match)+@($record.candidates)){if($null -eq $entry){continue};if($entry.sessionId -cnotmatch '^[0-9a-fA-F]{32}$'){throw 'Invalid capture reference identity'};$null=$protected.Add($entry.sessionId)}
            }
        }catch{$referencesKnown=$false;$referenceErrors+=($_.Exception.Message)}
    }
    $pinRoot=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'EndfieldXeSSDiag\SessionPins'
    try{Assert-NoReparse $pinRoot;if(Test-Path -LiteralPath $pinRoot){foreach($pin in Get-ChildItem -LiteralPath $pinRoot -File -Force){Assert-NoReparse $pin.FullName;if($pin.BaseName -cmatch '^[0-9a-fA-F]{32}$' -and $pin.Extension -ceq '.keep'){$null=$protected.Add($pin.BaseName)}}}}
    catch{$referencesKnown=$false;$referenceErrors+=($_.Exception.Message)}
    $sessions=@();$skipped=@()
    if(Test-Path -LiteralPath $rootFull){foreach($directory in Get-ChildItem -LiteralPath $rootFull -Directory -Force){
        $reason=$null
        try{
            if($directory.Name -cnotmatch '^[0-9a-fA-F]{32}$'){throw 'not_session_name'}
            Assert-NoReparse $directory.FullName
            $path=[IO.Path]::GetFullPath($directory.FullName)
            if(-not $path.StartsWith($rootFull+'\',[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetDirectoryName($path) -ine $rootFull){throw 'outside_session_root'}
            $items=@(Get-ChildItem -LiteralPath $path -Force);$bytes=[long]0;$fingerprint=@()
            foreach($file in $items){
                Assert-NoReparse $file.FullName
                if($file.PSIsContainer -or $file.Name -cnotmatch '^(launch\.json|result\.json|stdout\.txt|stderr\.txt|efu-runtime\.jsonl|observer\.jsonl(?:\.(?:measurements|measurements-summary|sr-observation)-[01]\.jsonl)?|retention-(?:plan|result)\.json)$'){throw 'unknown_session_contents'}
                $bytes+=$file.Length;$fingerprint+=($file.Name+':'+$file.Length+':'+$file.LastWriteTimeUtc.Ticks)
            }
            $launchPath=Join-Path $path 'launch.json'
            if(-not [IO.File]::Exists($launchPath) -or (Get-Item -LiteralPath $launchPath).Length -gt 1MB){throw 'ownership_unproven'}
            $launch=Get-Content -LiteralPath $launchPath -Raw|ConvertFrom-Json @managerJsonOptions
            if($DiagnosticOnly -and ($launch.diagnosticMode -ne $true -or $launch.runtimeStatusTransport -cne 'shared_memory_v1')){throw 'not_rc5_diagnostic_session'}
            if(-not [IO.Path]::IsPathRooted([string]$launch.sessionDirectory) -or -not [IO.Path]::IsPathRooted([string]$launch.environmentForChildOnly.XESS_OBSERVER_LOG)){throw 'ownership_unproven'}
            if($launch.sessionId -ine $directory.Name -or [IO.Path]::GetFullPath($launch.sessionDirectory) -ine $path -or [IO.Path]::GetFullPath($launch.environmentForChildOnly.XESS_OBSERVER_LOG) -ine (Join-Path $path 'observer.jsonl')){throw 'ownership_unproven'}
            $reason=if($directory.Name -ieq $Current){'current_session'}elseif($protected.Contains($directory.Name)){'capture_reference'}elseif(-not $referencesKnown){'references_unavailable'}else{$null}
            $sessions+=[pscustomobject]@{id=$directory.Name;directory=$path;bytes=$bytes;createdUtc=$directory.CreationTimeUtc.ToString('o');fingerprint=($fingerprint|Sort-Object)-join '|';protectedReason=$reason}
        }catch{$skipped+=[ordered]@{id=$directory.Name;reason=$_.Exception.Message}}
    }}
    $remainingCount=$sessions.Count;$remainingBytes=[long](($sessions|Measure-Object bytes -Sum).Sum);$candidates=@()
    foreach($session in @($sessions|Where-Object {-not $_.protectedReason}|Sort-Object createdUtc,id)){
        if($remainingCount -le $Limit -and $remainingBytes -le $ByteLimit){break}
        $candidates+=$session;--$remainingCount;$remainingBytes-=$session.bytes
    }
    return [ordered]@{schema=1;root=$rootFull;current=$Current;readOnly=$true;diagnosticOnly=[bool]$DiagnosticOnly;limit=$Limit;byteLimit=$ByteLimit;captureRoots=@($CaptureRoots);referencesKnown=$referencesKnown;referenceErrors=$referenceErrors
        sessions=$sessions;skipped=$skipped;candidates=$candidates;deleteCount=$candidates.Count;deleteBytes=[long](($candidates|Measure-Object bytes -Sum).Sum);remainingCount=$remainingCount;remainingBytes=$remainingBytes;budgetBlocked=($remainingCount -gt $Limit -or $remainingBytes -gt $ByteLimit)}
}
function Invoke-SessionRetention($Plan) {
    $rootFull=[IO.Path]::GetFullPath($sessionRoot).TrimEnd('\')
    if($Plan.root -ine $rootFull -or -not $Plan.referencesKnown){throw 'Unsafe or incomplete retention plan'}
    Assert-ProtectedAcl $rootFull
    $deleted=@();$failed=@()
    foreach($candidate in $Plan.candidates){
        if(@(Get-Process -Name Endfield,Games,EFU,Launcher,HypergryphLauncher -ErrorAction SilentlyContinue).Count){$failed+='process_running';break}
        $leases=@();$removedFiles=@()
        try{
            # Re-read references, ownership, sizes and reparse state immediately
            # before removal. No recursive delete, traversal, or shell fallback.
            $fresh=Get-SessionRetentionPlan $rootFull $Plan.current $Plan.captureRoots $Plan.limit $Plan.byteLimit -DiagnosticOnly:([bool]$Plan.diagnosticOnly)
            $match=@($fresh.candidates|Where-Object {$_.id -ceq $candidate.id -and $_.fingerprint -ceq $candidate.fingerprint})
            if(-not $fresh.referencesKnown -or $match.Count -ne 1){throw 'candidate_changed_or_protected'}
            $path=[IO.Path]::GetFullPath($match[0].directory)
            if([IO.Path]::GetDirectoryName($path) -ine $rootFull -or $candidate.id -cnotmatch '^[0-9a-fA-F]{32}$' -or $candidate.id -ieq $Plan.current){throw 'invalid_delete_target'}
            Assert-NoReparse $path
            $files=@(Get-ChildItem -LiteralPath $path -File -Force|Sort-Object Name)
            foreach($file in $files){Assert-NoReparse $file.FullName;$leases+=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Delete)}
            $fingerprint=(@(Get-ChildItem -LiteralPath $path -File -Force|ForEach-Object {$_.Name+':'+$_.Length+':'+$_.LastWriteTimeUtc.Ticks})|Sort-Object)-join '|'
            if($fingerprint -cne $candidate.fingerprint){throw 'candidate_changed_after_lock'}
            foreach($file in $files){[IO.File]::Delete($file.FullName);$removedFiles+=$file.Name}
            foreach($lease in $leases){$lease.Dispose()};$leases=@()
            [IO.Directory]::Delete($path,$false)
            $deleted+=[ordered]@{id=$candidate.id;bytes=$candidate.bytes;files=$removedFiles}
        }catch{$failed+=[ordered]@{id=$candidate.id;reason=$_.Exception.Message;removedFiles=$removedFiles}}
        finally{foreach($lease in $leases){$lease.Dispose()}}
    }
    return [ordered]@{schema=1;deleted=$deleted;failed=$failed;readOnly=$false}
}
function Read-SessionRetentionPolicy {
    $path=Join-Path $installRoot 'session-retention.json'
    if(-not (Test-Path -LiteralPath $path)){return [pscustomobject]@{mode='Preview';captureRoots=@()}}
    Assert-NoReparse $path;Assert-ProtectedAcl $path
    if((Get-Item -LiteralPath $path).Length -gt 32768){throw 'Retention policy too large'}
    $policy=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json @managerJsonOptions
    if($policy.schema -ne 1 -or $policy.mode -cnotin @('Preview','Auto')){throw 'Invalid retention policy'}
    return $policy
}
function New-SmokeWorkspace {
    $directory=Join-Path $installRoot ('smoke-tmp-'+$SessionId)
    Assert-ProtectedAcl $installRoot;Assert-NoReparse $directory
    if(Test-Path -LiteralPath $directory){throw 'Fresh smoke workspace already exists'}
    [IO.Directory]::CreateDirectory($directory)|Out-Null;Set-ProtectedAcl $directory $true
    return $directory
}
function Remove-SmokeWorkspace([string]$Directory) {
    $full=[IO.Path]::GetFullPath($Directory)
    if([IO.Path]::GetDirectoryName($full) -ine [IO.Path]::GetFullPath($installRoot) -or [IO.Path]::GetFileName($full) -cnotmatch '^smoke-tmp-[0-9a-fA-F]{32}$'){throw 'Smoke cleanup path outside its protected workspace'}
    Assert-NoReparse $full
    $items=@(Get-ChildItem -LiteralPath $full -Force)
    foreach($file in $items){
        Assert-NoReparse $file.FullName
        if($file.PSIsContainer -or $file.Name -cnotmatch '^(observer\.jsonl(\.(measurements|measurements-summary|sr-observation)-[01]\.jsonl)?|stdout\.txt|stderr\.txt|result\.json)$'){throw 'Unknown content in smoke workspace; refusing cleanup'}
    }
    foreach($file in $items){[IO.File]::Delete($file.FullName)}
    [IO.Directory]::Delete($full,$false)
}
function New-DiagnosticSession {
    foreach ($directory in @((Split-Path -Parent $sessionRoot), $sessionRoot)) {
        Assert-NoReparse $directory
        if (-not (Test-Path -LiteralPath $directory)) { [IO.Directory]::CreateDirectory($directory) | Out-Null; Set-ProtectedAcl $directory $true }
        else { Assert-ProtectedAcl $directory }
    }
    $session = Join-Path $sessionRoot $SessionId
    if (Test-Path -LiteralPath $session) { throw 'Session ID already exists; choose a fresh session.' }
    [IO.Directory]::CreateDirectory($session) | Out-Null; Set-ProtectedAcl $session $true
    return $session
}
function Set-ObserverChildEnvironment($StartInfo, [string]$Session) {
    $StartInfo.EnvironmentVariables.Remove('XESS_SR_REPLACE')
    foreach ($name in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_ADD_IMPLICIT_LAYER_PATH','VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ALLOW','XESS_SL_OBSERVER','XESS_BRIDGE_DEVICE','XESS_COMPUTE_OBSERVER','XESS_FSR_OBSERVER','XESS_FSR_UPSCALE_OBSERVER','XESS_RAWCOLOR_CAPTURE','XESS_PRESENTATION_OFF','XESS_CONTINUOUS_INPUTS','XESS_FSR_NATIVE_OFF','XESS_SDK_BIN','XESS_XEFG_LOW_RES_OUTPUT','XESS_WSI_ACQUIRE_OBSERVER','XESS_FAKE_SWAPCHAIN','XESS_GPU_TIMESTAMPS','XESS_TRACKING_CPU_TIMING','XESS_THREAD_CPU_SAMPLING','XESS_FRAME_CAP_30_TO_40','XESS_FRAME_CAP_OVERRIDE','XESS_FRAME_CAP_TARGET_FPS','XESS_FG_DEBUG_MODE','XESS_FG_SCENE_THRESHOLD','XESS_RUNTIME_STATUS','XESS_RUNTIME_IMAGE','XESS_DIAGNOSTIC_MODE','XESS_WSI_EXPECT_OBSERVER','XESS_WSI_EXPECT_DIAGNOSTICS')) {
        $StartInfo.EnvironmentVariables.Remove($name)
    }
    $StartInfo.EnvironmentVariables['VK_INSTANCE_LAYERS'] = $layerName
    $StartInfo.EnvironmentVariables['VK_LOADER_DEBUG'] = 'error,warn,layer'
    $StartInfo.EnvironmentVariables['XESS_OBSERVER_LOG'] = Join-Path $Session 'observer.jsonl'
    $StartInfo.EnvironmentVariables['XESS_DIAGNOSTIC_MODE'] = '1'
}

Assert-NoReparse $installRoot
$source = $null
$modSourceFiles = $null
if ($Action -in @('ReviewPlan', 'Install')) {
    if (-not $SourceDirectory) { $SourceDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\observer' }
    $SourceDirectory = (Get-Item -LiteralPath $SourceDirectory).FullName
    $sourceDll = Join-Path $SourceDirectory 'xess-vulkan-observer.dll'
    $sourceManifest = Join-Path $SourceDirectory 'VkLayer_xess_observer.json'
    $sourceSmoke = Join-Path (Split-Path -Parent $SourceDirectory) 'xess-vulkan-wsi-smoke.exe'
    Assert-PeDll $sourceDll
    Assert-PeDll $sourceSmoke $false
    $inputManifest = Get-Content -LiteralPath $sourceManifest -Raw | ConvertFrom-Json @managerJsonOptions
    if ($inputManifest.layer.name -cne $layerName -or $inputManifest.layer.type -cne 'GLOBAL' -or
        $inputManifest.layer.functions.vkNegotiateLoaderLayerInterfaceVersion -cne 'vkNegotiateLoaderLayerInterfaceVersion') {
        throw 'Source manifest does not match the expected explicit observer layer.'
    }
    $declaredDll = $inputManifest.layer.library_path
    if (-not [IO.Path]::IsPathRooted($declaredDll)) { $declaredDll = Join-Path $SourceDirectory $declaredDll }
    if ([IO.Path]::GetFullPath($declaredDll) -ne [IO.Path]::GetFullPath($sourceDll)) { throw 'Source manifest points at an unexpected DLL.' }
    $source = @{ dllSha256 = Get-Sha256 $sourceDll; manifestSha256 = Get-Sha256 $sourceManifest; smokeSha256 = Get-Sha256 $sourceSmoke; scriptSha256 = Get-Sha256 $PSCommandPath }
    if ($WithModLauncher) {
        if (-not $ModPayloadDirectory) { throw '-WithModLauncher requires an explicit -ModPayloadDirectory with the four reviewed optional payloads.' }
        $ModPayloadDirectory = (Get-Item -LiteralPath $ModPayloadDirectory).FullName
        if (-not [IO.Directory]::Exists($ModPayloadDirectory)) { throw 'Mod payload source must be a directory.' }
        $modSourceFiles = [ordered]@{}
        foreach ($name in $modOwnedNames) { $modSourceFiles[$name] = Join-Path $ModPayloadDirectory $name }
        Assert-PeDll $modSourceFiles['EndfieldXeSSLauncher.exe'] $false
        Assert-PeDll $modSourceFiles['EFU.exe'] $false
        if ($efuSha256 -cnotmatch '^[0-9a-f]{64}$' -or (Get-Sha256 $modSourceFiles['EFU.exe']) -cne $efuSha256) { throw 'EFU.exe does not match the reviewed locally built eff32bbc preview pin.' }
        if ((Get-Sha256 $modSourceFiles['EFU-LICENSE.txt']) -cne $efuLicenseSha256) { throw 'EFU-LICENSE.txt does not match the reviewed AGPL-3.0 license.' }
        $noticeItem = Get-Item -LiteralPath $modSourceFiles['EFU-SOURCE-NOTICE.txt']
        if ($noticeItem.Length -gt 65536) { throw 'EFU source notice is too large.' }
        $notice = [IO.File]::ReadAllText($noticeItem.FullName, $utf8)
        if (-not $notice.Contains($efuSourceCommit) -or -not $notice.Contains($efuSha256) -or
            -not $notice.Contains('https://github.com/DynamiByte/Endfield-Uncensored')) { throw 'EFU source notice is missing the pinned release/source attribution.' }
        $source['modLauncher'] = [ordered]@{ launcherSha256 = Get-Sha256 $modSourceFiles['EndfieldXeSSLauncher.exe']
            efuSha256 = Get-Sha256 $modSourceFiles['EFU.exe']; licenseSha256 = Get-Sha256 $modSourceFiles['EFU-LICENSE.txt']
            noticeSha256 = Get-Sha256 $modSourceFiles['EFU-SOURCE-NOTICE.txt'] }
        if ($WithXeFGSdk) {
            $sdkHashes = [ordered]@{ xeFGDllSha256=$xeFGDllSha256; xellDllSha256=$xellDllSha256; intelLicenseSha256=$intelLicenseSha256; xessSrDllSha256=$xessSrDllSha256 }
            $mapping = [ordered]@{ 'libxess_fg.dll'='xeFGDllSha256'; 'libxell.dll'='xellDllSha256'; 'LICENSE.txt'='intelLicenseSha256'; 'libxess.dll'='xessSrDllSha256' }
            foreach ($name in $mapping.Keys) {
                $modSourceFiles[$name] = Join-Path $ModPayloadDirectory $name
                if ((Get-Sha256 $modSourceFiles[$name]) -cne $sdkHashes.($mapping[$name])) { throw "SDK payload does not match the reviewed 3.0.2 pin: $name" }
            }
            $source['xeFGSdk'] = $sdkHashes
        }
    }
}
if($Action -eq 'RetentionPlan'){
    (Get-SessionRetentionPlan $sessionRoot $SessionId $RecorderCaptureRoots)|ConvertTo-Json -Depth 10
    return
}
if ($Action -eq 'ReviewPlan') {
    $installCommand = "& '$($PSCommandPath.Replace("'","''"))' -Action Install -SourceDirectory '$($SourceDirectory.Replace("'","''"))' -ExpectedDllSha256 '$($source.dllSha256)' -ExpectedManifestSha256 '$($source.manifestSha256)' -ExpectedSmokeSha256 '$($source.smokeSha256)' -ExpectedScriptSha256 '$($source.scriptSha256)'"
    if ($WithModLauncher) {
        $installCommand += " -WithModLauncher -ModPayloadDirectory '$($ModPayloadDirectory.Replace("'","''"))' -ExpectedModLauncherSha256 '$($source.modLauncher.launcherSha256)' -ExpectedEfuSha256 '$($source.modLauncher.efuSha256)' -ExpectedEfuLicenseSha256 '$($source.modLauncher.licenseSha256)' -ExpectedEfuNoticeSha256 '$($source.modLauncher.noticeSha256)'"
    }
    if ($WithXeFGSdk) {
        $installCommand += " -WithXeFGSdk -ExpectedXeFGDllSha256 '$($source.xeFGSdk.xeFGDllSha256)' -ExpectedXellDllSha256 '$($source.xeFGSdk.xellDllSha256)' -ExpectedIntelLicenseSha256 '$($source.xeFGSdk.intelLicenseSha256)'"
    }
    [ordered]@{
        action = 'ReviewPlan'; changesPerformed = $false; sourceDirectory = $SourceDirectory; sourceHashes = $source
        singleChainPolicyVersion = 1
        installRoot = $installRoot; copiedFiles = $ownedNames; receipt = $receiptPath
        registry = @{ hive = 'HKLM'; view = 'Registry64'; key = $registrySubKey; valueName = $manifestPath; type = 'REG_DWORD'; data = 0 }
        activation = 'Explicit layer; VK_INSTANCE_LAYERS only in a newly created official-launcher or optional exact-EFU child environment.'
        launcher = $LauncherPath; sessionRoot = $sessionRoot; currentRegistryState = Get-RegistryState
        installCommand = $installCommand
        standaloneLaunchPackageRequested = [bool]$WithModLauncher; modPayloadDirectory = $ModPayloadDirectory
        standaloneLaunch = $(if ($WithModLauncher -and $GameDirectory) { Get-ModLaunchSpecification $GameDirectory } else { $null })
        smokeCommand = "& '$installedScript' -Action Smoke"
        launchCommand = "& '$installedScript' -Action Launch"
        optInStreamlineTraceCommand = "& '$installedScript' -Action Launch -TraceStreamline"
        optInBridgeDeviceCommand = "& '$installedScript' -Action Launch -TraceStreamline -BridgeDevice"
        optInComputeObserverCommand = "& '$installedScript' -Action Launch -BridgeDevice -TraceCompute"
        optInFsrCallbackCommand = "& '$installedScript' -Action Launch -BridgeDevice -TraceFsr"
        standaloneLaunchCommand = $(if ($WithModLauncher) { "& '$installedScript' -Action LaunchMod" } else { $null })
        observeInputsLaunchCommand = $(if ($WithModLauncher) { "& '$installedScript' -Action LaunchMod -ObserveInputs" } else { $null })
        standaloneLaunchPolicy = 'independent_efu_preview'; standaloneResearchFeaturesAvailable = $false
        officialFallbackCommand = $(if ($WithModLauncher) { "& '$installedScript' -Action LaunchMod -OfficialFallback" } else { $null })
        xeFGSdkPackageRequested = [bool]$WithXeFGSdk
        presentationOffLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -BridgeDevice -PresentationOff" } else { $null })
        xeFGLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -LowResOutput -FakeSwapchain" } else { $null })
        xeFGLowResOutputLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -LowResOutput -FakeSwapchain" } else { $null })
        renderTailDiagnosticLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -TraceRenderTail" } else { $null })
        wsiAcquireTraceLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -LowResOutput -WsiAcquireTrace" } else { $null })
        fakeSwapchainLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -LowResOutput -FakeSwapchain" } else { $null })
        legacyChainLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -LowResOutput" } else { $null })
        xeFGFullResOutputLaunchCommand = $(if ($WithXeFGSdk) { "& '$installedScript' -Action LaunchMod -XeFG -FakeSwapchain" } else { $null })
        uninstallCommand = "& '$installedScript' -Action Uninstall"
        requiresNormalUacForChanges = $true; implicitLayersTouched = $false; gameFilesModified = $false
    } | ConvertTo-Json -Depth 7
    return
}
if ($Action -eq 'Status') {
    $receipt = $null
    if (Test-Path -LiteralPath $receiptPath) { $receipt = Read-Receipt; Assert-OwnedFiles $receipt }
    $registryState = Get-RegistryState
    $standaloneAvailable = [bool]$receipt -and $null -ne $receipt.modLauncher
    $configuredDirectory=if($standaloneAvailable){Read-GameDirectory}else{''}
    $compatibility = if ($standaloneAvailable -and $configuredDirectory) { Get-ModGameCompatibility $configuredDirectory } else { $null }
    $currentEfuInstalled = $standaloneAvailable -and $receipt.modLauncher.efuSha256 -ceq $efuSha256 -and $receipt.modLauncher.efuSourceCommit -ceq $efuSourceCommit
    [ordered]@{ installed = [bool]$receipt; standaloneLaunchAvailable = [bool]$standaloneAvailable
        configuredGameDirectory=$configuredDirectory;gamePathConfigured=[bool]$configuredDirectory
        independentLaunchAvailable = [bool]($currentEfuInstalled -and $compatibility.efuLaunchCompatible)
        officialFallbackAvailable = [bool]$standaloneAvailable
        presentationOffAvailable = $false; xeFGAvailable = $false
        efuAvailable = [bool]($currentEfuInstalled -and $compatibility.efuLaunchCompatible); efuEffectVerified = $false
        standaloneLaunchPolicy = 'independent_efu_preview'; researchFeaturesAvailable = $false
        powerShellRuntime = [ordered]@{ edition=$PSVersionTable.PSEdition; major=$PSVersionTable.PSVersion.Major; version=$PSVersionTable.PSVersion.ToString(); executable=$powerShell7Path }
        gameCompatibility = $compatibility
        receipt = $receipt; registry = $registryState; sessionRoot = $sessionRoot } | ConvertTo-Json -Depth 8
    return
}
if ($Action -eq 'Install' -and (-not $ExpectedDllSha256 -or -not $ExpectedManifestSha256 -or -not $ExpectedSmokeSha256 -or -not $ExpectedScriptSha256 -or
    $source.dllSha256 -ne $ExpectedDllSha256.ToLowerInvariant() -or $source.manifestSha256 -ne $ExpectedManifestSha256.ToLowerInvariant() -or
    $source.smokeSha256 -ne $ExpectedSmokeSha256.ToLowerInvariant() -or $source.scriptSha256 -ne $ExpectedScriptSha256.ToLowerInvariant())) {
    throw 'Install requires all four exact SHA256 values from ReviewPlan; source hashes must still match.'
}
if ($Action -eq 'Install' -and $WithModLauncher) {
    $expectedModHashes = [ordered]@{ launcherSha256=$ExpectedModLauncherSha256; efuSha256=$ExpectedEfuSha256
        licenseSha256=$ExpectedEfuLicenseSha256; noticeSha256=$ExpectedEfuNoticeSha256 }
    foreach ($field in $expectedModHashes.Keys) {
        $expected = $expectedModHashes[$field]
        if (-not $expected -or $source.modLauncher.$field -cne $expected.ToLowerInvariant()) {
            throw 'Optional standalone installation requires all four additional exact SHA256 values from ReviewPlan.'
        }
    }
}
if ($Action -eq 'Install' -and $WithXeFGSdk) {
    $expectedSdkHashes = [ordered]@{ xeFGDllSha256=$ExpectedXeFGDllSha256; xellDllSha256=$ExpectedXellDllSha256; intelLicenseSha256=$ExpectedIntelLicenseSha256 }
    foreach ($field in $expectedSdkHashes.Keys) {
        $expected = $expectedSdkHashes[$field]
        if (-not $expected -or $source.xeFGSdk.$field -cne $expected.ToLowerInvariant()) {
            throw 'SDK installation requires all three additional exact SHA256 values from ReviewPlan.'
        }
    }
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $parameters = @{ Action = $Action; LauncherPath = $LauncherPath; GameDirectory = $GameDirectory; SessionId = $SessionId; TraceStreamline = [bool]$TraceStreamline; BridgeDevice = [bool]$BridgeDevice; TraceCompute = [bool]$TraceCompute; TraceFsr = [bool]$TraceFsr; TraceFsrUpscale = [bool]$TraceFsrUpscale; CaptureRawColor = [bool]$CaptureRawColor; PresentationOff = [bool]$PresentationOff; XeFG = [bool]$XeFG; XessSr = [bool]$XessSr; DisableNoDither = [bool]$DisableNoDither; TraceRenderTail = [bool]$TraceRenderTail; WsiAcquireTrace = [bool]$WsiAcquireTrace; FakeSwapchain = [bool]$FakeSwapchain; GpuTimestamps = [bool]$GpuTimestamps; TrackingCpuTiming = [bool]$TrackingCpuTiming; ThreadCpuSampling = [bool]$ThreadCpuSampling; LowResOutput = [bool]$LowResOutput; OfficialFallback = [bool]$OfficialFallback; ObserveInputs = [bool]$ObserveInputs; Override30Fps=[bool]$Override30Fps;FrameCapTargetFps=$FrameCapTargetFps;FgDebugMode=$FgDebugMode;FgSceneIndex=$FgSceneIndex;ReleaseMinimal=[bool]$ReleaseMinimal;DiagnosticMode=[bool]$DiagnosticMode;ReleaseSessionRetention=[bool]$ReleaseSessionRetention;SessionRetentionMode=$SessionRetentionMode;RecorderCaptureRoots=@($RecorderCaptureRoots);ApproveRetention=[bool]$ApproveRetention }
    if ($Action -eq 'Install') {
        $parameters['SourceDirectory'] = $SourceDirectory
        $parameters['ExpectedDllSha256'] = $ExpectedDllSha256; $parameters['ExpectedManifestSha256'] = $ExpectedManifestSha256
        $parameters['ExpectedSmokeSha256'] = $ExpectedSmokeSha256; $parameters['ExpectedScriptSha256'] = $ExpectedScriptSha256
        $parameters['WithModLauncher'] = [bool]$WithModLauncher
        if ($WithModLauncher) {
            $parameters['ModPayloadDirectory'] = $ModPayloadDirectory
            $parameters['ExpectedModLauncherSha256'] = $ExpectedModLauncherSha256; $parameters['ExpectedEfuSha256'] = $ExpectedEfuSha256
            $parameters['ExpectedEfuLicenseSha256'] = $ExpectedEfuLicenseSha256; $parameters['ExpectedEfuNoticeSha256'] = $ExpectedEfuNoticeSha256
        }
        $parameters['WithXeFGSdk'] = [bool]$WithXeFGSdk
        if ($WithXeFGSdk) {
            $parameters['ExpectedXeFGDllSha256'] = $ExpectedXeFGDllSha256
            $parameters['ExpectedXellDllSha256'] = $ExpectedXellDllSha256
            $parameters['ExpectedIntelLicenseSha256'] = $ExpectedIntelLicenseSha256
        }
    }
    $request = @{ scriptPath = $PSCommandPath; scriptSha256 = Get-Sha256 $PSCommandPath; parameters = $parameters }
    if ($Action -eq 'Install') { $request.scriptSha256 = $ExpectedScriptSha256.ToLowerInvariant() }
    $requestBase64 = [Convert]::ToBase64String($utf8.GetBytes(($request | ConvertTo-Json -Depth 5 -Compress)))
    # The elevated process runs this fixed bootstrap, not an unchecked -File.
    # A read-sharing lock denies concurrent write/replacement through hashing and
    # parsing. Execute that parsed ScriptBlock, which retains its file metadata.
    # Release the lock before invocation so the installed helper can uninstall itself.
    $bootstrap = @'
$ErrorActionPreference = 'Stop'
try {
    if ($PSVersionTable.PSEdition -cne 'Core' -or $PSVersionTable.PSVersion.Major -ne 7) { throw 'Elevated helper requires PowerShell 7 Core.' }
    $env:PSModulePath = Join-Path $PSHOME 'Modules'
    $managerJsonOptions = @{}
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $managerJsonOptions.DateKind = 'String' }
    $request = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__REQUEST__')) | ConvertFrom-Json @managerJsonOptions
    $stream = [IO.File]::Open($request.scriptPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
        if ($hash -ne $request.scriptSha256) { throw 'Reviewed management script changed before elevation; refusing execution.' }
        $parsed = (Get-Command -Name $request.scriptPath -CommandType ExternalScript).ScriptBlock
    } finally { $stream.Dispose() }
    $parameters = @{}
    foreach ($property in $request.parameters.PSObject.Properties) { $parameters[$property.Name] = $property.Value }
    & $parsed @parameters
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
'@
    $bootstrap = $bootstrap.Replace('__REQUEST__', $requestBase64)
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
    $powershell = Get-TrustedPowerShell7
    $child = Start-Process -FilePath $powershell -Verb RunAs -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded) -WindowStyle Hidden -PassThru
    # Wait for the helper itself; Start-Process -Wait also waits for the launched
    # interactive game/launcher descendants and would hold this call until exit.
    $child.WaitForExit()
    if ($child.ExitCode -ne 0) { throw "Elevated $Action failed (exit $($child.ExitCode)); no UAC bypass was attempted. Run the same command in an administrator PowerShell to inspect the error." }
    if($Action -eq 'Smoke'){Write-Host 'Temporary WSI self-test finished; no diagnostic session retained.'}
    elseif($ReleaseMinimal -and -not $DiagnosticMode){Write-Host 'Independent zero-log launch request finished.'}
    elseif ($Action -in @('Launch','LaunchMod')) { Write-Host "Diagnostic session: $(Join-Path $sessionRoot $SessionId)" }
    else { Write-Host "$Action finished. Use -Action Status to inspect installed state." }
    return
}

if($Action -eq 'SetRetentionPolicy'){
    Assert-GameClosed
    $receipt=Read-Receipt;Assert-OwnedFiles $receipt;Assert-PeDll $dllPath $true -RequireRetention
    $roots=@($RecorderCaptureRoots|ForEach-Object {Normalize-GameDirectory $_})
    if($SessionRetentionMode -ceq 'Auto'){
        if(-not $ApproveRetention){throw 'Review a read-only RetentionPlan and explicitly approve it before enabling automatic deletion.'}
        $plan=Get-SessionRetentionPlan $sessionRoot $SessionId $roots -DiagnosticOnly
        if(-not $plan.referencesKnown){throw 'Cannot enable retention with unavailable capture references'}
    }
    $path=Join-Path $installRoot 'session-retention.json';Assert-NoReparse $path
    if(Test-Path -LiteralPath $path){Assert-ProtectedAcl $path}
    $temporary=Join-Path $installRoot ('retention.'+[guid]::NewGuid().ToString('N')+'.tmp')
    try{
        [IO.File]::WriteAllText($temporary,([ordered]@{schema=1;mode=$SessionRetentionMode;captureRoots=$roots;limit=20;byteLimit=524288000}|ConvertTo-Json -Depth 4),$utf8)
        Set-ProtectedAcl $temporary $false;[IO.File]::Move($temporary,$path,$true)
    }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
    [ordered]@{mode=$SessionRetentionMode;policySaved=$true;logsDeleted=$false}|ConvertTo-Json
    return
}
if($Action -eq 'SetGamePath'){
    $receipt=Read-Receipt;Assert-OwnedFiles $receipt
    $saved=Write-GameDirectory $GameDirectory
    [ordered]@{action='SetGamePath';gameDirectory=$saved;validated=$true}|ConvertTo-Json
    return
}
if ($Action -eq 'Install') {
    Assert-FrontendClosed
    Assert-GameClosed
    if (Test-Path -LiteralPath $installRoot) {
        $receipt = Read-Receipt; Assert-OwnedFiles $receipt
        $state = Get-RegistryState
        $sameModPackage = ([bool]$WithModLauncher -eq ($null -ne $receipt.modLauncher))
        if ($sameModPackage -and $WithModLauncher) {
            foreach ($field in @('launcherSha256','efuSha256','licenseSha256','noticeSha256')) {
                if ($receipt.modLauncher.sourceHashes.$field -cne $source.modLauncher.$field) { $sameModPackage = $false }
            }
        }
        $sameSdkPackage = ([bool]$WithXeFGSdk -eq ($null -ne $receipt.xeFGSdk))
        if ($sameSdkPackage -and $WithXeFGSdk) {
            foreach ($field in @('xeFGDllSha256','xellDllSha256','intelLicenseSha256')) {
                if ($receipt.xeFGSdk.sourceHashes.$field -cne $source.xeFGSdk.$field) { $sameSdkPackage = $false }
            }
        }
        if ($receipt.sourceDllSha256 -eq $source.dllSha256 -and $receipt.sourceManifestSha256 -eq $source.manifestSha256 -and $receipt.sourceSmokeSha256 -eq $source.smokeSha256 -and $receipt.sourceScriptSha256 -eq $source.scriptSha256 -and
            $sameModPackage -and $sameSdkPackage -and $state.exists -and $state.kind -eq 'DWord' -and $state.data -eq 0) { Write-Host 'Same reviewed observer is already installed.'; return }
        throw 'An owned but different/incomplete installation exists. Uninstall it before installing another version.'
    }
    if ((Get-RegistryState).exists) { throw 'Target registry value already exists without this installation receipt; refusing overwrite.' }
    [IO.Directory]::CreateDirectory($installRoot) | Out-Null
    Set-ProtectedAcl $installRoot $true
    $registered = $false; $created = @()
    try {
        Copy-Item -LiteralPath $sourceDll -Destination $dllPath
        $created += $dllPath; Set-ProtectedAcl $dllPath $false
        if ((Get-Sha256 $dllPath) -ne $source.dllSha256) { throw 'Copied DLL hash mismatch.' }
        $outputManifest = [ordered]@{ file_format_version = '1.2.0'; layer = [ordered]@{
            name = $layerName; type = 'GLOBAL'; library_path = $dllPath.Replace('\', '/'); api_version = '1.4.0'; implementation_version = '1'
            description = 'Explicit Vulkan diagnostic observer for Endfield XeSS research'
            functions = @{ vkNegotiateLoaderLayerInterfaceVersion = 'vkNegotiateLoaderLayerInterfaceVersion' }
        } }
        [IO.File]::WriteAllText($manifestPath, ($outputManifest | ConvertTo-Json -Depth 6), $utf8)
        $created += $manifestPath; Set-ProtectedAcl $manifestPath $false
        Copy-Item -LiteralPath $PSCommandPath -Destination $installedScript
        $created += $installedScript; Set-ProtectedAcl $installedScript $false
        if ((Get-Sha256 $installedScript) -ne $source.scriptSha256) { throw 'Copied management script does not match the reviewed script hash.' }
        Copy-Item -LiteralPath $sourceSmoke -Destination $smokePath
        $created += $smokePath; Set-ProtectedAcl $smokePath $false
        if ((Get-Sha256 $smokePath) -ne $source.smokeSha256) { throw 'Copied WSI smoke executable hash mismatch.' }
        if ($WithModLauncher) {
            $mapping = [ordered]@{ 'EndfieldXeSSLauncher.exe'='launcherSha256'; 'EFU.exe'='efuSha256'; 'EFU-LICENSE.txt'='licenseSha256'; 'EFU-SOURCE-NOTICE.txt'='noticeSha256' }
            foreach ($name in $mapping.Keys) {
                $destination = Join-Path $installRoot $name
                Copy-Item -LiteralPath $modSourceFiles[$name] -Destination $destination
                $created += $destination; Set-ProtectedAcl $destination $false
                if ((Get-Sha256 $destination) -cne $source.modLauncher.($mapping[$name])) { throw "Copied optional payload hash mismatch: $name" }
            }
        }
        if ($WithXeFGSdk) {
            $mapping = [ordered]@{ 'libxess_fg.dll'='xeFGDllSha256'; 'libxell.dll'='xellDllSha256'; 'LICENSE.txt'='intelLicenseSha256'; 'libxess.dll'='xessSrDllSha256' }
            foreach ($name in $mapping.Keys) {
                $destination = Join-Path $installRoot $name
                Copy-Item -LiteralPath $modSourceFiles[$name] -Destination $destination
                $created += $destination; Set-ProtectedAcl $destination $false
                if ((Get-Sha256 $destination) -cne $source.xeFGSdk.($mapping[$name])) { throw "Copied SDK payload hash mismatch: $name" }
            }
        }
        $receipt = [ordered]@{
            schemaVersion = 1; singleChainPolicyVersion = 1; productId = $productId; installationId = [guid]::NewGuid().ToString('N'); installedAt = (Get-Date).ToString('o')
            installRoot = $installRoot; layerName = $layerName; registrySubKey = $registrySubKey; registryValueName = $manifestPath
            sourceDllSha256 = $source.dllSha256; sourceManifestSha256 = $source.manifestSha256; sourceSmokeSha256 = $source.smokeSha256; sourceScriptSha256 = $source.scriptSha256
            files = @($ownedNames | ForEach-Object { @{ name = $_; sha256 = Get-Sha256 (Join-Path $installRoot $_) } })
        }
        if ($WithModLauncher) {
            $receipt['modLauncher'] = [ordered]@{ version = 1; efuVersion = '5.1.0'; efuSha256 = $efuSha256; efuLocalPatchId='camera-minhook-launcher-options-v3'
                efuSourceCommit = $efuSourceCommit; sourceHashes = $source.modLauncher }
        }
        if ($WithXeFGSdk) { $receipt['xeFGSdk'] = [ordered]@{ version=1; sdkVersion=$sdkVersion; sourceHashes=$source.xeFGSdk } }
        [IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 6), $utf8)
        $created += $receiptPath; Set-ProtectedAcl $receiptPath $false
        if($WithModLauncher -and $GameDirectory -and (Get-ModGameCompatibility $GameDirectory).efuLaunchCompatible){
            Write-GameDirectory $GameDirectory|Out-Null;$created+=$gameConfigurationPath
        }
        $key = Open-LayerKey $true $true
        try {
            if ($key.GetValueNames() -contains $manifestPath) { throw 'Registry value appeared during install; refusing overwrite.' }
            $key.SetValue($manifestPath, 0, [Microsoft.Win32.RegistryValueKind]::DWord); $registered = $true
        } finally { $key.Dispose() }
        Assert-OwnedFiles (Read-Receipt)
        Write-Host "Installed explicit layer. Receipt: $receiptPath"
    } catch {
        if ($registered) {
            $key = Open-LayerKey $true
            try { if ($key -and $key.GetValueKind($manifestPath) -eq 'DWord' -and $key.GetValue($manifestPath) -eq 0) { $key.DeleteValue($manifestPath, $false) } }
            finally { if ($key) { $key.Dispose() } }
        }
        # Only files this invocation just created, within the verified fixed root.
        foreach ($path in $created) { Assert-NoReparse $path; if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } }
        if (@(Get-ChildItem -LiteralPath $installRoot -Force).Count -eq 0) { Remove-Item -LiteralPath $installRoot }
        throw
    }
    return
}
if ($Action -eq 'Uninstall') {
    Assert-FrontendClosed
    Assert-GameClosed
    if (-not (Test-Path -LiteralPath $installRoot)) {
        if ((Get-RegistryState).exists) { throw 'Registry value exists without an ownership receipt; refusing removal.' }
        Write-Host 'Already uninstalled.'; return
    }
    $receipt = Read-Receipt; Assert-OwnedFiles $receipt $true
    $state = Get-RegistryState
    if ($state.exists -and ($state.kind -ne 'DWord' -or $state.data -ne 0)) { throw 'Owned registry value was modified; refusing removal.' }
    if ($state.exists) {
        $key = Open-LayerKey $true
        try { $key.DeleteValue($manifestPath, $false) } finally { $key.Dispose() }
    }
    foreach ($entry in $receipt.files) {
        $path = Join-Path $installRoot $entry.name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    if(Test-Path -LiteralPath $gameConfigurationPath){Assert-ProtectedAcl $gameConfigurationPath;Remove-Item -LiteralPath $gameConfigurationPath -Force}
    $retentionPolicy=Join-Path $installRoot 'session-retention.json'
    if(Test-Path -LiteralPath $retentionPolicy){Assert-ProtectedAcl $retentionPolicy;Remove-Item -LiteralPath $retentionPolicy -Force}
    Remove-Item -LiteralPath $receiptPath -Force
    if (@(Get-ChildItem -LiteralPath $installRoot -Force).Count -eq 0) { Remove-Item -LiteralPath $installRoot }
    Write-Host 'Removed the exact owned registry value and files. Diagnostic session logs were retained.'
    return
}
if ($Action -eq 'Smoke') {
    $receipt = Read-Receipt; Assert-OwnedFiles $receipt
    $state = Get-RegistryState
    if (-not $state.exists -or $state.kind -ne 'DWord' -or $state.data -ne 0) { throw 'Reviewed explicit layer is not registered as expected.' }
    $session = New-SmokeWorkspace
    try{
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $smokePath; $start.Arguments = '--frames 24'; $start.WorkingDirectory = $session
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    Set-ObserverChildEnvironment $start $session
    $start.EnvironmentVariables['XESS_WSI_EXPECT_OBSERVER']=$dllPath
    $start.EnvironmentVariables['XESS_WSI_EXPECT_DIAGNOSTICS']='1'
    if ($BridgeDevice) {
        $start.EnvironmentVariables['XESS_BRIDGE_DEVICE'] = '1'
        $start.Arguments += ' --api-1.3'
    }
    if ($TraceCompute) {
        $start.EnvironmentVariables['XESS_COMPUTE_OBSERVER'] = '1'
        $start.Arguments += ' --compute-smoke'
    }
    if ($TraceFsr) { $start.EnvironmentVariables['XESS_FSR_OBSERVER'] = '1' }
    if ($TraceFsrUpscale) { $start.EnvironmentVariables['XESS_FSR_UPSCALE_OBSERVER'] = '1' }
    $child = [Diagnostics.Process]::new(); $child.StartInfo = $start
    if (-not $child.Start()) { throw 'Unable to start the owned WSI smoke executable.' }
    $stdoutTask = $child.StandardOutput.ReadToEndAsync(); $stderrTask = $child.StandardError.ReadToEndAsync()
    $timedOut = -not $child.WaitForExit(35000)
    if ($timedOut) { $child.Kill(); $child.WaitForExit() }
    $stdout = $stdoutTask.GetAwaiter().GetResult(); $stderr = $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $session 'stdout.txt'), $stdout, $utf8)
    [IO.File]::WriteAllText((Join-Path $session 'stderr.txt'), $stderr, $utf8)
    $observerLog = Join-Path $session 'observer.jsonl'; $events = @()
    if (Test-Path -LiteralPath $observerLog) { $events = @(Get-Content -LiteralPath $observerLog | ForEach-Object { $_ | ConvertFrom-Json @managerJsonOptions }) }
    $destroy = @($events | Where-Object event -EQ 'destroy_device')
    $passed = -not $timedOut -and $child.ExitCode -eq 0 -and $stdout.Contains('WSI_SMOKE_PASS') -and
        ($events.event -contains 'create_swapchain') -and ($events.event -contains 'destroy_instance') -and
        $destroy.Count -eq 1 -and $destroy[0].present_calls -eq 24 -and $destroy[0].present_negative_results -eq 0
    if ($BridgeDevice) {
        $devices = @($events | Where-Object event -EQ 'create_device')
        $passed = $passed -and $devices.Count -eq 1 -and $devices[0].timeline_semaphore_requested -and
            ($devices[0].enabled_extensions -contains 'VK_KHR_external_memory_win32') -and
            ($devices[0].enabled_extensions -contains 'VK_KHR_external_semaphore_win32')
    }
    $result = [ordered]@{ test = 'elevated-HKLM-explicit-layer-WSI'; capturedAt = (Get-Date).ToString('o'); passed = [bool]$passed
        elevated = $admin; frames = 24; exitCode = $child.ExitCode; timedOut = $timedOut; eventCount = $events.Count
        observerDllSha256 = $receipt.sourceDllSha256; smokeSha256 = $receipt.sourceSmokeSha256; bridgeDeviceRequested = [bool]$BridgeDevice; gameTested = $false }
    if ($TraceCompute) {
        $computeSummaries = @($events | Where-Object event -EQ 'compute_observer_summary')
        $dispatches = @($events | Where-Object event -EQ 'compute_dispatch_sample')
        $passed = $passed -and $computeSummaries.Count -gt 0 -and $dispatches.Count -ge 2
        if ($computeSummaries.Count -gt 0 -and $dispatches.Count -ge 2) {
            $computeEnd = $computeSummaries[-1]
            $passed = $passed -and $computeEnd.dispatch_recording_calls -eq 24 -and $computeEnd.unknown_pipeline_dispatches -eq 0 -and
                $computeEnd.trace_failures -eq 0 -and $computeEnd.live_command_buffers -eq 0 -and $computeEnd.live_pipelines -eq 0 -and
                $dispatches[0].shader_generation -gt 0 -and $dispatches[0].pipeline_generation -gt 0
        }
    }
    $result.passed = [bool]$passed
    $result['computeObserverRequested'] = [bool]$TraceCompute
    if ($TraceFsr) {
        $refused = @($events | Where-Object { $_.event -eq 'fsr_observer_install' -and $_.state -eq 'refused_process_path' })
        $passed = $passed -and $refused.Count -eq 1
        $result.passed = [bool]$passed
    }
    if ($TraceFsrUpscale) {
        $refused = @($events | Where-Object { $_.event -eq 'upscale_observer_install' -and $_.state -eq 'refused_process_path' })
        $passed = $passed -and $refused.Count -eq 1
        $result.passed = [bool]$passed
    }
    $result['fsrCallbackObserverRequested'] = [bool]$TraceFsr
    $result['fsrUpscaleObserverRequested'] = [bool]$TraceFsrUpscale
    }finally{Remove-SmokeWorkspace $session}
    $result['temporaryFilesRemoved']=$true
    $result | ConvertTo-Json -Depth 5
    if (-not $passed) { throw 'Elevated observer smoke failed; temporary logs were removed.' }
    return
}
if ($Action -eq 'Launch') {
    Assert-GameClosed
    if(-not $GameDirectory){$GameDirectory=Read-GameDirectory}
    if(-not $GameDirectory){throw '请先在独立启动器中选择并验证游戏目录。'}
    if(-not $LauncherPath){$LauncherPath=(Get-ModLaunchSpecification $GameDirectory $true).executable}
    $receipt = Read-Receipt; Assert-OwnedFiles $receipt
    $state = Get-RegistryState
    if (-not $state.exists -or $state.kind -ne 'DWord' -or $state.data -ne 0) { throw 'Reviewed explicit layer is not registered as expected.' }
    $launcher = Get-Item -LiteralPath $LauncherPath
    if ($launcher.Name -cne 'Launcher.exe') { throw 'Launch must use the official Launcher.exe, not the game executable.' }
    if (-not (Test-Path -LiteralPath (Join-Path $GameDirectory 'Endfield.exe'))) { throw 'The supplied game directory does not contain Endfield.exe.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $launcher.FullName
    if ($signature.Status -ne 'Valid') { throw 'Official launcher signature is not valid; refusing elevated launch.' }
    $session = New-DiagnosticSession
    $settings = [ordered]@{
        VK_INSTANCE_LAYERS = $layerName; VK_LOADER_DEBUG = 'error,warn,layer'; XESS_OBSERVER_LOG = (Join-Path $session 'observer.jsonl')
        SL_LOG_LEVEL = '2'; SL_LOG_PATH = $session; SL_LOG_NAME = 'endfield-sl.log'; SL_ENABLE_CONSOLE_LOGGING = '0'
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $launcher.FullName; $start.WorkingDirectory = $launcher.DirectoryName; $start.UseShellExecute = $false
    # This is the user's interactive official launcher. Only its process tree
    # receives the diagnostic variables; the shell and global environment do not.
    Set-ObserverChildEnvironment $start $session
    if ($TraceStreamline) { $settings['XESS_SL_OBSERVER'] = '1' }
    if ($BridgeDevice) { $settings['XESS_BRIDGE_DEVICE'] = '1' }
    if ($TraceCompute) { $settings['XESS_COMPUTE_OBSERVER'] = '1' }
    if ($TraceFsr) { $settings['XESS_FSR_OBSERVER'] = '1' }
    if ($TraceFsrUpscale) { $settings['XESS_FSR_UPSCALE_OBSERVER'] = '1' }
    foreach ($name in $settings.Keys) { $start.EnvironmentVariables[$name] = $settings[$name] }
    $record = [ordered]@{
        startedAt = (Get-Date).ToString('o'); launcher = $launcher.FullName; gameDirectory = $GameDirectory
        launcherSha256 = Get-Sha256 $launcher.FullName; launcherSigner = $signature.SignerCertificate.Subject
        environmentForChildOnly = $settings; observerDllSha256 = $receipt.sourceDllSha256; explicitLayerInstalled = $true
        streamlineTraceRequested = [bool]$TraceStreamline
        runtimeStreamlineHooksRequested = [bool]$TraceStreamline
        bridgeDeviceRequirementsRequested = [bool]$BridgeDevice
        computeObserverRequested = [bool]$TraceCompute
        fsrCallbackObserverRequested = [bool]$TraceFsr
        fsrUpscaleObserverRequested = [bool]$TraceFsrUpscale
        modifiedGameFiles = $false; tagCallsVerified = $false; gameFrameInputsVerified = $false
        note = $(if ($TraceStreamline) {
            'Vulkan parameter observation plus opt-in in-process temporary forwarding hooks in three writable Unity function-pointer slots. No disk/code-page modification or fabricated return values. Successful launch does not prove hook activation or game input availability.'
        } else {
            'Vulkan parameter observation via the official launcher; runtime Streamline pointer hooks were not requested. Successful launch does not prove layer activation or game input availability.'
        })
    }
    if ($BridgeDevice) {
        $record.note += ' Device-creation bridge requirements are also requested: query support, copy the create-info, and enable two Win32 sharing extensions plus timeline when the typed chain can be preserved. No frame input copying or XeFG presentation is enabled by this setting. Inspect create_device events for the actual outcome.'
    }
    $launchRecord = Join-Path $session 'launch.json'
    if ($TraceCompute) {
        $record.note += ' Bounded Vulkan compute shader metadata, pipeline and command-recording observation is requested. This compute option alone does not patch FSR callbacks or enable resource copying or XeFG presentation. Shader names may be stripped; recording is not proof of GPU execution or resource semantics.'
    }
    if ($TraceFsr) {
        $record.note += ' Experimental, exact-version-locked FSR callback metadata observation is requested only at the first successful Vulkan device-creation boundary. It uses an in-process code detour, with original arguments and return path preserved by a tested Win64 forwarding entry. A successful install is not proof of complete thread quiescence or valid frame inputs. Module and forwarding code remain pinned until process exit; disabling sampling does not unpatch code. No resource pixels or XeFG presentation are enabled.'
    }
    [IO.File]::WriteAllText($launchRecord, ($record | ConvertTo-Json -Depth 6), $utf8); Set-ProtectedAcl $launchRecord $false
    $child = [Diagnostics.Process]::Start($start)
    $record['launcherProcessId'] = $child.Id
    [IO.File]::WriteAllText($launchRecord, ($record | ConvertTo-Json -Depth 6), $utf8)
    Write-Host "Official launcher started. Diagnostic session: $session"
}
if ($Action -eq 'LaunchMod') {
    $configured=Read-GameDirectory
    if(-not $configured){throw '尚未设置游戏路径。请在启动器中点击“更改…”选择 Endfield.exe。'}
    $GameDirectory=$configured
    # Serialize preparation/start only. No process is adopted or terminated.
    $launchMutex = [Threading.Mutex]::new($false, 'Global\EndfieldXeSSObserver.LaunchMod')
    $held = $false; $payloadLock = $null; $gameLocks = @{}; $child = $null; $session = $null; $record = $null
    try {
        try { $held = $launchMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
        if (-not $held) { throw 'Another standalone launch is already being prepared.' }
        Assert-ModGameClosed
        $receipt = Read-Receipt; Assert-OwnedFiles $receipt
        if ($null -eq $receipt.modLauncher) { throw 'Standalone launcher extension is not installed. Review and install the optional four-file package first.' }
        if ($XeFG -and $null -eq $receipt.xeFGSdk) { throw 'The experimental XeSS 2x candidate requires the reviewed SDK package.' }
        if ($FakeSwapchain) { Assert-SingleChainPolicy $receipt }
        if ($XessSr -and $receipt.xeFGSdk.sourceHashes.xessSrDllSha256 -cne $xessSrDllSha256) { throw 'Install the reviewed XeSS SR SDK payload before launching stage 1.' }
        $specification = Get-ModLaunchSpecification $GameDirectory ([bool]$OfficialFallback) ([bool]$ObserveInputs) ([bool]$XeFG)
        Assert-NoReparse $specification.executable
        $payload = Get-Item -LiteralPath $specification.executable
        if ($payload.PSIsContainer) { throw 'The fixed launch executable is unavailable.' }
        $payloadLock = [IO.File]::Open($payload.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $lockedPayloadHash = ([BitConverter]::ToString($sha.ComputeHash($payloadLock))).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
        $launcherSigner = $null
        if ($OfficialFallback) {
            if ($LauncherPath -and [IO.Path]::GetFullPath($LauncherPath) -ine $specification.executable) { throw '官方启动器路径必须与所选游戏目录对应。' }
            $signature = Get-AuthenticodeSignature -LiteralPath $payload.FullName
            Assert-OfficialLauncherIdentity $payload.FullName $signature
            $launcherSigner = $signature.SignerCertificate.Subject
            $compatibility = Get-ModGameCompatibility $GameDirectory
        } else {
            if ($efuSha256 -cnotmatch '^[0-9a-f]{64}$' -or $lockedPayloadHash -cne $efuSha256 -or
                $receipt.modLauncher.efuSha256 -cne $efuSha256 -or $receipt.modLauncher.efuSourceCommit -cne $efuSourceCommit) {
                throw 'Independent launch requires the reviewed eff32bbc source-built EFU payload. Legacy EFU receipts are accepted only for status/uninstall or explicit official fallback.'
            }
            $compatibility = Get-ModGameCompatibility $GameDirectory $gameLocks
            if (-not $compatibility.efuLaunchCompatible) { throw '当前游戏文件与本包已验证版本不匹配，独立启动已停止。请检查游戏目录及完整性；若游戏已更新，请等待适配版本，或自行使用官方启动器。不会自动转入官方路线。' }
        }
        $officialInstanceAlreadyRunning = @(Get-Process -Name Launcher,Games -ErrorAction SilentlyContinue).Count -gt 0
        $session=if(-not $ReleaseMinimal -or $DiagnosticMode){New-DiagnosticSession}else{$null}
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $specification.executable; $start.Arguments = $specification.arguments
        $start.WorkingDirectory = $specification.workingDirectory
        $start.UseShellExecute = $false
        $start.CreateNoWindow = -not $OfficialFallback
        $start.WindowStyle = if ($OfficialFallback) { [Diagnostics.ProcessWindowStyle]::Normal } else { [Diagnostics.ProcessWindowStyle]::Hidden }
        $runtimeLog = if ($OfficialFallback -or -not $session) { $null } else { Join-Path $session 'efu-runtime.jsonl' }
        $observerSettings = [ordered]@{}
        if ($ObserveInputs) { $observerSettings = Set-InputObservationEnvironment $start $session }
        elseif ($XeFG) { $observerSettings = Set-ContinuousOffEnvironment $start $session ([bool]$TraceRenderTail) ([bool]$LowResOutput) ([bool]$WsiAcquireTrace) ([bool]$FakeSwapchain) ([bool]$GpuTimestamps) ([bool]$TrackingCpuTiming) ([bool]$XessSr) ([bool]$ThreadCpuSampling) }
        else {
            Set-OfficialCompatibilityEnvironment $start
            if (-not $OfficialFallback) { $start.EnvironmentVariables['EFU_RUNTIME_LOG'] = $runtimeLog }
        }
        if(-not $OfficialFallback){
            Set-NoDitherEnvironment $start ([bool]$DisableNoDither)
            if($ObserveInputs -or $XeFG){$observerSettings['EFU_NO_DITHER']=$start.EnvironmentVariables['EFU_NO_DITHER']}
        }
        $record = [ordered]@{ action='LaunchMod'; status='prepared'; launchStarted=$false; backend=$specification.backend
            sessionId=$SessionId; sessionDirectory=$session; diagnosticMode=[bool]$DiagnosticMode; startedAt=(Get-Date).ToString('o'); elevated=[bool]$admin
            childExecutable=$specification.executable; childArguments=$specification.arguments; childProcessId=$null
            workingDirectory=$specification.workingDirectory; exactGamePath=$specification.gamePath; gameDirectory=$GameDirectory
            childExecutableSha256=$lockedPayloadHash; launcherSigner=$launcherSigner
            efuSha256=$(if ($OfficialFallback) { $null } else { $lockedPayloadHash })
            efuSourceCommit=$(if ($OfficialFallback) { $null } else { $efuSourceCommit }); efuLocallyBuilt=[bool](-not $OfficialFallback)
            efuLocallyPatched=[bool](-not $OfficialFallback); efuLocalPatchId=$(if ($OfficialFallback) { $null } else { 'camera-minhook-launcher-options-v3' })
            runtimeDiagnosticLog=$runtimeLog
            environmentForChildOnly=$(if ($ObserveInputs -or $XeFG) { $observerSettings } elseif ($OfficialFallback) { [ordered]@{} } else { [ordered]@{ EFU_RUNTIME_LOG=$runtimeLog; EFU_NO_DITHER=$start.EnvironmentVariables['EFU_NO_DITHER'] } })
            xessSrRequested=[bool]$XessSr; xessSrEnabledForChild=[bool]($XessSr -and $observerSettings['XESS_SR_REPLACE'] -ceq '1')
            observeInputsRequested=[bool]$ObserveInputs; explicitLayerRequested=[bool]($ObserveInputs -or $XeFG); officialLauncherStarted=$false
            renderTailDiagnosticRequested=[bool]$TraceRenderTail
            renderTailDiagnosticEnabledForChild=[bool]($XeFG -and $observerSettings['XESS_RENDER_TAIL_DIAGNOSTIC'] -ceq '1')
            renderTailAutomaticArm=$false
            wsiAcquireTraceRequested=[bool]$WsiAcquireTrace
            wsiAcquireTraceEnabledForChild=[bool]($XeFG -and $observerSettings['XESS_WSI_ACQUIRE_OBSERVER'] -ceq '1')
            threadCpuSamplingRequested=[bool]$ThreadCpuSampling
            threadCpuSamplingEnabledForChild=[bool]($XeFG -and $observerSettings['XESS_THREAD_CPU_SAMPLING'] -ceq '1')
            trackingCpuTimingRequested=[bool]$TrackingCpuTiming
            trackingCpuTimingEnabledForChild=[bool]($XeFG -and $observerSettings['XESS_TRACKING_CPU_TIMING'] -ceq '1')
            gpuTimestampsRequested=[bool]$GpuTimestamps
            gpuTimestampsEnabledForChild=[bool]($XeFG -and $observerSettings['XESS_GPU_TIMESTAMPS'] -ceq '1')
            fakeSwapchainRequested=[bool]$FakeSwapchain
            fakeSwapchainEnabledForChild=[bool]($XeFG -and $observerSettings['XESS_FAKE_SWAPCHAIN'] -ceq '1')
            xeFGLowResOutputRequested=[bool]$LowResOutput; xeFGUIModeRequested=$(if ($XeFG) { 'NONE' } else { $null })
            officialInstanceAlreadyRunning=[bool]$officialInstanceAlreadyRunning
            environmentCleanupScope='new_child_process_only'; existingOfficialInstanceEnvironmentVerified=$false
            rtssVulkanLayerDisableRequested=[bool]$XeFG
            launchPolicy=$specification.launchPolicy; gameCompatibility=$compatibility
            efuNoDitherRequested=[bool](-not $OfficialFallback -and -not $DisableNoDither); localLaunchSourceModRequested=[bool](-not $OfficialFallback)
            researchFeaturesApplied=$false; efuStarted=$false; efuNoDitherAvailable=$false; xeFGAvailable=$false
            streamlineTraceRequested=[bool]$TraceStreamline; runtimeStreamlineHooksRequested=[bool]$TraceStreamline
            bridgeDeviceRequirementsRequested=[bool]($BridgeDevice -or $XeFG); computeObserverRequested=[bool]$TraceCompute
            fsrCallbackObserverRequested=[bool]$TraceFsr; modifiedGameFiles=$false
            fsrUpscaleObserverRequested=[bool]($TraceFsrUpscale -or $ObserveInputs -or $XeFG)
            rawColorCaptureRequested=[bool]$CaptureRawColor
            presentationOffRequested=[bool]($PresentationOff -or $XeFG); presentationOffVerified=$false
            xeFGRequested=[bool]$XeFG; continuousInputsRequested=[bool]$XeFG; nativeFsrSuppressionRequested=[bool]$XeFG
            gameOnAdmitted=[bool]$XeFG; gameOnVerified=$false; gameVisualsVerified=$false; gameNormalExitVerified=$false
            gameProcessStartedVerified=$false; efuNoDitherVerified=$false; gameFrameInputsVerified=$false
            xeFGIntegrated=$false; generatedFramesVerified=$false
            note=$(if ($OfficialFallback) {
                'Explicit official fallback: opens the fixed signed official launcher with no arguments. Select Endfield and press Start in its window. Only the new entry environment is cleaned; an existing official instance environment is not inspected or changed. This route does not run EFU or enable no-dither, capture, the Vulkan observer or XeSS.'
            } elseif ($XeFG -and $TraceRenderTail) {
                'Explicit bounded render-tail diagnostics on the existing XeFG route. Keep the game frame-generation menu OFF for this capture. The child environment requests the diagnostic; startup does not arm or complete it. A separate scene marker from the observer waiting event is required. Existing independent launch, native-OFF graph, presentation and RTSS behavior remain unchanged.'
            } elseif ($XeFG -and $FakeSwapchain) {
                'Single-chain XeFG route selected by the frontend default or explicit FakeSwapchain helper switch. LocalFiOff is enabled by default on this route unless the DisableLocalFiOff file exists. Selected XeSS output, menu control, EFU and RTSS are retained. This record describes the request only; actual activation and per-frame fallback are reported by observer logs.'
            } elseif ($XeFG -and $WsiAcquireTrace) {
                'Explicit WSI acquire observation on the existing XeFG route. The child environment only requests acquire/status API diagnostics; observation does not change WSI behavior. Existing XeSS output selection, menu control, EFU and RTSS are retained. This launch record describes the request only; observer logs and scene operations are required to establish collected usage.'
            } elseif ($XeFG -and $LowResOutput) {
                'Experimental low-resolution output prototype on the explicit XeFG route, disabled by default. Downsamples the complete final Vulkan WSI image to a lower output canvas and keeps UI mode NONE, so UI may soften and still participates in interpolation. The game FSR3 frame-generation menu controls XeSS. This launch record describes the request and child environment only; continuous_present logs report actual low-resolution activation, source/output extents, region and fallback reason. Performance and power benefits remain unverified.'
            } elseif ($XeFG) {
                'Independent experimental XeSS 2x candidate for the fixed current profile: the game FSR3 frame-generation menu drives the request, with current upscale inputs, paired native FSR suppression, device bridge and the original window presentation route. FI/compute/Streamline observers and raw capture stay disabled. gameOnAdmitted identifies route admission, not actual ON: each frame still requires valid inputs, timing and paired gate suppression, otherwise XeSS stays OFF. Failed core admission retains the original game path and its native frame-generation settings. This candidate still requires game ON, visual and normal-exit validation; no verified generated XeSS frames or completed integration are claimed.'
            } elseif ($ObserveInputs) {
                'Independent input observation: the reviewed EFU starts with only the explicit Vulkan layer, current-version upscale observer and fixed session logs requested. Descriptor and inventory observers retain metadata for source/dispatch association. FI, compute tracing, Bridge device changes, presentation takeover, continuous inputs, copies and native FSR suppression are not requested. EFU no-dither is retained. Actual observer installation, frame inputs and visual effects remain unverified until scene diagnostics are reviewed.'
            } else {
                'Independent preview: starts the exact local eff32bbc-based camera-minhook-launcher-options-v3 build. CameraPitch uses the pinned private MinHook engine; the existing no-dither callback and upstream local OneChannel error-10100 compatibility behavior are retained. Bounded worker diagnostics record target resolution, hook installation and atomic callback counters; starting EFU does not verify those outcomes or the visual effect. Normal account login and anti-cheat are not replaced. Game files are locked only through EFU process creation, not guaranteed through its later game creation. No Vulkan observer, old FSR hook, capture, native-FSR suppression or XeSS path is activated. Initial injection remains asynchronous.'
            }) }
        $launchRecord=if($session){Join-Path $session 'launch.json'}else{$null}
        if($launchRecord){[IO.File]::WriteAllText($launchRecord, ($record | ConvertTo-Json -Depth 7), $utf8);Set-ProtectedAcl $launchRecord $false}
        if($session -and ($ReleaseSessionRetention -or ($ReleaseMinimal -and $DiagnosticMode))){
            try{
                Assert-PeDll $dllPath $true -RequireRetention
                $policy=Read-SessionRetentionPolicy
                $plan=Get-SessionRetentionPlan $sessionRoot $SessionId @($policy.captureRoots) -DiagnosticOnly:([bool]$ReleaseMinimal)
                [IO.File]::WriteAllText((Join-Path $session 'retention-plan.json'),($plan|ConvertTo-Json -Depth 10),$utf8)
                $result=if($policy.mode -ceq 'Auto'){Invoke-SessionRetention $plan}else{[ordered]@{readOnly=$true;mode='Preview';deleted=@();approvalRequired=$true}}
                [IO.File]::WriteAllText((Join-Path $session 'retention-result.json'),($result|ConvertTo-Json -Depth 10),$utf8)
            }catch{try{[IO.File]::WriteAllText((Join-Path $session 'retention-result.json'),([ordered]@{deleted=@();failed=$_.Exception.Message}|ConvertTo-Json),$utf8)}catch{}}
        }
        Assert-ModGameClosed
        $child = [Diagnostics.Process]::Start($start)
        if (-not $child) { throw 'Unable to create the reviewed launch process.' }
        $record['childProcessId'] = $child.Id; $record['launchStarted'] = $true
        $record['status'] = if ($OfficialFallback) { 'official_launcher_requested' } elseif ($XeFG) { 'efu_continuous_off_process_started' } elseif ($ObserveInputs) { 'efu_observer_process_started' } else { 'efu_process_started' }
        $record['officialLauncherStarted'] = [bool]$OfficialFallback; $record['efuStarted'] = [bool](-not $OfficialFallback)
        $record['childExitedAtRecord'] = $child.HasExited
        if ($child.HasExited) { $record['childExitCode'] = $child.ExitCode }
        if($launchRecord){[IO.File]::WriteAllText($launchRecord, ($record | ConvertTo-Json -Depth 7), $utf8)}
        $record | ConvertTo-Json -Depth 7
    } catch {
        if ($session -and $record) {
            $record['status'] = 'failed'; $record['error'] = $_.Exception.Message
            try { [IO.File]::WriteAllText((Join-Path $session 'launch.json'), ($record | ConvertTo-Json -Depth 7), $utf8) } catch { }
        }
        throw
    } finally {
        if ($child) { $child.Dispose() }
        if ($payloadLock) { $payloadLock.Dispose() }
        foreach ($stream in $gameLocks.Values) { $stream.Dispose() }
        if ($held) { $launchMutex.ReleaseMutex() }
        $launchMutex.Dispose()
    }
}
