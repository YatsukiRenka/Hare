# 构建

## 一次性准备

```powershell
pwsh tools\install-deps.ps1     # 预编译 librime + Boost 源码 + output\data
cmd  /c tools\build-boost.bat   # 编译 Boost 静态库，约 10 分钟
```

## 日常构建

```powershell
cmd /c tools\build-hare.bat              # x64 + Win32，Release
cmd /c tools\build-hare.bat rebuild      # 全量重建
cmd /c tools\build-hare.bat installer    # 另外打 NSIS 安装包
```

产物落在 `output\`：`HareServer.exe`、`HareDeployer.exe`、`WeaselSetup.exe`、`hare.dll`（Win32 TSF）、`harex64.dll`（x64 TSF）。

**不要直接调用 `build.bat`**，它需要包装脚本准备的环境，见下。

## 云同步核心测试

```powershell
cmd /c tools\test-cloud-sync.bat
cmd /c tools\test-cloud-sync.bat full   # 另加完整 x64、Win32 Release solution build
```

默认入口分别构建并运行 x64、Win32 的 `TestCloudSyncCore`，再定向编译两种架构的 Release `HareDeployer`；`full` 另外构建完整的双架构 Release solution。测试覆盖快照加密封装与名称认证、远端快照路径/重复冲突、S3 前缀规范化与返回键边界、本地条件创建，以及批量文件提交失败时的回滚和临时/备份文件清理。脚本直接调用 MSBuild，不调用会清理日志的上游 `build.bat`。

## 工具链

| 组件 | 版本 / 位置 | 说明 |
|---|---|---|
| Visual Studio | 2026 Enterprise 18.9 | 包装脚本用 `vswhere` 自动定位 |
| 平台工具集 | **v145**（MSVC 14.51） | 见下方「工具集命名」 |
| Windows SDK | 10.0.26100.0 | |
| Boost | 1.91.0，静态，`vc145` 标签 | 只编 serialization 与 thread |
| librime | 预编译，`rime-33e7814` | 不从源码构建 |
| WebView2 SDK | 1.0.4129.50，`deps\webview2` | 由 `install-deps.ps1` 从 nuget.org 取，只留头文件与静态加载器 |

VS 组件要求：「使用 C++ 的桌面开发」工作负载 + **C++ ATL** + **C++ MFC**（组件 ID `Microsoft.VisualStudio.Component.VC.ATLMFC`）。ATL 供 C++ 代码使用，MFC 只为资源脚本提供 `afxres.h`。

## 环境陷阱

这三条与 weasel 本身无关，是构建环境的特性，绕不开。

### `NoDefaultCurrentDirectoryInExePath`

某些 shell 会设置这个变量，cmd 于是不再从当前目录解析批处理。三处会因此失败：`build.bat` 的 `call env.bat`、Boost 的 `bootstrap.bat`、Boost 内部的 `guess_toolset.bat`。症状是 `'xxx.bat' is not recognized as an internal or external command`，极具误导性。

`tools\` 下的包装脚本开头都会清掉这个变量。**这是必须通过包装脚本构建的主要原因。**

### 工具集命名

`MSBuild\Microsoft\VC` 下的目录名是 **Visual Studio 版本号**，不是工具集名。VS2022 对应 `v170`，VS2026 对应 `v180`。而实际的 `PlatformToolset` 值在 `MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\` 下，VS2026 只有一个：**`v145`**。

填错会得到 `MSB8020: 无法找到 v180 的生成工具`。`env.bat` 里 `PLATFORM_TOOLSET=v145`，全部 54 处 vcxproj 通过 `$(PLATFORM_TOOLSET)` 变量引用它，不需要逐个修改工程文件。

### `afxres.h`

资源脚本 `#include "afxres.h"`，该头文件随 MFC 分发，缺失时报 `RC1015`。装上 MFC 组件即可。

不想装 MFC 的话，可以在 `include\afxres.h` 放一个转向 Windows SDK `<winres.h>` 的垫片：资源脚本用引号包含，会搜索 `.rc` 的 `AdditionalIncludeDirectories`，其中含 `$(SolutionDir)\include`。

注意 `INCLUDE` 环境变量对此无效——MSBuild 的 VC targets 会用 `IncludePath` 属性覆盖它，垫片必须落在工程的包含路径上。

### vswhere 与 prerelease

`vswhere` 默认只返回「完整可用」的实例，Visual Studio 2026 被标记为 prerelease，因此**必须加 `-all -prerelease`** 才能查到。漏掉这两个参数会挑中不带 ATL 的 Build Tools 实例，症状是 `atlbase.h` 找不到。

`tools\` 下的脚本不查组件 ID（VS2026 不上报 `VC.ATL`），而是直接检查 `VC\Tools\MSVC\*\atlmfc\include\atlbase.h` 是否存在来选实例。

### MSBuild 文件跟踪

Visual Studio 2026 18.9 的 `Tracker.exe` 在这台机器上会在启动编译器后退出，留下唯一线程处于 `Suspended` 的 `CL.exe`，MSBuild 随后永久等待。`tools\test-cloud-sync.bat` 显式设置 `TrackFileAccess=false` 绕过该跟踪器；这只关闭增量构建的文件访问日志，不改变编译、链接或测试内容。

### 批处理文件必须是纯 ASCII

`tools\*.bat` 中出现非 ASCII 字符（例如中文破折号）会在 OEM 代码页下被拆解，导致 `rem` 注释行被当作命令执行，报出一串莫名其妙的 `'xxx' is not recognized`。

## 依赖细节

### librime

`get-rime.ps1 -use dev` 下载预编译包并铺好 `include\rime_*.h`、`lib\rime.lib`、`lib64\rime.lib`、`output\rime.dll`、`output\Win32\rime.dll`、`output\data\opencc\`。整条 CMake 链因此可以绕开——本项目不修改 librime。

**副作用**：该脚本会杀掉正在运行的 `WeaselServer`。如果机器上装着小狼毫并正在使用，输入法会中断，需要手动重启它。

### Boost

weasel 直接使用 `boost::serialization`（`text_oarchive` / `text_woarchive`）和 `boost::thread`，都需要编译，不是纯头文件。其余 Boost 组件由预编译的 librime 自带，无需再编。

工程通过 `$(BOOST_ROOT)` 取头文件、`$(BOOST_ROOT)\stage\lib` 取库，所以 b2 必须用 `stage` 目标。x64 与 Win32 的库带不同架构标签，可以共存于同一个 `stage\lib`。

上游的 `install_boost.bat` 依赖 `aria2c` 和 `7z`，两者常常缺失，`tools\install-deps.ps1` 改用 curl 下载并自动寻找 7z。

### WebView2 SDK

`install-deps.ps1` 下载 `Microsoft.Web.WebView2` 的 nupkg（就是个 zip），从中取四样东西：`WebView2.h`、`WebView2EnvironmentOptions.h`，以及 x64 与 x86 的 `WebView2LoaderStatic.lib`。x86 那份落到 `deps\webview2\Win32\`，因为 MSBuild 管 32 位叫 `Win32`，工程于是可以无条件写 `deps\webview2\$(Platform)`。

**只取静态加载器**：产物旁边不需要多一个 `WebView2Loader.dll`，安装脚本的文件清单也不必跟着改。它与工程的 `/MT` 静态 CRT 直接兼容，无需额外开关。

运行时（Microsoft Edge WebView2 Runtime）是另一回事，不随本项目分发：Windows 11 自带，Windows 10 上要用户自己装。缺失时 `GetAvailableCoreWebView2BrowserVersionString` 会失败，设置面板据此提示并退出。

### output\data

`build.bat` 检查 `output\data\essay.txt` 与 `output\data\opencc\TSCharacters.ocd*`，缺失则调用 plum 联网拉取词库。从任何已安装的小狼毫的 `data` 目录拷贝过来即可跳过这一步。

## 上游同步

```
upstream  https://github.com/rime/weasel.git
分支      hare
基点      f9203ca
```

用 **merge 而非 rebase**，保留改动历史，冲突时只需处理紫毫碰过的那几处。

改动集中度是可维护性的关键：新功能尽量写进新文件，在上游文件里只留一个调用点。相关的历史数据见 `docs/ROADMAP.md`。
