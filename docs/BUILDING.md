# 构建说明

普通用户直接使用 Release 的 `*-win64.zip`。源码面向 Windows x64，主体为 C++20，构建与安装脚本使用 PowerShell 7。

## 模组与启动器

在 PowerShell 7 x64 中进入源码根目录：

```powershell
./scripts/fetch-toolchain.ps1
./scripts/fetch-sdks.ps1
./scripts/build.ps1 -ReleaseMinimal
```

工具链和 SDK 由 `toolchain.lock.json`、`dependencies.lock.json` 固定下载地址与 SHA-256；下载后会验证。请阅读 Intel SDK 及其他第三方许可。`third_party/minhook` 与 `third_party/streamline_headers` 保留对应原始许可证。

启动器的图标与清单已在 `assets/` 中提供，普通构建不需要额外的图形库。只有重新生成图标时才需使用 Python、Pillow 和 `scripts/generate-launcher-icon.py`。

默认精简发行配置：`Release`、`XESS_RELEASE_MINIMAL=ON`、`XESS_RESEARCH_DIAGNOSTICS=OFF`、`XESS_DEVELOPMENT_CPU_DIAGNOSTICS=OFF`。模组 DLL 输出到 `build-release/observer/`，启动器为 `build-release/EndfieldXeSSLauncher.exe`。RTSS SDK 仅用于可选独立工具，未安装时相关目标不会创建；普通发行功能不依赖 RTSS。

构建完成后，可使用锁定工具链内的 `ctest.exe --test-dir build-release --output-on-failure` 运行注册测试。包含需要实际 GPU 的测试，执行前退出游戏。没有对应 GPU／驱动的结果不代表图形兼容性验收。

## EFU 和发行打包

EFU 是独立的可执行程序，使用 Zig 构建。完整的已修改对应源码、原始许可及来源清单在二进制 Release 内的 `efu-corresponding-source/`。使用该源码记录的 Zig 0.16.0 Windows x86_64，在对应源码目录执行：

```powershell
zig build -Doptimize=ReleaseSmall
```

现有 `scripts/stage-mod-launcher.ps1` 使用固定开发工作目录 `out/efu-launcher-options-20261023/source/` 中的 EFU 及对应源码。重新打包前，需要把上述对应源码放到该位置并完成 EFU 构建，再执行：

```powershell
./scripts/package-release.ps1 -BuildDirectory build-release -Version YOUR-VERSION
```

打包还需要许可证中记录的构建工具／依赖文件可用。历史研究脚本可能依赖本机旧的 `out/` 取证材料，不属于全新检出即可执行的用户安装入口。发行 ZIP 内的 `Verify.cmd` 检查载荷哈希；`Install.cmd` 执行正常系统授权安装。

不能承诺不同机器上的重新编译逐位相同。发布文件是否对应某次已验收构建，应核对随 Release 提供的 SHA-256 和 `BUILD-INFO.json`。
