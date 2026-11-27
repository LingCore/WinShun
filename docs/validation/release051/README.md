# 0.51.0 验证记录

正常推送（`-Push Normal`）。GitHub 网页登录仍不可用，全程用本机 `gh` 保存的令牌（`docs/releasing.md` 的“GitHub 网页登录不了时”）。

版本内容（0.50.0 之后的提交，都在 main 上，没有别的分支）：

- #485 #484 #483 #482（验证见 `docs/validation/issues-482-485`）。
- #490 #493 #492 #489 #498 #497 #487（验证见 `docs/validation/issues-487-498`）；#495 测试连接、#499 打码点选、#430 文件夹里开终端、#476 内容搜索的文件数。
- 群聊反馈：公式窗口关闭图标、插件筛选框的 × 和计数、模糊匹配下的程序排序、便携文件管理器记住位置、剪贴板打开时钉住（验证见 `docs/validation/chat-1127`）。

## 自动测试

`build\release` 全量构建通过。只跑了这次改动涉及的 18 项，17 项通过：

`tst_mojibake`、`tst_encoding`、`tst_contentpane`、`tst_fuzzymatch`、`tst_previewinteractions`、`tst_previewinteractions_scaled`、`tst_launchernavigation`、`tst_settings`、`tst_annotmodel`、`tst_clipimage`、`tst_translate`、`tst_cloudbackup`、`tst_core`、`tst_commandlaunch`、`tst_semantic`、`tst_updater`、`tst_scoopinstall`。

`tst_shelllaunch` 没过，三处失败都和这次的改动无关（编进它的源码里这一版只改了 `FileManagers.cpp`），但其中一处在跑测试的人桌面上弹窗，已改测试（417348a8）：

- `appsFolderConsole`：通过开始菜单条目 `shell:AppsFolder\{1AC14E77…}\WindowsPowerShell\v1.0\powershell.exe` 启动 PowerShell。这台电脑的开始菜单里没有“Windows PowerShell”（只有 ISE、PowerShell 7），Windows 每跑一次就在桌面弹“找不到文件”。现在先用 `SHCreateItemFromParsingName` 查条目在不在（不启动），不在就跳过。
- `realDialogFocusAfterGoingToAFolder(the user goes into the list meanwhile)`：对话框刚显示出地址就去找文件列表，那时列表还没建好，报“找不到文件列表”。改成最多等 5 秒（对话框的窗口树核对过：`DirectUIHWND` 在 `SHELLDLL_DefView` 下）。等到之后它走到了真正的检查，**仍然失败**：焦点被放回文件名框。`FileDialog.cpp` 自 0.49.0（9d9b41b5）没有改过，要另外查。

改过测试后 `tst_shelllaunch` 全部：28 通过、1 失败（上面这条）、8 跳过。重新打包时没有再跑测试（代码与跑过测试时相同，只多了测试文件的改动）。

`tools/verify-installer.ps1`：6 项全过（打包时自动运行）。

## 安装和升级

- `tools/verify-installed-upgrade.ps1 -Mode InApp`：本机装的 0.50.0 通过软件内更新升到 0.51.0，用时 50.5 秒，用的是增量包 `WinShun-0.51.0-x64-delta-from-0.50.0.zip`（7.7 MB）。更新后服务在运行，没有提升权限，剪贴板历史的校验值不变，装好的 `WinShun.exe`（48791894…）和打包目录里的相同。结果见 [inapp-upgrade.json](inapp-upgrade.json)。
- `-Mode Portable`（#484 改了便携版更新文件的位置；用户同意后跑）：0.50.0 便携包自己更新到 0.51.0，用时 33.3 秒，同样走增量包；`data` 里的文件和标记文件都保留，更新后 `data\Updates` 在、下载的包已清掉；`%LOCALAPPDATA%\WinShun\Updates` 里没有这次留下的东西（那里只有装着的那份的更新日志，所以没删）。测试期间退出的已装 Win顺 之后恢复运行。结果见 [portable-upgrade.json](portable-upgrade.json)。

增量包三个（从 0.50.0、0.49.0、0.48.0），脚本各自还原比对过，7.7–17.1 MB；安装包 42.6 MB。

## 海报

`tools/screenshots/release051.py`：重拍 `preview-colors.jpg`（#482 改了预览的样子），标题不变“预览文档，语法上色”，副标题改成“……搜索的词标黄加粗”。用的是 `WinShun.exe --preview-results … --gallery-image …`（和 0.26.0 那张同一次运行），搜的词底色正好是 #FFE08A，只有第 20 行有行带，探针自己的底色和颜色检查都过了。

这次运行**不是完全离屏**：放预览位置那一项会把启动器和预览窗口放到屏幕坐标上，剪贴板尺寸那一项会把窗口置顶、移动真鼠标再放回去，所以 17:31–17:33 桌面上可能出现过这几个窗口（没有激活）。三项依赖屏幕宽度的检查在 2560 宽的屏幕上不过（与图无关），脚本按名字放过。以后重拍要先改出只拍颜色的离屏入口。

另外看到：这次运行里 Markdown 的一级标题是常规字重，而 `tst_contentpane` 的离屏图（同一构建）是粗体；0.50.0 时的图也已经是常规字重，不是 #482 引起的，还没查原因。

## 插件库

比对了各插件自 0.50.0 以来编译进去的文件（各插件 CMakeLists 列的源码、链接的 `wscore` 中实际用到的部分，对照 `git diff v0.50.0`）：

| 插件 | 版本 | 说明 |
|---|---|---|
| 截图与录屏 winshun.capture | 1.1.41 | #485、#489、#499、公式窗口关闭图标、#490；安装包附带的是同一份源码 |
| 翻译 winshun.translate | 2.4.0 | #495 测试连接 |
| 按意思找 winshun.semantic | 1.2.0 | #495 测试连接 |
| 云备份 winshun.cloudbackup | 1.1.0 | #495 测试连接 |
| 鼠标手势 winshun.gestures | 2.2.1 | 设置窗口编进了 `QmlRuntime.h`（#490）和 `InputBox.qml` |
| 开发工具 winshun.devtools | 1.0.11 | `QmlRuntime.h`（#490） |
| 图片文字搜索 winshun.ocr-search | 1.0.36 | `QmlRuntime.h`（#490） |

其余插件编译进去的文件没有变化（书签、置顶链接 `wscore`，但不用这一版改过的 `AppCatalog`、`CommandLaunch`），不发。权限都和已发布的版本相同。

## 没有覆盖的

- `tst_shelllaunch::realDialogFocusAfterGoingToAFolder` 的“用户点进文件列表”一行在本机稳定失败（见上），不是这一版引入的。
- #430 没有在真实资源管理器窗口里试；#499 没有在真实截图里点；#495 的按钮没有在真实设置窗口里点（插件进程用假服务器测过）；剪贴板“打开时钉住”没有真的连续粘贴；便携 Total Commander 记住位置只测了写入的一半（本机装着 Total Commander，读回的一半跳过）。
- #490 的崩溃这边没能复现，改了垃圾回收方式，不能证明已经修好。
- #497 反馈者那边的原因没有确认。

## 发布后核对

- `-Publish` 从 8be801a5 重新构建打包：三个增量包与实测时逐字节相同；安装包、便携包重新生成（SHA256 变了），里面的 `WinShun.exe`（48791894…）与实测装上的相同。之后用公开的安装包（240f7b01…）跑 `-Mode Setup`：9.1 秒，0.51.0，未提权，服务在运行，剪贴板历史不变。
- GitHub Release 8 个附件，远端 SHA256 与 `SHA256SUMS.txt` 一致，标签指向 8be801a5。lcstp.com 两个接口都是 v0.51.0，大小和 digest 一致，第一个镜像是 cnb.cool，说明里有增量行、没有立即推送行（正常推送）。CNB 上的 `SHA256SUMS.txt` 下载回来与 GitHub 一致。Scoop 27c1a195。
- 插件库 92cb329，线上 index 44 个插件，7 个新版本都在，签名有效（ws-index-2026a）。
- 关闭 #476 #482 #483 #484 #485 #489 #490 #492 #493 #495 #499；#430（Command 模式没做）、#497（等反馈者用 Directory Opus 再试）、#487、#498 留着。
