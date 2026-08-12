# 紫毫 / Hare

[rime/weasel](https://github.com/rime/weasel) 的分支，在小狼毫基础上加**图片皮肤**与**云同步**。Windows 桌面端，自用为主、顺手开源。

先读 `docs/`：

- `docs/DESIGN.md` — 需求与设计决策，以及每个决策背后的取舍
- `docs/BUILD.md` — 工具链与三个必须知道的环境陷阱
- `docs/ROADMAP.md` — 阶段划分、当前进度、各改动的落点

## 铁律

**不重命名任何源码文件或工程目录。** 仓库里 69 个文件名含 `Weasel`，保持原样。身份独立靠改产物名与字符串，不靠改文件名——重命名会让每次上游合并的冲突解决成本成倍上升。

**新功能写进新文件，在上游文件里只留一个调用点。** 这是这个分支能长期跟上游走的唯一办法。改动落点的选择要参考 `docs/ROADMAP.md` 里的上游改动频率表。

**不引入新的第三方库。** HTTP 用 WinHTTP，加解密用 CNG，都是系统自带。已有的依赖只有 Boost、ATL、预编译的 librime。

**用 merge 而非 rebase 同步上游**，保留改动历史。

## 构建

```powershell
cmd /c tools\build-hare.bat
```

**不要直接调用 `build.bat`。** 它需要包装脚本准备的环境，尤其是清除 `NoDefaultCurrentDirectoryInExePath`——这个变量会让 cmd 拒绝从当前目录解析批处理，导致 `call env.bat` 失败，报错信息极具误导性。首次构建的准备步骤见 `docs/BUILD.md`。

## 与官方小狼毫的关系

两者设计为可并存：TSF GUID、注册表路径、可执行文件名、安装目录全部独立。

但**用户目录与配置文件名刻意保持一致**（`weasel.yaml`、`weasel.custom.yaml`、`%APPDATA%\Rime` 语义），这样万象拼音等现成配置可以直接使用。改这部分等于把自己踢出 Rime 的配置生态。

## 许可

本项目是 weasel 的衍生作品，受 **GPLv3** 约束，分发二进制必须提供对应源码。librime 是 BSD-3-Clause。GPLv3 不含网络条款，服务端组件不受传染。
