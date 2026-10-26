# 终末地性能录制器

需要 64 位 Windows、.NET Framework 4.x 和已安装的 **PowerShell 7**。
GUI 固定使用 `%ProgramFiles%\PowerShell\7\pwsh.exe`，检查文件存在且主版本为 7；缺失会报错，不会回退到 Windows PowerShell 5.1。
后端及测试仅接受 PowerShell 7 Core，子脚本复用当前进程的实际 `pwsh.exe` 路径。

在 PowerShell 7 中构建：

```powershell
& ./tools/recorder/build.ps1
```

构建结果位于 `dist/EndfieldRecorder`。双击 `EndfieldRecorder.exe`，先选本局标签、小米性能模式（用户自选）和可选备注，再手动开始、停止并保存。已有九项采集开关、降级、会话匹配、哈希清单及离线恢复均保留；不会自动启动游戏或 RTSS。

正常游玩对比选“长时间评估”：PMDP 上屏帧、硬件、前台、电源和测量窗口汇总，逐帧测量默认不保存。标签可选同步、K2、K1、同步+共享、K2+T3.3、T4观察、T4阶段1、官方对照、自由游玩；英文短码进入目录名，标签及备注进入 request / summary。T4 观察时另勾选 Observer / EFU 诊断日志，以保留 shader 归档及 SR 观察流。录制器不会开启游戏内诊断或替换超分。

游戏关闭后，可在“下一局路线”选择同步、K2、K1、同步+共享并保存请求。这里只管理 ProgramData/EndfieldXeSSDiag 中 SyncCopyPath、AsyncCopyFrames、SharedCopySync 三个文件；游戏运行时拒绝修改，前后状态进入日志。SyncCopyPath 优先级最高；其他已有禁用标记只读，不会自动清除。2026-10-20 起新版单链默认同步+共享、T3.3 开启；K1/K2 分别显式写入 AsyncCopyFrames=1/2。DisableDepthComputePath 可以关闭 T3.3。同步+共享须安装支持它的新版 DLL，界面显示的是下一局请求，实际路线以测量流为准。

录制中按 **Ctrl+Shift+F9** 写一个手动标记并响短音。可选“记录 M / Esc”只保存游戏前台这两个键的 keydown，其他键不保存，停止即卸载；热键冲突、丢标记或关闭不完整会明确提示。markers.jsonl 保存序号、QPC 和 UTC，summary.md 按标记分段。按键只表明用户操作，不自动推断地图、菜单或加载持续时间。

开始和停止各保存一次只读配置快照（小于等于64字节标记的内容、安装ID、receipt/DLL SHA256、启动器命令行及测量流实际路线）；每10秒记录 AC/电池、电量、电源计划及有效 overlay。小米模式为用户选择，不冒充硬件自动检测。

保存后的 **summary.md** 可直接发给审核者：全程及每个标记段列出显示间隔 p50/p90/p99/max、>25/40/100ms 次数和每分钟率、legacy msUntilDisplayed、功耗/温度、等待和成功提交的复制命令。主统计只采用完整测量窗口证明的补帧开启且输入有效区间；关补帧、输入中断、语义 reset 和未知区间的时长另列。LongFrame / Followup 的卡顿补救 reset 不会把该卡顿从样本中删除；相关 SDK warning 单列为回退。不能辨认地图/菜单/加载时明确记为未知，不猜。旧 DLL 没有完整窗口证明时仍保留原始统计，但不会生成伪造的稳态结果。SDK 内部复制不可观测，保留 null。

30分钟体积目标约100MB以内：当前夹具及已有数据的投影约72MB，仍需真实长局验收；选逐帧明细、独立raw或SR观察归档会增加体积，不计入该投影。

`status.json`、`summary.json`、硬件/前台及系统采集器报告保存 `powerShellRuntime`：实际可执行文件路径、版本、edition 和进程 ID。`build-manifest.json` 记录构建宿主、运行要求、发行文件及全部录制器源文件的 SHA256。

兼容旧记录的前台采样器仍支持 M / Esc 按下、松开轮询，仅在新版可选“记录 M / Esc”开启且前台采集选中时运行；未勾选不会调用按键读取。旧字段位于 `power.jsonl`（同时勾选硬件时）或 `foreground.jsonl` 的 `operationKeyTransitions`，每项保留观测时刻及上一次有效采样的 QPC / UTC。精确 keydown 以新的 markers.jsonl 为准，不重复当成多次操作。

这使用已有的名义 **500 ms 轮询**，变化时间是首次观测到新状态的时刻，实际间隔请看相邻 QPC；短按可能完全漏掉，同轮观测到两键变化时无法判断先后。首次采样、重新回到前台及不可读后的首次采样只建立基线；离开前台或停止时不补造松开事件。`power-run.json` 的 `operationKeyObservation` 和 `summary.json` 的 `sourceStatus.foreground.coverage.operationKeyObservation` 保存采样方式、间隔、变化数及这些限制，原始记录随 ZIP 保留。

详细测量日志和诊断日志仍使用原来的两个开关；新增长时间汇总模式。要同时保存 K1/K2 逐帧测量与 SR 观测，请勾选“详细测量日志”和“Observer / EFU 诊断日志”，并启用完整明细。匹配到图层会话后，明细模式保存文件如下（`0/1` 表示两个轮换文件）：

| 开关 | 图层原文件 | 录制目录中的文件 |
|---|---|---|
| 详细测量日志 | `observer.jsonl.measurements-0/1.jsonl` | `measurements-0/1.stream.jsonl`、`measurements-0/1.snapshot.jsonl` |
| 详细测量日志 | `observer.jsonl.measurements-summary-0/1.jsonl` | `measurements-summary-0/1.stream.jsonl`、`measurements-summary-0/1.snapshot.jsonl` |
| Observer / EFU 诊断日志 | `observer.jsonl.sr-observation-0/1.jsonl` | `sr-observation-0/1.stream.jsonl`、`sr-observation-0/1.snapshot.jsonl` |

完整明细模式持续保存60秒摘要和1秒明细，停止时各保存完整行快照。长时间汇总模式只保留精简的1秒 measurement_status 及60秒汇总，不复制明细或 Stop 历史快照；读取覆盖仍有明确报告。两个 SR 观测文件仅由诊断开关控制且持续保存，即使关闭详细测量日志也保留，避免早期 shader 归档随后续轮换消失。普通 Observer / EFU 日志仍各保存最多2MiB尾部。SR 内容不会用于解析测量模式，也不会开启 SR 观察、GPU 计时或其他游戏诊断。

`measurement-stream-events.jsonl` 与 `sr-observation-stream-events.jsonl` 分别记录流偏移、轮换、积压和读取错误；`window.json` / `summary.json` 分开保存 `measurementStreams`、`srObservationStreams`。每类 source coverage 保存轮换数、未复制尾部字节及可能丢失说明。所有 stream、Stop 快照和轮换证据随清单进入 ZIP，离线恢复也保留这些文件。轮询只复制已完成的行；两次轮询之间已经被覆盖的内容仍可能丢失，不能把结果视为无损追踪。

GUI 从程序同目录读取外部 `record.ps1`，脚本未内嵌到 EXE。发行时须同步脚本；`build.ps1` 会复制脚本和说明并刷新构建清单。

在 PowerShell 7 中运行 CPU 和独立中性进程系统测试：

```powershell
& ./tools/recorder/test-recorder.ps1
& ./tools/recorder/test-recorder-controls.ps1
& ./tools/recorder/test-system.ps1
```

CPU 测试仅在游戏已关闭时检查“无游戏”拒绝分支；游戏正在运行时明确报告跳过该分支。其余测试使用模拟数据及测试自己拥有的进程，不启动游戏或 ETW 会话。

只验证保存、轮换、完整行边界、摘要 / SR 文件及离线 ZIP 恢复，可运行 `test-recorder.ps1 -SaveRegressionOnly`；此模式只使用测试自有文件和进程，不读取真实采集源。

已有保存失败录制可在 PowerShell 7 中离线恢复：

```powershell
& ./recover-recording.ps1 -SourceDirectory '完整录制目录路径'
```

恢复工具只读取保留文件，写入新的 `.recovered` 目录及 ZIP。已存在的恢复结果会拒绝覆盖；缺失的帧、ETW raw 和原进程退出状态不会补造。


纯 XeSS 候选的 SR schema 3 计数只表示命令已录入应用命令缓冲，不代表 GPU 已执行完成或已上屏。`sr_recorded_frames`、`sr_xess_recordings` 等不再沿用旧版 `sr_completed_frames` 的名称。摘要单独输出 `recordingCoverageEligible`；仅有录制计数时 `pureXeSSPowerEligible` 始终为 false，状态为 `recording_only_unverified` 或 `not_eligible`。未建立对应提交、完成与上屏的关联前，不输出 `verified_pure_xess`。旧 schema 保留原字段供核对，不自动升级或认证；原始帧率与功耗仍照常保存。
