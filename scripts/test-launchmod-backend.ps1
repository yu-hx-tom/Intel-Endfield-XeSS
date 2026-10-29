[CmdletBinding()]
param([string]$SourceDirectory, [string]$LauncherExecutable)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -cne 'Core' -or $PSVersionTable.PSVersion.Major -ne 7) { throw 'Backend tests require PowerShell 7 Core.' }
$jsonOptions = @{}
if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $jsonOptions.DateKind = 'String' }
$root = Split-Path -Parent $PSScriptRoot
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $root 'build\observer' }
if (-not $LauncherExecutable) { $LauncherExecutable = Join-Path $root 'build\EndfieldXeSSLauncher.exe' }
$management = Join-Path $PSScriptRoot 'manage-observer-install.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($management, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$checks = 0
$GpuTimestamps = $false
$TrackingCpuTiming = $false
$ThreadCpuSampling = $false
$Override30Fps = $false
$FrameCapTargetFps = 40
$FgDebugMode = 0
$FgSceneIndex = -1
$SrBackend = 'XeSS'
$fixedGameDirectory = Join-Path $root "out/path-fixture/launcher/games/Arknights Endfield"
$fixedOfficialLauncherPath = Join-Path (Split-Path -Parent (Split-Path -Parent $fixedGameDirectory)) "Launcher.exe"
function Read-GameDirectory { return $fixedGameDirectory }
function Check([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message }; $script:checks++ }
function Refuses([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Check $rejected $Message
}
# Load the three pure helpers, a process guard with a scoped fake process query,
# and literal pins. Never execute management
# Install, Launch, LaunchMod, Smoke, Uninstall, UAC or any child executable.
foreach ($name in @('baseOwnedNames','modOwnedNames','legacySdkOwnedNames','sdkOwnedNames','fsr4OwnedNames','fsr4XmxDllSha256','fsr4OriginalDllSha256','sdkVersion','xeFGDllSha256','xellDllSha256','intelLicenseSha256','xessSrDllSha256','efuSha256','previousPreviewEfuSha256','previousDitherEfuSha256','legacyEfuSha256','legacyEfuSourceCommit','efuLicenseSha256','efuSourceCommit','officialLauncherPublisher','previousUnitySha256','layerName')) {
    $assignment = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.VariablePath.UserPath -eq $name
    })
    Check ($assignment.Count -eq 1) "Missing literal pin: $name"
    $literals = @($assignment[0].Right.FindAll({ param($node) $node -is [Management.Automation.Language.StringConstantExpressionAst] }, $true))
    Check ($literals.Count -gt 0) "Pin is no longer literal text: $name"
    if ($name -in @('baseOwnedNames','modOwnedNames','legacySdkOwnedNames','sdkOwnedNames','fsr4OwnedNames')) { Set-Variable -Name $name -Value @($literals | ForEach-Object Value) }
    else { Check ($literals.Count -eq 1) "Unexpected pin expression: $name"; Set-Variable -Name $name -Value $literals[0].Value }
}
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'EndfieldXeSSObserver'
$efuPath = Join-Path $installRoot 'EFU.exe'
foreach ($name in @('Get-SdkFileMapping','Get-ReceiptOwnedNames','Assert-SingleChainPolicy','Get-ModLaunchSpecification','Normalize-GameDirectory','Set-ObserverChildEnvironment','Set-OfficialCompatibilityEnvironment','Set-InputObservationEnvironment','Set-ContinuousOffEnvironment','Set-NoDitherEnvironment','Assert-OfficialLauncherIdentity','Get-ModGameCompatibility','Get-GameSignature','Test-GameSignature','Assert-NoReparse','Assert-PeDll','Get-TrustedPowerShell7')) {
    $definition = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true))
    Check ($definition.Count -eq 1) "Missing pure helper: $name"
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
foreach($disabled in @($false,$true)){
    $preferenceChild=[Diagnostics.ProcessStartInfo]::new()
    Set-NoDitherEnvironment $preferenceChild $disabled
    Check ($preferenceChild.EnvironmentVariables['EFU_NO_DITHER'] -ceq $(if($disabled){'0'}else{'1'})) 'No-dither selection was not passed to the child.'
}
$trustedPwsh = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'PowerShell\7\pwsh.exe'
Check ((Get-TrustedPowerShell7) -ceq $trustedPwsh) 'Trusted PowerShell 7 version/signature check failed.'
& {
    function Get-Item { [pscustomobject]@{VersionInfo=[pscustomobject]@{FileMajorPart=5}} }
    Refuses { Get-TrustedPowerShell7 } 'Windows PowerShell version accepted by the trusted runtime check.'
}
$runtimeGate = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text.Contains('$PSVersionTable.PSEdition')
})
Check ($runtimeGate.Count -eq 1) 'Missing PowerShell 7 Core gate.'
# Execute only the actual pre-runtime policy, never a management action or UAC.
foreach ($timingRequested in @($false,$true)) { & {
    $Action='LaunchMod'; $XeFG=$true; $FakeSwapchain=$true; $XessSr=$true
    $GpuTimestamps=[switch]$timingRequested
    foreach ($statement in $ast.EndBlock.Statements) {
        if ($statement.Extent.StartOffset -ge $runtimeGate[0].Extent.StartOffset) { break }
        . ([scriptblock]::Create($statement.Extent.Text))
    }
    Check ([bool]$GpuTimestamps -eq $timingRequested) 'SR startup policy changed the explicit GPU timing choice.'
} }
$runtimeCondition = [scriptblock]::Create($runtimeGate[0].Clauses[0].Item1.Extent.Text.Replace('$PSVersionTable', '$fixtureVersion'))
foreach ($pair in @(@('Desktop',5,$true),@('Desktop',7,$true),@('Core',8,$true),@('Core',7,$false))) {
    $fixtureVersion = @{PSEdition=$pair[0];PSVersion=@{Major=$pair[1]}}
    Check ((& $runtimeCondition) -eq $pair[2]) 'Runtime gate admitted an unsupported PowerShell edition/version.'
}
$bootstrapDefinition = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$bootstrap' -and $node.Right.Extent.Text.Contains('Reviewed management script changed')},$true))
Check ($bootstrapDefinition.Count -eq 1 -and $bootstrapDefinition[0].Extent.Text.Contains('$env:PSModulePath = Join-Path $PSHOME ''Modules''') -and
    $bootstrapDefinition[0].Extent.Text.Contains('ConvertFrom-Json @managerJsonOptions')) 'Elevated bootstrap lost its child-only trusted modules or JSON date handling.'
$jsonCommands = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq 'ConvertFrom-Json'},$true))
foreach ($command in $jsonCommands) { Check ($command.Extent.Text.Contains('@managerJsonOptions')) 'Management JSON parser omitted scoped date preservation.' }
$dateText = '2026-10-19T10:54:01.1170127Z'
$dateRecord = ('{"installedAt":"' + $dateText + '"}') | ConvertFrom-Json @jsonOptions
Check ($dateRecord.installedAt -is [string] -and $dateRecord.installedAt -ceq $dateText) 'Receipt UTC text changed during JSON parsing.'
$parentRtss = $env:DISABLE_RTSS_LAYER
foreach ($inheritedRtss in @($null, '0', '1')) {
    foreach ($route in @('DefaultEFU', 'Official', 'ObserveInputs', 'XeFG')) {
        $rtssChild = [Diagnostics.ProcessStartInfo]::new()
        $rtssChild.EnvironmentVariables.Remove('DISABLE_RTSS_LAYER')
        if ($null -ne $inheritedRtss) { $rtssChild.EnvironmentVariables['DISABLE_RTSS_LAYER'] = $inheritedRtss }
        if ($route -ceq 'XeFG') { Set-ContinuousOffEnvironment $rtssChild 'C:\fixture-session' | Out-Null }
        elseif ($route -ceq 'ObserveInputs') { Set-InputObservationEnvironment $rtssChild 'C:\fixture-session' | Out-Null }
        else { Set-OfficialCompatibilityEnvironment $rtssChild }
        if ($route -ceq 'XeFG') {
            Check ($rtssChild.EnvironmentVariables['DISABLE_RTSS_LAYER'] -ceq '1') 'Only XeFG must request RTSS Vulkan layer isolation in its child.'
        } elseif ($null -eq $inheritedRtss) {
            Check (-not $rtssChild.EnvironmentVariables.ContainsKey('DISABLE_RTSS_LAYER')) "Route added an unsolicited RTSS setting: $route"
        } else {
            Check ($rtssChild.EnvironmentVariables['DISABLE_RTSS_LAYER'] -ceq $inheritedRtss) "Route overwrote the inherited RTSS value: $route"
        }
        Check ($env:DISABLE_RTSS_LAYER -ceq $parentRtss) 'Child RTSS isolation changed the parent environment.'
    }
}
$dummyHash = 'a' * 64
$legacy = [pscustomobject]@{ files=@($baseOwnedNames | ForEach-Object { [pscustomobject]@{name=$_;sha256=$dummyHash} }) }
Check (@(Get-ReceiptOwnedNames $legacy).Count -eq 4) 'Legacy four-file receipt no longer accepted.'
Assert-SingleChainPolicy ([pscustomobject]@{singleChainPolicyVersion=1})
Check $true 'New single-chain receipt accepts policy V1.'
Refuses { Assert-SingleChainPolicy $legacy } 'Old receipt without single-chain capability was accepted.'
foreach ($version in @($null,0,2,'1',$true)) {
    Refuses { Assert-SingleChainPolicy ([pscustomobject]@{singleChainPolicyVersion=$version}) } 'Changed or malformed single-chain capability was accepted.'
}

$modHashes = [pscustomobject]@{ launcherSha256=$dummyHash; efuSha256=$efuSha256; licenseSha256=$efuLicenseSha256; noticeSha256=$dummyHash }
$extension = [pscustomobject]@{ version=1; efuVersion='5.1.0'; efuSha256=$efuSha256; efuSourceCommit=$efuSourceCommit; sourceHashes=$modHashes }
$mapping = [ordered]@{ 'EndfieldXeSSLauncher.exe'='launcherSha256'; 'EFU.exe'='efuSha256'; 'EFU-LICENSE.txt'='licenseSha256'; 'EFU-SOURCE-NOTICE.txt'='noticeSha256' }
$mod = [pscustomobject]@{ files=@($legacy.files); modLauncher=$extension }
foreach ($name in $mapping.Keys) { $mod.files += [pscustomobject]@{name=$name;sha256=$modHashes.($mapping[$name])} }
Check (@(Get-ReceiptOwnedNames $mod).Count -eq 8) 'Reviewed eight-file receipt rejected.'
$legacyMod = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$legacyMod.modLauncher.efuSha256 = $legacyEfuSha256
$legacyMod.modLauncher.efuSourceCommit = $legacyEfuSourceCommit
$legacyMod.modLauncher.sourceHashes.efuSha256 = $legacyEfuSha256
($legacyMod.files | Where-Object name -CEQ 'EFU.exe').sha256 = $legacyEfuSha256
Check (@(Get-ReceiptOwnedNames $legacyMod).Count -eq 8) 'Legacy EFU receipt must remain readable for status and migration.'
$previousPreview = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$previousPreview.modLauncher.efuSha256 = $previousPreviewEfuSha256
$previousPreview.modLauncher.sourceHashes.efuSha256 = $previousPreviewEfuSha256
($previousPreview.files | Where-Object name -CEQ 'EFU.exe').sha256 = $previousPreviewEfuSha256
Check (@(Get-ReceiptOwnedNames $previousPreview).Count -eq 8) 'Previous unmodified preview receipt must remain readable for migration.'
$previousDither = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$previousDither.modLauncher.efuSha256 = $previousDitherEfuSha256
$previousDither.modLauncher.sourceHashes.efuSha256 = $previousDitherEfuSha256
($previousDither.files | Where-Object name -CEQ 'EFU.exe').sha256 = $previousDitherEfuSha256
Check (@(Get-ReceiptOwnedNames $previousDither).Count -eq 8) 'Previous dither candidate receipt must remain readable for removal.'
$mixedIdentity = $legacyMod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$mixedIdentity.modLauncher.efuSourceCommit = $efuSourceCommit
Refuses { Get-ReceiptOwnedNames $mixedIdentity } 'Mixed legacy payload/current source identity accepted.'
$invalid = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$invalid.files[4].name = '..\user-file.txt'
Refuses { Get-ReceiptOwnedNames $invalid } 'Traversal filename accepted.'
$invalid = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$invalid.files[4].name = 'EFU.exe'
Refuses { Get-ReceiptOwnedNames $invalid } 'Duplicate/missing filename accepted.'
$invalid = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$invalid.modLauncher.efuSha256 = $dummyHash
Refuses { Get-ReceiptOwnedNames $invalid } 'Changed official EFU hash accepted.'
$invalid = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$invalid.files[5].sha256 = $dummyHash
Refuses { Get-ReceiptOwnedNames $invalid } 'Payload/extension hash disagreement accepted.'
$invalid = [pscustomobject]@{ files=@($mod.files) }
Refuses { Get-ReceiptOwnedNames $invalid } 'Eight files without extension accepted.'
$sdkMapping = [ordered]@{ 'libxess_fg.dll'='xeFGDllSha256'; 'libxell.dll'='xellDllSha256'; 'LICENSE.txt'='intelLicenseSha256'; 'libxess.dll'='xessSrDllSha256' }
$sdkHashes = [pscustomobject]@{ xeFGDllSha256=$xeFGDllSha256; xellDllSha256=$xellDllSha256; intelLicenseSha256=$intelLicenseSha256; xessSrDllSha256=$xessSrDllSha256 }
$sdk = $mod | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$sdk | Add-Member -NotePropertyName xeFGSdk -NotePropertyValue ([pscustomobject]@{version=1;sdkVersion=$sdkVersion;sourceHashes=$sdkHashes})
foreach ($name in $sdkMapping.Keys) { $sdk.files += [pscustomobject]@{name=$name;sha256=$sdkHashes.($sdkMapping[$name])} }
Check (@(Get-ReceiptOwnedNames $sdk).Count -eq 12) 'Reviewed 12-file SDK receipt rejected.'
$fsr4Receipt=$sdk | ConvertTo-Json -Depth 10 | ConvertFrom-Json @jsonOptions
foreach($pair in @(@('amd_fidelityfx_upscaler_dx12.dll','fsr4XmxDllSha256',$fsr4XmxDllSha256),@('fsr-original.dll','fsr4OriginalDllSha256',$fsr4OriginalDllSha256))){
    $fsr4Receipt.xeFGSdk.sourceHashes | Add-Member -NotePropertyName $pair[1] -NotePropertyValue $pair[2]
    $fsr4Receipt.files += [pscustomobject]@{name=$pair[0];sha256=$pair[2]}
}
Check (@(Get-ReceiptOwnedNames $fsr4Receipt).Count -eq 14) 'FSR4 optional two-file receipt rejected.'
foreach($field in @('fsr4XmxDllSha256','fsr4OriginalDllSha256')){
    $invalid=$fsr4Receipt | ConvertTo-Json -Depth 10 | ConvertFrom-Json @jsonOptions
    $invalid.xeFGSdk.sourceHashes.$field=$dummyHash
    Refuses { Get-ReceiptOwnedNames $invalid } "Unpinned FSR4 hash accepted: $field"
}
$invalid=$fsr4Receipt | ConvertTo-Json -Depth 10 | ConvertFrom-Json @jsonOptions
$invalid.files=@($invalid.files | Where-Object name -cne 'fsr-original.dll')
Refuses { Get-ReceiptOwnedNames $invalid } 'FSR4 receipt accepted without weights DLL.'
foreach ($name in $sdkMapping.Keys) {
    $invalid = $sdk | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
    ($invalid.files | Where-Object name -CEQ $name).sha256 = $dummyHash
    Refuses { Get-ReceiptOwnedNames $invalid } "SDK payload/receipt disagreement accepted: $name"
}
foreach ($field in $sdkHashes.PSObject.Properties.Name) {
    $invalid = $sdk | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
    $invalid.xeFGSdk.sourceHashes.$field = $dummyHash
    Refuses { Get-ReceiptOwnedNames $invalid } "Unpinned SDK source hash accepted: $field"
}
$invalid = $sdk | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$invalid.xeFGSdk.sdkVersion = '3.0.1'
Refuses { Get-ReceiptOwnedNames $invalid } 'Unsupported SDK version accepted.'
$invalid = $sdk | ConvertTo-Json -Depth 8 | ConvertFrom-Json @jsonOptions
$invalid.files[8].name = '..\libxess_fg.dll'
Refuses { Get-ReceiptOwnedNames $invalid } 'SDK traversal name accepted.'
$invalid = [pscustomobject]@{files=@($sdk.files);xeFGSdk=$sdk.xeFGSdk}
Refuses { Get-ReceiptOwnedNames $invalid } 'SDK extension without standalone launcher accepted.'
$dependency = (Get-Content -LiteralPath (Join-Path $root 'dependencies.lock.json') -Raw | ConvertFrom-Json @jsonOptions).dependencies | Where-Object name -CEQ 'xess'
Check ($dependency.version -ceq $sdkVersion) 'Manager SDK version drifted from dependency lock.'
foreach ($pair in @(@('bin/libxess_fg.dll',$xeFGDllSha256),@('bin/libxell.dll',$xellDllSha256),@('LICENSE.txt',$intelLicenseSha256),@('bin/libxess.dll',$xessSrDllSha256))) {
    Check (($dependency.criticalFiles | Where-Object path -CEQ $pair[0]).sha256 -ceq $pair[1]) "SDK pin drifted from dependency lock: $($pair[0])"
}
$spec = Get-ModLaunchSpecification $fixedGameDirectory
Check ($spec.executable -ceq $efuPath) 'Independent default must start the protected EFU.'
Check ($spec.workingDirectory -ceq $fixedGameDirectory) 'Unexpected child working directory.'
Check ($spec.arguments -ceq ('--silent --gp "' + $fixedGameDirectory + '\Endfield.exe" --EFMI false')) 'Exact independent direct-launch arguments changed.'
Check ($spec.launchPolicy -ceq 'independent_efu_preview' -and -not $spec.officialGameStartRequired -and -not $spec.researchFeaturesAvailable) 'Independent launch must not require the official UI or enable old graphics research.'
$officialSpec = Get-ModLaunchSpecification $fixedGameDirectory $true
Check ($officialSpec.executable -ceq $fixedOfficialLauncherPath -and $officialSpec.arguments -ceq '' -and $officialSpec.officialGameStartRequired) 'Official fallback must remain explicit and use empty arguments.'
$observationSpec = Get-ModLaunchSpecification $fixedGameDirectory $false $true
Check ($observationSpec.executable -ceq $spec.executable -and $observationSpec.arguments -ceq $spec.arguments -and
    $observationSpec.launchPolicy -ceq 'independent_efu_observe_inputs' -and -not $observationSpec.researchFeaturesAvailable) 'Observation must preserve exact EFU arguments and identify its distinct policy without claiming input readiness.'
Refuses { Get-ModLaunchSpecification $fixedGameDirectory $true $true } 'Observation accepted official fallback.'
$continuousSpec = Get-ModLaunchSpecification $fixedGameDirectory $false $false $true
Check ($continuousSpec.executable -ceq $spec.executable -and $continuousSpec.arguments -ceq $spec.arguments -and
    $continuousSpec.launchPolicy -ceq 'independent_efu_continuous_off') 'Continuous OFF must keep exact EFU launch arguments and its distinct validation policy.'
Refuses { Get-ModLaunchSpecification $fixedGameDirectory $true $false $true } 'Continuous OFF accepted official fallback.'
Refuses { Get-ModLaunchSpecification $fixedGameDirectory $false $true $true } 'Continuous OFF mixed pure observation.'
Check (-not $spec.arguments.Contains('DX11')) 'DX11 argument appeared.'
Check ((Get-ModLaunchSpecification 'D:\other-game').gamePath -ceq 'D:\other-game\Endfield.exe') 'Selected local path must be used literally; compatibility is checked before launch.'
Refuses { Get-ModLaunchSpecification '\\server\share\game' } 'Network game directory accepted.'
Refuses { Get-ModLaunchSpecification 'relative\game' } 'Relative game directory accepted.'
Refuses { Get-ModLaunchSpecification ($fixedGameDirectory + '" --DX11') } 'Argument injection path accepted.'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.UseShellExecute = $false
foreach ($name in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_LOADER_LAYERS_ENABLE','XESS_SL_OBSERVER','XESS_BRIDGE_DEVICE','XESS_COMPUTE_OBSERVER','XESS_FSR_OBSERVER','XESS_FSR_UPSCALE_OBSERVER','XESS_RAWCOLOR_CAPTURE','XESS_PRESENTATION_OFF','XESS_CONTINUOUS_INPUTS','XESS_FSR_NATIVE_OFF','XESS_SDK_BIN','XESS_XEFG_LOW_RES_OUTPUT','XESS_WSI_ACQUIRE_OBSERVER','XESS_FAKE_SWAPCHAIN','XESS_GPU_TIMESTAMPS','XESS_TRACKING_CPU_TIMING')) {
    $start.EnvironmentVariables[$name] = 'inherited-unwanted'
}
$parentFsr = $env:XESS_FSR_OBSERVER
$parentRaw = $env:XESS_RAWCOLOR_CAPTURE
$parentContinuous = $env:XESS_CONTINUOUS_INPUTS
$parentNativeOff = $env:XESS_FSR_NATIVE_OFF
$parentLowResOutput = $env:XESS_XEFG_LOW_RES_OUTPUT
$parentAcquireTrace = $env:XESS_WSI_ACQUIRE_OBSERVER
$parentFakeSwapchain = $env:XESS_FAKE_SWAPCHAIN
$parentGpuTimestamps = $env:XESS_GPU_TIMESTAMPS
$parentTrackingCpuTiming = $env:XESS_TRACKING_CPU_TIMING
Set-ObserverChildEnvironment $start 'C:\fixture-session'
foreach ($name in @('VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_LOADER_LAYERS_ENABLE','XESS_SL_OBSERVER','XESS_BRIDGE_DEVICE','XESS_COMPUTE_OBSERVER','XESS_FSR_OBSERVER','XESS_FSR_UPSCALE_OBSERVER','XESS_RAWCOLOR_CAPTURE','XESS_PRESENTATION_OFF','XESS_CONTINUOUS_INPUTS','XESS_FSR_NATIVE_OFF','XESS_SDK_BIN','XESS_XEFG_LOW_RES_OUTPUT','XESS_WSI_ACQUIRE_OBSERVER','XESS_FAKE_SWAPCHAIN','XESS_GPU_TIMESTAMPS','XESS_TRACKING_CPU_TIMING')) {
    Check (-not $start.EnvironmentVariables.ContainsKey($name)) "Inherited override leaked into child: $name"
}
Check ($start.EnvironmentVariables['VK_INSTANCE_LAYERS'] -ceq $layerName) 'Wrong explicit layer.'
Check ($env:XESS_FSR_OBSERVER -ceq $parentFsr) 'Parent environment changed.'
Check ($env:XESS_RAWCOLOR_CAPTURE -ceq $parentRaw) 'Parent rawcolor environment changed.'
Check ($env:XESS_CONTINUOUS_INPUTS -ceq $parentContinuous -and $env:XESS_FSR_NATIVE_OFF -ceq $parentNativeOff) 'Parent XeFG environment changed.'
Check ($env:XESS_XEFG_LOW_RES_OUTPUT -ceq $parentLowResOutput) 'Parent low-resolution environment changed.'
Check ($env:XESS_WSI_ACQUIRE_OBSERVER -ceq $parentAcquireTrace) 'Parent acquire observation environment changed.'
Check ($env:XESS_FAKE_SWAPCHAIN -ceq $parentFakeSwapchain) 'Parent fake swapchain environment changed.'
Check ($env:XESS_GPU_TIMESTAMPS -ceq $parentGpuTimestamps) 'Parent GPU timestamp environment changed.'
Check ($env:XESS_TRACKING_CPU_TIMING -ceq $parentTrackingCpuTiming) 'Parent tracking timing environment changed.'
Refuses { & $management -Action Status -TraceFsrUpscale } 'Unpaired eight-input observer option accepted.'
foreach($invalidAction in @('Status','ReviewPlan','Install')){
    Refuses { & $management -Action $invalidAction -DisableNoDither } 'No-dither launch option accepted outside LaunchMod.'
}
Refuses { & $management -Action LaunchMod -OfficialFallback -DisableNoDither } 'Official route accepted a mod preference.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -CaptureRawColor -TraceFsr -TraceFsrUpscale } "Rawcolor option accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -CaptureRawColor -TraceFsr } 'Rawcolor accepted without TraceFsrUpscale.'
Refuses { & $management -Action LaunchMod -CaptureRawColor -TraceFsrUpscale } 'Rawcolor accepted without TraceFsr.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -PresentationOff -BridgeDevice } "OFF option accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -PresentationOff } 'OFF accepted without BridgeDevice.'
Refuses { & $management -Action LaunchMod -PresentationOff -BridgeDevice -CaptureRawColor -TraceFsr -TraceFsrUpscale } 'OFF/rawcapture conflict accepted.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG } "XeFG accepted outside LaunchMod: $actionName"
}
foreach ($switchName in @('OfficialFallback','ObserveInputs','BridgeDevice','TraceFsr','TraceFsrUpscale','TraceCompute','TraceStreamline','PresentationOff','CaptureRawColor')) {
    $invalidXeFG = @{Action='LaunchMod';XeFG=$true}
    $invalidXeFG[$switchName] = $true
    Refuses { & $management @invalidXeFG } "Continuous OFF mixed another switch: $switchName"
}
Refuses { & $management -Action LaunchMod -XeFG -PresentationOff -BridgeDevice -TraceFsr -TraceFsrUpscale -CaptureRawColor } 'XeFG/rawcapture conflict accepted.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG -TraceRenderTail } "Render-tail accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -TraceRenderTail } 'Render-tail accepted without XeFG.'
Refuses { & $management -Action LaunchMod -ObserveInputs -TraceRenderTail } 'Render-tail accepted with input-only observation.'
Refuses { & $management -Action LaunchMod -OfficialFallback -TraceRenderTail } 'Render-tail accepted with official handoff.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG -WsiAcquireTrace } "Acquire observation accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -WsiAcquireTrace } 'Acquire observation accepted without XeFG.'
Refuses { & $management -Action LaunchMod -ObserveInputs -WsiAcquireTrace } 'Acquire observation accepted with input-only observation.'
Refuses { & $management -Action LaunchMod -OfficialFallback -WsiAcquireTrace } 'Acquire observation accepted with official handoff.'
Refuses { & $management -Action LaunchMod -XeFG -TraceRenderTail -WsiAcquireTrace } 'Acquire observation accepted with render-tail.'
Refuses { & $management -Action LaunchMod -WsiAcquireTrace -TraceRenderTail -XeFG } 'Reordered acquire/render-tail conflict accepted.'
Refuses { Set-ContinuousOffEnvironment ([Diagnostics.ProcessStartInfo]::new()) 'C:\fixture-session' $true $false $true } 'Pure child helper accepted acquire/render-tail conflict.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG -FakeSwapchain } "Fake swapchain accepted outside LaunchMod: $actionName"
}
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG -GpuTimestamps } "GPU timestamps accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -GpuTimestamps } 'GPU timestamps accepted without XeFG.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG -TrackingCpuTiming } "Tracking timing accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -TrackingCpuTiming } 'Tracking timing accepted without XeFG.'
Refuses { & $management -Action LaunchMod -ThreadCpuSampling } 'Thread sampling accepted without XeFG.'
Refuses { & $management -Action Status -XeFG -ThreadCpuSampling } 'Thread sampling accepted outside LaunchMod.'
Refuses { & $management -Action LaunchMod -FakeSwapchain } 'Fake swapchain accepted without XeFG.'
Refuses { & $management -Action LaunchMod -XeFG -FakeSwapchain -WsiAcquireTrace } 'Fake swapchain accepted with acquire observation.'
Refuses { & $management -Action LaunchMod -XeFG -TraceRenderTail -FakeSwapchain } 'Fake swapchain accepted with render-tail.'
Refuses { Set-ContinuousOffEnvironment ([Diagnostics.ProcessStartInfo]::new()) 'C:\fixture-session' $false $false $true $true } 'Pure child helper mixed fake swapchain and acquire observation.'
Refuses { Set-ContinuousOffEnvironment ([Diagnostics.ProcessStartInfo]::new()) 'C:\fixture-session' $true $false $false $true } 'Pure child helper mixed fake swapchain and render-tail.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -XeFG -LowResOutput } "Low-resolution accepted outside LaunchMod: $actionName"
}
Refuses { & $management -Action LaunchMod -LowResOutput } 'Low-resolution accepted without XeFG.'
Refuses { & $management -Action LaunchMod -ObserveInputs -LowResOutput } 'Low-resolution accepted with input-only observation.'
Refuses { & $management -Action LaunchMod -OfficialFallback -LowResOutput } 'Low-resolution accepted with official handoff.'
Refuses { & $management -Action LaunchMod -XeFG -LowResOutput -TraceRenderTail } 'Low-resolution/render-tail conflict accepted.'
Refuses { & $management -Action LaunchMod -LowResOutput -TraceRenderTail -XeFG } 'Reordered low-resolution/render-tail conflict accepted.'
Refuses { Set-ContinuousOffEnvironment ([Diagnostics.ProcessStartInfo]::new()) 'C:\fixture-session' $true $true } 'Pure child helper accepted both modifiers.'
Refuses { & $management -Action ReviewPlan -WithXeFGSdk } 'SDK package accepted without standalone launcher.'
Refuses { & $management -Action Status -WithXeFGSdk -WithModLauncher } 'SDK package selection accepted outside review/install.'
foreach ($actionName in @('ReviewPlan','Install','Status','Launch','Smoke','Uninstall')) {
    Refuses { & $management -Action $actionName -ObserveInputs } "Input observation accepted outside LaunchMod: $actionName"
}
foreach ($switchName in @('OfficialFallback','TraceStreamline','BridgeDevice','TraceCompute','TraceFsr','TraceFsrUpscale','CaptureRawColor','PresentationOff','XeFG')) {
    $mixedObservation = @{Action='LaunchMod';ObserveInputs=$true}
    $mixedObservation[$switchName] = $true
    Refuses { & $management @mixedObservation } "Input observation accepted mixed switch: $switchName"
}

# Exercise production UAC serialization, environment cleanup and the launch
# record in isolation. No process start, UAC or management mutation is executed.
$parameterAssignment = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$parameters' -and
    $node.Right.Extent.Text.Contains('TraceFsrUpscale') -and $node.Right.Extent.Text.Contains('CaptureRawColor')
},$true))
Check ($parameterAssignment.Count -eq 1) 'Missing structured UAC parameter construction.'
$launchModBranch = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -ceq '$Action -eq ''LaunchMod'''
})
Check ($launchModBranch.Count -eq 1) 'Missing LaunchMod branch.'
$recordAssignment = @($launchModBranch[0].FindAll({param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$record' -and
    $node.Right.Extent.Text.Contains('rawColorCaptureRequested')
},$true))
Check ($recordAssignment.Count -eq 1) 'Missing explicit compatibility record.'
$launchCommands = @($launchModBranch[0].FindAll({param($node) $node -is [Management.Automation.Language.CommandAst]},$true) | ForEach-Object { $_.GetCommandName() })
Check ($launchCommands -contains 'Set-OfficialCompatibilityEnvironment') 'LaunchMod must clean the official child environment.'
Check ($launchCommands -notcontains 'Set-ObserverChildEnvironment') 'LaunchMod must never activate the observer environment.'
Check ($launchCommands -contains 'Set-InputObservationEnvironment') 'LaunchMod omitted its explicit input-only observer route.'
$observationBranch = @($launchModBranch[0].FindAll({param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text -ceq '$ObserveInputs' -and
    $node.Clauses[0].Item2.Extent.Text.Contains('Set-InputObservationEnvironment')
},$true))
Check ($observationBranch.Count -eq 1) 'Input-only observer activation must require the explicit ObserveInputs switch.'
Check ($launchCommands -contains 'Set-ContinuousOffEnvironment' -and
    $observationBranch[0].Extent.Text.Contains('elseif ($XeFG)')) 'Continuous OFF must require its separate explicit XeFG branch.'
Check ($launchModBranch[0].Extent.Text.Contains('$XeFG -and $null -eq $receipt.xeFGSdk')) 'Continuous OFF must require the owned SDK receipt before child creation.'
Check ($launchCommands -contains 'Assert-OfficialLauncherIdentity') 'LaunchMod omitted official publisher validation.'
Check ($launchModBranch[0].Extent.Text.Contains('$receipt.modLauncher.efuSourceCommit -cne $efuSourceCommit')) 'Independent launch omitted the current EFU identity guard.'
Check ($launchModBranch[0].Extent.Text.Contains('Get-ModGameCompatibility $GameDirectory $gameLocks')) 'Independent game identities must stay locked through Process.Start.'
$environmentWrites = @($launchModBranch[0].FindAll({param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -like '*EnvironmentVariables*'
},$true))
Check ($environmentWrites.Count -eq 1 -and $environmentWrites[0].Left.Extent.Text -ceq '$start.EnvironmentVariables[''EFU_RUNTIME_LOG'']') 'LaunchMod may set only its fixed runtime diagnostic log, never old graphics variables.'
$runtimeLogAssignment = @($launchModBranch[0].FindAll({param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$runtimeLog'
},$true))
Check ($runtimeLogAssignment.Count -eq 1) 'Missing protected per-session runtime log selection.'
$officialSignature = [pscustomobject]@{Status='Valid'; SignerCertificate=[pscustomobject]@{Subject=$officialLauncherPublisher}}
Assert-OfficialLauncherIdentity $fixedOfficialLauncherPath $officialSignature
Check (Test-GameSignature $officialSignature) 'Expected game publisher refused.'
Check (Test-GameSignature ([pscustomobject]@{Status='NotSigned'})) 'Unsigned game should remain eligible for strict hashes.'
Check (-not (Test-GameSignature ([pscustomobject]@{Status='HashMismatch';SignerCertificate=$officialSignature.SignerCertificate}))) 'Invalid signature accepted.'
Check (-not (Test-GameSignature ([pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=unrelated'}}))) 'Wrong game publisher accepted.'
Assert-OfficialLauncherIdentity 'D:\other\Launcher.exe' $officialSignature
Refuses { Assert-OfficialLauncherIdentity 'D:\other\other.exe' $officialSignature } 'Wrong launcher basename accepted.'
Refuses { Assert-OfficialLauncherIdentity $fixedOfficialLauncherPath ([pscustomobject]@{Status='NotSigned';SignerCertificate=$officialSignature.SignerCertificate}) } 'Unsigned official launcher accepted.'
Refuses { Assert-OfficialLauncherIdentity $fixedOfficialLauncherPath ([pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Subject='CN=unrelated publisher'}}) } 'Unrelated publisher accepted.'
Refuses { Assert-OfficialLauncherIdentity $fixedOfficialLauncherPath ([pscustomobject]@{Status='Valid';SignerCertificate=$null}) } 'Missing signer certificate accepted.'
foreach ($requested in @($false,$true)) {
  foreach ($officialRequested in @($false,$true)) {
    & {
        $Action='LaunchMod'; $CaptureRawColor=$requested; $PresentationOff=$requested; $XeFG=$false
        $TraceFsr=$requested; $TraceFsrUpscale=$requested; $TraceStreamline=$requested; $BridgeDevice=$requested; $TraceCompute=$requested
        $OfficialFallback=$officialRequested; $ObserveInputs=$false; $TraceRenderTail=$false; $WsiAcquireTrace=$false; $FakeSwapchain=$false; $GpuTimestamps=$false; $TrackingCpuTiming=$false; $LowResOutput=$false
        $LauncherPath=$fixedOfficialLauncherPath; $GameDirectory=$fixedGameDirectory; $SessionId='0123456789abcdef0123456789abcdef'
        . ([scriptblock]::Create($parameterAssignment[0].Extent.Text))
        $decoded=($parameters|ConvertTo-Json -Compress)| ConvertFrom-Json @jsonOptions
        foreach ($field in @('CaptureRawColor','PresentationOff')) {
            Check ($decoded.$field -is [bool] -and $decoded.$field -eq $requested) "Structured UAC lost typed request: $field"
        }
        Check ($decoded.XeFG -is [bool] -and -not $decoded.XeFG) 'Legacy request must not select continuous OFF.'
        Check ($decoded.OfficialFallback -is [bool] -and $decoded.OfficialFallback -eq $officialRequested) 'Structured UAC lost explicit official fallback selection.'
        Check ($decoded.ObserveInputs -is [bool] -and -not $decoded.ObserveInputs) 'Default UAC request accidentally selected input observation.'
        Check ($decoded.TraceRenderTail -is [bool] -and -not $decoded.TraceRenderTail) 'Default UAC request accidentally enabled render-tail.'
        Check ($decoded.WsiAcquireTrace -is [bool] -and -not $decoded.WsiAcquireTrace) 'Default UAC request accidentally enabled acquire observation.'
        Check ($decoded.GpuTimestamps -is [bool] -and $decoded.GpuTimestamps -eq $GpuTimestamps) 'Structured UAC lost GPU timestamp request.'
    Check ($decoded.TrackingCpuTiming -is [bool] -and $decoded.TrackingCpuTiming -eq $TrackingCpuTiming) 'Structured UAC lost tracking timing request.'
    Check ($decoded.ThreadCpuSampling -is [bool] -and $decoded.ThreadCpuSampling -eq $ThreadCpuSampling) 'Structured UAC lost thread sampling request.'
    Check ($decoded.FakeSwapchain -is [bool] -and -not $decoded.FakeSwapchain) 'Default UAC request accidentally enabled fake swapchain.'
        Check ($decoded.LowResOutput -is [bool] -and -not $decoded.LowResOutput) 'Default UAC request accidentally enabled low-resolution output.'
        $childEnvironment=[Diagnostics.ProcessStartInfo]::new(); $childEnvironment.UseShellExecute=$false
        $remove=@('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_ADD_IMPLICIT_LAYER_PATH',
            'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG',
            'XESS_FSR_OBSERVER','XESS_BRIDGE_DEVICE','XESS_UNKNOWN_FUTURE_OPTION','XESS_RAWCOLOR_CAPTURE',
            'XESS_CONTINUOUS_INPUTS','XESS_FSR_NATIVE_OFF','XESS_SDK_BIN','XESS_RENDER_TAIL_DIAGNOSTIC','XESS_XEFG_LOW_RES_OUTPUT','XESS_WSI_ACQUIRE_OBSERVER','XESS_FAKE_SWAPCHAIN','XESS_GPU_TIMESTAMPS','XESS_TRACKING_CPU_TIMING','SL_LOG_PATH','SL_LOG_LEVEL','SL_UNKNOWN_FUTURE_OPTION','EFU_RUNTIME_LOG')
        foreach ($field in $remove) { $childEnvironment.EnvironmentVariables[$field]='inherited-unwanted' }
        $childEnvironment.EnvironmentVariables['END_FIELD_TEST_PRESERVED']='keep'
        $parentSnapshot=$env:XESS_FSR_OBSERVER
        Set-OfficialCompatibilityEnvironment $childEnvironment
        foreach ($field in $remove) { Check (-not $childEnvironment.EnvironmentVariables.ContainsKey($field)) "Official child inherited stale research override: $field" }
        Check ($childEnvironment.EnvironmentVariables['END_FIELD_TEST_PRESERVED'] -ceq 'keep') 'Unrelated child environment was removed.'
        Check ($env:XESS_FSR_OBSERVER -ceq $parentSnapshot) 'Official cleanup changed the parent environment.'
        $session='C:\fixture-session'; $admin=$false; $specification=Get-ModLaunchSpecification $fixedGameDirectory $officialRequested
        $lockedPayloadHash=$dummyHash; $launcherSigner=if($officialRequested){$officialLauncherPublisher}else{$null}
        . ([scriptblock]::Create($runtimeLogAssignment[0].Extent.Text))
        $officialInstanceAlreadyRunning=$requested; $compatibility=[ordered]@{researchFeaturesAvailable=$false;state='unreviewed_game_build'}
        $start=$childEnvironment; if(-not $OfficialFallback){Set-NoDitherEnvironment $childEnvironment ([bool]$DisableNoDither)}
        . ([scriptblock]::Create($recordAssignment[0].Extent.Text))
        $roundtrip=($record|ConvertTo-Json -Depth 7)| ConvertFrom-Json @jsonOptions
        foreach ($field in @('renderTailDiagnosticRequested','renderTailDiagnosticEnabledForChild','renderTailAutomaticArm')) {
            Check ($roundtrip.$field -is [bool] -and -not $roundtrip.$field) "Default record falsely enabled or armed render-tail: $field"
        }
        foreach ($field in @('wsiAcquireTraceRequested','wsiAcquireTraceEnabledForChild')) {
            Check ($roundtrip.$field -is [bool] -and -not $roundtrip.$field) "Default record falsely enabled acquire observation: $field"
        }
        foreach ($field in @('fakeSwapchainRequested','fakeSwapchainEnabledForChild')) {
            Check ($roundtrip.$field -is [bool] -and -not $roundtrip.$field) "Default record falsely enabled fake swapchain: $field"
        }
        Check ($roundtrip.xeFGLowResOutputRequested -is [bool] -and -not $roundtrip.xeFGLowResOutputRequested -and
            $null -eq $roundtrip.xeFGUIModeRequested) 'Legacy/default record falsely requested low-resolution output or XeFG UI mode.'
        foreach ($field in @('rawColorCaptureRequested','presentationOffRequested')) {
            Check ($roundtrip.$field -is [bool] -and $roundtrip.$field -eq $requested) "Launch record lost explicit experiment request: $field"
        }
        foreach ($field in @('researchFeaturesApplied','explicitLayerRequested','efuStarted','efuNoDitherAvailable','xeFGAvailable','xeFGRequested',
            'gameOnAdmitted','rtssVulkanLayerDisableRequested','gameOnVerified','gameVisualsVerified','gameNormalExitVerified',
            'gameProcessStartedVerified','efuNoDitherVerified','gameFrameInputsVerified','xeFGIntegrated','generatedFramesVerified',
            'existingOfficialInstanceEnvironmentVerified')) {
            Check ($roundtrip.$field -is [bool] -and -not $roundtrip.$field) "Official request falsely claimed applied/verified state: $field"
        }
        Check ($roundtrip.backend -ceq $specification.backend -and $roundtrip.launchPolicy -ceq $specification.launchPolicy) 'Record lost the selected launch mode.'
        Check ($roundtrip.childArguments -ceq $specification.arguments -and $roundtrip.childExecutable -ceq $specification.executable) 'Record differs from selected launch specification.'
        Check ($roundtrip.efuNoDitherRequested -eq (-not $officialRequested) -and $roundtrip.localLaunchSourceModRequested -eq (-not $officialRequested)) 'Independent preview must record local no-dither/source-mod requests without claiming success.'
        if ($officialRequested) {
            Check (@($roundtrip.environmentForChildOnly.PSObject.Properties).Count -eq 0) 'Official fallback activated an EFU environment.'
        } else {
            Check (@($roundtrip.environmentForChildOnly.PSObject.Properties).Count -eq 2 -and $roundtrip.environmentForChildOnly.EFU_NO_DITHER -ceq '1' -and $roundtrip.environmentForChildOnly.EFU_RUNTIME_LOG -ceq (Join-Path $session 'efu-runtime.jsonl')) 'EFU diagnostic path must be the fixed new session log.'
        }
        Check ($roundtrip.officialInstanceAlreadyRunning -eq $requested -and $roundtrip.environmentCleanupScope -ceq 'new_child_process_only') 'Existing instance environment limitation was lost.'
    }
  }
}

foreach ($route in @('ObserveInputs','XeFG','TraceRenderTail','LowResOutput','WsiAcquireTrace','WsiAcquireTraceLowRes','FakeSwapchain','FakeSwapchainLowRes','GpuTimestampsNative','GpuTimestampsFake','TrackingCpuNative','TrackingCpuFake','TrackingCpuGpuNative','TrackingCpuGpuFake','ThreadCpuFake')) { & {
    $continuousOff=$route -cne 'ObserveInputs'; $TraceRenderTail=$route -ceq 'TraceRenderTail'
    $WsiAcquireTrace=$route -in @('WsiAcquireTrace','WsiAcquireTraceLowRes'); $LowResOutput=$route -in @('LowResOutput','WsiAcquireTraceLowRes','FakeSwapchainLowRes','GpuTimestampsNative','GpuTimestampsFake')
    $FakeSwapchain=$route -in @('FakeSwapchain','FakeSwapchainLowRes','GpuTimestampsFake','TrackingCpuFake','TrackingCpuGpuFake','ThreadCpuFake')
    $GpuTimestamps=$route -in @('GpuTimestampsNative','GpuTimestampsFake','TrackingCpuGpuNative','TrackingCpuGpuFake')
    $ThreadCpuSampling=$route -eq 'ThreadCpuFake'
    $TrackingCpuTiming=$route -in @('TrackingCpuNative','TrackingCpuFake','TrackingCpuGpuNative','TrackingCpuGpuFake')
    $Action='LaunchMod'; $ObserveInputs=-not $continuousOff; $OfficialFallback=$false
    $TraceStreamline=$false; $BridgeDevice=$false; $TraceCompute=$false; $TraceFsr=$false; $TraceFsrUpscale=$false
    $CaptureRawColor=$false; $PresentationOff=$false; $XeFG=$continuousOff
    $LauncherPath=$fixedOfficialLauncherPath; $GameDirectory=$fixedGameDirectory; $SessionId='0123456789abcdef0123456789abcdef'
    . ([scriptblock]::Create($parameterAssignment[0].Extent.Text))
    $decoded=($parameters|ConvertTo-Json -Compress)| ConvertFrom-Json @jsonOptions
    Check ($decoded.ObserveInputs -is [bool] -and $decoded.ObserveInputs -eq (-not $continuousOff) -and
        $decoded.XeFG -is [bool] -and $decoded.XeFG -eq $continuousOff) 'Structured UAC lost the separate observation/continuous OFF selection.'
    Check ($decoded.TraceRenderTail -is [bool] -and $decoded.TraceRenderTail -eq $TraceRenderTail) 'Structured UAC lost the typed render-tail modifier.'
    Check ($decoded.WsiAcquireTrace -is [bool] -and $decoded.WsiAcquireTrace -eq $WsiAcquireTrace) 'Structured UAC lost the typed acquire observer modifier.'
    Check ($decoded.GpuTimestamps -is [bool] -and $decoded.GpuTimestamps -eq $GpuTimestamps) 'Structured UAC lost GPU timestamp request.'
    Check ($decoded.TrackingCpuTiming -is [bool] -and $decoded.TrackingCpuTiming -eq $TrackingCpuTiming) 'Structured UAC lost tracking timing request.'
    Check ($decoded.ThreadCpuSampling -is [bool] -and $decoded.ThreadCpuSampling -eq $ThreadCpuSampling) 'Structured UAC lost thread sampling request.'
    Check ($decoded.FakeSwapchain -is [bool] -and $decoded.FakeSwapchain -eq $FakeSwapchain) 'Structured UAC lost the typed fake swapchain modifier.'
    Check ($decoded.LowResOutput -is [bool] -and $decoded.LowResOutput -eq $LowResOutput) 'Structured UAC lost the typed low-resolution modifier.'
    $childEnvironment=[Diagnostics.ProcessStartInfo]::new(); $childEnvironment.UseShellExecute=$false
    $remove=@('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','VK_ADD_IMPLICIT_LAYER_PATH',
        'VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_LAYERS_ALLOW','VK_LOADER_DEBUG',
        'XESS_UNKNOWN_FUTURE_OPTION','XESS_SL_OBSERVER','XESS_FSR_OBSERVER','XESS_COMPUTE_OBSERVER','XESS_BRIDGE_DEVICE',
        'XESS_RAWCOLOR_CAPTURE','XESS_PRESENTATION_OFF','XESS_CONTINUOUS_INPUTS','XESS_FSR_NATIVE_OFF','XESS_SDK_BIN','XESS_RENDER_TAIL_DIAGNOSTIC','XESS_XEFG_LOW_RES_OUTPUT','XESS_WSI_ACQUIRE_OBSERVER','XESS_FAKE_SWAPCHAIN','XESS_GPU_TIMESTAMPS','XESS_TRACKING_CPU_TIMING',
        'SL_LOG_PATH','SL_UNKNOWN_FUTURE_OPTION','EFU_RUNTIME_LOG')
    foreach ($field in $remove) { $childEnvironment.EnvironmentVariables[$field]='inherited-unwanted' }
    $childEnvironment.EnvironmentVariables['END_FIELD_TEST_PRESERVED']='keep'
    $parentSnapshot=$env:XESS_FSR_UPSCALE_OBSERVER
    $session='C:\fixture-session'
    $observerSettings=if ($continuousOff) { Set-ContinuousOffEnvironment $childEnvironment $session $TraceRenderTail $LowResOutput $WsiAcquireTrace $FakeSwapchain $GpuTimestamps $TrackingCpuTiming $false $ThreadCpuSampling } else { Set-InputObservationEnvironment $childEnvironment $session }
    $expected=[ordered]@{VK_INSTANCE_LAYERS=$layerName;VK_LOADER_DEBUG='error,warn,layer';XESS_DIAGNOSTIC_MODE='1';
        XESS_OBSERVER_LOG=(Join-Path $session 'observer.jsonl');XESS_FSR_UPSCALE_OBSERVER='1';EFU_RUNTIME_LOG=(Join-Path $session 'efu-runtime.jsonl')}
    if ($continuousOff) {
        $expected['XESS_CONTINUOUS_INPUTS']='1'; $expected['XESS_FSR_NATIVE_OFF']='1'; $expected['XESS_BRIDGE_DEVICE']='1'
        $expected['XESS_PRESENTATION_OFF']='1'; $expected['XESS_SDK_BIN']=$installRoot
        $expected['DISABLE_RTSS_LAYER']='1'
    }
    if ($TraceRenderTail) { $expected['XESS_RENDER_TAIL_DIAGNOSTIC']='1' }
    if ($WsiAcquireTrace) { $expected['XESS_WSI_ACQUIRE_OBSERVER']='1' }
    if ($FakeSwapchain) { $expected['XESS_FAKE_SWAPCHAIN']='1' }
    if ($GpuTimestamps) { $expected['XESS_GPU_TIMESTAMPS']='1' }
    if ($TrackingCpuTiming) { $expected['XESS_TRACKING_CPU_TIMING']='1' }
    if ($ThreadCpuSampling) { $expected['XESS_THREAD_CPU_SAMPLING']='1' }
    if ($LowResOutput) { $expected['XESS_XEFG_LOW_RES_OUTPUT']='1' }
    Check ($observerSettings.Count -eq $expected.Count) 'Observation/continuous OFF environment differs from its exact whitelist.'
    foreach ($field in $expected.Keys) {
        Check ($observerSettings[$field] -ceq $expected[$field] -and
            $childEnvironment.EnvironmentVariables[$field] -ceq $expected[$field]) "Input observation lost exact child value: $field"
    }
    foreach ($field in $remove | Where-Object { -not $expected.Contains($_) }) {
        Check (-not $childEnvironment.EnvironmentVariables.ContainsKey($field)) "Observation inherited a forbidden override: $field"
    }
    Check ($childEnvironment.EnvironmentVariables['END_FIELD_TEST_PRESERVED'] -ceq 'keep') 'Observation removed an unrelated child value.'
    Check ($env:XESS_FSR_UPSCALE_OBSERVER -ceq $parentSnapshot) 'Input observation changed the parent environment.'
    Check ($env:XESS_XEFG_LOW_RES_OUTPUT -ceq $parentLowResOutput) 'Selected route changed the parent low-resolution environment.'
    Check ($env:XESS_WSI_ACQUIRE_OBSERVER -ceq $parentAcquireTrace) 'Selected route changed the parent acquire observation environment.'
    Check ($env:XESS_FAKE_SWAPCHAIN -ceq $parentFakeSwapchain) 'Selected route changed the parent fake swapchain environment.'
    Check ($env:XESS_GPU_TIMESTAMPS -ceq $parentGpuTimestamps) 'Selected route changed the parent GPU timestamp environment.'
    Check ($env:XESS_TRACKING_CPU_TIMING -ceq $parentTrackingCpuTiming) 'Selected route changed the parent tracking timing environment.'
    $admin=$false; $specification=Get-ModLaunchSpecification $fixedGameDirectory $false (-not $continuousOff) $continuousOff
    $lockedPayloadHash=$dummyHash; $launcherSigner=$null; $officialInstanceAlreadyRunning=$false
    $compatibility=[ordered]@{researchFeaturesAvailable=$false;state='reviewed_preview_launch_build'}
    . ([scriptblock]::Create($runtimeLogAssignment[0].Extent.Text))
    . ([scriptblock]::Create($recordAssignment[0].Extent.Text))
    $roundtrip=($record|ConvertTo-Json -Depth 7)| ConvertFrom-Json @jsonOptions
    Check ($roundtrip.renderTailDiagnosticRequested -is [bool] -and
        $roundtrip.renderTailDiagnosticRequested -eq $TraceRenderTail -and
        $roundtrip.renderTailDiagnosticEnabledForChild -is [bool] -and
        $roundtrip.renderTailDiagnosticEnabledForChild -eq $TraceRenderTail) 'Render-tail record does not match the exact child environment.'
    Check ($roundtrip.renderTailAutomaticArm -is [bool] -and -not $roundtrip.renderTailAutomaticArm) 'Launcher must never arm render-tail capture automatically.'
    Check ($roundtrip.wsiAcquireTraceRequested -is [bool] -and $roundtrip.wsiAcquireTraceRequested -eq $WsiAcquireTrace -and
        $roundtrip.wsiAcquireTraceEnabledForChild -is [bool] -and $roundtrip.wsiAcquireTraceEnabledForChild -eq $WsiAcquireTrace) 'Acquire observation record differs from the exact child request.'
    Check ($roundtrip.fakeSwapchainRequested -is [bool] -and $roundtrip.fakeSwapchainRequested -eq $FakeSwapchain -and
        $roundtrip.fakeSwapchainEnabledForChild -is [bool] -and $roundtrip.fakeSwapchainEnabledForChild -eq $FakeSwapchain) 'Fake swapchain record differs from the exact child request.'
    Check ($roundtrip.gpuTimestampsRequested -is [bool] -and $roundtrip.gpuTimestampsRequested -eq $GpuTimestamps -and
        $roundtrip.gpuTimestampsEnabledForChild -is [bool] -and $roundtrip.gpuTimestampsEnabledForChild -eq $GpuTimestamps) 'GPU timestamp record differs from typed child request.'
    Check ($roundtrip.trackingCpuTimingRequested -is [bool] -and $roundtrip.trackingCpuTimingRequested -eq $TrackingCpuTiming -and
        $roundtrip.trackingCpuTimingEnabledForChild -is [bool] -and $roundtrip.trackingCpuTimingEnabledForChild -eq $TrackingCpuTiming) 'Tracking timing record differs from typed child request.'
    foreach ($field in @('trackingCpuTimingMeasured','cpuTrackingOverheadVerified','gpuTimestampsAvailable','gpuTimestampsCollected','fakeSwapchainEnabled','virtualSwapchainEnabled','singleSwapchainVerified','wsiAcquireTraceCollected','wsiAcquireTraceVerified')) {
        Check ($roundtrip.PSObject.Properties.Name -cnotcontains $field) "Observation launch record must not claim runtime implementation or collection: $field"
    }
    Check ($roundtrip.xeFGLowResOutputRequested -is [bool] -and $roundtrip.xeFGLowResOutputRequested -eq $LowResOutput) 'Low-resolution record lost its explicit request.'
    Check (($continuousOff -and $roundtrip.xeFGUIModeRequested -ceq 'NONE') -or
        (-not $continuousOff -and $null -eq $roundtrip.xeFGUIModeRequested)) 'Launch record must identify requested UI NONE only for XeFG.'
    foreach ($field in @('xeFGLowResOutputActive','xeFGLowResOutputEnabled','xefg_low_res_active','xefg_output_extent','xefg_source_extent')) {
        Check ($roundtrip.PSObject.Properties.Name -cnotcontains $field) "Launch record must not claim runtime low-resolution state: $field"
    }
    foreach ($field in @('explicitLayerRequested','fsrUpscaleObserverRequested','efuNoDitherRequested','localLaunchSourceModRequested')) {
        Check ($roundtrip.$field -is [bool] -and $roundtrip.$field) "Observation record lost explicit request: $field"
    }
    foreach ($field in @('researchFeaturesApplied','efuStarted','computeObserverRequested','fsrCallbackObserverRequested',
        'streamlineTraceRequested','runtimeStreamlineHooksRequested','rawColorCaptureRequested',
        'gameOnVerified','gameVisualsVerified','gameNormalExitVerified',
        'gameProcessStartedVerified','gameFrameInputsVerified','xeFGIntegrated','generatedFramesVerified')) {
        Check ($roundtrip.$field -is [bool] -and -not $roundtrip.$field) "Input observation falsely requested an experiment or verified result: $field"
    }
    Check ($roundtrip.observeInputsRequested -eq (-not $continuousOff)) 'Pure observation must never silently enable continuous inputs.'
    Check ($roundtrip.gameOnAdmitted -is [bool] -and $roundtrip.gameOnAdmitted -eq $continuousOff) 'Only the explicit XeFG candidate admits game ON; admission is distinct from verified results.'
    Check ($roundtrip.rtssVulkanLayerDisableRequested -is [bool] -and $roundtrip.rtssVulkanLayerDisableRequested -eq $continuousOff) 'Only XeFG records an explicit child RTSS Vulkan layer disable request.'
    foreach ($field in @('bridgeDeviceRequirementsRequested','presentationOffRequested','xeFGRequested','continuousInputsRequested','nativeFsrSuppressionRequested')) {
        Check ($roundtrip.$field -is [bool] -and $roundtrip.$field -eq $continuousOff) "Observation/continuous OFF mode lost explicit selection: $field"
    }
    Check ($roundtrip.launchPolicy -ceq $specification.launchPolicy -and $roundtrip.backend -ceq $specification.backend) 'Observation/continuous OFF record lost its distinct backend policy.'
    Check (@($roundtrip.environmentForChildOnly.PSObject.Properties).Count -eq $expected.Count) 'Observation/continuous OFF record environment differs from the child plan.'
    foreach ($field in $expected.Keys) { Check ($roundtrip.environmentForChildOnly.$field -ceq $expected[$field]) "Observation record lost exact child value: $field" }
} }

# The official instance guard intentionally allows an existing Launcher/Games.
# Basename matches for game/EFU remain conservative even when paths are unknown.
$modGuard=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-ModGameClosed'},$true))
Check ($modGuard.Count -eq 1) 'Missing official compatibility process guard.'
& {
    . ([scriptblock]::Create($modGuard[0].Extent.Text))
    $fakeGameMatches=@()
    function Get-Process {
        [CmdletBinding()] param([string[]]$Name)
        if (($Name -join ',') -cne 'Endfield,EFU') { throw 'Compatibility guard must not refuse existing official launcher instances.' }
        $fakeGameMatches
    }
    Assert-ModGameClosed
    foreach ($processName in @('Endfield','EFU')) {
        $fakeGameMatches=@([pscustomobject]@{ProcessName=$processName;Id=101;Path=$null})
        Refuses { Assert-ModGameClosed } "Existing $processName accepted without a normal exit."
    }
}

# SDK selection/hashes survive the same structured UAC request, without running
# the bootstrap or any mutation. Use the real Install parameter branch body.
$sdkInstallParameters = @($ast.FindAll({param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -ceq '$Action -eq ''Install''' -and
    $node.Clauses[0].Item2.Extent.Text.Contains('$parameters[''WithXeFGSdk'']')
},$true))
Check ($sdkInstallParameters.Count -eq 1) 'SDK install parameters missing from structured UAC branch.'
& {
    $Action='Install';$WithModLauncher=$true;$WithXeFGSdk=$true;$ModPayloadDirectory='C:\fixture'
    $ExpectedXeFGDllSha256=$xeFGDllSha256;$ExpectedXellDllSha256=$xellDllSha256;$ExpectedIntelLicenseSha256=$intelLicenseSha256
    $ExpectedModLauncherSha256=$dummyHash;$ExpectedEfuSha256=$efuSha256;$ExpectedEfuLicenseSha256=$efuLicenseSha256;$ExpectedEfuNoticeSha256=$dummyHash
    $parameters=@{}
    . ([scriptblock]::Create($sdkInstallParameters[0].Extent.Text))
    $decoded=($parameters|ConvertTo-Json -Compress)| ConvertFrom-Json @jsonOptions
    Check ($decoded.WithXeFGSdk -is [bool] -and $decoded.WithXeFGSdk) 'SDK selection is not a typed bool through UAC.'
    Check ($decoded.ExpectedXeFGDllSha256 -ceq $xeFGDllSha256 -and $decoded.ExpectedXellDllSha256 -ceq $xellDllSha256 -and $decoded.ExpectedIntelLicenseSha256 -ceq $intelLicenseSha256) 'UAC request dropped SDK reviewed hashes.'
}

# Staging's real literal copy list stays at eight by default and adds exactly
# three SDK files on request. Do not execute staging or copy a shared build.
$stageTokens=$null;$stageErrors=$null
$stageAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'stage-mod-launcher.ps1'),[ref]$stageTokens,[ref]$stageErrors)
Check ($stageErrors.Count -eq 0) 'Staging parser errors.'
$copyAssignment=@($stageAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$copies'
})
$sdkCopies=@($stageAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -ceq '$WithXeFGSdk' -and $_.Extent.Text.Contains('$copies[')
})
Check ($copyAssignment.Count -eq 1 -and $sdkCopies.Count -eq 1) 'Expected literal SDK stage copy branch.'
foreach($requested in @($false,$true)) {
    & {
        $WithXeFGSdk=$requested
        . ([scriptblock]::Create($copyAssignment[0].Extent.Text))
        . ([scriptblock]::Create($sdkCopies[0].Extent.Text))
        $expectedCount=if($requested){12}else{8}
        Check ($copies.Count -eq $expectedCount) 'Staging default/SDK payload count changed.'
        if($requested) {
            Check ($copies['third_party\xess\bin\libxess_fg.dll'] -ceq 'mod\libxess_fg.dll' -and
                   $copies['third_party\xess\bin\libxell.dll'] -ceq 'mod\libxell.dll' -and
                   $copies['third_party\xess\LICENSE.txt'] -ceq 'mod\LICENSE.txt') 'SDK stage source/flat installed file mapping differs.'
        }
    }
}

# Exercise the real guard body without creating, terminating or inspecting a
# process. The scoped query represents an absent, running, or inaccessible-path
# frontend. The guard must reject by basename even when its path is unavailable.
$guardDefinition = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-FrontendClosed' }, $true))
Check ($guardDefinition.Count -eq 1) 'Missing frontend lifetime guard.'
& {
    . ([scriptblock]::Create($guardDefinition[0].Extent.Text))
    $script:guardQueryCount = 0
    $fakeFrontendMatches = @()
    function Get-Process {
        [CmdletBinding()] param([string[]]$Name)
        if ($Name.Count -ne 1 -or $Name[0] -cne 'EndfieldXeSSLauncher') { throw 'Unexpected process query in frontend guard.' }
        $script:guardQueryCount++
        $fakeFrontendMatches
    }
    Assert-FrontendClosed
    Check ($script:guardQueryCount -eq 1) 'Absent frontend should allow update.'
    $fakeFrontendMatches = @([pscustomobject]@{ProcessName='EndfieldXeSSLauncher';Id=101;Path='C:\Program Files\EndfieldXeSSObserver\EndfieldXeSSLauncher.exe'})
    Refuses { Assert-FrontendClosed } 'Running frontend allowed package modification.'
    $fakeFrontendMatches = @([pscustomobject]@{ProcessName='EndfieldXeSSLauncher';Id=102;Path=$null})
    Refuses { Assert-FrontendClosed } 'Unknown-path matching frontend allowed package modification.'
    Check ($script:guardQueryCount -eq 3) 'Frontend guard skipped process query.'
}
$gameGuard = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-GameClosed' }, $true))
Check ($gameGuard.Count -eq 1) 'Missing installation game/official-launcher guard.'
& {
    . ([scriptblock]::Create($gameGuard[0].Extent.Text))
    $fakeProcessName = ''
    function Get-Process {
        [CmdletBinding()] param([string[]]$Name)
        if($fakeProcessName -and $Name -ccontains $fakeProcessName){[pscustomobject]@{Name=$fakeProcessName;Id=101;Path=$null}}
    }
    Assert-GameClosed
    Check $true 'Absent game and official launcher should allow update.'
    foreach($fakeProcessName in @('Launcher','Games','HypergryphLauncher','Endfield','EFU')){
        Refuses {Assert-GameClosed} "Running $fakeProcessName allowed package modification."
    }
}
foreach ($actionName in @('Install','Uninstall')) {
    $condition = '$Action -eq ''' + $actionName + ''''
    $branch = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -ceq $condition
    })
    Check ($branch.Count -eq 1) "Missing $actionName mutation branch."
    $first = $branch[0].Clauses[0].Item2.Statements[0]
    Check ($first -is [Management.Automation.Language.PipelineAst] -and $first.PipelineElements[0].GetCommandName() -ceq 'Assert-FrontendClosed') "Frontend guard is not first in $actionName."
}
$guardCalls = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq 'Assert-FrontendClosed' }, $true))
Check ($guardCalls.Count -eq 2) 'Frontend guard must apply only to Install/Uninstall, never LaunchMod/Smoke/Status.'

# Distribution defaults never preselect a developer machine's game directory.
foreach($name in @('LauncherPath','GameDirectory')){
    $parameter=@($ast.ParamBlock.Parameters|Where-Object {$_.Name.VariablePath.UserPath -ceq $name})
    Check ($parameter.Count -eq 1 -and $parameter[0].DefaultValue.SafeGetValue() -ceq '') "Nonempty machine-specific default: $name"
}
# Actual read-only ReviewPlan with staged local copies; no executable is run.
$fixture = Join-Path $root ('out\launchmod-backend-selftest-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
# The real RelWithDebInfo observer can exceed 16 MiB; retain PE/export checks.
$observerDll = Join-Path $SourceDirectory 'xess-vulkan-observer.dll'
$observerDllBytes = (Get-Item -LiteralPath $observerDll).Length
Assert-PeDll $observerDll
Check $true 'Current observer failed bounded PE/export validation.'
if (-not ('LaunchmodBackendSparse' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class LaunchmodBackendSparse {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool DeviceIoControl(SafeFileHandle file, uint code, IntPtr input, uint inputBytes, IntPtr output, uint outputBytes, out uint returned, IntPtr overlapped);
}
'@
}
$oversizedPe = Join-Path $fixture 'oversized-pe-size-only.bin'
$oversizedStream = [IO.File]::Open($oversizedPe, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $returned = [uint32]0
    Check ([LaunchmodBackendSparse]::DeviceIoControl($oversizedStream.SafeFileHandle, 0x900c4, [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [ref]$returned, [IntPtr]::Zero)) 'Could not mark PE-size fixture sparse.'
    $oversizedStream.SetLength(24MB + 1)
    Check (((Get-Item -LiteralPath $oversizedPe).Attributes -band [IO.FileAttributes]::SparseFile) -ne 0) 'PE-size fixture is not sparse.'
    $sizeError = $null
    # An exclusive handle makes any attempted content read fail with a different error.
    try { Assert-PeDll $oversizedPe } catch { $sizeError = $_.Exception.Message }
    Check ($sizeError -ceq 'Unexpected observer DLL size.') 'Oversized PE was not rejected before reading its contents.'
} finally { $oversizedStream.Dispose(); Remove-Item -LiteralPath $oversizedPe }
$junction=Join-Path $fixture 'reparse-game'
New-Item -ItemType Junction -Path $junction -Target $fixture|Out-Null
try {Refuses {Normalize-GameDirectory $junction} 'Reparse-point game directory was accepted.'}
finally {Remove-Item -LiteralPath $junction -Force}
# Full three-file identity is required for preview direct launch. No fixture
# enables the old graphics path or loads a game module.
& {
    function Get-ModLaunchSpecification([string]$Directory) { [ordered]@{gamePath=(Join-Path $fixture 'Endfield.exe')} }
    function Get-GameSignature([string]$Path) { if(-not (Test-Path -LiteralPath $Path)){throw 'fixture missing'};[pscustomobject]@{Status='NotSigned';SignerCertificate=$null} }
    $pinsAssignment = @($ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$reviewedGamePins' })
    Check ($pinsAssignment.Count -eq 1) 'Missing complete reviewed game identity pins.'
    . ([scriptblock]::Create($pinsAssignment[0].Extent.Text))
    Check ($reviewedGamePins.Count -eq 3 -and $reviewedGamePins.Contains('OneChannelLC.dll')) 'Independent preview omitted its OneChannel identity.'
    $missing=Get-ModGameCompatibility $fixedGameDirectory
    Check ($missing.state -ceq 'unavailable' -and -not $missing.efuLaunchCompatible -and -not $missing.researchFeaturesAvailable) 'Missing game must refuse direct launch.'
    foreach($name in $reviewedGamePins.Keys){[IO.File]::WriteAllBytes((Join-Path $fixture $name),[byte[]]@(1,2,3,4))}
    $changed=Get-ModGameCompatibility $fixedGameDirectory
    Check ($changed.state -ceq 'unreviewed_game_build' -and -not $changed.efuLaunchCompatible -and -not $changed.researchFeaturesAvailable) 'Changed game enabled direct launch or graphics research.'
    foreach($name in @($reviewedGamePins.Keys)){$reviewedGamePins[$name]=(Get-FileHash -LiteralPath (Join-Path $fixture $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
    $locks=@{}
    try {
        $matching=Get-ModGameCompatibility $fixedGameDirectory $locks
        Check ($matching.state -ceq 'reviewed_preview_launch_build' -and $matching.efuLaunchCompatible -and -not $matching.researchFeaturesAvailable) 'Full preview identity must enable only the direct launch route.'
        Check ($locks.Count -eq 3) 'Game module identities were not locked through process creation.'
        Refuses { [IO.File]::OpenWrite((Join-Path $fixture 'OneChannelLC.dll')).Dispose() } 'Pinned OneChannel module remained writable during launch preparation.'
    } finally {foreach($stream in $locks.Values){$stream.Dispose()}}
    & {
        $installRoot=$fixture;$gameConfigurationPath=Join-Path $fixture 'game-path.txt';$utf8=[Text.UTF8Encoding]::new($false)
        function Assert-ProtectedAcl([string]$Path) {} # Fixture only; production ACL enforcement is retained.
        $denyAtomic=$false
        function Set-ProtectedAcl([string]$Path,[bool]$Directory) {if($denyAtomic){throw 'simulated interrupted replacement'}}
        foreach($name in @('Read-GameDirectory','Write-GameDirectory')){
            $definition=$ast.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
            . ([scriptblock]::Create($definition[0].Extent.Text))
        }
        Check ((Read-GameDirectory) -ceq '') 'Missing path config must remain unconfigured.'
        Write-GameDirectory $fixture|Out-Null
        Check ((Read-GameDirectory) -ceq $fixture) 'Verified chosen path did not survive atomic save/read.'
        $before=[IO.File]::ReadAllText($gameConfigurationPath)
        $denyAtomic=$true
        Refuses {Write-GameDirectory $fixture} 'Interrupted atomic path update unexpectedly succeeded.'
        Check ([IO.File]::ReadAllText($gameConfigurationPath) -ceq $before) 'Interrupted path update destroyed prior configuration.'
        $denyAtomic=$false;$reviewedGamePins['Endfield.exe']='0'*64
        Refuses {Write-GameDirectory $fixture} 'Unverified game path was saved.'
        Check ([IO.File]::ReadAllText($gameConfigurationPath) -ceq $before) 'Invalid update replaced verified path.'
        Check (@(Get-ChildItem -LiteralPath $fixture -Filter 'game-path.*.tmp').Count -eq 0) 'Path temp file leaked after failure.'
        Remove-Item -LiteralPath $gameConfigurationPath
    }
    foreach($name in $reviewedGamePins.Keys){Remove-Item -LiteralPath (Join-Path $fixture $name)}
}
Copy-Item -LiteralPath $LauncherExecutable -Destination (Join-Path $fixture 'EndfieldXeSSLauncher.exe')
Copy-Item -LiteralPath (Join-Path $root 'out\efu-launcher-options-20261023\source\zig-out\bin\EFU.exe') -Destination (Join-Path $fixture 'EFU.exe')
Copy-Item -LiteralPath (Join-Path $root 'out\efu-preview-20261015\source\LICENSE.txt') -Destination (Join-Path $fixture 'EFU-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'docs\efu-source-notice.txt') -Destination (Join-Path $fixture 'EFU-SOURCE-NOTICE.txt')
$planText = & $management -Action ReviewPlan -SourceDirectory $SourceDirectory -WithModLauncher -ModPayloadDirectory $fixture -GameDirectory $fixedGameDirectory
$plan = $planText | ConvertFrom-Json @jsonOptions
Check (-not $plan.changesPerformed) 'ReviewPlan claims changes.'
Check ($plan.singleChainPolicyVersion -eq 1) 'New review plan omitted the required single-chain capability.'
Check ($plan.copiedFiles.Count -eq 8) 'Optional ReviewPlan did not list eight payloads.'
Check ($plan.sourceHashes.modLauncher.efuSha256 -ceq $efuSha256) 'Plan lost official EFU pin.'
Check ($plan.standaloneLaunch.arguments -ceq $spec.arguments) 'Plan argument mismatch.'
$emptyPathPlan=(& $management -Action ReviewPlan -SourceDirectory $SourceDirectory -WithModLauncher -ModPayloadDirectory $fixture)|ConvertFrom-Json @jsonOptions
Check ($null -eq $emptyPathPlan.standaloneLaunch -and -not $emptyPathPlan.changesPerformed) 'Unconfigured distribution plan must remain valid without selecting a game path.'
& {
    $gameConfigurationPath=Join-Path $fixture 'never-configured/game-path.txt'
    $reader=@($ast.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Read-GameDirectory'},$true))
    . ([scriptblock]::Create($reader[0].Extent.Text))
    Check ((Read-GameDirectory) -ceq '') 'First install must not synthesize a game directory.'
}
Check ($plan.standaloneLaunchPolicy -ceq 'independent_efu_preview' -and -not $plan.standaloneResearchFeaturesAvailable) 'Plan lost independent default or falsely enabled graphics research.'
foreach ($parameter in @('ExpectedDllSha256','ExpectedManifestSha256','ExpectedSmokeSha256','ExpectedScriptSha256',
                        'ExpectedModLauncherSha256','ExpectedEfuSha256','ExpectedEfuLicenseSha256','ExpectedEfuNoticeSha256')) {
    Check ($plan.installCommand.Contains('-' + $parameter + ' ')) "Missing exact reviewed hash argument: $parameter"
}
[IO.File]::WriteAllText((Join-Path $fixture 'review-plan.json'), ($plan | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
$legacyPlan = (& $management -Action ReviewPlan -SourceDirectory $SourceDirectory) | ConvertFrom-Json @jsonOptions
Check ($legacyPlan.copiedFiles.Count -eq 4 -and -not $legacyPlan.standaloneLaunchPackageRequested) 'Legacy ReviewPlan changed ownership scope.'
# A changed notice must fail before any elevation or file installation.
[IO.File]::WriteAllText((Join-Path $fixture 'EFU-SOURCE-NOTICE.txt'), 'missing source attribution')
Refuses { & $management -Action ReviewPlan -SourceDirectory $SourceDirectory -WithModLauncher -ModPayloadDirectory $fixture } 'Unattributed EFU notice accepted.'
Copy-Item -LiteralPath (Join-Path $root 'docs\efu-source-notice.txt') -Destination (Join-Path $fixture 'EFU-SOURCE-NOTICE.txt')
foreach($pair in @(@('bin\libxess_fg.dll','libxess_fg.dll'),@('bin\libxell.dll','libxell.dll'),@('bin\libxess.dll','libxess.dll'),@('LICENSE.txt','LICENSE.txt'))) {
    Copy-Item -LiteralPath (Join-Path $root ('third_party\xess\' + $pair[0])) -Destination (Join-Path $fixture $pair[1])
}
$sdkPlan = (& $management -Action ReviewPlan -SourceDirectory $SourceDirectory -WithModLauncher -ModPayloadDirectory $fixture -WithXeFGSdk) | ConvertFrom-Json @jsonOptions
Check ($sdkPlan.copiedFiles.Count -eq 12 -and $sdkPlan.xeFGSdkPackageRequested -and -not $sdkPlan.changesPerformed) 'SDK ReviewPlan must list 12 files without writes.'
Check ($sdkPlan.observeInputsLaunchCommand.Contains('-ObserveInputs') -and -not $sdkPlan.observeInputsLaunchCommand.Contains('-BridgeDevice') -and -not $sdkPlan.observeInputsLaunchCommand.Contains('-TraceFsr')) 'Observation review command must use its single explicit switch.'
foreach($parameter in @('ExpectedXeFGDllSha256','ExpectedXellDllSha256','ExpectedIntelLicenseSha256')) {
    Check ($sdkPlan.installCommand.Contains('-' + $parameter + ' ')) "Missing reviewed SDK hash argument: $parameter"
}
Check ($sdkPlan.presentationOffLaunchCommand.Contains('-PresentationOff') -and $sdkPlan.presentationOffLaunchCommand.Contains('-BridgeDevice') -and -not $sdkPlan.presentationOffLaunchCommand.Contains('-CaptureRawColor')) 'OFF plan command lost fixed runtime options.'
Check ($sdkPlan.xeFGLaunchCommand.Contains('-XeFG')) 'Continuous OFF plan command lost the existing XeFG switch.'
Check ($sdkPlan.renderTailDiagnosticLaunchCommand.Contains('-XeFG') -and
    $sdkPlan.renderTailDiagnosticLaunchCommand.Contains('-TraceRenderTail') -and
    -not $sdkPlan.xeFGLaunchCommand.Contains('-TraceRenderTail')) 'Render-tail review command must be separate and explicitly opt in.'
Check ($sdkPlan.wsiAcquireTraceLaunchCommand.Contains('-XeFG') -and
    $sdkPlan.wsiAcquireTraceLaunchCommand.Contains('-LowResOutput') -and
    $sdkPlan.wsiAcquireTraceLaunchCommand.Contains('-WsiAcquireTrace') -and
    -not $sdkPlan.wsiAcquireTraceLaunchCommand.Contains('-TraceRenderTail') -and
    -not $sdkPlan.xeFGLaunchCommand.Contains('-WsiAcquireTrace') -and
    -not $sdkPlan.renderTailDiagnosticLaunchCommand.Contains('-WsiAcquireTrace')) 'Acquire observation review command must explicitly opt in while keeping XeFG low-resolution output.'
Check ($sdkPlan.fakeSwapchainLaunchCommand.Contains('-XeFG') -and $sdkPlan.fakeSwapchainLaunchCommand.Contains('-LowResOutput') -and
    $sdkPlan.fakeSwapchainLaunchCommand.Contains('-FakeSwapchain') -and -not $sdkPlan.fakeSwapchainLaunchCommand.Contains('-WsiAcquireTrace') -and
    -not $sdkPlan.fakeSwapchainLaunchCommand.Contains('-TraceRenderTail') -and $sdkPlan.xeFGLaunchCommand.Contains('-FakeSwapchain') -and
    -not $sdkPlan.wsiAcquireTraceLaunchCommand.Contains('-FakeSwapchain')) 'XeFG review default and compatibility command use one chain without enabling trace diagnostics.'
Check ($sdkPlan.xeFGLowResOutputLaunchCommand.Contains('-XeFG') -and
    $sdkPlan.xeFGLowResOutputLaunchCommand.Contains('-LowResOutput') -and
    -not $sdkPlan.xeFGLowResOutputLaunchCommand.Contains('-TraceRenderTail') -and
    $sdkPlan.xeFGLaunchCommand.Contains('-LowResOutput') -and
    -not $sdkPlan.renderTailDiagnosticLaunchCommand.Contains('-LowResOutput')) 'XeFG review default keeps low-resolution output and omits render-tail.'
Check ($sdkPlan.legacyChainLaunchCommand.Contains('-XeFG') -and $sdkPlan.legacyChainLaunchCommand.Contains('-LowResOutput') -and
    -not $sdkPlan.legacyChainLaunchCommand.Contains('-FakeSwapchain')) 'Legacy review command must preserve the original dual-chain route.'
Check ($sdkPlan.xeFGFullResOutputLaunchCommand.Contains('-XeFG') -and $sdkPlan.xeFGFullResOutputLaunchCommand.Contains('-FakeSwapchain') -and
    -not $sdkPlan.xeFGFullResOutputLaunchCommand.Contains('-LowResOutput')) 'Full-resolution review command must use one chain without low-resolution output.'
foreach ($parameter in @('BridgeDevice','TraceFsr','TraceFsrUpscale','PresentationOff','CaptureRawColor','ObserveInputs')) {
    Check (-not $sdkPlan.xeFGLaunchCommand.Contains('-' + $parameter)) "Continuous OFF plan command mixed another switch: $parameter"
}
[IO.File]::WriteAllText((Join-Path $fixture 'sdk-review-plan.json'), ($sdkPlan | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $fixture 'LICENSE.txt'), 'modified SDK license')
Refuses { & $management -Action ReviewPlan -SourceDirectory $SourceDirectory -WithModLauncher -ModPayloadDirectory $fixture -WithXeFGSdk } 'Changed Intel license accepted.'
Copy-Item -LiteralPath (Join-Path $root 'third_party\xess\LICENSE.txt') -Destination (Join-Path $fixture 'LICENSE.txt')
foreach($pair in @(@('bin\libxess_fg.dll','libxess_fg.dll'),@('bin\libxell.dll','libxell.dll'),@('bin\libxess.dll','libxess.dll'))) {
    [IO.File]::WriteAllBytes((Join-Path $fixture $pair[1]),[byte[]]@(1,2,3))
    Refuses { & $management -Action ReviewPlan -SourceDirectory $SourceDirectory -WithModLauncher -ModPayloadDirectory $fixture -WithXeFGSdk } "Changed SDK binary accepted: $($pair[1])"
    Copy-Item -LiteralPath (Join-Path $root ('third_party\xess\' + $pair[0])) -Destination (Join-Path $fixture $pair[1])
}

$legacySdk = $sdk | ConvertTo-Json -Depth 10 | ConvertFrom-Json
$legacySdk.xeFGSdk.sourceHashes.PSObject.Properties.Remove('xessSrDllSha256')
$legacySdk.files = @($legacySdk.files | Where-Object name -cne 'libxess.dll')
Check (@(Get-ReceiptOwnedNames $legacySdk).Count -eq 11) 'Prior SDK receipt must remain readable for removal.'
$srChild = [Diagnostics.ProcessStartInfo]::new()
foreach ($timingRequested in @($true,$false)) {
    $XessSr=$true; $XeFG=$true; $FakeSwapchain=$true; $GpuTimestamps=$timingRequested
    . ([scriptblock]::Create($parameterAssignment[0].Extent.Text))
    $srRequest=($parameters|ConvertTo-Json -Compress)|ConvertFrom-Json @jsonOptions
    Check ($srRequest.XessSr -is [bool] -and $srRequest.XessSr) 'Structured UAC request lost SR activation.'
    Check ($srRequest.GpuTimestamps -is [bool] -and $srRequest.GpuTimestamps -eq $timingRequested) 'Structured SR request changed the GPU timing choice.'
    Set-ContinuousOffEnvironment $srChild 'C:\fixture-session' $false $false $false $true $timingRequested $false $true | Out-Null
    Check ($srChild.EnvironmentVariables['XESS_SR_REPLACE'] -ceq '1') 'SR opt-in child request missing.'
    Check ($srChild.EnvironmentVariables.ContainsKey('XESS_GPU_TIMESTAMPS') -eq $timingRequested) 'SR child GPU timestamps differ from explicit diagnostic choice.'
    if ($timingRequested) { Check ($srChild.EnvironmentVariables['XESS_GPU_TIMESTAMPS'] -ceq '1') 'Explicit SR GPU timestamp value missing.' }
}
Set-ContinuousOffEnvironment $srChild 'C:\fixture-session' | Out-Null
Check (-not $srChild.EnvironmentVariables.ContainsKey('XESS_SR_REPLACE')) 'A later normal launch must remove inherited SR opt-in.'
Check (-not $srChild.EnvironmentVariables.ContainsKey('XESS_SR_BACKEND')) 'A normal launch inherited an SR backend.'
& {
    $SrBackend='Fsr4XmxLite';$XessSr=$true
    $child=[Diagnostics.ProcessStartInfo]::new()
    Set-ContinuousOffEnvironment $child 'C:\fixture-session' $false $false $false $true $false $false $true | Out-Null
    Check ($child.EnvironmentVariables['XESS_SR_BACKEND'] -ceq 'fsr4_xmx_lite') 'FSR4 selected backend lost in child environment.'
    Refuses { Set-ContinuousOffEnvironment $child 'C:\fixture-session' } 'FSR4 accepted without SR replacement.'
    . ([scriptblock]::Create($parameterAssignment[0].Extent.Text))
    Check ($parameters.SrBackend -ceq 'Fsr4XmxLite') 'Elevation lost the selected FSR4 backend.'
}

foreach($debugValue in 0..3){foreach($sceneValue in 0..6){foreach($capValue in @($false,$true)){
    & {$FgDebugMode=$debugValue;$FgSceneIndex=$sceneValue;$Override30Fps=$capValue
        $child=[Diagnostics.ProcessStartInfo]::new();$child.EnvironmentVariables['XESS_FG_DEBUG_MODE']='inherited';$child.EnvironmentVariables['XESS_FRAME_CAP_30_TO_40']='inherited'
        Set-ContinuousOffEnvironment $child 'D:\fixture'|Out-Null
        Check ($child.EnvironmentVariables['XESS_FG_SCENE_THRESHOLD'] -ceq [string]$sceneValue) 'Threshold typed child setting lost.'
        Check ($child.EnvironmentVariables.ContainsKey('XESS_FG_DEBUG_MODE') -eq ($debugValue -gt 0)) 'Debug one-shot off inherited unwanted mode.'
        Check ($child.EnvironmentVariables.ContainsKey('XESS_FRAME_CAP_OVERRIDE') -eq $capValue) 'Cap mapping leaked or was lost.'
        Check (-not $child.EnvironmentVariables.ContainsKey('XESS_FRAME_CAP_30_TO_40')) 'Legacy inherited cap flag leaked.'
        if($capValue){Check ($child.EnvironmentVariables['XESS_FRAME_CAP_TARGET_FPS'] -ceq [string]$FrameCapTargetFps) 'FPS target not propagated.'}
    }
}}}
$installBranch=@($ast.EndBlock.Statements|Where-Object {$_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -ceq '$Action -eq ''Install'''})
$uninstallBranch=@($ast.EndBlock.Statements|Where-Object {$_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -ceq '$Action -eq ''Uninstall'''})
Check ($installBranch[0].Extent.Text.Contains('Write-GameDirectory $GameDirectory')) 'Install omitted verified legacy path migration.'
Check ($uninstallBranch[0].Extent.Text.Contains('Remove-Item -LiteralPath $gameConfigurationPath')) 'Uninstall omitted protected path configuration cleanup.'
foreach($target in 30..120){
    & {$Override30Fps=$true;$FrameCapTargetFps=$target;$child=[Diagnostics.ProcessStartInfo]::new();Set-ContinuousOffEnvironment $child 'D:\fixture'|Out-Null
        Check ($child.EnvironmentVariables['XESS_FRAME_CAP_TARGET_FPS'] -ceq [string]$target) 'Valid FPS target lost during child launch.'}
}
foreach($target in @(0,29,121,1000)){Refuses {& $management -Action ReviewPlan -FrameCapTargetFps $target} 'Out-of-range FPS argument accepted.'}

# RC5: exercise the real child-environment functions without launching a process.
& {
    foreach($diagnostics in @($false,$true)){
        $child=[Diagnostics.ProcessStartInfo]::new()
        foreach($key in @('XESS_OBSERVER_LOG','EFU_RUNTIME_LOG','VK_LOADER_DEBUG','XESS_RUNTIME_STATUS','XESS_DIAGNOSTIC_MODE')){$child.EnvironmentVariables[$key]='inherited'}
        $directory=if($diagnostics){'D:\fixture'}else{''}
        $settings=Set-ContinuousOffEnvironment $child $directory $false $false $false $true $false $false $true
        Check ($settings['XESS_DIAGNOSTIC_MODE'] -ceq $(if($diagnostics){'1'}else{'0'})) 'Diagnostic mode was not explicit.'
        Check (-not $child.EnvironmentVariables.ContainsKey('XESS_RUNTIME_STATUS') -and -not $child.EnvironmentVariables.ContainsKey('XESS_RUNTIME_IMAGE')) 'Removed status channel leaked into a child.'
        foreach($key in @('XESS_OBSERVER_LOG','EFU_RUNTIME_LOG','VK_LOADER_DEBUG')){
            Check ($child.EnvironmentVariables.ContainsKey($key) -eq $diagnostics) ('No-log child leaked a diagnostic variable: '+$key)
        }
        Check ($settings['XESS_SR_REPLACE'] -ceq '1' -and $settings['XESS_FAKE_SWAPCHAIN'] -ceq '1' -and $settings['DISABLE_RTSS_LAYER'] -ceq '1') 'Zero-log mode changed the render route.'
    }
}
$sessionDecision=@($launchModBranch[0].FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$session' -and $node.Right.Extent.Text.Contains('$ReleaseMinimal')},$true))
Check ($sessionDecision.Count -eq 1) 'Missing explicit no-session decision.'
& {
    $script:sessionCreations=0
    function New-DiagnosticSession {$script:sessionCreations++;'D:\fixture-session'}
    foreach($pair in @(@($true,$false,0),@($true,$true,1),@($false,$false,1))){
        $ReleaseMinimal=$pair[0];$DiagnosticMode=$pair[1];$script:sessionCreations=0
        . ([scriptblock]::Create($sessionDecision[0].Extent.Text))
        Check ($script:sessionCreations -eq $pair[2]) 'Default release must not create a session; diagnostic/development must.'
    }
}
# Smoke workspaces are protected-install temporary children, never Sessions.
& {
    foreach($name in @('New-SmokeWorkspace','Remove-SmokeWorkspace')){
        $definition=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name},$true))
        . ([scriptblock]::Create($definition[0].Extent.Text))
    }
    $installRoot=Join-Path $fixture 'smoke-install';[IO.Directory]::CreateDirectory($installRoot)|Out-Null
    $SessionId='0123456789abcdef0123456789abcdef'
    function Assert-ProtectedAcl {}
    function Set-ProtectedAcl {}
    $workspace=New-SmokeWorkspace
    [IO.File]::WriteAllText((Join-Path $workspace 'observer.jsonl'),'fixture')
    Remove-SmokeWorkspace $workspace
    Check (-not [IO.Directory]::Exists($workspace)) 'Completed smoke left a workspace.'
    $workspace=New-SmokeWorkspace
    [IO.File]::WriteAllText((Join-Path $workspace 'user-file.txt'),'preserve')
    Refuses {Remove-SmokeWorkspace $workspace} 'Unknown smoke content deleted.'
    Check ([IO.File]::Exists((Join-Path $workspace 'user-file.txt'))) 'Unknown smoke file changed.'
    Refuses {Remove-SmokeWorkspace $fixture} 'Out-of-root smoke cleanup accepted.'
}

[ordered]@{ passed=$true; checks=$checks; parserErrors=0; fixture=$fixture; managementActionsExecuted=@('ReviewPlan')
    peSizeBoundary=@{observerBytes=$observerDllBytes;observerAccepted=$true;oversizedBytes=(24MB + 1);sparseFixture=$true;oversizedRejectedBeforeRead=$true}
    executablesRun=$false; installedFilesChanged=$false; gameFilesChanged=$false; legacyFourFileReceiptAccepted=$true } | ConvertTo-Json -Depth 4
