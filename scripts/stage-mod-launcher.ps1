[CmdletBinding()]
param([switch]$WithXeFGSdk, [string]$PayloadPackageDirectory, [string]$BuildDirectory='build', [string]$Fsr4XmxDirectory)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -cne 'Core' -or $PSVersionTable.PSVersion.Major -ne 7) { throw 'Staging requires PowerShell 7 Core.' }
$stageJsonOptions = @{}
if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $stageJsonOptions.DateKind = 'String' }
$project = Split-Path -Parent $PSScriptRoot
$utf8 = [Text.UTF8Encoding]::new($false)
$package = Join-Path $project ('out\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-mod-launcher-package')
if (Test-Path -LiteralPath $package) { throw 'Package already exists; refusing to overwrite.' }
$observer = Join-Path $package 'payload\observer'
$mod = Join-Path $package 'mod'
[IO.Directory]::CreateDirectory($observer) | Out-Null
[IO.Directory]::CreateDirectory($mod) | Out-Null
$copies = [ordered]@{
    'scripts\manage-observer-install.ps1' = 'manage-observer-install.ps1'
    'build\observer\xess-vulkan-observer.dll' = 'payload\observer\xess-vulkan-observer.dll'
    'build\observer\VkLayer_xess_observer.json' = 'payload\observer\VkLayer_xess_observer.json'
    'build\xess-vulkan-wsi-smoke.exe' = 'payload\xess-vulkan-wsi-smoke.exe'
    'build\EndfieldXeSSLauncher.exe' = 'mod\EndfieldXeSSLauncher.exe'
    'out\efu-launcher-options-20261023\source\zig-out\bin\EFU.exe' = 'mod\EFU.exe'
    'out\efu-launcher-options-20261023\source\LICENSE.txt' = 'mod\EFU-LICENSE.txt'
    'docs\efu-source-notice.txt' = 'mod\EFU-SOURCE-NOTICE.txt'
}
if ($WithXeFGSdk) {
    $copies['third_party\xess\bin\libxess.dll'] = 'mod\libxess.dll'
    $copies['third_party\xess\bin\libxess_fg.dll'] = 'mod\libxess_fg.dll'
    $copies['third_party\xess\bin\libxell.dll'] = 'mod\libxell.dll'
    $copies['third_party\xess\LICENSE.txt'] = 'mod\LICENSE.txt'
}
$files = @()
$payloadRecord = if ($PayloadPackageDirectory) { Get-Content -LiteralPath (Join-Path $PayloadPackageDirectory 'package.json') -Raw | ConvertFrom-Json @stageJsonOptions } else { $null }
foreach ($relative in $copies.Keys) {
    $source = if($relative.StartsWith('build\')) { Join-Path (Join-Path $project $BuildDirectory) $relative.Substring(6) } else { Join-Path $project $relative }
    if ($PayloadPackageDirectory -and $relative -notin @('scripts\manage-observer-install.ps1','build\EndfieldXeSSLauncher.exe')) {
        $source = Join-Path $PayloadPackageDirectory $copies[$relative]
        $record = @($payloadRecord.files | Where-Object name -CEQ $copies[$relative])
        if ($record.Count -ne 1 -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $record[0].sha256) { throw "Reviewed payload snapshot changed: $relative" }
    }
    $destination = Join-Path $package $copies[$relative]
    $before = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $source -Destination $destination
    $after = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($after -cne $before) { throw "Package copy mismatch: $relative" }
    $relocated = $false
    if ($relative -ceq 'build\observer\VkLayer_xess_observer.json') {
        # The developer manifest is absolute. Relocate only the copied manifest;
        # ReviewPlan checks that it resolves to this snapshot's DLL, not build/.
        $manifest = Get-Content -LiteralPath $destination -Raw | ConvertFrom-Json @stageJsonOptions
        $manifest.layer.library_path = './xess-vulkan-observer.dll'
        [IO.File]::WriteAllText($destination, ($manifest | ConvertTo-Json -Depth 10), $utf8)
        $after = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        $relocated = $true
    }
    $files += [ordered]@{ name=$copies[$relative]; sha256=$after; sourceSha256=$before; manifestRelocated=$relocated; bytes=(Get-Item -LiteralPath $destination).Length }
}
if ($Fsr4XmxDirectory) {
    if (-not $WithXeFGSdk -or -not [IO.Path]::IsPathFullyQualified($Fsr4XmxDirectory)) { throw 'FSR4 staging requires the SDK payload and an absolute source directory.' }
    $fsr4Files = [ordered]@{ 'amd_fidelityfx_upscaler_dx12.dll'='74d18f09285150fa5531d1e5ffbad6a21b677e4eb1bc34cb580b2963f560e740'; 'fsr-original.dll'='c33a287f3048bf02311cae430b6a977500b94ca7b315cd7bff751e6b03e483e3' }
    foreach ($name in $fsr4Files.Keys) {
        $source = Join-Path $Fsr4XmxDirectory $name
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $fsr4Files[$name]) { throw "Unreviewed FSR4 XMX file: $name" }
        $destination = Join-Path $mod $name;Copy-Item -LiteralPath $source -Destination $destination
        $after = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($after -cne $fsr4Files[$name]) { throw "FSR4 package copy mismatch: $name" }
        $files += [ordered]@{name="mod\$name";sha256=$after;sourceSha256=$after;manifestRelocated=$false;bytes=(Get-Item -LiteralPath $destination).Length}
    }
}
# Verify the reviewed source before copying. Only checkout-location metadata
# is normalized afterwards; code, patches and licenses remain byte-identical.
$efuSourceRoot = Join-Path $project 'out\efu-launcher-options-20261023\source'
$efuProvenancePath = Join-Path $efuSourceRoot 'LOCAL-SOURCE-PROVENANCE.json'
$efuProvenance = Get-Content -LiteralPath $efuProvenancePath -Raw | ConvertFrom-Json @stageJsonOptions
if ($efuProvenance.commit -cne 'eff32bbc89baaf25fcb25f2a8903990e172f319e' -or $efuProvenance.localPatchId -cne 'camera-minhook-launcher-options-v3' -or $efuProvenance.files.Count -ne 48) { throw 'Unexpected corresponding EFU source identity.' }
$sourceSnapshot = Join-Path $package 'efu-corresponding-source'
$correspondingSourceFiles = @()
foreach ($entry in $efuProvenance.files) {
    if ($entry.path -cnotmatch '^[A-Za-z0-9._/-]+$' -or $entry.path.StartsWith('/') -or
        @($entry.path.Split('/') | Where-Object { $_ -eq '..' }).Count) { throw 'Unsafe corresponding-source path.' }
    $sourcePath = Join-Path $efuSourceRoot $entry.path
    if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) { throw 'Reviewed EFU source changed before staging.' }
    $destination = Join-Path $sourceSnapshot $entry.path
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destination
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) { throw 'Corresponding-source copy mismatch.' }
    $reviewedHash=$entry.sha256
    if($entry.path -ceq 'local-patches/UPSTREAM-SOURCE-PROVENANCE.json'){
        $metadata=Get-Content -LiteralPath $destination -Raw|ConvertFrom-Json @stageJsonOptions
        $metadata.PSObject.Properties.Remove('sourceDirectory')
        $metadata|Add-Member -NotePropertyName localCheckoutPathOmitted -NotePropertyValue $true
        [IO.File]::WriteAllText($destination,($metadata|ConvertTo-Json -Depth 12),$utf8)
        $entry.sha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        $entry.bytes=(Get-Item -LiteralPath $destination).Length
    }
    $correspondingSourceFiles += [ordered]@{ name=$entry.path; sha256=$entry.sha256; bytes=$entry.bytes; reviewedSourceSha256=$reviewedHash }
}
$efuProvenance|Add-Member -NotePropertyName distributionMetadataNormalization -NotePropertyValue ([ordered]@{
    originalManifestSha256=(Get-FileHash -LiteralPath $efuProvenancePath -Algorithm SHA256).Hash.ToLowerInvariant()
    change='Removed only sourceDirectory from local-patches/UPSTREAM-SOURCE-PROVENANCE.json; code and licenses unchanged'
})
[IO.File]::WriteAllText((Join-Path $sourceSnapshot 'SOURCE-PROVENANCE.json'),($efuProvenance|ConvertTo-Json -Depth 12),$utf8)
# ReviewPlan is a read-only management action; staging never installs or launches.
$manager = Join-Path $package 'manage-observer-install.ps1'
$plan = (& $manager -Action ReviewPlan -SourceDirectory $observer -WithModLauncher -ModPayloadDirectory $mod -WithXeFGSdk:$WithXeFGSdk) | ConvertFrom-Json @stageJsonOptions
$expectedFiles = if ($WithXeFGSdk) { 12 } else { 8 }
if ($Fsr4XmxDirectory) { $expectedFiles += 2 }
if ($plan.changesPerformed -or $plan.copiedFiles.Count -ne $expectedFiles) { throw 'Unexpected launcher review-plan file count.' }
[IO.File]::WriteAllText((Join-Path $package 'review-plan.json'), ($plan | ConvertTo-Json -Depth 10), $utf8)
$statePath = Join-Path $project 'out\standalone-launcher-pending-deployment.json'
$priorState = if (Test-Path -LiteralPath $statePath) { Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json @stageJsonOptions } else { $null }
$efuHash = ($files | Where-Object { $_.name -ceq 'mod\EFU.exe' }).sha256
$priorEfuVerified = $null -ne $priorState -and $priorState.efuEffectVerified -and
    $efuHash -ceq '65d34a67ae1e82b03e22ea7f38baffd5d75bcf5706a4f896e4633b55e2597e94' -and
    $efuHash -ceq $priorState.sourceHashes.modLauncher.efuSha256
$priorVerification = [ordered]@{ verified=[bool]$priorEfuVerified; scope='earlier_user_confirmed_current_game_scenes'
    defaultNoDitherEnabled=[bool]$priorEfuVerified; altF12DisablesNoDither=[bool]$priorEfuVerified
    altF12VerificationScope='historical_prior_game_version_not_retested_current'
    sameEfuBinaryAsPreviouslyVerified=[bool]$priorEfuVerified; efuSha256=$efuHash
    evidence='out/standalone-launcher-pending-deployment.json'; evidenceSha256=if ($priorState) { (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
    currentCandidateGameTested=$false; applicableToUpdatedGame=[bool]$priorEfuVerified; enabledByCurrentLaunchRoute=$true }
$efuVerificationSentence = if ($priorEfuVerified) {
    '用户已在当前游戏版本确认同一 EFU 的免虚化，Alt+F12 切换功能保留。'
} else {
    '本包包含基于上游 eff32bbc 的本地 CameraPitch/MinHook 修复。默认请求免虚化及 Alt+F12，需进入当前游戏场景确认效果。'
}
$readme = @'
终末地独立启动器：1.6 预览适配包

本包保留独立启动；--xefg 默认使用 XeFG 单链和低分辨率输出，--xefg --full-res-output 选择单链全分辨率输出。
XeSS 2× 基础路径与低分辨率输出均已实机验收（2026-10-17 1080p A/B：1% low 57–62 对 42，核显功耗 14 W 对 21 W）。
已有场景证据：out/20261015-on-scene-7ccc85ea6f2249a981214a931d0a6bb1/{review.json,off-review.json,exit.json}。
独立 EXE 提供直接启动游戏、托盘驻留和日志入口。
点击“启动游戏”由本机源码构建的固定 EFU 预览直接启动游戏；无需先打开官方窗口。
EFU 默认请求免虚化及 Alt+F12 切换；已有同一二进制的验证记录见下。
默认启动不加载 Vulkan 观察层或 XeSS；游戏保持原有渲染器及登录流程。
显式运行 EndfieldXeSSLauncher.exe --observe-inputs 请求当前固定版本的上采样输入纯观察。
该入口记录资源及 dispatch 关联诊断；本候选尚待游戏场景验证，不代表 XeSS 已接通。
--official 保留显式官方备用入口，需要在官方窗口点击“开始游戏”。
@EFU_VERIFICATION@

必须在游戏、独立开发启动器、托盘中的鹰角启动器和 EFU 全部正常退出后安装。
review-plan.json 记录本包所有载荷的精确哈希与本机安装命令。
已有旧观察层时，先通过它自己的受保护管理脚本正常卸载，再安装本包。
安装需正常 Windows UAC；本包不包含静默提权或安全系统绕过。
安装位置：C:\Program Files\EndfieldXeSSObserver
运行其中的 EndfieldXeSSLauncher.exe，点击“启动游戏”。

@DEVELOPMENT_MODE@
旧 --capture-rawcolor-once 和 --presentation-off 参数仍只记录请求。
--xefg 默认请求单链和低分辨率输出（--xefg --low-res-output 为等价旧写法）；--xefg --full-res-output 选择单链 2880 全分辨率输出。
--xefg --trace-acquire 显式请求 acquire 用法仅观察，保留现有 XeSS 默认低分辨率输出、免虚化和 RTSS。
该诊断默认关闭，与 --trace-render-tail 互斥；采集结果需完成场景操作后查看 observer.jsonl。
--xefg --fake-swapchain 保留为默认单链的兼容写法，与上述诊断互斥。
--xefg --legacy-chain 保留原双链备用，可加 --full-res-output 或 --low-res-output；无 --xefg 的 EFU 启动不变。
--xefg --xess-sr 开启纯XeSS超分核心替换；游戏菜单仍选择AMD FSR3和放大档位。
超分启动器显示当前SR输出、停用原因、恢复阻塞及补帧单／双输出；隐藏或最小化时暂停刷新。
SR输入不足时可以显示颜色过渡，无法继续时保持输出并请求引擎原生分辨率；不恢复FSR超分核心。
相关命令和缺失内容已退役后可在同一交换链恢复；真正未解决的资源依赖仍阻止恢复。
产品SR使用按set发布的引用位图和当前缺失内容位图；无法使用快路径时保留完整查询，不因位图宽度停用SR。
图像、view及layout的退役只使实际关联set的位图失效；无关对象的创建销毁不更新全局位图epoch。
产品默认不记录consumer audit或尾部通用命令；SrObservation或SrFullGraphObservation显式诊断仍可启用。
附件读取／完整覆盖、secondary命令和undefined-layout丢弃等产品保护继续保留。
两个 trace 参数继续原双链诊断路径，与 --legacy-chain / --fake-swapchain 互斥。
单链默认启用 LocalFiOff；创建 C:\ProgramData\EndfieldXeSSDiag\DisableLocalFiOff 后重启游戏可停用。
原 LocalFiOff 文件仍可为旧路由显式启用；DisableLocalFiOff 文件优先，启动器不创建或删除任何诊断文件。
游戏 FSR3 帧生成菜单 ON 触发 XeSS；完整最终画面降采样后使用 NONE 模式，UI 参与插值。
输入或能力不满足时回退全分辨率；实际低分状态、源/输出尺寸、区域及回退原因以 continuous_present 帧日志为准。
RTSS 帧率显示保留；仅此启动子进程隔离 RTSS Vulkan 层，全局设置不变，默认及纯观察入口不请求此隔离。
实际输入、时序和双 gate 抑制条件不满足时，XeSS 保持 OFF。
核心准入失败时保留游戏原生路径，原生帧生成设置仍由游戏决定。
XeSS 是否真正生效，以本次实际 SDK 插帧和呈现记录验证，不能仅依据开关名称。

启动器可选择本地 Endfield.exe；游戏路径由可信辅助脚本经系统授权保存，
每次启动仍检查完整游戏身份。旧固定目录通过校验时升级自动迁移。
设置仅下次启动生效：2×／3×／4×、免虚化、场景切换阈值。
默认阈值不调用 SDK setter；高级调试只对一次启动请求有效。
“替换30帧档上限”默认关闭；旁边输入30–120的整数（默认40，例如45、50）。
游戏菜单仍选择30，层将本局 Unity 的30限帧请求映射为所选值；60及其他档不变。
输入是基础帧上限，显示帧数约为基础帧乘以插帧倍率；实际以本局日志和上屏统计为准。
调试显示会改变输出口径，不能用于正式功耗或阈值成功率对比。
详细使用和待验收范围见 docs/codex-launcher-settings-guide-2026-10-24.md。
本包由本机源码构建，只支持经过固定哈希校验的游戏版本。
EFU 的许可证和上游固定版本源码链接位于 mod/EFU-LICENSE.txt 与
mod/EFU-SOURCE-NOTICE.txt。免虚化指近距离角色淡出/抖动透明，
不是关闭所有景深或运动模糊。
完整对应源码和嵌入字体的许可证保留在 efu-corresponding-source 目录。
'@
$readme = $readme.Replace('@EFU_VERIFICATION@', $efuVerificationSentence)
if ($Fsr4XmxDirectory) {
    $apiNotice=Get-Content -LiteralPath (Join-Path $project 'third_party/ffx-api/api/include/ffx_api.h') -TotalCount 21
    [IO.File]::WriteAllText((Join-Path $package 'FFX-API-NOTICE.txt'),(($apiNotice -replace '^// ?', '') -join [Environment]::NewLine),$utf8)
    $readme = @'
FSR4 XMX Lite 首版（本地实验候选，尚未实机验收）
启动器“超分方案”只提供 XeSS / FSR4 XMX Lite 两项，下次启动游戏生效。
FSR4 固定使用查询得到的 4.1.2X Lite，不提供其他模型；帧生成继续使用 XeSS。
游戏内仍选择 AMD FSR3 及放大档位，以提供原有输入和菜单控制。
首次短测建议 1080p、平衡档、帧生成关闭；先确认超分，再开启补帧。
默认不启用诊断日志或监控面板。此候选不会自动安装或更新 GitHub 发行版。
已通过 B390 上真实 D3D12 执行和 1080p/1440p Vulkan 往返测试；
游戏中的画质、帧生成组合、切菜单/分辨率/退出及功耗仍需验收。
XMX DLL 来源为用户提供的本地包，公开分发前另行核对其授权与来源。

'@ + $readme
}
$developmentMode = if ($WithXeFGSdk) {
    '默认 EXE 保持 EFU 免虚化直启；显式 --xefg 使用随包的 Intel 原版 SDK，默认单链、LocalFiOff 和低分辨率输出。' + "`r`n" +
    '--xefg --full-res-output 保留全分辨率基线；菜单 ON 才触发 XeSS，实际输出尺寸与回退见帧日志。RTSS 帧率显示保留。'
} else {
    '显式 --observe-inputs 只请求输入诊断；--xefg 实验 XeSS 2× 需要另行安装带 SDK 的载荷包。'
}
$readme = $readme.Replace('@DEVELOPMENT_MODE@', $developmentMode)
[IO.File]::WriteAllText((Join-Path $package 'READ-ME.txt'), $readme, $utf8)
$record = [ordered]@{ createdAt=(Get-Date).ToString('o'); packageDirectory=$package; payloadSnapshotDirectory=$PayloadPackageDirectory; files=$files
    singleChainPolicyVersion=1
    correspondingEfuSourceDirectory=$sourceSnapshot; correspondingEfuSourceFiles=$correspondingSourceFiles
    installPerformed=$false; gameLaunched=$false; officialLauncherModified=$false
    standaloneLaunchPolicy='independent_efu_preview'; officialGameStartRequired=$false
    officialFallbackFrontendSwitch='--official'; efuSourceCommit='eff32bbc89baaf25fcb25f2a8903990e172f319e'; efuLocallyBuilt=$true
    efuSourceModified=$true; efuLocalPatchId='camera-minhook-launcher-options-v3'; minhookCommit='c3fcafdc10146beb5919319d0683e44e3c30d537'
    researchFeaturesAvailable=$false; efuEnabledByLaunchRoute=$true; observerEnabledByLaunchRoute=$false
    observeInputsAvailable=$true; observeInputsFrontendSwitch='--observe-inputs'; observeInputsHelperSwitch='-ObserveInputs'
    observeInputsLaunchPolicy='independent_efu_observe_inputs'; observeInputsEnabledByDefault=$false; observeInputsGameVerified=$false
    efuPriorVerification=$priorVerification; efuTestedInThisCandidate=$false; gameTested=$false
    gameXeFGIntegrated=$false; gameGeneratedFramesVerified=$false
    rawColorCaptureEnabledByDefault=$false; developmentCaptureFrontendSwitch='--capture-rawcolor-once'
    xeFGSdkPackaged=[bool]$WithXeFGSdk; xeFGSdkVersion=if ($WithXeFGSdk) { '3.0.2' } else { $null }
    fsr4XmxLite=[ordered]@{packaged=[bool]$Fsr4XmxDirectory;model='4.1.2X Lite';gameVerified=$false;publicRedistributionReviewed=$false;selection='startup_only'}
    presentationOffEnabledByDefault=$false; presentationOffFrontendSwitch='--presentation-off'
    presentationOffGameVerified=$false
    xeFGFrontendSwitch='--xefg'; xeFGExperimental=$true; xeFGEnabledByLaunchRoute=$false
    wsiAcquireTrace=[ordered]@{ available=[bool]$WithXeFGSdk; enabledByDefault=$false
        frontendSwitch='--xefg --trace-acquire'; helperSwitch='-XeFG -LowResOutput -WsiAcquireTrace'
        childEnvironment='XESS_WSI_ACQUIRE_OBSERVER=1'; observationOnly=$true; gameVerified=$false }
    fakeSwapchain=[ordered]@{ available=[bool]$WithXeFGSdk; enabledByDefault=$false; defaultForXeFGFrontend=[bool]$WithXeFGSdk
        frontendSwitch='--xefg'; compatibleFrontendSwitch='--xefg --fake-swapchain'; legacyFrontendSwitch='--xefg --legacy-chain'
        helperSwitch='-XeFG -LowResOutput -FakeSwapchain'
        childEnvironment='XESS_FAKE_SWAPCHAIN=1'; localFiOffDefault=$true
        localFiOffDisableFile='C:\ProgramData\EndfieldXeSSDiag\DisableLocalFiOff'; gameVerified=$false; singleSwapchainVerified=$false }
    xeFGLowResOutput=[ordered]@{ available=[bool]$WithXeFGSdk; requestedDefaultOff=$false; defaultForXeFGFrontend=[bool]$WithXeFGSdk
        frontendSwitch='--xefg'; legacyFrontendSwitch='--xefg --low-res-output'; fullResFrontendSwitch='--xefg --full-res-output'
        helperSwitch='-XeFG -LowResOutput -FakeSwapchain'; uiModeRequested='NONE'
        gameVerified=[bool]$WithXeFGSdk; benefitsVerified=[bool]$WithXeFGSdk; verificationEvidence='docs/codex-handoff-2026-10-17.md' }
    xeFGRtssVulkanLayerDisableRequested=[bool]$WithXeFGSdk; rtssGlobalSettingsChanged=$false; rtssCompatibilityFixed=$false
    continuousOffRouteAvailable=[bool]$WithXeFGSdk; continuousOffLaunchPolicy='independent_efu_continuous_off'
    gameOnAdmitted=[bool]$WithXeFGSdk; gameOnVerified=$false; gameVisualsVerified=$false; gameNormalExitVerified=$false
    continuousOffGameVerified=$false
    developmentCaptureRequiresSessionProcessArmMarker=$true }
[IO.File]::WriteAllText((Join-Path $package 'package.json'), ($record | ConvertTo-Json -Depth 6), $utf8)
$record | ConvertTo-Json -Depth 6
