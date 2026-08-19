# 设计

紫毫（Hare）是 [rime/weasel](https://github.com/rime/weasel) 的分支，目标是在小狼毫的基础上提供两样官方不会做的东西：**图片皮肤**与**云同步**。当前已具备加密的词库快照云同步及其 WebView2 设置页，图片皮肤尚未实现。

## 定位

自用为主，顺手开源。不做账号系统、不买 Authenticode 代码签名证书、不做多租户。这条定位决定了后面所有的取舍：凡是只为「给陌生用户用」而存在的复杂度，一律不做。

## 许可

| 范围 | 许可 | 边界 |
|---|---|---|
| 从 weasel 继承的文件 | **GPL-3.0** | 上游许可不因分支而改变，修改时保留原许可声明 |
| 紫毫编写的文件（包括 `worker/`） | **AGPL-3.0-or-later** | 文件标明本许可 |
| librime | BSD-3-Clause | 独立依赖，沿用其许可 |
| `third_party/argon2/` | CC0 / Apache-2.0 | 双授权，沿用其许可 |

GPL-3.0 第 13 条允许把 GPL-3.0 作品与 AGPL-3.0 作品组合分发，紫毫即为这样的组合作品；组合不改变各部分的许可，weasel 衍生部分仍按 GPL-3.0，紫毫编写部分仍按 AGPL-3.0-or-later。把 `worker/` 部署成网络服务供他人使用时，AGPL 的网络条款要求公开服务端源码。

## 依赖边界

工程依赖包括 Boost、ATL/WTL、预编译的 librime、WebView2 SDK（只取头文件与静态加载器），以及 `third_party/argon2/` 下随仓库保存的 Argon2 参考实现。依赖边界按代码最终进入哪里判定，不按数量判定。

依赖限制要换取的是两件事：不把额外代码装进别人的进程，也不让每次上游合并都处理新的构建接缝；它不是「第三方代码越少越好」。手写而未采用现成库的代码正是本项目解析缺陷的集中处：S3 与 WebDAV 后端的对象名和路径处理，以及 URL 组件处理。因此，容器、归档格式、字符集与实体解码等凡有规范可依的工作，优先采用已有实现；只在平台已经提供原语时自行组合，不重写原语。

1. **链入 `hare.dll`（TSF 文本服务）的代码只用系统原语。** 这个 DLL 会进入用户打字所到的每一个应用进程；多一份依赖就可能让别人的程序崩溃、触发杀毒软件启发式规则，或与宿主已经加载的另一版本相撞。这条边界是绝对的。
2. **Hare 自有进程可以收纳纯数据或自包含算法。** `HareDeployer.exe`、`HareServer.exe` 中的依赖必须同时满足：源码随仓库放在 `third_party/` 而非 submodule，没有需要分发的运行时 DLL，构建时不需要包管理器，和其他新功能一样只在上游代码里留一个调用点，许可同时兼容 AGPL-3.0-or-later 与 GPL-3.0（具体边界见 `CLAUDE.md` 的「许可」）。`third_party/argon2/` 就是这种形态。新目录与单一调用点本身不产生逐次上游合并冲突；持续改造构建系统或维护 submodule 才会，因此这些条件缺一不可。
3. **仅用于构建的工具与 `worker/` 不受依赖限制。** Worker 是运行在 Cloudflare、在 MSVC 解决方案之外构建和部署的 JavaScript，它的依赖不增加上游合并成本；构建期工具同理，也可以用清单式包管理器获取依赖。依赖获取由 `tools/install-deps.ps1` 与仓内源码完成，贡献者的门槛来自这套专用准备流程，而不是依赖的数量。

这三层不放宽加密与 TLS 传输的选型：分别固定使用 CNG 与 WinHTTP，由 Windows Update 维护。采用 OpenSSL 或 libcurl，就要为全部已安装用户自行响应 CVE，并向已经安装的每个用户分发运行时 DLL；这是更差的责任边界，不是技术口味。

## 身份

中文名「紫毫」，英文名 Hare。命名沿用 Rime 生态以制笔兽毛命名的传统——小狼毫用狼毫、鼠须管用鼠须，紫毫即兔毫。

与官方小狼毫**并存**，两者可同时注册在系统里。因此以下标识必须独立：

- TSF CLSID、profile GUID、语言栏按钮 GUID、显示属性 GUID（均在 `WeaselTSF/Globals.cpp`）
- 注册表 `HKLM\SOFTWARE\Rime\Hare`（枢纽在 `include/WeaselConstants.h`）
- 可执行文件名、安装目录、开始菜单组、语言栏显示名
- `installation.yaml` 的 `distribution_code_name` / `distribution_name`

而以下标识**保持与小狼毫一致**，否则会脱离万象拼音等现成配置生态：

- `weasel.yaml` / `weasel.custom.yaml` 文件名
- 用户目录默认 `%APPDATA%\Rime`，以及通过注册表 `RimeUserDir` 覆盖的语义

### 共用用户目录的边界

两者可以指向同一个用户目录并共享全部配置，但代价有两条，都是硬的。

**同一时刻只能有一个在运行。** 用户词库是 LevelDB，开启时持有排他锁；先启动的那个服务端占住 `*.userdb/LOCK` 之后，另一个连合并快照都做不到，日志里表现为 `failed synchronizing 6/6 user dicts`。Rime 的维护模式（`client.StartMaintenance()`）解决不了这件事——它只能让本发行版自己的服务端让路，管不到另一个发行版的进程。

**每次切换发行版都会触发全量重建。** `installation.yaml` 只记得住一个发行版及其 librime 版本，紫毫用 1.17.0、小狼毫用 1.13.1，于是另一个启动时读到「上一个是对方」，判定 `modifications detected. workspace needs update` 并进入维护模式重建全部词典。重建期间**只能输入英文**，因为此时没有可用的词典。

因此共用是过渡状态，不是终点：紫毫将自带用户目录，配置由导入功能带进来而不是共享（见 `docs/ROADMAP.md` 阶段七）。在那之前并存测试时，切换前先让另一个退出（`WeaselServer.exe /q` 或 `HareServer.exe /q`），并接受切换后的一次重建。

**源码文件名与工程目录一律不改。** 仓库里有 69 个文件名含 `Weasel`，重命名它们会让每次上游合并的冲突解决成本成倍上升，而用户可见的身份独立并不需要改文件名——改产物名和字符串就够了。

### 共存所需的独立标识

只要有一项与官方重合，两者就会互相干扰，且症状往往指向别处。完整清单：

| 标识 | 位置 |
|---|---|
| TSF CLSID、profile GUID、语言栏按钮 GUID、显示属性 GUID | `WeaselTSF/Globals.cpp` |
| **同一对 GUID 的第二份副本**，以及 `PSZTITLE_HANS` / `PSZTITLE_HANT` 里内嵌的 GUID 字符串 | `WeaselSetup/imesetup.cpp` |
| 单实例互斥体 `(HARE)Furandōru-Sukāretto-` | `WeaselIPCServer/WeaselServerImpl.cpp` |
| Deployer 互斥体 `HareDeployerExclusiveMutex` | `WeaselDeployer/WeaselDeployer.cpp`、`WeaselTSF/WeaselTSF.cpp` |
| IPC 管道名与 IPC 窗口类名 | `include/WeaselIPC.h` |
| 服务名 `HareInputService` | `WeaselServer/WeaselService.h` |
| 注册表根键 | `include/WeaselConstants.h`，另有 `WeaselSetup/WeaselSetup.cpp` 的 `KEY` 常量与 `Updates` 子键 |
| WinSparkle 注册表路径 | `WeaselServer/WeaselServerApp.cpp` |
| 复制进 `System32` 的 TSF DLL 文件名 | `WeaselSetup/imesetup.cpp` 的 `srcFileName` 与各 `destPath` |
| Windows 错误报告 LocalDumps 键 | `WeaselSetup/imesetup.cpp` 的 `WEASEL_WER_KEY` |
| 日志目录与 librime 的 `app_name` | `include/WeaselUtility.h`、`WeaselTSF/dllmain.cpp`、`RimeWithWeasel.cpp`、`Configurator.cpp` |
| 托盘菜单调用的可执行文件名 | `WeaselServer/WeaselServerApp.cpp`、`WeaselSetup/WeaselSetup.cpp`、`WeaselTSF/WeaselTSF.cpp` |

两个特征症状值得记住：**单实例互斥体重合**会让后启动的服务端立刻退出并返回 −1，因为 `ServerImpl::Start()` 认定自己是重复实例；**`imesetup.cpp` 里那份 GUID 副本没同步**则会让 `InstallLayoutOrTip` 把官方的配置文件加进语言列表，表现为注册成功但语言栏里始终看不到新输入法。

## 图片皮肤

官方小狼毫的候选窗只支持纯色、圆角、阴影、字体，没有任何背景图能力，这是官方的明确立场（「不称皮肤，而称样式、布局与配色」）。

### 渲染

候选窗是 `WS_EX_LAYERED` 分层窗口，逐像素 alpha，形状与阴影用 GDI+、文字用 Direct2D + DirectWrite 画进内存 DC，最后一次 `UpdateLayeredWindow` 提交。这是输入热路径，每次按键都会重绘。

图片皮肤尚未进入这条管线：`DoPaint()` 当前只绘制纯色背景、文字与状态图标，`UIStyle` 和 `Layout` 也没有背景图、九宫格边距或立绘字段。设计要求背景图与立绘在同一管线中合成，**单窗口一次出图**。立绘允许溢出候选窗边界，窗口矩形因此需要按立绘扩大，`Layout` 的尺寸计算与屏幕边缘避让逻辑要一并处理。之所以不用第二个窗口画立绘，是因为快速跟随光标时两个分层窗口必然出现不同步的拖影。

设计中的绘制路径分三层组织：**背景源 → 帧 → 合成**。首个落地版本只支持静态图，动图与过渡动画通过接入定时器实现，不改结构；当前绘制代码尚无这三层抽象。

### 皮肤格式

设计中的皮肤是 `skins/<name>/` 目录，内含图片与 `skin.yaml`，声明九宫格边距、立绘位置与一套配色。当前没有皮肤目录的发现与加载、`skin.yaml` 解析或图片解码路径。

皮肤配色的设计是选中即可使用，同时允许 `weasel.custom.yaml` 覆盖——Rime 的配置合并机制天然支持。皮肤可以声明可选的暗色变体，未声明则深色模式下沿用亮色那套。

搜狗 `.ssf` 皮肤导入器尚未实现；设计上**只解包抽取图片**。`.ssf` 本身是 ZIP，`skin.ini` 是 UTF-16LE 的 INI。不解析它的布局参数：立绘定位是形如 `custom0_align = 0,0,0,0,1,0,0,2,6,0` 的十个无文档数字，且不同皮肤版本语义不一致；位置与边距在 GUI 里拖拽调整，绕开逆向。

## 云同步

Rime 自带的同步机制是：把用户词库导出成 `.userdb.txt` 快照写入 `sync_dir/<installation_id>/`，再合并该目录下其他 installation 的快照。紫毫不替换这套机制，而是在它前后各接一段——把快照搬到云端。

### 关键性质

词频合并是**可交换、幂等**的：同一个（编码, 词）取使用次数较大者。这在数学上是 CRDT，**不存在冲突**，因此不需要冲突解决界面。合并本身交给 Rime，云端只负责搬运文件。

### 入口

`WeaselDeployer/Configurator.cpp` 的 `SyncUserData()`，由 `HareDeployer.exe /sync` 触发。托盘菜单与开始菜单里的「同步用户资料」入口是上游就有的，无需新建。

调用顺序不可颠倒：**先拉取远端快照落盘，再调用 `RimeSyncUserData()`，最后读取导出结果上传**。顺序反了这一轮就合并不到对端数据。

### 后端

四选一，同一时刻只启用一种，共用一个 `SyncBackend` 接口：

| 后端 | 用户需要填 |
|---|---|
| R2 直连（SigV4） | endpoint、Access Key ID、Secret Access Key、bucket |
| R2 经 Worker 代理 | Worker URL、自设 token |
| WebDAV | 服务地址、用户名、密码 |
| 本地目录 | 一个目录路径 |

Worker 后端当前只接收已经部署好的 Worker URL 与自设 token，设置页没有一键部署入口。设计是在设置页接入 Cloudflare 的 Deploy 按钮，自动创建并绑定 R2 bucket，让用户不必接触 account ID 与密钥；代价是源仓库必须公开，且用户需要 GitHub 或 GitLab 账号。

本地目录后端把用户选定的目录视为受信存储位置，不是针对同账户恶意进程的沙箱边界；同账户进程若能在路径校验与文件打开之间替换 junction，本就同时拥有 Rime 用户目录和注册表凭证的读写权限。真正需要跨信任边界时使用只暴露对象接口的 S3、WebDAV 或 Worker 后端。

R2 免费额度为 10 GB 存储、每月 100 万次 Class A、1000 万次 Class B，出网流量免费。按单设备每小时同步一次估算，免费额度足以覆盖数千设备月。

### 同步范围

以下是配置与皮肤纳入同步后的设计边界。当前实现只搬运 `*.userdb.txt`；配置与皮肤同步、拉取 YAML 的语法校验、覆盖前的时间戳备份以及校验通过后的自动部署尚未落地，实施进度只在 `docs/ROADMAP.md` 阶段四维护。

| 内容 | 策略 |
|---|---|
| 词频 `*.userdb.txt` | 双向，合并交给 Rime |
| 配置 `*.yaml`、`custom_phrase.txt` | 双向，时间戳新的胜出 |
| 皮肤 `skins/` | 双向，取并集 |

硬排除，不提供选项：`build/`（编译产物）、`*.userdb/`（LevelDB 目录，直接同步会损坏）、`*.gram`（数百 MB 的语法模型）、`dicts/` 与 `lua/`（方案发布包的内容，应随上游版本走）、`replacer.userdb.txt`（由方案数据生成，换机器会自行重建，体积却有数 MB）。

配置采用时间戳胜出策略，因此**覆盖本地文件前必须先存一份带时间戳的副本**到 `sync/_local_backup/`。拉取到的 YAML 要先做语法校验，通过才落盘并自动重新部署；校验失败则丢弃并提示。

### 触发

程序启动时拉取一次，之后定时同步（默认 1 小时，可配置），托盘保留手动项。定时器挂在常驻的 `HareServer` 上——`HareDeployer` 是一次性进程，挂不住定时器。不依赖退出事件，关机时进程是被杀掉的，钩子不可靠。

### 加密

词库里是用户输入过的每一个词，属高敏感数据，因此端到端加密。

- **数据密钥（DEK）**：32 字节随机，所有设备共用
- **密钥加密密钥（KEK）**：主密码经 Argon2id（m=64MB, t=3, p=1）派生
- 被 KEK 包装后的 DEK 存在云端；首次建立使用后端的原子 create-if-absent，已有对象永不覆盖，并发设备读取同一个远端胜者；新设备输入主密码即可解开，之后用 DPAPI 缓存在本地，开机自动同步不再需要密码
- 主密码轮换的设计只重新包装 DEK，不必重新加密全部历史数据；当前设置页只提交一个密码，存储已有包装密钥时 `SetUpDataKey()` 只用它尝试解包，没有旧密码解包、再用新密码包装并写回的路径
- 内容加密使用 CNG 的 AES-GCM

主密码的设计要求是最短 10 位，不强制字符类型组合，另用 zxcvbn 风格的评分拦截可预测弱密码。当前实现只检查最短 10 位，可预测弱密码拦截尚未落地，实施进度只在 `docs/ROADMAP.md` 阶段五维护。强制组合规则会把用户推向 `Password1!` 这类可预测模式，实测有效熵反而更低，NIST SP 800-63B 现行版亦明确反对。

**R2 凭证与 API token 不能用作密钥材料**：凭证需要定期轮换而密钥必须恒定，一次轮换就会让所有历史密文无法解开；且服务方持有凭证副本，用它派生密钥等于把钥匙交给了想要防范的对象。

凭证一律用 DPAPI 加密存放在本地，且**必须排除在同步范围之外**——否则密钥会被自己同步上云，还会与「拉取配置才能拿到凭证」形成循环依赖。DPAPI 绑定当前 Windows 账户，无法跨设备解密，这对凭证正合适（每台设备本就各自配置一次），但对 DEK 不适用，故有上面的包装方案。

## 界面

设置面板嵌在 `HareDeployer` 里，用 WebView2 实现，设计为紫毫全部图形配置的入口：同步配置（选后端、填凭证、同步间隔、启动时同步）、皮肤选择（缩略图墙、拖拽导入、九宫格边距与立绘位置编辑）、插件与开关、方案与部署、通用设置。当前 WebView2 页面只提供同步配置、连接测试、密钥建立、手动同步与同步计划；方案选择与配色方案由既有 Win32 对话框提供，重新部署由托盘入口提供，皮肤、插件与开关、方案与部署、通用配置页面均未接入。同步范围的多选留到同步范围真的不止词库时再加。

### 通用配置：描述驱动，不做树编辑器

通用配置页尚未实现；以下是它的设计。Rime 的配置没有固定结构：任意 YAML 树，加 patch 机制，加每个方案自定义的键。想把所有配置项做成表单，等于给一门无模式的配置语言做编辑器；退而求其次做通用树编辑器，则是把 YAML 的结构原样甩给用户——人要改的是「简繁切换」这种意图，不是 `switches/@3/reset` 这条路径。

因此面板按**描述**渲染，而不是按结构：

- 一份配置项描述表（路径、类型、值域、标签、说明、分组）驱动分组表单，**新增设置项是加一条数据，不是写一段界面代码**
- **方案可以自带描述**：方案包里放一份 `hare-settings.yaml`，面板即可显示该方案自己的开关与选项，而不必认识任何具体方案。这是留给 Rime 生态的设置贡献点
- 搜索优先：输「繁」跳到简繁开关。人靠关键词找设置，不靠层级导航
- 描述覆盖不到的键落到「高级」页的文本编辑（语法高亮、保存前校验、时间戳备份）。文本比树顺手，而且这是罕用路径
- 写入一律 patch 进 `*.custom.yaml`，不动原文件，可回滚，也不与方案升级打架
- 外观类设置由**真实的渲染管线**出图预览，与皮肤面板共用同一套能力

设计中有两件外部工具做不到、只能由紫毫自己做的事：改完配置后用紫毫自己的部署器、对着紫毫自己的用户目录重新部署；以及在同一个窗口里与同步、皮肤设置并列。第三方 Rime 图形配置工具仍然有用，因此设计要求面板检测到就提供入口，并明确写出它编辑的是哪个用户目录；当前 WebView2 页面尚无这项检测与入口。

候选窗本体当前**不受影响**，使用现有的 GDI+ / Direct2D 分层窗口管线。WebView2 只出现在设置面板：`HareDeployer` 是一次性进程，开窗那几十秒的启动开销与内存占用碰不到输入延迟；而候选窗是每次按键都要重绘的热路径，绝不能引入。皮肤面板尚未实现；其实时预览的设计要求是真实管线出图后送进页面，而不是在 HTML 里重画一个近似的候选窗——否则调出来的边距与立绘位置在真实渲染下对不上。

页面与宿主之间传的是一条扁平记录（单元分隔符隔开的 `key=value`，值百分号编码），不引入 JSON 库。**凭证只出宿主不入页面**：页面只知道某项「已保存」，留空即不改动。**面向用户的文案全在页面里**，宿主只回报状态码，因此 C++ 源码里没有非 ASCII 字面量，也不必在几份 `.rc` 语言块之间同步措辞。

主密码只经面板输入。命令行入口对同机其他进程可见，因此不提供。

## 自动更新

设计沿用 WinSparkle，并为紫毫提供正式与测试两个 feed。当前 `HareServer` 在 `win_sparkle_init()` 前调用 `win_sparkle_set_appcast_url()` 显式设置紫毫正式 feed；显式 URL 的优先级高于资源，因此 `WeaselServer/WeaselServer.rc` 中仍指向上游的四条 `APPCAST` 资源不会成为运行时 feed。手动检查路径会读取 `Software\Rime\Hare` 下的 `UpdateChannel` 并选择正式或测试 URL，但它在初始化后再次调用配置接口，而仓内 WinSparkle 接口要求配置函数只在首次初始化前调用，测试通道尚无符合该接口约束的生效路径。WinSparkle 的注册表路径为 `Software\Rime\Hare\Updates`。

更新包的设计使用 **EdDSA 签名**：自己生成密钥对，公钥编进可执行文件，私钥存 GitHub Secrets 由 Actions 签名，零成本，防止 feed 被劫持后推送恶意更新。当前捆绑的 WinSparkle 接口只提供 DSA 公钥验证，仓库中没有签名公钥、EdDSA 验证接入或 Actions 签名步骤，因此这条签名链尚未实现。这与 Authenticode 代码签名证书是两回事——后者用于消除 SmartScreen 警告，需要年费，不在本项目范围内。

`update/` 目录下的 `appcast.xml`、`testing-appcast.xml`、`bump-version.ps1` 是可复用的上游发布工具链骨架；当前两个 appcast 仍指向小狼毫，脚本只维护版本而不签名。设计要求在紫毫 feed、产物名与签名链就绪后复用这套结构。

## 明确不做

- **打字粒子特效**：搜狗那种效果绑定在其私有候选窗实现里，任何输入法都无法从系统层面获得。要做只能另写一个全局透明叠加层程序（低级键盘钩子 + 分层穿透窗口），相当于第三个独立功能模块。
- **词典逐条编辑与语言模型训练**：词库的日常维护靠输入本身与 `custom_phrase.txt`，逐条编辑词库是给词典作者用的工具，不是给使用者的；语言模型由方案随发布包提供。
- **移动端**：iOS 的仓输入法要求 `sync_dir` 必须落在它自己申请的 iCloud 容器路径下，第三方写不进去；Android 的同文新版走 SAF，外部程序未必能访问其数据目录。跨端的障碍是各家输入法的沙盒策略，不是同步协议本身。
