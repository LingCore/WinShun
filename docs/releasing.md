# 发布新版本

“发布新版本”包含代码梳理、文档、截图、验证、打包、提交推送和 GitHub Release。只有上传一个新程序，或者只修改 README 文字，都不算完成。用户明确限制范围时以当次要求为准。

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

脚本构建（传了 `-Tests` 就跑这几个测试），再生成标准安装包、便携包、完整离线安装包、独立增强 OCR 组件 ZIP 及 `SHA256SUMS.txt`。输出目录内运行着 Win顺，或部署目录含 `data` 时应换一个新目录。不要删除用户数据以绕过保护。

构建依赖与应用内公开 GitHub 文件的默认镜像统一维护在 `resources/downloads/github-mirrors.txt`；修改后重新配置并构建。`cmake/Download.cmake` 优先镜像、再原站，每个响应都按固定 SHA256 检查，失败不使用半个文件；下载回归包含本地故障注入，公网验证需显式开启 `WINSHUN_UPDATE_LIVE=1`，不会运行安装程序。见 [镜像审计记录](download-mirrors-audit.md)。

标准包只带完整常规字体，不带独立粗体、ONNX Runtime 和 OCR 模型。`tools/package_ocr.py` 依据 `resources/ocr/package.json` 校验每个原文件的大小与 SHA256，再生成 `WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip`；完整离线安装包由标准目录加同一份已校验组件构成，文件名为 `WinShun-<版本>-x64-offline-setup.exe`。组件 ZIP 必须随同一版本 Release 一起上传，应用内下载地址使用该版本标签；只上传主安装包会使首次安装增强识别失败。组件换版本或原文件时，同步更新清单、`OcrFiles::kVersion` 和下载大小，再验证取消、损坏包、离线安装、截图续识别及旧文件兼容。

按改动验证实际生产路径。涉及录屏时，检查真实 MP4 / GIF 能解码、分辨率与帧率、暂停继续、再录一段及旧文件保留；音频未实测就不要写“音频已实测”。明确区分当前系统上的验证与尚未覆盖的系统。

核对便携包只含发布文件与空的 `data` 说明，不能带索引、设置、剪贴板或演示配置。核对安装包版本、部署程序和截图记录的程序 SHA256，以及四个产物的校验值。构建、测试或验证失败就先修复，不能跳过失败直接发布。

## 安装和升级必须实测

安装、升级出问题已经反复出现（#68、#97、#274、#300），每次都是“脚本只编译过，没有真的跑过”。发布前这两项都要有结果，写进当次的发布验证记录：

- **占用文件的实测（自动，必过）**：`tools/verify-installer.ps1 -WorkDir <新目录>`。它用同一份 `installer/WinShun.iss` 编译测试版（`/DTestInstall`：独立的 AppId 和程序名，不建快捷方式、不跑 [Run]、不写卸载项、不提权，不会碰本机装着或运行着的 Win顺），真的安装、真的静默升级，覆盖：正常升级、自己的程序还在运行、别的程序加载着库不放、别的程序在读文件、文件被独占打开（应当很快放弃、说明是谁、旧版本完好）。`scripts/release.ps1` 在打包前自动运行，失败就不出包。改过 `installer/` 下任何文件，或 `src/updater`、`Updater.cpp` 里启动安装程序的部分，提交前就要先跑一遍。
- **真实升级（要一台干净的 Windows）**：在虚拟机或 Windows 沙盒里，装上一个正式版并运行，再用新安装包覆盖升级（一次双击安装，一次应用内更新），确认新版本启动、设置和索引还在、索引服务正常。本机 2026-11-12 没有可用的隔离环境（没有虚拟机，启用 Windows 沙盒时系统报“组件存储已损坏”）；没有做这一项时，在发布验证记录的“没有覆盖的”里写明，不要写成已验证。

安装脚本处理被占用文件的办法（2026-11-12 起）：关掉 Win顺、结束所有从安装目录运行的程序之后，仍被占着的程序、库和字体文件改名挪到一边（`名字.wsold`），新文件照常写入，占用它的程序继续用旧的那份；旧文件在放开之后由本次安装结束时、下次安装、卸载或（为所有用户安装时）下次重启清掉。只有既打不开又挪不动的文件（被独占打开）才让安装停下，此时把已挪开的放回原名，旧版本保持完整，日志里写 `Update blocked by:`，Win顺 的通知据此说明是谁占着。不要再改回“等一会儿，不行就放弃或问用户”。

## 推送与发布

官方可选插件（以 `CMakeLists.txt` 里 `library_plugins` 列的为准，不固定个数）也要随宿主协议变动核对：构建目录 `<BuildDir>/library-plugins` 中的包（`release.ps1` 另外导出 `.wsplugin` 到 `<OutputDir>/library-plugins`）先用插件库工具检查真实归档，确认 `minWinShun`、入口文件、权限、许可证和 README，再以新的插件版本发布到 `LingCore/WinShun-plugins`。记录主仓库源码提交，联网生成条目并核对 index 的公开结果后，才发布依赖它们的宿主版本。旧插件版本已经发布时必须核对原附件，不可用本次重新编译的文件覆盖；内容变化就升插件版本。官方原生包的受限白名单不适用于社区插件。

提交所有本次必要改动并推送。保存已验证的提交 SHA，再次检查远端有无同版本 Release。

先创建草稿 Release，使用明确的提交 SHA 为目标，上传标准安装包、便携包、完整离线安装包、增强 OCR 组件 ZIP 和 SHA256 文件。确认五个附件齐全、大小及远端 SHA256 与本地一致后，才发布草稿并设为最新正式版本。最后确认公开 Release、最新版本接口和标签提交一致；不要在没有核验结果时仅凭上传命令成功报告完成。

软件内更新问的是 lcstp.com，它最多 10 分钟后才从 GitHub 同步到新版本。发布后用 `curl` 查这两个地址，核对 `tag_name`、文件名、`size` 和 `digest` 是否和 `SHA256SUMS.txt` 一致：

```powershell
curl.exe -s 'https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631'
curl.exe -s 'https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631&package=portable'
```

Scoop：`scripts/release.ps1 -Publish` 在 Release 创建成功之后，把 `bucket/winshun.json` 的 `version`、`url`、`hash` 改成这次的便携包（只改这三处，保持 4 空格缩进和无 BOM 的 UTF-8），只提交这一个文件（`git commit --only`，工作区里可能有别人的修改）为 “Scoop: winshun <版本>” 并推送。用户用 `scoop bucket add winshun https://github.com/LingCore/WinShun` 加入的就是主仓库，所以推送要在 Release 之后，清单不能比 zip 先到。`checkver` / `autoupdate` 写好了（`$sha256\s+$basename` 只匹配便携包那一行），发布后可以用 Scoop 自带的 `bin\checkver.ps1 -App winshun -Dir bucket -Update`（先复制一份目录）核对：它会用公开的 SHA256SUMS.txt 算出同样的值。`tests/tst_scoopinstall.cpp` 检查清单是合法 JSON、正则能在样例校验文件里只匹配便携包。

已发布成功的单次任务不得重复执行；若由一次性自动化触发，成功后关闭它。论坛文案可另行提供，但发布到论坛或向他人发消息需要用户明确要求。
