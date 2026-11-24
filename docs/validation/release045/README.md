# 0.45.0 验证记录

发布提交 19212864，立即推送（`-Push Now`，用户要求）。内容是 0.44.0 之后 main 上的全部提交（#422、#427、#428、#432、#434–#436、#438–#441、#443–#448、#450、#451，录屏画质与帧率、鼠标拖尾、截图随安装包附带、贴图分组和历史、聚光灯、长截图自动滚动、Tessoa 接管的对话框、插件库卡片管理等），加上发版前修好的“安装程序最后一页还开着时 Win顺 就弹出来”（5b1eb4a9）。

## 自动测试

只跑了这次改动涉及的测试（`scripts/release.ps1 -Tests`）：
`tst_aisettings`、`tst_annotate`、`tst_annotmodel`、`tst_appearance`、`tst_bookmarks`、`tst_capture`、`tst_capturesettings`、`tst_clipfiles`、`tst_dialogbarsuggestions`、`tst_editor`、`tst_elide`、`tst_encoding`、`tst_filedialog`、`tst_foldermenu`、`tst_framereveal`、`tst_fuzzymatch`、`tst_gestures`、`tst_hotpages`、`tst_idletrim`、`tst_keyrouter`、`tst_localsend`、`tst_numberhints`、`tst_ocr`、`tst_paster`、`tst_picturewrite`、`tst_pinclipboard`、`tst_pingroups`、`tst_pinhistory`、`tst_pinwindow`、`tst_plugin`、`tst_presenter`、`tst_recording`、`tst_settings`、`tst_settingsnav`、`tst_stitch`、`tst_taskbarwindow`、`tst_translate`、`tst_traymenu`。

发布前打包（`dist\release045`）时这 36 组全部通过。这一轮的代码在正式发布时没有变化（之后只提交了文档和海报），正式发布那次打包不再重复跑测试。

另外单独跑了两组（它们在 ctest 里有已知问题，没放进 `-Tests`）：
- `tst_core`（电源命令、路径读取的单元测试在里面）：直接运行，288 个通过、3 个按条件跳过。
- `tst_shelllaunch`（移到回收站、XYplorer 路径读取在里面）：`appsFolderConsole` 两个用例照旧失败。这台电脑的“所有应用”里没有“Windows PowerShell”，属于环境不满足，0.44.0 的记录里写过。`startsOurOwnProgramSetToRunAsAdministrator` 第一次失败，退出码 0xC000013A，因为它和同时进行的软件内升级测试撞在一起：升级测试会结束所有 Win顺。单独重跑通过。其余 26 个通过、5 个按条件跳过。

## 安装程序的“完成”（5b1eb4a9）

- 安装程序在最后一页之前就以 `--background --setup-page` 在后台启动 Win顺（静默安装不带 `--setup-page`）。带这个参数时，欢迎引导先不显示，由一个临时的消息窗口等“完成”：勾着“运行 Win顺”时收到 `show` 才显示，没勾时收到 `--quit` 就退出；安装程序没说话就结束了，10 秒后照样显示引导并在日志里写一条警告。
- 用一个代替安装程序的父进程、全新的 `--profile` 实测三种情况：点“完成”前什么都不显示、点了之后出现“欢迎使用 Win顺”、退出正常；没勾时什么都不显示、`--quit` 能结束；安装程序先结束时不立刻显示、约 13 秒内出现、日志有警告。前后核对了注册表里的“在此搜索”项和开机任务，没有变化。
- `tools/verify-installer.ps1` 6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。
- 没有在虚拟机里用真实的安装包从头装一遍看“完成”这一步。

## 安装和升级

- 发布前打包（`dist\release045`）里的 `verify-installer.ps1`：6 项全过。
- `tools/verify-installed-upgrade.ps1 -Mode InApp -Feed dist\release045 -Version 0.45.0`：本机装的 0.44.0 走软件内更新到 0.45.0，下载的是 `WinShun-0.45.0-x64-delta-from-0.44.0.zip`（10 936 604 字节），用时 48.6 秒。更新后服务在运行，新版本以普通权限启动。装好的 `WinShun.exe`（1a1f47cc…）和打包出来的相同。测试前本机没有运行 Win顺，测试后新版本在运行。

## 海报

`tools/screenshots/release045.py`，用 0.45.0 部署里的截图插件跑它自己的 `--preview-recording --auto --trail --fps 60`。这是真实的录屏面板，自建演示窗口、真的录了几秒，200% 渲染。

- `recording-ultra.jpg`（新）：录屏选项展开，画质“超清”、帧率“60 帧”、鼠标拖尾打开，上方显示“3200 × 1840 · 60 帧/秒”。“保存到”那一行会露出预览的工作目录，所以裁掉了。放在 `docs/截图.md` 的录屏一组。

没有新海报的：贴图分组和贴图历史、聚光灯（预览的画布上同时有别的测试标注）、长截图自动滚动、模糊匹配、电源命令、移到回收站、界面大小快捷键、剪贴板纯文本粘贴、手势按键、对话框建议来源、悬停提示分类、插件库卡片。

## 插件库

7 个官方插件发了新版本。包直接取自发布前打包的 `dist\release045\library-plugins`：构建开始时工作区是干净的（01f32c2c），之后只提交了文档和海报，所以和从干净副本编出来的是同一份源码。

| 插件 | 版本 | 最低 Win顺 | 说明 |
|---|---|---|---|
| 截图与录屏 winshun.capture | 1.1.33 | 0.18.0 | 录屏画质和帧率、鼠标拖尾、贴图分组和历史、截图并直接贴图、存回原文件、聚光灯、长截图自动滚动等；安装包附带的就是这一份 |
| 鼠标手势 winshun.gestures | 2.1.3 | 0.41.0 | 中键、侧键和组合手势（#436），翻译密钥被拒时说明（#444） |
| 翻译 winshun.translate | 2.2.5 | 0.17.0 | 401/403 显示成要设置的一行（#444） |
| 局域网传输 winshun.localsend | 1.0.9 | 0.17.0 | 读不到剪贴板时写日志（#435） |
| 浏览器书签 winshun.bookmarks | 2.2.1 | 0.17.0 | 浏览历史只用 `bh`，`ls` 让给局域网传输 |
| 开发工具 winshun.devtools | 1.0.5 | 0.45.0 | 共用的窗口、主题和搜索代码变了 |
| 图片文字搜索 winshun.ocr-search | 1.0.30 | 0.45.0 | 同上 |

其余 7 个官方插件（休息提醒、云备份、缩写展开、资源管理器标签页、语义搜索、剪贴板同步、网页搜索）：按构建依赖（`ninja -t inputs`，再顺着 `#include` 找头文件）和 v0.44.0 比，编译进去的文件和包目录都没变，不发新版本。7 个包的权限都没变。

每个包先用插件库的 `check.check_package` 检查真实归档，0 个错误、0 个警告。条目由 `tools/entry.py` 从 Release 附件生成（插件库 9f75e84）。发布流程成功后核对线上 index：41 个插件，签名正确（ws-index-2026a），7 个插件的版本、最低版本、大小和 SHA256 都和本地包一致。

## 发布

`scripts/release.ps1 -Publish -Push Now`（`dist\release045pub`，同一个构建目录）。代码和发布前那次打包相同，这次没再跑测试；`verify-installer.ps1` 对正式包 6 项全过。

- 发布提交 19212864，标签 v0.45.0 指向它，最新版本接口返回 v0.45.0。
- 8 个附件（安装包、便携包、离线安装包、增强 OCR 组件、3 个增量包、SHA256SUMS.txt）：GitHub 上每个附件的大小和 SHA256 都和 `SHA256SUMS.txt` 一致。3 个增量包和发布前那次逐字节相同；正式包里的 `WinShun.exe`（1a1f47cc…）和升级实测装上的是同一份，所以这台电脑现在装的就是正式版。
- 说明末尾有立即推送那行（`<!-- winshun-push now -->`）和增量那行。
- CNB 镜像：脚本上传了 8 个文件，下载回来校验通过；安装包的 CNB 地址 302 到 asset.cnb.cool。
- Scoop：dae2420a（Scoop: winshun 0.45.0），已推送。
- lcstp.com：发布后约 9 分钟两条回答（安装版、便携版）都是 v0.45.0，文件名、大小、SHA256 和 `SHA256SUMS.txt` 一致，第一个镜像是 cnb.cool，正文里有立即推送那行和增量那行。
- 已发布的 19 个 issue（#422、#427、#428、#432、#434、#435、#436、#438、#439、#440、#441、#443、#444、#445、#446、#447、#448、#450、#451）回复后关闭。

## 没有覆盖的

- 录屏在 AMD、Intel 显卡上的编码，以及真机 Windows 10 1809 上的 60 帧没有实测（录屏的测量和 Windows 10 虚拟机结果见 `docs/validation/recording-clarity`）。
- 贴图分组、贴图历史、存回原文件、聚光灯、长截图自动滚动、FZF 模糊匹配、电源命令、移到回收站、`Ctrl++` 界面大小、对话框建议来源、悬停提示分类，以各自提交里的测试和预览检查为准，这次发布没有在真实桌面上逐个重测。
- 鼠标手势的中键、侧键和组合手势以 `tst_gestures` 为准，没有用真实鼠标逐个试。
