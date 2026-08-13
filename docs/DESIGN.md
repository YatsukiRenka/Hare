# 设计

紫毫（Hare）是 [rime/weasel](https://github.com/rime/weasel) 的分支，在小狼毫的基础上加两样官方不会做的东西：**图片皮肤**与**云同步**。

## 定位

自用为主，顺手开源。不做账号系统、不买 Authenticode 代码签名证书、不做多租户。这条定位决定了后面所有的取舍：凡是只为「给陌生用户用」而存在的复杂度，一律不做。

## 许可

| 组件 | 许可 | 后果 |
|---|---|---|
| weasel（本项目基础） | **GPLv3** | 分发二进制必须提供对应源码 |
| librime | BSD-3-Clause | 保留版权声明即可 |
| plum | LGPL-3.0 | |

紫毫是 weasel 的衍生作品，因此**客户端代码必须 GPLv3 开源**。GPLv3 没有网络条款（它不是 AGPL），所以任何服务端组件不受传染。

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

两者可以指向同一个用户目录并共享全部配置，但**同一时刻只能有一个在运行**。用户词库是 LevelDB，开启时持有排他锁；先启动的那个服务端占住 `*.userdb/LOCK` 之后，另一个连合并快照都做不到，日志里表现为 `failed synchronizing 6/6 user dicts`。

Rime 的维护模式（`client.StartMaintenance()`）解决不了这件事——它只能让本发行版自己的服务端让路，管不到另一个发行版的进程。

对最终用户这不构成问题，他们只会用一个。开发期两者并存测试时，切换前需要先让另一个退出（`WeaselServer.exe /q` 或 `HareServer.exe /q`）。

**源码文件名与工程目录一律不改。** 仓库里有 69 个文件名含 `Weasel`，重命名它们会让每次上游合并的冲突解决成本成倍上升，而用户可见的身份独立并不需要改文件名——改产物名和字符串就够了。

### 共存所需的独立标识

只要有一项与官方重合，两者就会互相干扰，且症状往往指向别处。完整清单：

| 标识 | 位置 |
|---|---|
| TSF CLSID、profile GUID、语言栏按钮 GUID、显示属性 GUID | `WeaselTSF/Globals.cpp` |
| **同一对 GUID 的第二份副本**，以及 `PSZTITLE_HANS` / `PSZTITLE_HANT` 里内嵌的 GUID 字符串 | `WeaselSetup/imesetup.cpp` |
| 单实例互斥体 `(WEASEL)Furandōru-Sukāretto-` | `WeaselIPCServer/WeaselServerImpl.cpp` |
| Deployer 互斥体 `WeaselDeployerExclusiveMutex` | `WeaselDeployer/WeaselDeployer.cpp`、`WeaselTSF/WeaselTSF.cpp` |
| IPC 管道名与 IPC 窗口类名 | `include/WeaselIPC.h` |
| 服务名 `WeaselInputService` | `WeaselServer/WeaselService.h` |
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

背景图与立绘都在这条管线里合成，**单窗口一次出图**。立绘允许溢出候选窗边界，窗口矩形因此需要按立绘扩大，`Layout` 的尺寸计算与屏幕边缘避让逻辑要一并处理。之所以不用第二个窗口画立绘，是因为快速跟随光标时两个分层窗口必然出现不同步的拖影。

绘制路径分三层组织：**背景源 → 帧 → 合成**。第一版只有静态图，动图与过渡动画通过接入定时器实现，不改结构。

### 皮肤格式

一个皮肤是 `skins/<name>/` 目录，内含图片与 `skin.yaml`，声明九宫格边距、立绘位置与一套配色。

配色由皮肤自带，选中即可用，同时允许 `weasel.custom.yaml` 覆盖——Rime 的配置合并机制天然支持。皮肤可以声明可选的暗色变体，未声明则深色模式下沿用亮色那套。

搜狗 `.ssf` 皮肤的导入**只解包抽取图片**。`.ssf` 本身是 ZIP，`skin.ini` 是 UTF-16LE 的 INI。不解析它的布局参数：立绘定位是形如 `custom0_align = 0,0,0,0,1,0,0,2,6,0` 的十个无文档数字，且不同皮肤版本语义不一致；位置与边距在 GUI 里拖拽调整，绕开逆向。

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

Worker 那条路配合 Cloudflare 的 Deploy 按钮，可以自动创建并绑定 R2 bucket，用户不必接触 account ID 与密钥；代价是源仓库必须公开，且用户需要 GitHub 或 GitLab 账号。

R2 免费额度为 10 GB 存储、每月 100 万次 Class A、1000 万次 Class B，出网流量免费。按单设备每小时同步一次估算，免费额度足以覆盖数千设备月。

### 同步范围

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
- 被 KEK 包装后的 DEK 存在云端；新设备输入主密码即可解开，之后用 DPAPI 缓存在本地，开机自动同步不再需要密码
- 换密码只需重新包装 DEK，不必重新加密全部历史数据
- 内容加密使用 CNG 的 AES-GCM

主密码要求最短 10 位，不强制字符类型组合，另用 zxcvbn 评分拦截可预测的弱密码。强制组合规则会把用户推向 `Password1!` 这类可预测模式，实测有效熵反而更低，NIST SP 800-63B 现行版亦明确反对。

**R2 凭证与 API token 不能用作密钥材料**：凭证需要定期轮换而密钥必须恒定，一次轮换就会让所有历史密文无法解开；且服务方持有凭证副本，用它派生密钥等于把钥匙交给了想要防范的对象。

凭证一律用 DPAPI 加密存放在本地，且**必须排除在同步范围之外**——否则密钥会被自己同步上云，还会与「拉取配置才能拿到凭证」形成循环依赖。DPAPI 绑定当前 Windows 账户，无法跨设备解密，这对凭证正合适（每台设备本就各自配置一次），但对 DEK 不适用，故有上面的包装方案。

## 界面

两个设置面板嵌在 `HareDeployer` 里，用 WebView2 实现：同步配置（选后端、填凭证、同步间隔、启动时同步）与皮肤选择（缩略图墙、拖拽导入、九宫格边距与立绘位置编辑）。同步范围的多选留到同步范围真的不止词库时再加。

候选窗本体**不受影响**，继续使用现有的 GDI+ / Direct2D 分层窗口管线。WebView2 只出现在设置面板：`HareDeployer` 是一次性进程，开窗那几十秒的启动开销与内存占用碰不到输入延迟；而候选窗是每次按键都要重绘的热路径，绝不能引入。皮肤面板的实时预览同样由真实管线出图后送进页面，而不是在 HTML 里重画一个近似的候选窗——否则调出来的边距与立绘位置在真实渲染下对不上。

页面与宿主之间传的是一条扁平记录（单元分隔符隔开的 `key=value`，值百分号编码），不引入 JSON 库。**凭证只出宿主不入页面**：页面只知道某项「已保存」，留空即不改动。**面向用户的文案全在页面里**，宿主只回报状态码，因此 C++ 源码里没有非 ASCII 字面量，也不必在几份 `.rc` 语言块之间同步措辞。

主密码只经面板输入。命令行入口对同机其他进程可见，因此不提供。

## 自动更新

沿用 WinSparkle，feed 指向紫毫自己的地址而非官方。四条 `APPCAST` 资源在 `WeaselServer/WeaselServer.rc`：`FEEDURL`、`MANUALUPDATEFEEDURL`、`TESTINGFEEDURL`、`TESTINGMANUALUPDATEFEEDURL`，分正式与测试两个通道。WinSparkle 的注册表路径为 `Software\Rime\Hare\Updates`。

更新包用 **EdDSA 签名**：自己生成密钥对，公钥编进可执行文件，私钥存 GitHub Secrets 由 Actions 签名，零成本，防止 feed 被劫持后推送恶意更新。这与 Authenticode 代码签名证书是两回事——后者用于消除 SmartScreen 警告，需要年费，不在本项目范围内。

`update/` 目录下的 `appcast.xml`、`testing-appcast.xml`、`bump-version.ps1` 是上游现成的发布工具链，改 URL 即可复用。

## 明确不做

- **打字粒子特效**：搜狗那种效果绑定在其私有候选窗实现里，任何输入法都无法从系统层面获得。要做只能另写一个全局透明叠加层程序（低级键盘钩子 + 分层穿透窗口），相当于第三个独立功能模块。
- **可视化配置面板**（指方案、词典、快捷键这类通用配置）：`franvz9/Rime_vision`（MIT，Tauri）已经做得比第一版能做到的更好，自用场景直接装它更划算。紫毫的 GUI 只覆盖它无法感知的两块——图片皮肤与云同步。
- **移动端**：iOS 的仓输入法要求 `sync_dir` 必须落在它自己申请的 iCloud 容器路径下，第三方写不进去；Android 的同文新版走 SAF，外部程序未必能访问其数据目录。跨端的障碍是各家输入法的沙盒策略，不是同步协议本身。
