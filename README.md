# I卡终末地xess超分+帧生成

面向 Intel 显卡的《明日方舟：终末地》独立启动工具，提供 XeSS／FSR4 XMX Lite 超分辨率、XeSS 2×／3×／4×帧生成、可选免虚化和自定义基础帧率设置。

启动器采用紧凑界面，程序、任务栏和托盘使用统一图标。同一 Windows 用户在当前会话中只保留一个启动器实例；再次启动时直接唤出已有窗口，已收起到托盘时也会恢复。

**技术路径：游戏保留 Vulkan 渲染。** XeSS 超分在 Vulkan 内执行；选择 FSR4 XMX Lite 时，通过同一块显卡的共享资源交给 D3D12 执行超分，再回到 Vulkan。帧生成使用 XeSS FG，并通过 D3D12／DXGI 输出。超分方案在启动前选择。

## 环境要求

| 项目 | 要求 |
|---|---|
| 系统 | Windows x64；推荐 Windows 11。XeSS SDK 最低要求 Windows 10 build 19043 或 Windows 11 build 22000，本项目主要在 Windows 11 验证 |
| Intel 显卡 | XeSS FG 支持范围内的 Arc A／B 系列独显，以及兼容的 Core Ultra 核显。具体以设备和驱动初始化结果为准 |
| 驱动 | Intel 驱动 `32.0.101.7029` 或更新版本；建议使用该设备适用的较新正式驱动 |
| 脚本环境 | Microsoft PowerShell 7 **x64**，安装到默认系统目录 `C:\Program Files\PowerShell\7`；Windows PowerShell 5.1 不可替代 |
| 运行库 | Microsoft Visual C++ x64 Redistributable `14.40.33810` 或更新版本 |
| 图形能力 | 支持 Vulkan、D3D12，以及工具所需的同一 GPU 跨 API 共享纹理和同步能力 |
| 游戏 | 与发行包固定模块哈希、发行者及挂接点检查匹配的《终末地》Windows 版本。游戏更新后可能需要重新适配 |

当前反复实机验证的设备是 **Core Ultra X7 358H／Arc B390 核显**。SDK 支持某个系列，不等于本工具已对该系列所有型号完成验证。旧 Iris Xe／UHD 不能因支持 XeSS 超分就被认定支持 XeSS 帧生成；也不承诺其他厂商显卡兼容。

依据：[Intel XeSS 硬件支持表](https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html)、[本项目所用 XeSS SDK 3.0.2 的环境要求](https://github.com/intel/xess/blob/v3.0.2/doc/xess_fg_developer_guide_english.md#requirements)。运行所需的 XeSS DLL 随发行包提供，不需要另装 XeSS SDK、Vulkan SDK 或编译工具。RTSS、PresentMon 和性能录制器也不是使用前提。

## 下载、安装和启动

1. 在本项目 [Releases](https://github.com/yu-hx-tom/Intel-Endfield-XeSS/releases) 下载 `*-win64.zip`，普通用户不用下载 GitHub 自动生成的 Source code。
2. 安装上述 PowerShell 7 x64 和 VC++ x64 运行库，把 ZIP **完整解压**到普通本地目录，不要直接在压缩包内运行。
3. 正常退出游戏、我们的旧启动器，以及托盘中的鹰角官方启动器。
4. 双击 `Verify.cmd` 校验文件，再双击 `Install.cmd`，按正常 Windows 提示授权。安装过程不修改游戏磁盘文件。
5. 双击 `Launch.cmd`。首次使用选择包含 `Endfield.exe` 的游戏目录，保存后点击“启动游戏”。

PowerShell 安装方法见[微软官方说明](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows)。如使用 winget，可手动执行：

```powershell
winget install --id Microsoft.PowerShell --source winget --installer-type wix --architecture x64
```

已有旧版时，先使用旧版 `Uninstall.cmd` 正常卸载，再安装新包；先记录自己的设置。不要直接用新 DLL 覆盖旧安装目录。

## 游戏内怎么设置

- **超分：**启动器选择 XeSS 或 FSR4 XMX Lite，游戏内“画质提升”选择 **AMD FSR3**，再照常选择超分档位。放大档位由启动器选定的超分方案接管；建议首次使用全屏 1920×1080、平衡档。FSR4 需要对应的 XMX 模型文件，当前为实验功能。
- **帧生成：**在游戏里开启 **FSR3 Frame Generation**，实际由 XeSS 帧生成接管；关闭该选项则关闭插帧，所选超分继续运行。不要另外叠加其他插帧工具。
- **倍率：**在启动器选择 **2×、3×或4×**并保存，然后重新启动游戏。倍率不支持游戏内热切换，基础帧率乘以倍率才是目标显示帧率，实际结果受负载和显示器限制。
- **40帧配3×：**勾选启动器里的自定义“30帧档”映射，输入 `40`，选择 `3×`并保存；下次进入游戏时，游戏内限帧选择 **30帧**，该档实际映射为40基础帧，目标约120显示帧。可输入30–120整数；游戏的60帧档不变。
- **免虚化：**由启动器开关选择是否启用；下次启动游戏生效。
- **其他设置：**场景切换阈值先保持默认。首次验证后再按设备性能调整分辨率、画质和倍率。

登录／“点击进入游戏”页保持单帧，正式进入场景后按游戏设置启用插帧。本次修复也会在角色菜单内暂停插帧，退出菜单后自动恢复；所选超分继续运行。FSR4 路线已在本机验证菜单及切档；长时间稳定性、功耗和所有显卡型号尚未充分验证。

## 精简发行版与排错

默认不显示 SR／FG 实时状态面板，不采集运行状态，不启动性能录制，也不写运行日志。启动器仅提供设置、路径检查、启动结果和错误提示。

遇到异常时，先正常退出游戏，在启动器勾选一次“诊断模式”后重新启动，复现后通过“打开诊断日志”取得该局记录；下一次启动诊断会自动关闭。分享日志前检查本地路径等个人信息。

游戏更新后出现“尚未适配”，请等待新适配包；不要删除校验或手工改哈希放行。卸载使用 `Uninstall.cmd`。关闭启动器窗口会隐藏到托盘，点击“退出”才会退出启动器，退出启动器不会关闭正在运行的游戏。

本工具是非官方运行时修改，兼容性及游戏规则风险需自行评估；不保证账号安全。程序未做代码签名，请从本仓库 Release 获取并核对 SHA-256，不要为运行它而关闭系统防护。

## 许可证与源码

免虚化来自 [DynamiByte/Endfield-Uncensored](https://github.com/DynamiByte/Endfield-Uncensored)（EFU），采用 [AGPL-3.0](https://github.com/DynamiByte/Endfield-Uncensored/blob/eff32bbc89baaf25fcb25f2a8903990e172f319e/LICENSE.txt)。本项目调用基于该提交修改并独立编译的 EFU；发行包内的 `efu-corresponding-source/` 提供对应修改后源码，`mod/EFU-SOURCE-NOTICE.txt` 和 `mod/EFU-LICENSE.txt` 记录来源、修改及许可。

Intel XeSS、MinHook 等保留各自许可证与原作者署名；详见发行包内 `THIRD-PARTY-NOTICES.txt` 与 `licenses/`。FSR4 XMX 模型为第三方修改版本，其再分发许可需要单独核实。第三方名称不表示官方认可。

开发与构建说明见[源码构建说明](https://github.com/yu-hx-tom/Intel-Endfield-XeSS/blob/main/docs/BUILDING.md)。本项目没有承诺逐位一致的可复现构建。
