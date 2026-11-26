# 发布新版本

“发布新版本”包含代码梳理、文档、截图、验证、打包、提交推送和 GitHub Release。只有上传一个新程序，或者只修改 README 文字，都不算完成。用户明确限制范围时以当次要求为准。

## 步骤总览

用户说“发布新版本”就按这个顺序做完，每一步的做法和注意事项在后面各节。

1. **收拢代码**：查 `git status`、各分支和 `.worktrees/`、其他正在工作的会话；把做完的分支合并进 `main`，未提交的修改核对后提交；合并后全量构建一次（各分支自己的会话不一定构建过全部测试）。合并完的分支和工作树在发布成功后回收。
2. **定版本**：改 `CMakeLists.txt` 的 `project(VERSION)`；有变化的官方插件各升一版。
3. **文档与截图**：发布说明、中英文 README、使用说明、features.en.md、涉及的插件文档；新功能补拍海报。
4. **验证与打包**：只跑本次改动涉及的测试；`scripts/release.ps1` 打包（自带安装实测）；本机跑一次应用内升级实测；写 `docs/validation/release<版本>/`。
5. **推送与发布**：推送 `main`；先发布有变化的官方插件到插件库；再 `scripts/release.ps1 -Publish`（GitHub Release、CNB 镜像、Scoop 一并完成）。
6. **发布后核对**：GitHub 附件的 SHA256、CNB 下载、lcstp.com 两个接口、插件库 index；关闭这一版解决的 issue 并回复；回收分支、工作树和本次的临时构建目录。

开始前先确认 GitHub 能不能用，见[GitHub 网页登录不了时](#github-网页登录不了时)。

## 先确认发布内容

检查 `git status --short`、最近提交、现有版本与 GitHub 最新 Release。若有协作聊天正在修改同一项目，先确认其结果；不要和另一个发布流程同时写版本、图片或安装包。区分本次改动、无关编辑和用户数据，按功能拆分 commit，不用清理或回滚抹平差异。

版本来自 `CMakeLists.txt` 的 `project(VERSION)`。结合实际增量选择下一个版本，不覆盖已经发布的同版本附件或移动旧标签。主仓库为 [LingCore/WinShun](https://github.com/LingCore/WinShun)，插件库为 [LingCore/WinShun-plugins](https://github.com/LingCore/WinShun-plugins)，不要新建替代仓库。

## 文档与截图

- README 中英文介绍、默认行为、快捷键和功能列表应与最终代码一致；README 只写概览（是什么、三步上手、功能表、插件表、常用按键、下载安装、最近更新、常见问题），细节写进 [使用说明](使用说明.md) 和英文的 [features.en.md](features.en.md)，三处同步更新。
- 更新 `docs/使用说明.md`；涉及插件数据、权限或 SDK 的改动，同步中英文插件文档和示例。
- 写 `docs/release-notes/v<版本>.md`。首段及 `## 新功能 · What's new` 是更新对话框解析的内容，保持现有格式；写清下载文件名和升级时如何保留数据。
- 先看旧海报及 [截图项目说明](../tools/screenshots/README.md)，再补拍新增和变动的界面。使用真实新版程序、生产 QML / 控制器与隔离演示数据；未变化的功能可沿用已核验的图。不要把伪造界面或旧验证结果当成本次证据。
- 沿用 `art.compose(..., "square", ...)`：Logo、LingCore 署名、彩色标题、副标题、背景、底部文字均保留。海报使用原 `.jpg` 路径，1440×1440、质量 96、`subsampling=0`。README 只放封面图 `cover.jpg`；其余功能图放在 [docs/截图.md](截图.md)，按功能分组，以 `width="49%"` 排成两列，新功能的图加进对应分组。
- 原图按物理像素或高像素密度实际渲染，不放大低清位图。逐张检查标题、裁切、文字、当前界面和私人信息，再检查 GitHub 上的真实双列效果。先提交更新的图片，再引用该提交 SHA 的不可变 raw 地址，避免同名旧图缓存；仓库内的图片路径保持不变。相对引用加版本查询参数已验证不能可靠刷新 GitHub raw 跳转，不再采用。

这些约束针对本项目的品牌海报。清晰度问题应从原图与导出质量解决；把海报换成纯窗口截图，会同时丢掉品牌元素和原有版式，不能算质量修复。

## 验证与打包

优先用临时配置和现有 `--preview-*` 模式；录屏用自建演示窗口，不采集用户文档和声音。不要为了截图或替换 DLL 退出用户程序。

使用新的绝对输出路径，例如：

```powershell
.\scripts\release.ps1 -OutputDir 'F:\电脑便捷工具\dist\release015'
```

构建目录中的程序也在运行时，可以同时指定 `-BuildDir 'F:\电脑便捷工具\build\release-isolated'`。构建、CTest、部署、可选插件与 OCR 打包都使用该目录，默认的 `build/release` 不受影响。`scripts/build.ps1` 同样支持 `-BuildDir`；部署前拒绝覆盖源码、构建目录、含 `data` 的目录和运行中的 Win顺。

测试只按需跑：不跑全部 CTest，发布也一样。用 `-Tests` 传本次改动涉及的测试（ctest `-R` 正则，例如 `-Tests 'tst_capture|tst_clipimage'`）；不传就不跑测试。新功能和修复照常做实际验证。

脚本构建（传了 `-Tests` 就跑这几个测试），再生成标准安装包、便携包、完整离线安装包、独立增强 OCR 组件 ZIP、增量更新包及 `SHA256SUMS.txt`。输出目录内运行着 Win顺，或部署目录含 `data` 时应换一个新目录。不要删除用户数据以绕过保护。

增量更新包（见 [diagnostics-and-updates.md](architecture/diagnostics-and-updates.md)）：脚本从 GitHub 下最近 `-DeltaCount`（默认 3）个能用增量的已发布版本的便携包，各生成一个 `WinShun-<版本>-x64-delta-from-<旧版本>.zip`（`build\…\WinShunDelta.exe make`，带增量安装程序 `WinShun-<版本>-x64-delta-setup.exe`），再用同一个工具按 Win顺 的代码还原一遍，和这次部署逐个文件比对，不一致就停下。比安装包的 60% 还大的不发。生成的隐藏行写进 `<输出目录>\delta-notes-line.txt`，`-Publish` 时加在发布说明末尾（`docs/release-notes` 里的原文不改）一起上传；这些包也是 Release 附件，也写进 `SHA256SUMS.txt`。发布后查 lcstp.com 的回答时，`body` 末尾应有这行 `<!-- winshun-delta 1 … -->`。

构建依赖与应用内公开 GitHub 文件的默认镜像统一维护在 `resources/downloads/github-mirrors.txt`；修改后重新配置并构建。`cmake/Download.cmake` 优先镜像、再原站，每个响应都按固定 SHA256 检查，失败不使用半个文件；下载回归包含本地故障注入，公网验证需显式开启 `WINSHUN_UPDATE_LIVE=1`，不会运行安装程序。

标准包只带完整常规字体，不带独立粗体、ONNX Runtime 和 OCR 模型。截图插件 `winshun.capture` 的解包副本（`library-plugins\winshun.capture`，本次构建产物）随安装包、离线安装包和便携包附带，供首次引导不联网安装；它与插件库里同版本号的包必须是同一份源码，所以截图源码（含编译进去的共享文件）有变化就升截图插件版本并发布到插件库。`tools/package_ocr.py` 依据 `resources/ocr/package.json` 校验每个原文件的大小与 SHA256，再生成 `WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip`；完整离线安装包由标准目录加同一份已校验组件构成，文件名为 `WinShun-<版本>-x64-offline-setup.exe`。组件 ZIP 必须随同一版本 Release 一起上传，应用内下载地址使用该版本标签；只上传主安装包会使首次安装增强识别失败。组件换版本或原文件时，同步更新清单、`OcrFiles::kVersion` 和下载大小，再验证取消、损坏包、离线安装、截图续识别及旧文件兼容。

按改动验证实际生产路径。涉及录屏时，检查真实 MP4 / GIF 能解码、分辨率与帧率、暂停继续、再录一段及旧文件保留；音频未实测就不要写“音频已实测”。明确区分当前系统上的验证与尚未覆盖的系统。

核对便携包只含发布文件与空的 `data` 说明，不能带索引、设置、剪贴板或演示配置。核对安装包版本、部署程序和截图记录的程序 SHA256，以及四个产物的校验值。构建、测试或验证失败就先修复，不能跳过失败直接发布。

## 安装和升级必须实测

安装、升级出问题已经反复出现（#68、#97、#274、#300），每次都是“脚本只编译过，没有真的跑过”。发布前这两项都要有结果，写进当次的发布验证记录：

- **占用文件的实测（自动，必过）**：`tools/verify-installer.ps1 -WorkDir <新目录>`。它用同一份 `installer/WinShun.iss` 编译测试版（`/DTestInstall`：独立的 AppId 和程序名，不建快捷方式、不跑 [Run]、不写卸载项、不提权，不会碰本机装着或运行着的 Win顺），真的安装、真的静默升级，覆盖：正常升级、自己的程序还在运行、别的程序加载着库不放、别的程序在读文件、文件被独占打开（应当很快放弃、说明是谁、旧版本完好）。`scripts/release.ps1` 在打包前自动运行，失败就不出包。改过 `installer/` 下任何文件，或 `src/updater`、`Updater.cpp` 里启动安装程序的部分，提交前就要先跑一遍。
- **真实升级（在开发机上做，先问用户）**：`tools/verify-installed-upgrade.ps1` 对本机 Program Files 里装着的那份做真实更新，每种方式都可以加 `-Hold`（另一个程序加载着程序目录里的库不放），跑完核对版本、新版本是否以普通权限启动、索引服务、自启任务、设置和历史。它会关掉并**替换**本机装着的 Win顺、弹 UAC，所以每次都要先得到用户同意；跑完本机装的就是新构建。
  - `-Mode InApp -Feed <发布目录> -Version <新版本>`：**应用内更新整条链路**（检查、下载、校验、安装程序、守护的助手、新版本启动）。本机起一个只听 127.0.0.1 的假更新服务（`tools/update-feed.py`），已装的那份带着 `WINSHUN_UPDATE_AUTOINSTALL` 启动后自己完成更新。已装的版本要有这个开关（0.25.0 之后的版本）。每次发布至少跑这一项。已装的版本能用增量、发布目录里有对应它的增量包时，走的是增量更新（报告的 `downloaded` 是 `…-delta-from-….zip`）；加 `-NoDelta` 测完整包这条路。
  - `-Mode Setup -Setup <安装包>`：按应用内更新的参数直接静默运行安装包。
  - `-Mode Wizard -Setup <安装包>`：像双击那样走向导，页面由提权的 `tools/installer-wizard-clicks.ps1` 点过去。改过向导页面或安装模式的询问时跑。
  - `-Mode Portable -PortableZip <旧便携包> -Feed <发布目录> -Version <新版本>`：便携版自己更新（换文件的助手）。改过 `src/updater` 时跑；期间会暂时退出已装的那份，结束后恢复。
  - “只为我安装”的升级要先卸载所有用户版（两种装法互斥），做法和注意事项见 `docs/validation/installer-held-files-20261112`（卸载会带走开机自启任务，装回后用 `WinShun.exe --take-autostart --had-autostart` 补上）。安装模式相关的代码有改动时才做。
  - 没有做的项，在发布验证记录的“没有覆盖的”里写明，不要写成已验证。

安装脚本处理被占用文件的办法（2026-11-12 起）：关掉 Win顺、结束所有从安装目录运行的程序之后，仍被占着的程序、库和字体文件改名挪到一边（`名字.wsold`），新文件照常写入，占用它的程序继续用旧的那份；旧文件在放开之后由本次安装结束时、下次安装、卸载或（为所有用户安装时）下次重启清掉。只有既打不开又挪不动的文件（被独占打开）才让安装停下，此时把已挪开的放回原名，旧版本保持完整，日志里写 `Update blocked by:`，Win顺 的通知据此说明是谁占着。不要再改回“等一会儿，不行就放弃或问用户”。

## 推送与发布

官方可选插件（以 `CMakeLists.txt` 里 `library_plugins` 列的为准，不固定个数）也要随宿主协议变动核对：构建目录 `<BuildDir>/library-plugins` 中的包（`release.ps1` 另外导出 `.wsplugin` 到 `<OutputDir>/library-plugins`）先用插件库工具检查真实归档，确认 `minWinShun`、入口文件、权限、许可证和 README，再以新的插件版本发布到 `LingCore/WinShun-plugins`。记录主仓库源码提交，联网生成条目并核对 index 的公开结果后，才发布依赖它们的宿主版本。旧插件版本已经发布时必须核对原附件，不可用本次重新编译的文件覆盖；内容变化就升插件版本。官方原生包的受限白名单不适用于社区插件。

提交所有本次必要改动并推送。保存已验证的提交 SHA，再次检查远端有无同版本 Release。

先创建草稿 Release，使用明确的提交 SHA 为目标，上传标准安装包、便携包、完整离线安装包、增强 OCR 组件 ZIP、增量更新包和 SHA256 文件。确认附件齐全、大小及远端 SHA256 与本地一致后，才发布草稿并设为最新正式版本。最后确认公开 Release、最新版本接口和标签提交一致；不要在没有核验结果时仅凭上传命令成功报告完成。

软件内更新问的是 lcstp.com，它最多 10 分钟后才从 GitHub 同步到新版本。发布后用 `curl` 查这两个地址，核对 `tag_name`、文件名、`size` 和 `digest` 是否和 `SHA256SUMS.txt` 一致：

```powershell
curl.exe -s 'https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631'
curl.exe -s 'https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631&package=portable'
```

两条都要查到新版本：安装版那条好了不代表便携版也好了。lcstp.com 每个平台和架构只认一个安装包、一个 zip，同类多出一个就两个都不用，这条又会退回上一个版本。所以 Release 上另外加的附件（离线安装包、OCR 组件、增量包）要在服务器里登记为“不用于自动更新”。0.36.0 第一次带增量包时，便携版就因此停在 0.35.0，直到服务器把 `-delta-from-` 的 zip 和 `-delta-setup.exe` 也算进去。以后再给 Release 加新种类的附件，先改服务器。

### GitHub 网页登录不了时

发版不经过网页：推送、建 Release、传附件、发布插件、回复 issue 全部用这台电脑上 `gh` 保存的令牌。网页登录不了（密码、两步验证丢失）不等于不能发版，先查令牌还在不在：

```powershell
gh auth status                                                   # 应显示 Logged in ... account LingCore
gh api repos/LingCore/WinShun --jq .permissions.push             # true
gh api repos/LingCore/WinShun-plugins --jq .permissions.push     # true
git ls-remote --heads origin main                                # 能列出提交
```

四条都通过就照常发版，流程一步不变。这期间注意：

- 不要运行 `gh auth logout`、`gh auth login`、`gh auth refresh`，也不要清 Windows 凭据管理器里 github.com 的条目：重新授权要在网页上登录，令牌丢了就找不回来。
- 在网页上改密码、走账号找回流程可能让现有令牌失效。用户处理账号之前先把要发的版本发完，处理之后重新跑上面四条。
- 只能在网页上做的事这段时间做不了：改仓库设置和 Actions 密钥（插件库 index 的签名密钥在 GitHub Environment 里）、审批首次贡献者的 Actions 运行、建新的个人令牌。插件库的发布和签名由 Actions 完成，令牌能触发，不受影响。

令牌失效（上面任何一条失败）时**不能发版**，只能做到打包：

- 能做：构建、测试、`scripts/release.ps1`（不带 `-Publish`）出安装包和便携包、`tools/verify-installer.ps1`、本机应用内升级实测。增量包要从 GitHub 下载旧版本，下不到时脚本只警告、不出增量包。
- 做不了：软件内更新、下载页和 CNB 镜像都以 GitHub Release 为准（lcstp.com 从 GitHub 同步，`scripts/cnb-mirror.ps1` 按 GitHub 的附件和 SHA256 核对），插件库 index 和 Scoop 清单也在 GitHub。没有 GitHub，新版本到不了任何用户手里。
- 这时如实告诉用户“包已打好、没有发布”，把产物留在 `dist/<任务名>/`，等账号恢复后从同一个提交重新走“推送与发布”。不要改成只在 CNB 建 Release：软件内更新看不到它，还会和之后 GitHub 上的同名标签对不上。

### 国内镜像（CNB）

腾讯 CNB 上的 https://cnb.cool/lcstp/WinShun 是 GitHub Release 的国内镜像（#407）：同一个标签、同一个提交，附件同名同内容。组织 `lcstp` 用 lcstp.com 域名认证（Cloudflare 上 `cnb.lcstp.com` 的 TXT 记录，不要删）。

- `release.ps1 -Publish` 在 GitHub 发布成功后自动运行 `scripts/cnb-mirror.ps1`：推送标签和 main，建 CNB Release（说明去掉 `winshun-` 开头的隐藏行），上传缺的附件，再不带令牌从 cnb.cool 把每个文件下载回来，核对 GitHub 给的 SHA256。这一步失败只警告、不中断（GitHub 已经发出去了），按提示单独重跑；重跑只补缺的。补传旧版本：`./scripts/cnb-mirror.ps1 -Tag vX.Y.Z`。
- 下载地址 `https://cnb.cool/lcstp/WinShun/-/releases/download/<标签>/<文件名>`，302 到 `asset.cnb.cool`（不带参数），不用登录。软件内更新要走 CNB，lcstp.com 得把这个地址放在每个附件 `mirrors` 的最前面。客户端按顺序试、按 GitHub 的 SHA256 校验，增量包按“同目录换文件名”推出地址，所以已装的版本不用改；CNB 上缺文件只是多试一个源。发布后查 lcstp.com 时顺便看第一个镜像是不是 cnb.cool。
- 令牌：CNB 个人设置 → 访问令牌 “WinShun 发布同步”，只限 lcstp/WinShun，`repo-code` 和 `repo-release` 读写，永久有效。存在 Windows 凭据管理器（git 对 https://cnb.cool、用户名 `cnb` 的凭据），脚本用 `git credential fill` 读；环境变量 `CNB_TOKEN` 优先。换令牌时在自己的终端运行（令牌不进命令历史）：

  ```powershell
  $t = Read-Host 'CNB 令牌'; "protocol=https`nhost=cnb.cool`nusername=cnb`npassword=$t`n" | git credential approve; Remove-Variable t
  ```

- 额度：附件算对象存储，免费 100 GiB；一个版本约 155 MB。软件内更新只用最新版本，CNB 上的旧版本 Release 可以删。

### 推送方式：立即推送 / 正常推送

每次发布由开发者决定用哪一种。用户没说就用正常推送；用户说“立即推送”“马上推给大家”“这是重要更新”之类，就用立即推送。发布前给用户的汇报里写明这次用的是哪一种。

| | 正常推送（默认，`-Push Normal`） | 立即推送（`-Push Now`） |
|---|---|---|
| 用户什么时候看到托盘提示 | 每份 Win顺 在发布后 24 小时内各自固定的一个随机时刻（`update.ini` 的 `RolloutSlot`，第一次用时随机抽取） | lcstp.com 同步到新版本后（最多 10 分钟），下一次检查（每 10 分钟一次）就提示 |
| 提示的样子 | “Win顺 x.y.z 可以更新了” | “Win顺 x.y.z 是重要更新 / 建议尽快更新”，更新对话框标题也这样写 |
| 没更新时再提醒 | 同一版本最多三天一次 | 每天一次，直到更新或跳过 |
| 适合 | 平常的功能和修复。分批到达，万一新版本有问题只影响先到的一部分人，下载也分散 | 严重问题、安全问题、更新链路本身的问题 |

两种方式下，用户自己点“检查更新”或打开设置里的更新页，都能马上看到新版本；跳过的版本两种都不再自动提醒。

做法：`-Push Now` 让脚本在发布说明末尾加一行隐藏注释 `<!-- winshun-push now -->`（和 `winshun-delta` 那行一样，`docs/release-notes` 里的原文不改；原文里手写了这行，脚本会停下）。lcstp.com 原样转发说明正文，服务器不用改。发布后核对 lcstp.com 回答的 `body`：立即推送时应有这行，正常推送时没有。

发布后改主意：正常推送改成立即推送，就把公开 Release 的说明下载下来（`gh release view vX.Y.Z --repo LingCore/WinShun --json body --jq .body > notes.md`），在末尾加上这行，再 `gh release edit vX.Y.Z --repo LingCore/WinShun --notes-file notes.md`。lcstp.com 10 分钟内同步，各客户端在下一次检查时按立即推送提示。去掉这行就回到正常推送，已经弹出的提示不会撤回。

客户端从启动时起每 10 分钟查一次 lcstp.com（第二次起带上一次回答的 ETag，没变时服务器只回 304，没有正文）。实现见 [diagnostics-and-updates.md](architecture/diagnostics-and-updates.md)，测试 `tst_updater` 的 `pushNowOrInTurn`；本机假服务 `tools/update-feed.py` 加 `--push-now` 模拟立即推送。

Scoop：`scripts/release.ps1 -Publish` 在 Release 创建成功之后，把 `bucket/winshun.json` 的 `version`、`url`、`hash` 改成这次的便携包（只改这三处，保持 4 空格缩进和无 BOM 的 UTF-8），只提交这一个文件（`git commit --only`，工作区里可能有别人的修改）为 “Scoop: winshun <版本>” 并推送。用户用 `scoop bucket add winshun https://github.com/LingCore/WinShun` 加入的就是主仓库，所以推送要在 Release 之后，清单不能比 zip 先到。`checkver` / `autoupdate` 写好了（`$sha256\s+$basename` 只匹配便携包那一行），发布后可以用 Scoop 自带的 `bin\checkver.ps1 -App winshun -Dir bucket -Update`（先复制一份目录）核对：它会用公开的 SHA256SUMS.txt 算出同样的值。`tests/tst_scoopinstall.cpp` 检查清单是合法 JSON、正则能在样例校验文件里只匹配便携包。

WinGet：包 `LingCore.WinShun` 在 `microsoft/winget-pkgs` 的 `manifests/l/LingCore/WinShun`，由 `SpecterShell/Dumplings`（`Tasks/LingCore.WinShun/Script.ps1`）自动跟踪更新，不归本仓库维护。Release 公开后会有自动提交的 “New version: LingCore.WinShun version <版本>” PR，由 winget 维护者审核合并后才上架，所以 `winget install` 拿到新版本通常比 Release 晚一两天；清单里的安装包是离线安装包 `WinShun-<版本>-x64-offline-setup.exe`（自带增强文字识别），`Scope: machine`，`winget install` 装成给所有用户的版本（`C:\Program Files\WinShun`，带索引服务）。发布后核对最新那个 PR 的 `InstallerUrl`、`InstallerSha256`（与公开的 `SHA256SUMS.txt` 一致）以及 `ReleaseNotes` / `ReleaseNotesUrl` 即可，不用自己建清单；README 和[使用说明](使用说明.md#用-winget-安装更新卸载)只写命令、不写版本号，免得包还没跟上时对不上。旧清单里多带的 `Microsoft.VCRedist.2015+.x64` 依赖（程序自己带着 C++ 运行库）、与 `Scope: machine` 矛盾的 `DefaultInstallLocation`（`%LocalAppData%\Programs\WinShun`）以及 `en-US` 里停在 `v0.8.0` 的 `ReleaseNotesUrl`，已通过 `microsoft/winget-pkgs#485535` 和 `SpecterShell/Dumplings#166` 提交清理。

已发布成功的单次任务不得重复执行；若由一次性自动化触发，成功后关闭它。论坛文案可另行提供，但发布到论坛或向他人发消息需要用户明确要求。
