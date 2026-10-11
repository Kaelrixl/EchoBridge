# EchoBridge

**让 Windows 电脑接收手机、平板的蓝牙音频，并通过电脑的扬声器或耳机播放。**

EchoBridge 是常驻系统托盘的 A2DP Sink 工具。当前版本使用原生 C++ 编写，提供单个 `EchoBridge.exe`，双击即可运行，无需安装 .NET 桌面运行时。

> [!NOTE]
> 这是原生 C++ 版。`v1.0.0` 是需要 .NET 的历史 C# 版；旧源码保存在开发者本机的 `archive/csharp/`，不随当前仓库上传。

## ✨ 功能

- **托盘操作**：查看、连接和断开已配对的蓝牙音频设备，刷新列表或一键断开全部。
- **多设备连接**：程序最多同时管理 4 台连接；实际音频表现也取决于 Windows 和蓝牙设备。
- **自动重连**：可在启动时按保存的顺序逐台尝试连接上次使用的设备。
- **开机自启动**：可从托盘菜单开启或关闭。
- **适配缩放**：菜单会根据所在显示器的缩放比例绘制。

## 📋 运行要求

| 项目 | 要求 |
| --- | --- |
| 系统 | Windows 10 版本 2004（Build 19041）或更新版本；发布的 EXE 为 x64 |
| 蓝牙 | 支持 A2DP 的蓝牙适配器；手机、平板等发送设备须先与电脑配对 |
| 额外运行时 | 无需安装 .NET 桌面运行时 |

## 🚀 快速开始

1. 在 **Windows 蓝牙设置**中，将手机或平板与电脑配对。
2. 从 [最新发布页](https://github.com/Kaelrixl/EchoBridge/releases/latest) 下载 `EchoBridge.exe`，放到有写入权限的文件夹。
3. 双击运行，右键点击系统托盘中的 EchoBridge 图标，进入「设备」子菜单并选择设备。
4. 在手机或平板上将蓝牙音频输出切换到电脑，然后播放声音。

> [!TIP]
> 程序没有常驻主窗口。运行后请在任务栏右侧的系统托盘查找图标；图标也可能收在「隐藏的图标」区域。

### 托盘菜单

| 菜单项 | 用途 |
| --- | --- |
| 设备 | 勾选设备连接；再次点击已连接设备可断开 |
| 刷新设备 | 重新扫描可用设备 |
| 断开全部 | 断开当前所有设备 |
| 启动自动重连 | 下次启动时尝试恢复上次连接的设备 |
| 开机自启动 | 随 Windows 登录启动 EchoBridge |
| 蓝牙设置 | 打开 Windows 蓝牙设置 |
| 退出 | 退出程序 |

## 💾 配置与更新

程序在 **EXE 同级目录**保存 `EchoBridge.json`，记录自动重连、开机自启动偏好及设备名单。配置文件在首次保存设置后生成；删除它可恢复默认设置。

更新版本时，先退出旧程序，再用新版 `EchoBridge.exe` 替换原文件，**保留同目录的 `EchoBridge.json`**。如果将程序移到其他目录，请在新位置重新确认「开机自启动」设置。

> [!IMPORTANT]
> 不要直接从 ZIP 压缩包内运行，也不要把 EXE 放到无法写入配置的目录。自动重连依赖已保存的设备名单。

## ❓ 常见问题

**设备列表为空？** 先确认设备已在 Windows 中配对，再点击「刷新设备」。

**显示已连接，但没有声音？** 检查手机或平板是否将音频输出切换到电脑，以及 Windows 当前使用的播放设备。仍无声音时，可只断开并重新连接无声的设备。

**下载后 Windows 提示“已保护你的电脑”？** 当前 GitHub 发布的 EXE 尚未进行代码签名，新下载文件可能触发 SmartScreen 的“无法识别的应用”提示。请先核对下载来源是本仓库的[发布页](https://github.com/Kaelrixl/EchoBridge/releases)，并自行确认文件可信；不要为此关闭系统防护。

**换了新版后自动重连名单不见了？** 检查新版 EXE 是否仍与原来的 `EchoBridge.json` 放在同一目录。

## 🛠️ 从源码构建

构建机需要 Visual Studio 2022 C++ 构建工具、Windows SDK 和 CMake；运行机不需要这些开发工具。

```powershell
pwsh -File .\build.ps1
```

本地产物位于 `build/out/Release/EchoBridge.exe`。也可以在仓库的 [GitHub Actions](https://github.com/Kaelrixl/EchoBridge/actions) 页面下载成功构建的 `EchoBridge-win-x64` 产物；正式使用建议优先下载发布页中的 EXE。

### 项目结构

```text
.github/workflows/  Windows 远程构建
Assets/             程序图标
include/            C++ 头文件
resources/          Windows 资源与应用清单
src/                C++ 源文件
CMakeLists.txt       CMake 工程入口
build.ps1           本地构建入口
```

`archive/`、`build/` 和 `out/` 是本地目录，已被 Git 忽略。

## 📄 致谢与许可

核心思路受到 [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 的启发。EchoBridge 使用 Windows Runtime 的 `AudioPlaybackConnection` API，未复用该项目代码。

本项目采用 [MIT 许可证](LICENSE)。
