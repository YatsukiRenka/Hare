# 换机继续开发

从零开始在一台新机器上接着做，按顺序走完这份文档即可。全部前提都在仓库里，除了两样东西：本机的开发工具，以及你自己的云存储凭证。

## 一、机器前提

| 需要 | 说明 |
|---|---|
| Visual Studio 2026 | 「使用 C++ 的桌面开发」工作负载，**必须含 ATL 与 MFC** |
| Windows SDK | 10.0.26100 或更新 |
| Git | 克隆仓库与子模块 |
| Node.js | 仅在需要部署 Cloudflare Worker 时用到（`npx wrangler`） |
| 一份已安装的 Rime 发行版 | 可选，用于取共享数据；没有的话构建会自动联网拉取 |

MFC 只为资源脚本提供 `afxres.h`，缺了会报 `RC1015`。工具集与其他陷阱见 [BUILD.md](BUILD.md)。

## 二、拉取与构建

```powershell
git clone https://github.com/YatsukiRenka/Hare.git
cd Hare
git submodule update --init plum

pwsh tools\install-deps.ps1     # 预编译 librime、Boost 源码、共享数据
cmd  /c tools\build-boost.bat   # 约 10 分钟
cmd  /c tools\build-hare.bat    # 约 35 秒，产物在 output\
```

`install-deps.ps1` 默认从 `D:\App\Rime\weasel-0.17.4\data` 取共享数据。新机器上路径不同就用 `-SharedDataSource` 指过去，或者不管它——找不到时会退回让 `build.bat` 联网拉取。

上游远端需要自己加回来（`git clone` 只带 origin）：

```powershell
git remote add upstream https://github.com/rime/weasel.git
```

## 三、安装以便实际测试

打包并安装，`/D=` 指定安装位置：

```powershell
cmd /c tools\build-hare.bat weasel installer
.\output\archives\hare-<版本>-installer.exe /S /D=D:\App\Rime
```

安装器的 `.onInit` 在注册表 `InstallDir` 为空时会强写 `%PROGRAMFILES64%\Rime` 而忽略 `/D=`。想装到别处就先建好这个值：

```powershell
New-Item -Path 'HKLM:\SOFTWARE\WOW6432Node\Rime\Hare' -Force
Set-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Rime\Hare' -Name InstallDir -Value 'D:\App\Rime'
```

让紫毫与既有配置共用用户目录：

```powershell
New-Item -Path 'HKCU:\Software\Rime\Hare' -Force
Set-ItemProperty -Path 'HKCU:\Software\Rime\Hare' -Name RimeUserDir -Value '<你的 Rime 用户目录>'
```

**同一时刻只能运行一个发行版**。用户词库是 LevelDB，持有排他锁；共用用户目录时另一个会连快照都合并不了，日志里是 `failed synchronizing 6/6 user dicts`。切换前先 `WeaselServer.exe /q` 或 `HareServer.exe /q`。

开发期只替换二进制而不重装：

```powershell
Copy-Item output\Hare*.exe, output\hare*.dll <安装目录> -Force
```

## 四、配置云同步

```powershell
pwsh tools\configure-sync.ps1 -Backend s3 -Endpoint https://<账号>.r2.cloudflarestorage.com -Bucket <桶名>
# 密钥留空会交互式询问，不会落盘、不会进仓库

& '<安装目录>\HareDeployer.exe' /cloudkey:<主密码>
```

返回码即失败原因：0 成功、1 密码太短、2 后端未配置、3 密码错、4 密钥生成失败、5 发布失败、6 缓存失败、7 存储不可达。

每套存储有各自独立的数据密钥，**换后端要重跑 `/cloudkey`**。`configure-sync.ps1` 会自动清掉本机缓存的旧密钥。

## 五、怎么验证

`tools\s3.ps1` 用来直接检查桶内容，凭证走参数或 `HARE_S3_*` 环境变量：

```powershell
pwsh tools\s3.ps1 list
pwsh tools\s3.ps1 get hare/<id>/wanxiang.userdb.txt
pwsh tools\s3.ps1 purge ''      # 清空
```

**模拟另一台设备**——验证拉取、解密、合并的核心手法：

```powershell
# 改 installation.yaml 里的 installation_id，清空 sync 目录，同步
# 若另一台设备的目录出现且内容可读，说明 List、Get、解密、合并全通
```

**验证前缀过滤是否真的生效**（也就是查询串有没有到达服务端）：往桶里放一个前缀之外的对象，例如 `zzz-outside/decoy.userdb.txt`。前缀过滤生效时它对紫毫不可见；若查询串被丢弃，List 会返回它，名字被 `substr` 截错，随后 `Get` 404 导致整个拉取失败。

**验证注册表隔离**：把紫毫的 `RimeUserDir` 指向一个全新目录，另一个发行版的键保持不动，然后同步——文件应当落在新目录里。曾经有一版两个键恰好指向同一处，掩盖了紫毫在读别人配置的 bug；**验证要能区分「碰巧对」和「真的对」**。

## 六、当前进度与下一步

进度见 [ROADMAP.md](ROADMAP.md)，设计取舍见 [DESIGN.md](DESIGN.md)，审查结论见 [REVIEW-NOTES.md](REVIEW-NOTES.md)——**动云同步代码之前先读第三份**，里面记着哪些「问题」是经实测判定为不成立的，以免重复修改。

下一阶段是 WebView2 设置面板。它要解决的短板很具体：主密码目前经 `HareDeployer.exe /cloudkey:<密码>` 传入，命令行对同机其他进程可见；四种后端的配置也还只能靠改注册表。
