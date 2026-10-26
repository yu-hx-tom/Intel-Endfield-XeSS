#requires -Version 7.0
#requires -PSEdition Core
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 7){throw 'Build the recorder with PowerShell 7 (pwsh.exe).'}
$powerShellRuntime=[ordered]@{executable=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName;version=$PSVersionTable.PSVersion.ToString();edition=$PSVersionTable.PSEdition;processId=$PID}
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo 'dist/EndfieldRecorder'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$compiler=Join-Path $repo '.tools/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
$csc=Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$requiredPowerShell=Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'PowerShell/7/pwsh.exe'
$include='D:/Programfiles/RivaTuner Statistics Server/SDK/Include'
$inventory=Join-Path $repo 'out/20261016-low-stutter/etw-readonly-inventory.exe'
foreach($path in @($compiler,$csc,$include,$inventory,$requiredPowerShell)){if(-not(Test-Path -LiteralPath $path)){throw "Required build dependency missing: $path"}}
if([Diagnostics.FileVersionInfo]::GetVersionInfo($requiredPowerShell).FileMajorPart -ne 7){throw 'The installed recorder runtime must be PowerShell 7.'}
[void][IO.Directory]::CreateDirectory($OutputDirectory)
& $csc /nologo /target:winexe /platform:x64 /optimize+ /langversion:5 /codepage:65001 /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll ("/out:"+(Join-Path $OutputDirectory 'EndfieldRecorder.exe')) (Join-Path $PSScriptRoot 'EndfieldRecorder.cs') (Join-Path $PSScriptRoot 'RecorderControls.cs')
if($LASTEXITCODE -ne 0){throw 'Recorder GUI build failed.'}
foreach($name in @('pmdp-reader','rtss-poll')){
    & $compiler -std=c++20 -O2 -static ("-I"+$include) (Join-Path $PSScriptRoot ($name+'.cpp')) -o (Join-Path $OutputDirectory ($name+'.exe'))
    if($LASTEXITCODE -ne 0){throw "Recorder helper build failed: $name"}
}
foreach($name in @('record.ps1','RecorderContext.ps1','RecorderSummary.ps1','capture-power.ps1','capture-system.ps1','recover-recording.ps1','SystemSampler.cs','README.md')){
    $text=[IO.File]::ReadAllText((Join-Path $PSScriptRoot $name),[Text.Encoding]::UTF8)
    [IO.File]::WriteAllText((Join-Path $OutputDirectory $name),$text,[Text.UTF8Encoding]::new($true))
}
Copy-Item -LiteralPath $inventory -Destination (Join-Path $OutputDirectory 'etw-readonly-inventory.exe')
$instructions=@'
终末地性能录制器

运行要求：64 位 Windows、.NET Framework 4.x，以及安装到 Program Files\PowerShell\7 的 PowerShell 7。
采集脚本只使用该目录的 pwsh.exe；缺失或版本不符会明确报错，不会回退到 Windows PowerShell 5.1。

1. 双击 EndfieldRecorder.exe，按 Windows 提示授权。
2. 自己启动游戏，进入需要测试的场景。
3. 选本局标签、小米性能模式、可选备注。正常游玩选“长时间评估”，逐帧明细默认关闭；九项采集仍可逐项开关。
4. 点击“开始录制”，看到红色“正在录制”后回游戏测试。
5. 测完点击“停止并保存”，等待显示“保存完成”。
6. 点击“打开输出文件夹”。每次录制都有带标签短码的文件夹、summary.md和同名 ZIP。

录制中 Ctrl+Shift+F9 打点并响短音。可选记录 M / Esc 只保存目标游戏前台这两个键的按下时刻；停止即关闭。
游戏关闭后可选下一局同步/K2/K1/同步+共享；只管理三个白名单开关，不自动开游戏。默认保留同步回退。
同步+共享须安装支持新版DLL，界面只是启动请求，实际路线以测量流确认。
开始和结束保存只读配置及安装哈希，每10秒记录电源计划、AC/电池和overlay；小米模式为用户选择。
summary.md 按标记分段，稳态主统计需要新版图层的完整窗口证据；旧数据无此证据时标未知，不补造结果。

录制由你手动开始、结束，不依赖聊天消息，也不会修改游戏或 RTSS 设置。
点击停止前请先完成游戏操作；工具会记录前台/后台情况。
窗口中的采集项目均可独立开关：
- 独立 PresentMon 完整帧与延迟
- 现有 PMDP 上屏帧
- RTSS 提交帧间隔
- 硬件功耗、温度、频率、显存
- 前台与窗口状态
- 后台 CPU、磁盘、DPC、服务
- 游戏线程起止快照
- 详细测量日志（等待、Present、GPU 时间、复制计数）
- Observer / EFU 诊断日志
预设“常用性能”默认选择 PMDP、RTSS、硬件、前台、详细测量日志。
预设“卡顿排查”选择全部；“长时间稳定性”保留原项目；“长时间评估”使用 PMDP、硬件、前台、电源和精简窗口汇总。
手动修改任何勾选项会切换为“自定义”，其他选项保留。选择保存在工具旁的 recorder-settings.json，重新打开会恢复。
旧版或损坏的设置会回到“常用性能”并在窗口显示警告，不会修改游戏。较小屏幕可滚动选项区查看全部项目。
每次 request.json 都保存本次确定的九项布尔开关；未勾选项不会启动相应采集。
GPU 计时等仅保存游戏已有记录，未开启或缺失会明确标注；工具不会启用游戏内新诊断。
status.json、summary.json 和采集器报告中的 powerShellRuntime 保存实际采集进程路径、版本、Core 版本及 PID。
本机需保留 RTSS 的 PresentMon 2.3.1。缺失的数据源和采集错误会写入结果。
勾选独立 PresentMon 后，ETW 名额不足或独立 PresentMon 启动失败时，会继续保存其他已勾选数据，并注明缺少独立 raw。
已勾选 PMDP 时可复用现有 PMDP；不会停止任何外部 ETW 会话。
现有 PMDP 也可能没有帧数据，请查看 summary.json 中 frameSources 的实际行数和缺失警告。
工具不会自动启动游戏或 RTSS，也不会清理外部 ETW 会话。
保存位置：本工具目录中的 Captures 文件夹。
'@
[IO.File]::WriteAllText((Join-Path $OutputDirectory '使用说明.txt'),$instructions,[Text.UTF8Encoding]::new($true))
$names=@('EndfieldRecorder.exe','record.ps1','RecorderContext.ps1','RecorderSummary.ps1','capture-power.ps1','capture-system.ps1','recover-recording.ps1','SystemSampler.cs','README.md','pmdp-reader.exe','rtss-poll.exe','etw-readonly-inventory.exe','使用说明.txt')
$files=@($names|ForEach-Object{$path=Join-Path $OutputDirectory $_;[ordered]@{name=$_;bytes=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}})
$sources=@(Get-ChildItem -LiteralPath $PSScriptRoot -File|Where-Object {$_.Extension -in @('.ps1','.cs','.cpp','.md')}|Sort-Object Name|ForEach-Object {[ordered]@{name=('tools/recorder/'+$_.Name);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}})
[ordered]@{builtUtc=[DateTime]::UtcNow.ToString('o');powerShellRuntime=$powerShellRuntime;runtimeRequirements=[ordered]@{powerShellMajor=7;powerShellExecutable=$requiredPowerShell;windowsPowerShellFallback=$false;gui='.NET Framework 4.x WinForms'};files=$files;sources=$sources;gameFilesChanged=$false;rtssConfigurationChanged=$false}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $OutputDirectory 'build-manifest.json') -Encoding utf8
Write-Output (Join-Path $OutputDirectory 'EndfieldRecorder.exe')
