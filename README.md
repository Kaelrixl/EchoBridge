# EchoBridge

EchoBridge 是 Windows 系统托盘蓝牙音频接收工具。电脑通过 A2DP Sink 接收手机、平板等已配对设备的音频，并用电脑的扬声器或耳机播放。

当前主工程使用原生 C++ 和 Windows Runtime API，发布产物是单个 `EchoBridge.exe`，运行时无需安装 .NET。旧版 C# 工程保存在开发者本机的 `archive/csharp/`，归档目录不会上传到仓库。

## 功能

- 托盘菜单中查看、连接和断开已配对的蓝牙音频设备；最多同时连接 4 台。
- 刷新设备列表、断开全部连接。
- 启动时自动重连上次连接的设备，并可设置开机自启动。
- 打开 Windows 蓝牙设置。
- 根据显示器的缩放比例绘制托盘菜单。

## 使用方法

1. 在 Windows 蓝牙设置中配对音频发送设备。
2. 从 [Releases](https://github.com/Kaelrixl/EchoBridge/releases/latest) 下载最新原生版的 `EchoBridge.exe`，双击运行。程序启动后显示在系统托盘，不弹出主窗口。`v1.0.0` 是需要 .NET 的历史 C# 版。
3. 右键托盘图标，打开「设备」子菜单并选择设备。连接数达到 4 台时，先断开一台再连接其他设备。
4. 在手机或平板上选择电脑作为蓝牙音频输出目标。

程序会在 EXE 同级目录生成 `EchoBridge.json` 保存偏好；删除该文件可重置配置。请把 EXE 放在有写入权限的目录中。

运行环境：Windows 10 版本 2004（Build 19041）或更新版本、支持 A2DP 的蓝牙适配器。不需要 .NET 桌面运行时。

## 从源码构建

构建机需要 Visual Studio 2022 C++ 构建工具、Windows 10/11 SDK 和 CMake；运行机不需要这些开发工具。

```powershell
pwsh -File .\build.ps1
```

产物位于 `build/out/Release/EchoBridge.exe`。不安装本地构建工具时，可以在 GitHub 仓库的「Actions」页面下载最新成功构建的 `EchoBridge-win-x64` 产物。

项目结构：

```text
.github/workflows/  Windows 构建与产物上传
Assets/             程序图标
include/            C++ 头文件
resources/          Windows 资源与应用清单
src/                C++ 源文件
CMakeLists.txt       CMake 工程入口
build.ps1           本地构建入口
```

`archive/`、`build/`、`out/` 是本地目录，已被 Git 忽略。

## 常见问题

**设备列表里没有设备？** 先确认设备已经在 Windows 蓝牙设置中完成配对，再点击托盘菜单的「刷新设备」。

**连接后没有声音？** 在发送设备上将音频输出切换到电脑，并检查 Windows 当前播放设备。必要时点击「断开全部」后重新连接。

## 致谢与许可

核心思路受到 [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 的启发。EchoBridge 使用 Windows Runtime 的 `AudioPlaybackConnection` API，未复用该项目代码。

本项目采用 [MIT 许可证](LICENSE)。
