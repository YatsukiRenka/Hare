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
4. WebDAV 与 Worker 代理两个后端。**代码已完成、尚未对真实服务验证**：`WebDavBackend.h/.cpp`（PROPFIND / MKCOL / GET / PUT，HTTP Basic）与 `WorkerBackend.h/.cpp`，Worker 本体在 `worker/`。WebDAV 需要一个账号（坚果云给的是应用密码）才能验；Worker 需要先部署到 Cloudflare。
5. 一键部署按钮：仓库公开后，`Deploy to Cloudflare` 会读 `worker/wrangler.jsonc` 自动创建并绑定 R2 桶。
6. 配置与皮肤纳入同步范围，含 YAML 校验、时间戳备份与校验通过后的自动部署。

配置存放在注册表 `HKCU\Software\Rime\Hare\CloudSync`。`Backend` 取 `localdir` / `s3` / `webdav` / `worker` 之一，其余键按后端而定：`LocalDir`；`Endpoint`、`Bucket`、`Prefix`、`AccessKeyId`、`SecretAccessKey`；`DavUrl`、`DavUsername`、`DavPassword`；`WorkerUrl`、`WorkerToken`。凭证一律是 DPAPI 加密后的 `REG_BINARY`，其余是 `REG_SZ`。这些**不放在 Rime 用户目录**，**不放在 Rime 用户目录**——那个目录本身要被同步，把凭证放进去等于上传密钥，还会形成「先拉配置才能拿到凭证」的循环依赖。

验收记录：伪造设备的快照分别放进本地目录后端与 Cloudflare R2，同步后其词条（`紫毫 c=9`、`望舒 c=6` 等）出现在本机导出的快照里，与本机原有词条并存；上传的对象只有 `*.userdb.txt`，配置文件与 `replacer.userdb.txt` 按规则排除。

`tools\s3.ps1` 可用来检查桶内容（`list` / `get` / `put` / `delete` / `purge`），凭证走参数或 `HARE_S3_*` 环境变量，脚本本身不含任何密钥。

后端接口只负责「按名字存取字节」（`List` / `Get` / `Put`），目录遍历、文件过滤与加解密全部收在 `CloudSync.cpp`。这样加了新后端也不可能漏掉加密，而不是每个后端各写一遍。

密钥的建立目前靠临时命令行 `HareDeployer.exe /cloudkey:<主密码>`：已有密钥就用密码解开，没有就生成一把随机 DEK、包装后发布到存储的 `keys/dek.bin`，然后用 DPAPI 缓存在本机。**密码出现在命令行上，同机其他进程可见**，这是等设置面板落地前的权宜之计。实测 Argon2id（m=64MB, t=3, p=1）单次派生约 2.4 秒。

验收记录（加密）：上传后的对象不含任何可读词典标记；把 `installation.yaml` 的 `installation_id` 改成另一个值以模拟新设备后同步，另一台设备的密文快照被正确拉回、解密并合并。

**deployer 侧的日志看不到**：`WeaselDeployer` 进程里的 `LOG()` 写不进日志文件，上游自己的 `LOG(INFO) << "WeaselDeployer reporting."` 同样不出现——初始化过文件 sink 的那个 glog 实例在 `rime.dll` 里。云同步的错误信息因此只能靠返回值和外部观察判断，诊断信息要面向用户展示时，得走 GUI 而不是日志。

依赖上保持克制：HTTP 用 WinHTTP、加解密用 CNG，都是系统自带；不引入 OpenSSL 或 libcurl，每多一个第三方库就多一层上游合并的负担。

阶段四已完成，含三轮独立审查发现的全部有效问题。判定为不成立的结论与其证据记在 [REVIEW-NOTES.md](REVIEW-NOTES.md)，动这块代码前先读。

留下的短板有两处，都由下一阶段解决：主密码经 `HareDeployer.exe /cloudkey:<密码>` 传入，命令行对同机其他进程可见；四种后端的配置只能改注册表，`tools\configure-sync.ps1` 只是把这件事变成一条命令，不是界面。

## 阶段五：WebView2 设置面板

两个面板：同步配置与皮肤选择。需要 `Microsoft.Web.WebView2` SDK 与 WRL。

## 阶段六：图片皮肤

改 `WeaselUI/WeaselPanel.cpp` 的绘制路径与 `Layout` 的尺寸计算。把绘制拆成背景源、帧、合成三层，图片解码与九宫格逻辑放进新文件。附带一个 `.ssf` 解包器（ZIP 容器，`skin.ini` 为 UTF-16LE），只抽取图片资源。

第一版全静态；动图与过渡动画在架构就位后接定时器实现。

## 参考环境

开发者机器上的既有安装，用于对照与取用共享数据：

```
官方小狼毫程序   D:\App\Rime\weasel-0.17.4
Rime 用户目录     D:\App\Rime\UserData      （注册表 HKCU\Software\Rime\Weasel\RimeUserDir 指向此处）
```

用户目录里跑的是万象拼音 base 全拼方案，配 RIME-LMDG 语法模型，默认英文标点。紫毫与小狼毫共用这个目录。
