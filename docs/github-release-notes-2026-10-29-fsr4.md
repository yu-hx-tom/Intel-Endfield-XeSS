# FSR4 XMX Lite 实验版

本版本在已有 XeSS 超分和 XeSS 帧生成之外，增加 **FSR4 XMX Lite** 超分选项。游戏仍使用 Vulkan；FSR4 在 D3D12 执行后回到 Vulkan，帧生成继续使用 XeSS。启动器中选择超分方案和插帧倍率，保存后在下一次启动游戏时生效。

## 使用方法

1. 解压 `*-win64.zip`，运行 `Verify.cmd`，再运行 `Install.cmd` 并按 Windows 提示授权。系统要求及更新步骤见包内 README。
2. 打开启动器，选择 **FSR4 XMX Lite** 或 **XeSS**。首次使用建议选择 2× 帧生成。
3. 游戏内“画质提升”选择 **AMD FSR3**，超分档位照常选择；需要插帧时开启 **FSR3 Frame Generation**，由 XeSS 帧生成接管。

## 验证范围

在 Core Ultra X7 358H／Arc B390 核显及 Intel 32.0.101.8860 驱动上，FSR4 与 XeSS 2× 帧生成、菜单和超分切档已进行短时实机验证。其他设备、倍率和长时间稳定性尚未充分验证；FSR4 功耗波动、长帧和补帧短暂单输出的问题仍在排查。实际效果及性能因设备和游戏场景而异。

## 免虚化来源

免虚化使用 [DynamiByte/Endfield-Uncensored](https://github.com/DynamiByte/Endfield-Uncensored)（EFU），采用 [AGPL-3.0](https://github.com/DynamiByte/Endfield-Uncensored/blob/eff32bbc89baaf25fcb25f2a8903990e172f319e/LICENSE.txt)。本项目基于提交 `eff32bbc89baaf25fcb25f2a8903990e172f319e` 修改并独立编译 EFU，启动器以独立进程调用。对应修改源码在发行包 `efu-corresponding-source/`；许可和修改说明在 `mod/EFU-LICENSE.txt` 与 `mod/EFU-SOURCE-NOTICE.txt`。

FSR4 XMX 模型来自第三方整合包，模型 DLL 的公开再分发许可尚未核实；AMD FFX API 头文件的 MIT 许可不代表模型 DLL 使用相同许可。XeSS、MinHook 等其他组件的说明见发行包 `THIRD-PARTY-NOTICES.txt` 和 `licenses/`。

这是非官方实验版，不包含游戏程序或游戏资源。程序未签名，请校验随包提供的 SHA-256。
