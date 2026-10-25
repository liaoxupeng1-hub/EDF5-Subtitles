# EDF5 Subtitles — EDF6-style display

《地球防卫军5》PC 版字幕显示插件。基于 [vixen256/subtitles](https://github.com/vixen256/subtitles) 的原字幕项目重建，修复部分带后缀的语音没有字幕的问题，并通过游戏实际语音播放状态控制字幕保留与消失。

当前源码版本：**1.2.0-test2**。用户已确认当前安装能够正常运行，长播报显示正常，暂时没有需要调整的地方。

## 更新内容

- 完整字幕 ID 优先匹配；找不到时识别末尾 `_S`、`_E`、`_R` 并回退到基础 ID，补上旧版漏掉的 `_S`。原表明确标记 `***no_subtitle***` 的条目继续保持无字幕。
- 跟踪每一次实际语音播放实例；语音有效期间保留字幕，不按字数估时，也不设置旧版的固定 9 秒上限。同名语音、连续语音及重叠语音分别管理。
- 语音结束后默认保留 1 秒，再用 1 秒淡出。暂停期间冻结字幕计时；字号、结束后保留时间和淡出时间可配置。
- 参考 EDF6 已核实的布局规则：画面下部居中、半透明黑色圆角字幕框、自动换行、字幕堆叠移动、长文本裁剪与滚动，以及随分辨率缩放。
- 支持本次核实的两个 EDF5 可执行文件版本；加载前核对 SHA-256，未知版本不安装钩子。
- 外置字幕表与字体保持独立。各语言包只需要替换显示插件和显示配置，原加载器、字幕文本和语音资源可继续沿用。

这是参考 EDF6 行为制作的 EDF5 兼容实现。原生 EDF6 的字体、字号及内部行距尚未完全恢复；当前使用原语言包的 `font.ttf`，默认基准字号为 32。源码中没有移植或分发 EDF6 的游戏 DLL。

## 使用所需文件

先按原字幕 MOD 的说明安装 EDF5 ModLoader 和对应语言包，再使用本工程构建的 `subtitles.dll` 替换旧插件。加载器需要正常安装；仅复制字幕 DLL 不能代替加载器。

```text
EDF5.exe
winmm.dll                       # 原 ModLoader
ModLoader.ini                   # 原加载器配置
Mods/
  Plugins/
    subtitles.dll               # 本工程构建输出
    subtitles.toml              # tools/subtitles.toml
    font.ttf                    # 原语言包字体
  MISSION/
    EDF6_VOICETABLE.SUBTITLE.SGO  # 原语言包字幕表
```

配置示例见 [tools/subtitles.toml](tools/subtitles.toml)。`diagnostics = true` 时在插件目录生成 `subtitles-test.log`，可改为 `false` 关闭诊断。

## 构建

需要 Windows x64、PowerShell 和 MSYS2 UCRT64 的 `g++`。第三方依赖源码已随仓库保留，普通克隆即可构建，无需另取子模块。

在项目根目录运行：

```powershell
# g++.exe 已在 PATH 时
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1
# 或指定自己安装的 UCRT64 bin 目录，例如
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 -Toolchain 'C:\msys64\ucrt64\bin'
```

输出为 `build/subtitles.dll`。脚本同时构建并运行不依赖游戏资源的自动检查。仅在用户自行提供本地文件时执行真实游戏文件检查，缺少可选输入时会明确显示 `SKIP`。

可选输入（不会随仓库提供）：

| 输入 | 用途 |
| --- | --- |
| 环境变量 `EDF6_TEST_MISSION_DIR` 与 `EDF5_TEST_SUBTITLE_TABLE` | 六份 EDF6 各语言/默认语音表与一份 EDF5 外置字幕表检查；需要两项同时提供 |
| 环境变量 `EDF5_TEST_GAME_EXE`、`EDF5_TEST_OTHER_EXE` | 可分别指定两个已知版本 EXE，验证实际版本保护；不启动游戏 |
| `build/preview/EDF6_VOICETABLE.SUBTITLE.SGO` | 实际钩子代码的模拟语音捕获、嵌套及并发检查；不注入游戏 |

已核实的 EDF5.exe SHA-256：

```text
6C2E54FE48EAD4FDC80B1EC34733E52044BDA3DF8E2FC0AFC9E6F6A93F23F7F6
3512D2A2E61D532C5D12DC0C5FD1AC61F6743E13349AE21301BB8449DD4BAE5F
```

## 源码结构与验证边界

- `Source/Runtime.cpp`：游戏版本保护、语音播放实例捕获及播放状态查询。
- `Source/SubtitleTable.cpp`：外置 DSGO 字幕表读取、UTF-16 转换及 ID 匹配。
- `Source/SubtitleModel.h`：逐条字幕生命周期与参考布局。
- `Source/Overlay.cpp`：DX11 绘制与 ModLoader 入口。
- `tests/`：时序、字幕表、加载保护、实际钩子模拟及离屏预览源码。
- `docs/`：不含游戏二进制的播放/布局静态依据。

此前离线检查覆盖七份实际字幕表、畸形数据、长播报模拟、同名与重叠语音、暂停、渐隐、并发捕获、两个 EXE 版本及多分辨率离屏绘制。用户实测确认当前安装及长播报正常。五个语言压缩包已核对只有显示 DLL 和显示配置被替换，其他文件解压后的字节保持一致；这不等于五种语言均已逐一游戏实测。

尚未验证所有关卡、所有暂停/重试场景、所有 MOD 组合及声音开始的采样级同步。缺少实际播放 ID 的成功语音请求会记录诊断，不会用猜测时长冒充同步字幕。

## 来源与许可

原字幕项目：[vixen256/subtitles](https://github.com/vixen256/subtitles)；原 MOD 页面：[Nexus Mods](https://www.nexusmods.com/earthdefenseforce5/mods/229)。本项目代码沿用 [Unlicense](LICENSE)，第三方代码遵守其各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

仓库只提供源码、配置、构建脚本和说明。游戏文件、字幕表、语音、字体、ModLoader 二进制及语言发布包需自行从合法安装或对应原发布页取得。
