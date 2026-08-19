# 路线

阶段顺序有意为之：先做落在低频文件上的云同步，用它把工具链、WebView2 嵌入、配置读写这些基础设施验证一遍，再去碰高频改动的渲染路径。

## 上游改动频率

决定改动落点时的核心依据。冲突只发生在改动行附近，但文件越活跃，风险越高。

| 文件 | 最近 25 次提交跨度 | 频率 |
|---|---|---|
| `WeaselDeployer/Configurator.cpp` | 2012-05 → 2025-01，十三年 | 近年每年 1–2 次 |
| `WeaselUI/WeaselPanel.cpp` | 2024-01 → 2026-02，25 个月 | 约每月 1 次 |

因此云同步几乎零风险，图片皮肤需要把改动面压到最小。

## 阶段一：地基（完成）

- 克隆 rime/weasel，`origin` 改名 `upstream`，工作分支 `hare`，基点 `f9203ca`
- 预编译 librime、Boost 1.91.0 静态库、`output\data` 就位
- `env.bat` 与 `tools\` 下的包装脚本
- 基线构建通过：x64 与 Win32，0 错误

## 阶段二：改名（完成）

| 位置 | 内容 | 状态 |
|---|---|---|
| `include/WeaselConstants.h` | `WEASEL_CODE_NAME` → `Hare`，`WEASEL_REG_KEY` → `Software\Rime\Hare` | 完成 |
| `WeaselTSF/Globals.cpp` | 四个 GUID 全部重新生成 | 完成 |
| 各 `*.vcxproj` | 产物改为 `HareServer.exe`、`HareDeployer.exe`、`HareSetup.exe`、`hare.dll`、`harex64.dll` | 完成 |
| `WeaselSetup/imesetup.cpp` | `has_installed()` 检查 `System32\hare.dll` | 完成 |
| `WeaselTSF/Register.cpp` | ARM64 重定向 DLL 名 | 完成 |
| `output/install.nsi` | 安装目录 `hare-<版本>`、开始菜单组、文件清单、卸载项、注册表、安装包文件名 | 完成 |
| 各 `*.rc` | 显示字符串、菜单文案、版本资源、`FILE_NAME` | 完成 |
| `include/WeaselUtility.h` | `get_weasel_ime_name()` 返回「紫毫」/ Hare；日志目录 `%TEMP%\rime.hare`；UI 语言注册表键 | 完成 |
| `RimeWithWeasel.cpp`、`Configurator.cpp` | `app_name` 为 `rime.hare`，错误提示里的日志路径 | 完成 |

`installation.yaml` 的 distribution 字段不需要单独改：`distribution_name` 取自 `get_weasel_ime_name()`，`distribution_code_name` 取自 `WEASEL_CODE_NAME`，两者都已改。

刻意保留未改的三处：`resource\weasel.ico` 图标文件名（美术资源，换图时再说）、注册表值名 `WeaselRoot`（住在 `Software\Rime\Hare` 下，与官方无冲突，改它要牵动四处代码却零收益）、命令行参数 `/weaseldir`（内部参数，不可见）。

产物改名的做法：三个 exe 工程的 `<OutputFile>` 原本写死 `$(ProjectName)`，改为 `$(TargetName)`，再在各工程 `Microsoft.Cpp.targets` 导入之前插一个无条件的 `<PropertyGroup>` 设定 `TargetName`。这样上游的默认行为不变（`TargetName` 默认等于 `ProjectName`），改动面只有两处。WeaselTSF 则是直接改它原有的 `TargetName` 与 `OutputFile`。

`.rc` 文件是 UTF-16LE 编码，按字节直接编辑会损坏文件，需要用支持该编码的方式读写。

系统级标识（GUID、互斥体、管道、服务名、注册表键等）必须全部独立，完整清单见 `docs/DESIGN.md` 的「共存所需的独立标识」。

**验收已通过**：紫毫与小狼毫的输入法配置文件同时出现在 zh-Hans-CN 的输入法列表中，`HareServer` 与 `WeaselServer` 可同时运行，紫毫部署时读取的是共享的 `D:\App\Rime\UserData`，日志独立在 `%TEMP%\rime.hare`。

新的 GUID：

```
c_clsidTextService          {F158648F-2429-4760-B7DB-3A9117E71847}
c_guidProfile               {3EEA6D3E-1DD4-42AC-8E6A-A4501E1D2FB3}
c_guidLangBarItemButton     {CD262B19-B159-40FD-8151-588C4402FE1A}
c_guidDisplayAttributeInput {E17A8C85-D946-468B-8606-07F7C0BD179A}
```

验收：安装后语言栏中「紫毫」与「小狼毫」并列出现，各自独立工作，紫毫读取同一个用户目录。

## 阶段三：自动更新指向自身

改 `WeaselServer/WeaselServer.rc` 的四条 `APPCAST` 资源与 WinSparkle 注册表路径，生成 EdDSA 密钥对，公钥编入可执行文件，私钥进 GitHub Secrets。复用 `update/` 下现成的 appcast 与 `bump-version.ps1`，由 GitHub Actions 在打 tag 时发布到 GitHub Pages。

## 阶段四：云同步

入口是 `WeaselDeployer/Configurator.cpp` 的 `SyncUserData()`，在其前后各接一段，本体逻辑写进新文件，上游文件只留一个调用点。

推进次序：

1. **本地目录后端**跑通完整链路。**已完成**：`WeaselDeployer/CloudSync.h` 与 `CloudSync.cpp`，在 `SyncUserData()` 里前后各一个调用点。第一版只搬运 `*.userdb.txt`，配置与皮肤留到第 6 步——它们需要 YAML 校验，是另一件事。
2. HTTP 层与 SigV4，接上 R2 直连。**已完成**：`CloudHttp.h/.cpp`（WinHTTP + CNG 的 SHA-256、HMAC-SHA-256、DPAPI）与 `S3Backend.h/.cpp`。凭证以 DPAPI 加密后存为注册表 `REG_BINARY`。
3. 加密层。**已完成**：`CloudCrypto.h/.cpp`。Argon2id 用 `third_party/argon2/` 的参考实现（CC0 / Apache-2.0 双授权），AES-256-GCM、随机数、DPAPI 走 CNG。
4. WebDAV 与 Worker 代理两个后端。**代码完成，已有服务行为做过开发机手工验证**：坚果云会忽略 `If-None-Match: *` 并覆盖旧内容，新客户端现已在正式密钥 PUT 前用非敏感对象识别并安全拒绝；旧 Worker 同样会在正式 PUT 前被能力握手拒绝。新版 Worker 已部署到 Cloudflare，线上验证确认能力标记精确匹配、第二次条件 PUT 返回 412、探针内容保持且 `keys/dek.bin` 前后不变。实现位于 `WebDavBackend.h/.cpp`、`WorkerBackend.h/.cpp` 与 `worker/`。
5. 一键部署按钮：仓库公开后，`Deploy to Cloudflare` 会读 `worker/wrangler.jsonc` 自动创建并绑定 R2 桶。
6. 配置与皮肤纳入同步范围，含 YAML 校验、时间戳备份与校验通过后的自动部署。

配置存放在注册表 `HKCU\Software\Rime\Hare\CloudSync`。`Backend` 取 `localdir` / `s3` / `webdav` / `worker` 之一，其余键按后端而定：`LocalDir`；`Endpoint`、`Bucket`、`Prefix`、`AccessKeyId`、`SecretAccessKey`；`DavUrl`、`DavUsername`、`DavPassword`；`WorkerUrl`、`WorkerToken`。凭证一律是 DPAPI 加密后的 `REG_BINARY`，其余是 `REG_SZ`。这些**不放在 Rime 用户目录**，**不放在 Rime 用户目录**——那个目录本身要被同步，把凭证放进去等于上传密钥，还会形成「先拉配置才能拿到凭证」的循环依赖。

验收记录：伪造设备的快照分别放进本地目录后端与 Cloudflare R2，同步后其词条（`紫毫 c=9`、`望舒 c=6` 等）出现在本机导出的快照里，与本机原有词条并存；上传的对象只有 `*.userdb.txt`，配置文件与 `replacer.userdb.txt` 按规则排除。

`tools\s3.ps1` 可用来检查桶内容（`list` / `get` / `put` / `delete` / `purge`）。endpoint、桶名、Access Key 与前缀可走参数或 `HARE_S3_*` 环境变量；Secret 只接受 `SecureString` 参数或安全提示输入，脚本本身不含任何密钥。

后端接口只负责「按名字存取字节」（`List` / `Get` / `Put` / `PutIfAbsent`），目录遍历、文件过滤与加解密全部收在 `CloudSync.cpp`。这样加了新后端也不可能漏掉加密，而不是每个后端各写一遍。`PutIfAbsent` 专用于唯一的数据密钥：本地目录用同卷临时文件加不覆盖重命名，S3 用服务端条件写；WebDAV 会先用保留的非敏感对象实测 `If-None-Match: *` 是否真能阻止覆盖，不支持就拒绝建钥；Worker 先完成协议能力握手，再把同一条件原子传给 R2。

密钥经设置面板的主密码框建立：存储已有 `keys/dek.bin` 时用密码解开，没有时生成随机 DEK 并以 create-if-absent 发布；并发建钥时只有一个设备能创建，其他设备读取并采用远端胜者，任何路径都不会覆盖已经存在的密钥。DEK 随后用 DPAPI 缓存在本机。主密码不经命令行。实测 Argon2id（m=64MB, t=3, p=1）单次派生约 2.4 秒。

开发机手工验收记录（2026-08-19）：用当前 R2 账号的新 S3 凭据直接执行产品后端并发验收，同一新对象的两个 `S3Backend::PutIfAbsent` 恰好一个创建、一个返回已存在，第三次仍返回已存在；GET 内容等于两个竞争者之一，List 能看到对象，随机缺失对象返回 404。旧凭据得到的 `403 AccessDenied` 因而确认是授权问题，不是 SigV4 实现问题；这项线上验收不属于 `tools\test-cloud-sync.bat` 的自动测试范围。

验收记录（加密）：上传后的对象不含任何可读词典标记；把 `installation.yaml` 的 `installation_id` 改成另一个值以模拟新设备后同步，另一台设备的密文快照被正确拉回、解密并合并。

**deployer 侧的日志看不到**：`WeaselDeployer` 进程里的 `LOG()` 写不进日志文件，上游自己的 `LOG(INFO) << "WeaselDeployer reporting."` 同样不出现——初始化过文件 sink 的那个 glog 实例在 `rime.dll` 里。云同步的错误信息因此只能靠返回值和外部观察判断，诊断信息要面向用户展示时，得走 GUI 而不是日志。

依赖上保持克制：HTTP 用 WinHTTP、加解密用 CNG，都是系统自带；不引入 OpenSSL 或 libcurl，每多一个第三方库就多一层上游合并的负担。

阶段四的加密词库同步核心链路可用。判定为不成立的结论与其证据记在 [REVIEW-NOTES.md](REVIEW-NOTES.md)，动这块代码前先读。

`tools\test-cloud-sync.bat` 会构建并运行 x64、Win32 的 `TestCloudSyncCore`，再定向编译两种架构的 Release `HareDeployer`；加 `full` 会追加完整双架构 Release solution build。它覆盖快照 envelope 与名称认证、远端名称/路径冲突、S3 前缀边界、本地条件创建，以及批量文件提交失败时的回滚和临时/备份清理；不调用会清理日志的上游构建脚本。

拉取会先把本轮全部对象下载、认证、解密并校验到内存，确认整批有效后才提交本地文件；本地提交先完整落临时文件，任一替换失败会反向恢复已经替换的目标。若 Windows 拒绝回滚替换或提交后的备份清理，事务返回 `kRecoveryRequired` 并把旧内容保留为目标旁的 `.hare.bak.*`，不把恢复材料静默删掉。

阶段四仍有两项开放工作：

- **主密码轮换路径**。存储已有包装密钥时，`SetUpDataKey` 只把输入当作解包密码；输入新密码只会得到解包失败，没有「旧密码解包 DEK、用新密码重新包装」的流程。
- **S3 兼容存储的 Region 设置**。R2 接受 SigV4 scope 中的 `auto`，其他 S3 兼容端点要求自己的真实 region；固定 `auto` 会让这些端点拒绝签名。

## 阶段五：WebView2 设置面板

同步配置、连接测试、密钥建立、手动同步与同步计划都由设置面板提供。皮肤面板随阶段六一起做——皮肤功能尚不存在，先做面板等于对着空气设计。

阶段五仍有一项开放工作：**zxcvbn 风格的弱密码检查**。`docs/DESIGN.md` 要求拦截可预测密码，宿主只执行最短 10 位校验；`HareDeployer.exe` 的依赖层允许把许可兼容的词频表作为纯数据放进 `third_party/`，因此该检查有符合依赖边界的落点。

入口：托盘菜单「云同步设置」，或 `HareDeployer.exe /settings`。面板本体在 `WeaselDeployer/SettingsPanel.h/.cpp` 与 `WeaselDeployer/settings.html`，配置的写入侧在 `CloudSync.cpp`（`SyncConfig::Save`、`SaveSyncSchedule`、`TestBackend`），上游文件里只有 `WeaselDeployer.cpp` 的一个分支和 `WeaselServerApp.cpp` 的两处调用。

**主密码只经面板的密码框进入进程。** `/cloudkey:<密码>` 不是受支持的入口，避免命令行向同机其他进程暴露密码。

几个决定：

- **页面作为资源编入可执行文件**（`IDR_SETTINGS_PAGE`，`RCDATA`），用 `NavigateToString` 加载。不落地成文件就不必改安装脚本的文件清单，也没有「面板去读安装目录里的 HTML」这条可被替换的路径。
- **WebView2 的用户数据目录显式指向 `%LOCALAPPDATA%\Hare\WebView2`**。默认位置是可执行文件旁边，即安装目录，标准用户写不进去。
- **只静态链接 `WebView2LoaderStatic.lib`**，不随产物分发任何 DLL。运行时本身 Windows 11 自带，Windows 10 上缺失时面板报 `IDS_STR_NO_WEBVIEW2` 并退出，而不是静默失败。
- **消息桥不用 JSON**。传的是一条扁平记录，格式为单元分隔符隔开的 `key=value`，值按 `encodeURIComponent` 的规则百分号编码。为十几个短字符串引入一个 JSON 库，代价是每次上游合并都要多带一个依赖。
- **凭证不回传给页面**。页面只知道某项凭证「已保存」，输入框留空即表示不改动。
- **面向用户的文案全部在页面里**，宿主只回报状态码（`test_ok`、`key_wrong_password` 等）。这样 C++ 源码里没有一个非 ASCII 字面量，也不必在三份 `.rc` 语言块之间同步措辞。
- **换存储才丢弃本机缓存的数据密钥**，换凭证不丢。密钥属于存储，而轮换 access key 仍是同一个存储，此时要求重新输入主密码是无谓的。
- **同步范围的多选项暂不提供**。同步范围只有词库快照，配置与皮肤要等阶段四第 6 步；摆出一个不起作用的开关比没有这个开关更糟。

同步计划（间隔、启动时同步）落在 `WeaselServer/SyncScheduler.h/.cpp`：常驻进程才挂得住定时器，`HareDeployer` 是一次性的。定时器**每次到点都重读注册表**，因此在面板里改间隔不需要重启服务端。不挂退出钩子——注销时进程是被杀掉的，钩子不可靠。

`Configurator::SyncUserData()` 区分两种失败：Rime 自身的合并失败返回 1，云端拉取或发布失败返回 `kCloudSyncFailed`（2）；调用方必须把云端发布失败保留为可见结果。

验收记录（对 Cloudflare R2 实测）：面板打开后正确显示注册表里已存的 S3 配置、凭证显示为「已保存」、设备标识与快照目录取自 `installation.yaml`；「测试连接」对 R2 返回成功；「保存」写回后 `DataKey` 保留、凭证二进制未变；切到本地目录后端并保存，`DataKey` 按预期被清除而 S3 各字段仍在；用错误的主密码「建立密钥」返回「密码不对」且云端密钥与本机缓存都未被改动；「立即同步」返回成功。定时器把间隔设为 1 分钟后，`HareServer` 在启动后约 25 秒拉起一次 `HareDeployer /sync`，此后每分钟一次，快照文件的时间戳随之推进。

## 阶段六：图片皮肤与皮肤面板

改 `WeaselUI/WeaselPanel.cpp` 的绘制路径与 `Layout` 的尺寸计算。把绘制拆成背景源、帧、合成三层，图片解码与九宫格逻辑放进新文件。附带一个 `.ssf` 解包器（ZIP 容器，`skin.ini` 为 UTF-16LE），只抽取图片资源。

第一版全静态；动图与过渡动画在架构就位后接定时器实现。

皮肤选择面板与阶段五的同步面板并列，复用同一套宿主与消息桥：缩略图墙、拖拽导入、九宫格边距与立绘位置编辑。**实时预览必须由真实的 D2D / GDI+ 管线出图**再送进页面，用 HTML 画一个「差不多的候选窗」只会让调出来的位置在真实渲染下对不上。

## 阶段七：独立用户目录与数据导入

共用用户目录的代价已经实测出来了，比预期大：**两个发行版的 librime 版本不同**（紫毫 1.17.0，小狼毫 1.13.1），而 `installation.yaml` 只记得住一个。每切换一次发行版，另一个启动时都会读到「上一个发行版是对方」，判定 `modifications detected. workspace needs update`，随即进入维护模式重建全部词典——重建期间只能输入英文。加上用户词库是 LevelDB、开启时持排他锁，两者同时运行必然有一个拿不到词库。

因此紫毫改为**自带用户目录**，默认不再指向小狼毫那一份；已经设过 `RimeUserDir` 的机器保持原样。

代价是拿不到现成配置了，由**导入功能**补回来，落在设置面板的新页：选择另一个 Rime 发行版的用户目录（或一份备份），复制方案、词典、`*.custom.yaml`、`custom_phrase.txt`。

**用户词库只能经文本快照导入，绝不复制 LevelDB 目录**。`*.userdb` 的磁盘格式与 librime 版本绑定，跨版本复制正是丢词库的经典方式；Rime 自己的导出格式（`*.userdb.txt`）是版本无关的，导入即合并，与云同步用的是同一条路径。

## 阶段八：符号候选

目标行为，参照搜狗：

1. 输入中文数字的拼音时，首页末位给出序号符号（①②③⋯）
2. 输入能联想到符号的词时，首页末位给出该符号（右 → `→`，上 → `↑`，圆 → `○`，方 → `□`）
3. 一个专门的键，把与当前词相关的符号一次列全（`yi` + 符号键 → `①Ⅰ⑴⒈ⅰ❶㈠`）

**这件事不在前端这一层。** 候选的产生属于 librime 与方案（含 Lua），紫毫和小狼毫一样只负责渲染 librime 给出的候选；在 C++ 里插候选是改错了地方，且换方案就失效。

正确的落点是方案数据与 Lua 过滤器，紫毫负责随发行版携带这份数据、并在设置面板给出开关。动手前先确认现成的部分：万象已经带了 `lua_processor@*wanxiang.super_symbols*P`（选「（字符分类）」候选时展开该分类全部条目）与 `super_tips`（提示区显示符号等对应关系），`wanxiang_symbols.yaml` 就是它的数据。第一步是查清上面三条里哪几条只是配置问题，剩下的才需要自己写过滤器。

## 阶段九：配置与插件面板

紫毫的图形配置收进阶段五那个 WebView2 壳里。设计与理由见 [DESIGN.md](DESIGN.md) 的「通用配置：描述驱动，不做树编辑器」；这里只记落地次序。

推动它的是阶段七：用户目录独立之后，第三方 Rime 图形工具默认按小狼毫的注册表键或 `%APPDATA%\Rime` 找目录，改的不是紫毫这一份，改完的重新部署也不是紫毫来做。**外部工具仍然有用，面板检测到就给入口，并写明它编辑的是哪个目录。**

三片交付，每片单独可用：

1. **插件与开关页**。列出方案实际加载的 Lua 模块与它声明的 switches，可开关。这些能力现在完全是隐形的——万象带着二十多个 Lua 模块（计算器、时间日期、超级符号、预测联想、输入统计），没有任何界面告诉用户它们存在，「插件自由度低」的错觉由此而来。数据来源是方案配置里的 `lua_processor@` / `lua_filter@` 声明与 `switches` 段。
2. **方案与部署页**。方案启用与排序走 librime 的 levers API（上游 `SwitcherSettingsDialog` 有现成用法），外加一个重新部署按钮。做完这片，日常配置不再需要离开紫毫。
3. **描述驱动的通用设置 + 高级文本页**。描述表先覆盖 `weasel.yaml` 与 `default.yaml` 的常用项；方案自带的 `hare-settings.yaml` 由方案作者提供。描述之外的键落到高级页的文本编辑，带语法高亮、保存前校验与时间戳备份。

写入一律 patch 进 `*.custom.yaml`。校验、备份、校验通过后自动部署这套机制与阶段四第 6 步是同一件事，两边共用。

外观类设置的实时预览要等阶段六：预览必须由真实的 D2D / GDI+ 管线出图，在 HTML 里画一个近似的候选窗只会让调出来的值在真实渲染下对不上。

## 参考环境

开发者机器上的既有安装，用于对照与取用共享数据：

```
官方小狼毫程序   D:\App\Rime\weasel-0.17.4
小狼毫用户目录   D:\App\Rime\UserData        （HKCU\Software\Rime\Weasel\RimeUserDir）
紫毫程序         D:\App\Rime\hare-0.17.4
紫毫用户目录     D:\App\Rime\HareUserData    （HKCU\Software\Rime\Hare\RimeUserDir）
```

两个用户目录里跑的都是万象拼音 base 全拼方案，配 RIME-LMDG 语法模型，默认英文标点。紫毫这一份是从小狼毫那份复制过来的：方案、词典、`*.custom.yaml`、`custom_phrase.txt` 直接复制，`installation.yaml` 与 `build\` 由紫毫自己重新生成（设备标识因此是独立的），用户词库经 `sync\imported-from-weasel\` 下的 `*.userdb.txt` 快照导入而非复制 LevelDB 目录。这就是阶段七要做成功能的那套动作的手工版。

**Rime 只合并已经存在的用户词库**（`synchronizing 0 user dicts`）：新目录里 `*.userdb` 要等第一次真正用它输入时才创建，因此快照要在那之后再同步一次才会并进来。阶段七的导入功能得自己把这一步做掉，而不是留给用户猜。
