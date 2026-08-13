# 紫毫 / Hare

Windows 平台的 Rime 输入法，[小狼毫（rime/weasel）](https://github.com/rime/weasel)的分支，加了两样官方不做的东西：**图片皮肤**与**端到端加密的云同步**。

名字取自制笔的兽毛——小狼毫用狼毫，鼠须管用鼠须，紫毫即兔毫，白居易《紫毫笔》「尖如锥兮利如刀」。

## 现状

云同步已可用，图片皮肤尚未开始。

| 功能 | 状态 |
|---|---|
| 与官方小狼毫并存安装 | 可用 |
| 词频云同步，四种后端 | 可用 |
| 端到端加密 | 可用 |
| 云同步设置面板 | 可用 |
| 定时同步 | 可用 |
| 图片皮肤与皮肤面板 | 未开始 |
| 独立用户目录与数据导入 | 未开始，目前靠注册表 `RimeUserDir` 手工分开 |
| 符号候选（序号、箭头、符号键） | 未开始 |
| 配置与插件面板 | 未开始 |
| 自动更新指向本项目 | 地址已改，签名与发布流水线未做 |

尚无发布版本。想用得自己编译，见 [docs/BUILD.md](docs/BUILD.md)。

## 云同步

Rime 自带把用户词库导出为快照、再合并其他机器快照的机制。紫毫不替换它，只在前后各接一段，把快照搬到云端：

```
拉取远端快照  →  Rime 自行合并  →  上传本机快照
```

词频合并是取较大使用次数，可交换且幂等，**不存在冲突**，因此没有冲突解决界面这种东西。

**四种后端，同时只启用一种：**

| 后端 | 要填的东西 |
|---|---|
| 本地目录 | 一个路径，配合任意网盘的同步文件夹 |
| S3 兼容存储 | endpoint、桶名、Access Key ID、Secret Access Key |
| WebDAV | 服务地址、用户名、应用密码 |
| Worker 代理 | Worker 网址、访问口令 |

S3 后端在 Cloudflare R2 上验证过，也适用于 MinIO、Backblaze B2 等；WebDAV 在坚果云上验证过。[`worker/`](worker/) 里的 Cloudflare Worker 是为了把配置从四项压到两项——它自己持有 R2 凭证，用户不必接触。

后端、凭证、主密码、同步间隔都在设置面板里填。三个入口：托盘图标与语言栏按钮的右键菜单「云同步设置」，开始菜单的「【紫毫】云同步设置」，或 `HareDeployer.exe /settings`。**主密码没有命令行入口**——命令行对同机其他进程可见。

定时同步挂在常驻的 `HareServer` 上，默认每小时一次并在启动时同步一次；它每次到点都会重读设置，改了间隔不必重启。

**加密**。词库记录了用户输入过的每一个词，因此在离开本机前就已加密：

- 32 字节随机数据密钥（DEK）用 AES-256-GCM 加密快照
- 主密码经 Argon2id（m=64MB, t=3, p=1）派生密钥来包装 DEK，包装结果存在云端
- 新设备只需知道主密码；解开后 DEK 用 DPAPI 缓存本机，之后无人值守同步不再需要密码
- 换密码只重新包装 DEK，不必重加密历史数据

选内存硬的 Argon2id 而非 PBKDF2，是因为这里的攻击是**离线**的：拿到存储读权限的人可以把包装后的密钥取走慢慢爆破，无法限速也无法察觉。同等硬件下 Argon2id 每次猜测的代价比 PBKDF2 高两到三个数量级。

主密码要求最短 10 位，不强制字符类型组合——组合规则会把人推向可预测的替换写法，实测有效熵反而更低，这也是 NIST SP 800-63B 的现行建议。

## 与官方小狼毫的关系

两者设计为可并存：TSF GUID、注册表路径、可执行文件名、安装目录、IPC 通道全部独立。

**配置文件名保持一致**（`weasel.yaml`、`weasel.custom.yaml`、`%APPDATA%\Rime` 语义），因此万象拼音、雾凇拼音这类现成配置直接可用。

但**不要让两者共用同一个用户目录**，代价有两条：

- `installation.yaml` 只记得住一个发行版及其 librime 版本，两者版本不同，于是每次切换发行版，另一个启动时都判定配置变了并重建全部词典——重建期间没有可用词典，只能输入英文
- 用户词库是 LevelDB，开启时持排他锁，共用目录时同一时刻只能运行一个

各用各的目录就没有这两条：注册表 `HKCU\Software\Rime\Hare\RimeUserDir` 指向紫毫自己的目录，把方案、词典、`*.custom.yaml` 复制过去即可；用户词库要经 `*.userdb.txt` 快照导入，**不要复制 `*.userdb` 那些 LevelDB 目录**——它们的格式与 librime 版本绑定。这套动作正在做成设置面板里的导入功能，见 [docs/ROADMAP.md](docs/ROADMAP.md) 阶段七。

## 构建

```powershell
pwsh tools\install-deps.ps1       # 预编译 librime、Boost 源码、WebView2 SDK、共享数据
cmd  /c tools\build-boost.bat     # 编译 Boost 静态库
cmd  /c tools\build-hare.bat      # 构建，产物在 output\
cmd  /c tools\build-hare.bat installer   # 另外打 NSIS 安装包
```

需要 Visual Studio 的 C++ 桌面开发工作负载，含 ATL 与 MFC。**不要直接调用 `build.bat`**——它依赖包装脚本准备的环境。详情与几个环境陷阱见 [docs/BUILD.md](docs/BUILD.md)。

设置面板用 WebView2：SDK 只取头文件与静态加载器，产物旁边不多一个 DLL；运行时 Windows 11 自带，Windows 10 上缺失时面板会明说并退出。

## 文档

- [docs/CONTINUE.md](docs/CONTINUE.md) — 新机器上从克隆到跑起来的完整流程
- [docs/DESIGN.md](docs/DESIGN.md) — 设计决策与背后的取舍
- [docs/BUILD.md](docs/BUILD.md) — 工具链与环境陷阱
- [docs/ROADMAP.md](docs/ROADMAP.md) — 阶段划分与当前进度
- [docs/REVIEW-NOTES.md](docs/REVIEW-NOTES.md) — 审查结论，含判定不成立的条目与证据
- [CLAUDE.md](CLAUDE.md) — 给 AI 助手的项目约定

## 上游

本项目基于 rime/weasel，项目主页 https://rime.im 。其他平台的 Rime 发行版：Linux 用 ibus-rime 或 fcitx5-rime，macOS 用【鼠鬚管】，Android 用【同文】，iOS 用【仓】。

## 许可

紫毫新增的代码按 **AGPL-3.0-or-later** 授权，见 [LICENSE](LICENSE)。

由小狼毫衍生的部分仍受 **GPL-3.0** 约束，见 [LICENSE.txt](LICENSE.txt)。上游代码的许可不因分支而改变，紫毫也无权替它重新授权；GPL-3.0 第 13 条允许将 GPL-3.0 作品与 AGPL-3.0 作品组合分发，本项目正是这样的组合作品。

实际影响：分发二进制必须一并提供完整源码；**若把紫毫的同步服务端部署成网络服务供他人使用，服务端源码同样必须公开**——这正是选择 AGPL 而非 GPL 的原因。[`worker/`](worker/) 下的 Worker 属于此列。

第三方组件：

- [librime](https://github.com/rime/librime) — BSD-3-Clause
- [plum](https://github.com/rime/plum) — LGPL-3.0
- [Argon2 参考实现](https://github.com/P-H-C/phc-winner-argon2)（`third_party/argon2/`） — CC0 1.0 / Apache-2.0 双授权

上游其余依赖的许可见 [LICENSE.txt](LICENSE.txt)。
