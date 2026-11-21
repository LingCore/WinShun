# 0.43.0 验证记录

发布提交 2d406d66（Release 0.43.0），正常推送。0.42.0 之后的 16 个功能和修复提交（#415–#421、#423、#424，文件夹菜单直接列在第一层、贴图直接选字、任务栏搜索框先变窄等），加上发布前工作区里另一个会话留下、未提交的一批开合动画改动：用户确认“提交了一起发”，检查、编译、实测后拆成 355db20f（搜索框和剪贴板淡出、等淡出完再开设置、弹出更快）、1068b37d（主题小动画跟随系统“动画效果”）、e87e564f（WinGet 文档）三个提交。

## 自动测试

只跑了这次改动涉及的 21 组：`tst_aisettings`、`tst_annotate`、`tst_annotmodel`、`tst_capture`、`tst_capturekeys`、`tst_capturesettings`、`tst_core`、`tst_editor`、`tst_foldermenu`、`tst_gestures`、`tst_ocr`、`tst_pinclipboard`、`tst_pinwindow`、`tst_plugin`、`tst_settings`、`tst_settingsfilters`、`tst_stage7`、`tst_taskbarroom`、`tst_taskbarwindow`、`tst_traymenu`、`tst_placement`。

- 第一次打包时 `tst_core` 失败一次（`release.ps1` 不打印失败的用例），随后单独重跑通过；第二次打包和正式发布时 21 组全部通过。`tst_core` 里 `ServiceLocation`（#424）和 `PathText`（#416）的用例都在其中。
- 开合动画那批改动另跑了 `tst_placement`、`tst_displaygeometry`、`tst_previewplacement`，都通过。

## 开合动画（实测）

用这批改动自带的探针，在真实桌面上跑真实的 `App`（隔离配置）：

- `launcher-settings.json`：`--launcher-settings-check`。从搜索框 Logo 打开设置：淡出 28 步、约 123 ms 消失，设置窗口在淡出结束后才出现（第一次约 464 ms、之后约 187 ms），过程中没有失焦（deact=0）、没有闪白（opaqueFlash=0）、两个窗口没有重叠（overlap=0）、悬停提示没有残留；关掉动画时直接消失（约 32 ms）；淡出中途再打开搜索框会取消待开的设置（cancel-mid-fade 通过）。
- `--launcher-open-check`（结果 200 KB，没放进仓库）：8 种打开方式（冷启动、空的、8 行、1 行、清空、内存整理后、切范围、直接开面板）都是第一帧不透明度 0、偏 12 px，约 130–141 ms 到位，GUI 线程没有停顿；面板展开收起通过。

## 安装和升级

- `installer-test.json`：正式发布时 `scripts/release.ps1 -Publish` 对正式包跑的 `tools/verify-installer.ps1`，6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。发布前两次打包中的第二次也是 6 项全过。
- 正式发布（`release.ps1 -Publish -Push Normal`）：8 个附件（安装包、便携包、离线安装包、增强 OCR 组件、3 个增量包、SHA256SUMS.txt），GitHub 上每个附件的大小和 SHA256 与本地一致，标签 v0.43.0 指向 2d406d66，最新版本接口返回 v0.43.0；CNB 镜像 8 个文件下载回来核对通过；Scoop 清单 5d8c3934。发布后 lcstp.com 的两条回答（安装版、便携版）都是 v0.43.0，文件名、大小、SHA256 与 Release 一致，第一个镜像是 cnb.cool，正文有增量那行、没有立即推送那行。
- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`，用发布前那次打包（`dist\release043`）。本机装着的 0.42.0 走软件内更新到 0.43.0，下载的是 `WinShun-0.43.0-x64-delta-from-0.42.0.zip`（7 564 590 字节），用时 51.7 秒；被占用的 `WinShun.exe` 挪开后照常写入；更新后服务在运行、开机任务指向 Program Files、剪贴板历史的 SHA256 不变，新版本以普通权限自己启动。那次打包已经带着海报用的预览分支，正式版 `WinShun.exe`（b4b4facc…）和它逐字节相同，所以本机现在装的就是正式版程序；之后只改了文档。

## 海报

`tools/screenshots/release043.py`，正式版程序和正式 QML：

- `foldermenu-inline.jpg`（新）：文件夹菜单直接列在第一层。`--preview-results --style-windows` 离屏 200% 渲染 `FolderMenuWindow`，条目由 `FolderMenu` 自己的 `favoriteGroupEntries` / `groupEntries` / `assembleGroups` 分组（收藏夹、最近的文件夹选“全部”，收藏的应用展开，已打开的文件夹是二级菜单）；文件夹图标是系统的，应用图标是这台电脑上的微信、Visual Studio Code 和终端。

没有海报：钉住的搜索框在任务栏上（真实任务栏）、贴图里直接选字和 `Shift+C`（截图插件的预览里没有贴图）、截图编辑时 `Esc` 一步一步退、默认系统右键菜单、手动输入文件夹、任务栏搜索框先变窄、对话框旁的搜索框点进去不列常用位置、Directory Opus 的“此电脑”：设置里的一行或真实程序的窗口，由开发这些功能的会话实测或只在测试里验证。

## 插件库

4 个官方插件发了新版本，源码 2d406d66，从 `git archive` 出来的干净副本编译（`build\plugins043`，只编 `library_plugins`）：

| 插件 | 版本 | 最低 Win顺 | 说明 |
|---|---|---|---|
| 截图与录屏 winshun.capture | 1.1.31 | 0.18.0 | 贴图直接选字、`Shift+C` 复制全部文字（#423）、编辑时 `Esc` 一步一步退（#415）、文字识别气泡样式 |
| 鼠标手势 winshun.gestures | 2.1.1 | 0.41.0 | 选中文字快捷键带 `Alt`/`Win`/`Shift` 时不再误触；设置窗口边框（#419） |
| 图片文字搜索 winshun.ocr-search | 1.0.28 | 0.43.0 | 窗口边框（#419），共用代码变了 |
| 开发者工具 winshun.devtools | 1.0.3 | 0.43.0 | JSON 窗口边框（#419），共用代码变了 |

其余 9 个官方插件（剪贴板同步、书签、休息提醒、云备份、缩写展开、资源管理器标签页、LocalSend、语义搜索、翻译）按构建依赖（`ninja -t inputs`）比对各自上次发布的源码提交，编译进去的文件都没有变化，不发新版本。权限都没有变化。每个包都先用插件库的 `check.check_package` 检查真实归档，0 个错误、0 个警告；条目由 `tools/entry.py` 从 Release 附件生成（插件库 46aa0cf），发布流程成功后核对线上 index：41 个插件，签名正确（ws-index-2026a），4 个插件的版本、最低版本、大小和 SHA256 都和本地包一致。

## 没有覆盖的

- 钉住的搜索框在任务栏上（#418）、对话框旁的搜索框（#417）、Directory Opus（#416）由开发它们的会话在暂存副本上实测（见各自提交说明），这次发布没有重测；#419–#421、#423、#424 和贴图选字、文件夹菜单分组、任务栏搜索框变窄的验证以对应测试和提交说明里的检查为准。
- 鼠标手势 2.1.1 的 `Alt`/`Win`/`Shift` 屏蔽键没有在真实浏览器里重测。
- 开合动画只在这台电脑（Windows 11）上实测，没在 Windows 10 虚拟机里测。
