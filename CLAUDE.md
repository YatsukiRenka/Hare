# 紫毫 / Hare

[rime/weasel](https://github.com/rime/weasel) 的分支，在小狼毫基础上加**图片皮肤**与**云同步**。Windows 桌面端，自用为主、顺手开源。

被要求「继续开发」时，按此顺序读，然后接着 `docs/ROADMAP.md` 里下一个未完成的阶段做：

1. `docs/CONTINUE.md` — 换机后的完整启动流程：前提、构建、安装、配置同步、验证手法
2. `docs/ROADMAP.md` — 阶段划分、当前进度、各改动的落点
3. `docs/DESIGN.md` — 需求与设计决策，以及每个决策背后的取舍
4. `docs/REVIEW-NOTES.md` — **动云同步代码前必读**，记着哪些「缺陷」经实测判定为不成立，以及反复出现的缺陷模式
5. `docs/BUILD.md` — 工具链与环境陷阱

## 铁律

**不重命名任何源码文件或工程目录。** 仓库里 69 个文件名含 `Weasel`，保持原样。身份独立靠改产物名与字符串，不靠改文件名——重命名会让每次上游合并的冲突解决成本成倍上升。

**新功能写进新文件，在上游文件里只留一个调用点。** 这是这个分支能长期跟上游走的唯一办法。改动落点的选择要参考 `docs/ROADMAP.md` 里的上游改动频率表。

**依赖按代码落点分三级。** 链入 `hare.dll` 的代码只用系统原语；`HareDeployer.exe`、`HareServer.exe` 只可引入 `third_party/` 下、无需运行时 DLL 与构建期包管理器、仅有一个调用点、许可同时兼容 AGPL-3.0-or-later 与 GPL-3.0 的纯数据或自包含算法；仅用于构建和 `worker/` 的依赖不限。加密与 TLS 传输固定使用 CNG、WinHTTP。边界与取舍见 `docs/DESIGN.md` 的「依赖边界」。

**用 merge 而非 rebase 同步上游**，保留改动历史。

## 构建

```powershell
cmd /c tools\build-hare.bat
cmd /c tools\test-cloud-sync.bat
```

**不要直接调用 `build.bat`。** 它需要包装脚本准备的环境，尤其是清除 `NoDefaultCurrentDirectoryInExePath`——这个变量会让 cmd 拒绝从当前目录解析批处理，导致 `call env.bat` 失败，报错信息极具误导性。`test-cloud-sync.bat` 不调用上游构建脚本，也不删除构建日志。首次构建的准备步骤见 `docs/BUILD.md`。

## 与官方小狼毫的关系

两者设计为可并存：TSF GUID、注册表路径、可执行文件名、安装目录全部独立。

但**用户目录与配置文件名刻意保持一致**（`weasel.yaml`、`weasel.custom.yaml`、`%APPDATA%\Rime` 语义），这样万象拼音等现成配置可以直接使用。改这部分等于把自己踢出 Rime 的配置生态。

## 许可

紫毫新增的代码按 **AGPL-3.0-or-later** 授权；由 weasel 衍生的部分仍是 **GPL-3.0**，上游代码的许可不因分支而改变。GPL-3.0 第 13 条允许两者组合分发，本项目即为这样的组合作品。

新写的文件请标注 AGPL-3.0-or-later。修改上游文件时不要改动其原有许可声明。

AGPL 的网络条款意味着：把同步服务端部署成网络服务供他人使用，服务端源码必须公开。`worker/` 属于此列。librime 是 BSD-3-Clause，`third_party/argon2/` 是 CC0 / Apache-2.0。
