# 0.44.0 验证记录

发布提交 f9ddc9a6（Release 0.44.0），正常推送。0.43.0 之后的 5 个功能提交（记录截图单独设置 30de9dbc、排除目录和已跳过文件按需搜 431f9c9a、列表 1–9 编号跟着滚动 b72153e4 / d1be350d、以管理员身份在终端中打开 #425 ab6a222d），加上发布前工作区里另一个会话留下、未提交的一大批改动：按用户“又改了很多，请再次发布新版本”和上次“提交了一起发”的做法，编译、测试、实测后拆成 9838b090（键盘钩子在自己的线程上收尾）、d41b52db（工具窗口的应用图标、窗口重建后保持样式、剪贴板预览窗口的边框，鼠标手势卡片在 Windows 10 上的边）、16bc8a67（列表编号和 #425 的预览检查）、eb484ef4（设置窗口后退/前进、滚到底切页、设置之间的跳转、托盘图标点击动作、打开设置的快捷键）四个提交。

## 自动测试

只跑了这次改动涉及的测试：`tst_aisettings`、`tst_appearance`、`tst_backupservice`、`tst_capturesettings`、`tst_clipimage`、`tst_commandicons`、`tst_commandlaunch`、`tst_commandline`、`tst_darkframe`、`tst_dialogbarvisibility`、`tst_displaygeometry`、`tst_foldermenu`、`tst_gestures`、`tst_keyrouter`、`tst_settings`、`tst_settingsnav`、`tst_taskbarwindow`、`tst_traymenu`、`tst_windowvisibility`，打包时 19 组全部通过；`tst_core` 见下。

- `tst_core`（排除目录按需搜的单元测试在里面）：第一次打包时通过；第二次打包时在 ctest 里失败一次（这个测试在 ctest 里不打印输出，看不到是哪个用例），随后直接运行同一个程序 287 个用例全部通过。0.43.0 发布时也出现过一次同样的偶发失败。第三次打包没有再带它，正式发布时再跑。
- `tst_shelllaunch`：`appsFolderConsole` 两个用例失败，其余 25 个通过、5 个按条件跳过。这个用例借“所有应用”（`shell:AppsFolder`）里的“Windows PowerShell”启动 PowerShell，而这台电脑的“所有应用”里只有 Windows PowerShell ISE，没有这一项；手动用同样的方式启动，60 秒也起不来。这条启动路径和测试本身自 0.43.0 起都没改过（#425 只改了“在终端中打开”），属于本机环境不满足前提，不是这次的回归，打包时去掉了它。
- 这批改动自带的 `tst_settingsnav`（后退/前进的历史、侧键和 `WM_APPCOMMAND`、滚到底切页）、`tst_keyrouter` 新增的两个收尾用例、`tst_taskbarwindow` 新增的应用图标用例、`tst_clipimage` 新增的预览窗口样式用例都在通过的测试里。

## 设置窗口（实测）

用这批改动自带的检查，真实的 `App`、隔离配置，不碰正在用的 Win顺：

- `settings-nav-check.txt`：`--probe-runtime <目录> --settings-motion-check --nav-check --renderer d3d11`，25 项全部通过：空历史、走过 4 处后能后退不能前进、标题栏上的箭头算按钮旁边的空白算标题栏、客户区和标题栏上的鼠标侧键、点后退回到常规且滚动位置还是 260、`WM_APPCOMMAND` 前进、右键前进按钮列出 2 处并能跳过去、搜索算一步（后退先回搜索结果再回外观）、在页面底部快速甩不切页、停一下再滚一格出提示、第二格切到下一页的顶部、切页后紧跟的一格被吃掉、在顶部往上滚回到上一页的底部、标签之间和页面之间都能切、拾穗计划页的提示、重新打开时历史清空、无障碍树仍然紧凑。
- `--preview-settings`：所有设置页的预览和检查 0 个警告，其中新加的“跨页跳转和按键一览”检查逐个点了 9 个跳转链接，都落到预期的页面和标签；关掉文件夹菜单时，收藏夹页的链接指向文件夹菜单的总开关；按键一览列出 `Win+V`、`Win+S` 和文件夹菜单的快捷键。
- `--settings-motion-check`（不带 `--nav-check`）：换分类和打开设置的动作检查通过（passed=true），GUI 线程最长停顿 8.3 ms。

## 安装和升级

- 发布前打包（`dist\release044`）：第一次因 `tst_shelllaunch`、第二次因 `tst_core` 停下（见上），第三次通过；它跑的 `tools/verify-installer.ps1` 6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。
- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`，用 `dist\release044`。本机装着的 0.43.0 走软件内更新到 0.44.0，下载的是 `WinShun-0.44.0-x64-delta-from-0.43.0.zip`（7 892 874 字节），用时 47.6 秒；更新后服务在运行、开机任务指向 Program Files、剪贴板历史的 SHA256 不变，新版本以普通权限自己启动。
- 正式发布（`release.ps1 -Publish -Push Normal`，`dist\release044pub`）：20 组测试（含 `tst_core`）全部通过，`verify-installer.ps1` 对正式包 6 项全过。8 个附件（安装包、便携包、离线安装包、增强 OCR 组件、3 个增量包、SHA256SUMS.txt），GitHub 上每个附件的大小和 SHA256 与 SHA256SUMS 一致，标签 v0.44.0 指向 f9ddc9a6，最新版本接口返回 v0.44.0；Scoop 清单 69677561。CNB 镜像 8 个文件下载回来，`sha256sum -c` 全部通过，SHA256SUMS 和 GitHub 上的逐字节相同。发布后 lcstp.com 的两条回答（安装版、便携版）都是 v0.44.0，文件名、大小、SHA256 与 Release 一致，第一个镜像是 cnb.cool，正文有增量那行、没有立即推送那行。3 个增量包和发布前那次逐字节相同，本机现在装的 `WinShun.exe`（e3054638…）和正式版相同。

## 海报

`tools/screenshots/release044.py`，0.44.0 程序和正式 QML：

- `settings-back.jpg`（新）：设置窗口能后退、前进。真实的设置窗口（`--nav-check` 走完一圈后右键前进按钮、打开历史列表时抓的窗口，200%），列表里是“快捷键 › 按键一览”和“外观”，下面正好是新的“打开设置”快捷键。

没有海报：滚到底切页和设置之间的跳转（同一个窗口，动起来才看得出）、托盘图标的点击和打开设置的快捷键、记录截图（设置里的一行）、排除目录和已跳过文件的提示、以管理员身份在终端中打开（菜单里的一行）、编号跟着滚动、钉住的搜索框的任务栏图标和剪贴板预览窗口的边框。

## 插件库

4 个官方插件发了新版本，源码 f9ddc9a6，从 `git archive` 出来的干净副本编译（`build\plugins044`，只编 `library_plugins`）：

| 插件 | 版本 | 最低 Win顺 | 说明 |
|---|---|---|---|
| 截图与录屏 winshun.capture | 1.1.32 | 0.18.0 | 共用的窗口代码变了（WindowEffects、WindowFrame、TitleBar、DropMenu、列表滚动条） |
| 鼠标手势 winshun.gestures | 2.1.2 | 0.41.0 | Windows 10 上卡片和气泡画 1 像素的边；共用的窗口代码和搜索核心变了 |
| 开发者工具 winshun.devtools | 1.0.4 | 0.44.0 | 共用的窗口代码和搜索核心变了 |
| 图片文字搜索 winshun.ocr-search | 1.0.29 | 0.44.0 | 共用的窗口代码和搜索核心变了 |

其余 9 个官方插件（剪贴板同步、书签、休息提醒、云备份、缩写展开、资源管理器标签页、LocalSend、语义搜索、翻译）按构建依赖（`ninja -t inputs`）比对各自上次发布的源码提交，编译进去的文件都没有变化；按头文件名比对时出现的 `Engine.h`、`Protocol.h` 是别的插件目录里的同名文件，各自目录里的没变。不发新版本。权限都没有变化。每个包都先用插件库的 `check.check_package` 检查真实归档，0 个错误、0 个警告；条目由 `tools/entry.py` 从 Release 附件生成（插件库 495df80），发布流程成功后核对线上 index：41 个插件，签名正确（ws-index-2026a），4 个插件的版本、最低版本、大小和 SHA256 都和本地包一致。

## 没有覆盖的

- 托盘图标的左键、中键动作和打开设置的快捷键只在 `tst_settings`、`tst_aisettings` 和设置页预览里验证，没有在真实托盘上逐个点过。
- 排除目录按需搜、列表编号跟着滚动、#425 以管理员身份打开终端，以对应提交里的测试和预览检查为准，这次发布没有在真实桌面上重测；以管理员身份打开会弹 UAC，没有实际确认。
- 工具窗口的应用图标、剪贴板预览窗口的边框以 `tst_taskbarwindow`、`tst_clipimage` 为准；鼠标手势卡片在 Windows 10 上的边没有在 Windows 10 虚拟机里看过。
- 设置窗口的检查只在这台电脑（Windows 11，100%）上跑过。
