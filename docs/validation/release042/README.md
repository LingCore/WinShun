# 0.42.0 验证记录

发布提交 fed24d4f（Release 0.42.0），正常推送。合并了两个分支：`selection-hotkey`（鼠标手势 2.1.0，选中文字后按快捷键）和 `capture-stall-diag`（#325 截图卡住时的记录）。

## 自动测试

只跑了这次改动涉及的 16 组（`tst_appearance` 四种缩放算 4 个，共 19 个）：`tst_appearance_*`、`tst_capture`、`tst_clipimage`、`tst_clipboardpictureview`、`tst_displaypulse`、`tst_displaysetup`、`tst_everything`、`tst_foldermenu`、`tst_gestures`、`tst_graphicsloss`、`tst_querysyntax`、`tst_recording`、`tst_settings`、`tst_shunsync`、`tst_plugin`、`tst_aisettings`。发布前打包和正式发布时各跑了一遍，全部通过。

- `tst_graphicsloss` 要真实窗口（D3D11）：调用它的终端里设了 `QT_QPA_PLATFORM=offscreen` 时会失败，这次在 `tests/CMakeLists.txt` 里把它固定为 `QT_QPA_PLATFORM=windows`（b040d05e）。
- `tst_shelllaunch`（OneCommander 的几项在里面）发布前跑了一次：OneCommander 相关的 `oneCommanderLookalike`、`oneCommanderSessionTabs`、`oneCommanderTypingFocus` 通过，要真实 OneCommander 窗口的几项照常跳过。同一文件里的 `appsFolderConsole`（通过 `shell:AppsFolder` 启动 PowerShell）两次都失败：这台电脑上 Explorer 经 `shell:AppsFolder` 启动程序就要 14.7 秒，超过测试等的 10 秒。这部分启动代码（`Shell.cpp`、`CommandLaunch.cpp`）自 0.41.0 没有改动，所以正式发布时没把 `tst_shelllaunch` 放进去。

## 安装和升级

- `installer-test.json`：正式发布时 `scripts/release.ps1 -Publish` 对正式包跑的 `tools/verify-installer.ps1`，6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。发布前那次打包也是 6 项全过。
- 正式发布（`release.ps1 -Publish -Push Normal`）：8 个附件（安装包、便携包、离线安装包、增强 OCR 组件、3 个增量包、SHA256SUMS.txt），GitHub 上每个附件的大小和 SHA256 与本地一致，标签 v0.42.0 指向 fed24d4f；CNB 镜像 8 个文件下载回来核对通过；Scoop 清单 0ad70426。正式版 `WinShun.exe`（f6c0cb78…）和拍海报用的程序逐字节相同。
- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`。本机装着的 0.41.0 走软件内更新到 0.42.0，下载的是 `WinShun-0.42.0-x64-delta-from-0.41.0.zip`（7 315 984 字节），用时 76.9 秒；更新后服务在运行、开机任务指向 Program Files、剪贴板历史的 SHA256 不变，新版本以普通权限自己启动。用的是发布前那次打包（`dist\release042`）。之后只改了 `Preview.cpp` 里出海报用的预览分支（`--clipboard-picture-view --gallery-image`）和文档，更新链路的代码没动。

- `setup-upgrade.json`：发布后（用户放行）用正式的 `WinShun-0.42.0-x64-setup.exe`（SHA256 与 Release 一致）跑 `tools/verify-installed-upgrade.ps1 -Mode Setup`，覆盖 InApp 实测装上的那份：退出码 0，用时 11.2 秒，装完自己重新运行；装好的 `WinShun.exe` 是正式版（f6c0cb78…），服务在运行、开机任务指向 Program Files、剪贴板历史的 SHA256 不变。

## 海报

`tools/screenshots/release042.py`，正式版程序和正式 QML：

- `sync.jpg`（新）：隔离的运行探针（私有配置和注册表项，不碰正在用的 Win顺）装上这次的剪贴板同步插件包，另一台设备是 `sync_peer.exe`（只有 ShunSync 引擎）。两边事先放进同一个组，对方复制两个文件、一个文件夹和一段文字后，拍 `同步` 列表。设备行显示的是这台电脑由路由器分到的局域网地址（默认路由走的是代理软件的虚拟网卡 198.18.0.1，不用它）。探针结束后注册表净变化 0。
- `clipboard-picture.jpg`（新，#414）：剪贴板窗口在 200% 下离屏渲染，用临时历史，选中图片那一行，大图在旁边打开。两个窗口的画面在 `Preview.cpp` 里并排拼好，圆角在 `release042.py` 里各自加上。
- `appearance.jpg`、`admin-paste.jpg`（重拍）：外观页加了“搜索框阴影”（#412），剪贴板快捷键页加了“粘贴为纯文本的快捷键”（#408）。

## 插件库

5 个官方插件发了新版本，源码 fed24d4f，从 `git archive` 出来的干净副本编译（`build\plugins042`）：

| 插件 | 版本 | 最低 Win顺 | 说明 |
|---|---|---|---|
| 剪贴板同步 winshun.sync | 1.0.0（新） | 0.42.0 | 新插件；`WinShunSync.exe` 登记为官方原生文件（插件库 `tools/registry.py`） |
| 截图与录屏 winshun.capture | 1.1.30 | 0.18.0 | 跨屏截图 #409、1809 上录屏、#325 卡住时的记录 |
| 鼠标手势 winshun.gestures | 2.1.0 | 0.41.0 | 选中文字后按快捷键；浏览器里右键加滚轮切标签页 |
| 图片文字搜索 winshun.ocr-search | 1.0.27 | 0.42.0 | 共用代码变了，用起来没有变化 |
| 开发者工具 winshun.devtools | 1.0.2 | 0.42.0 | 共用代码变了，用起来没有变化 |

权限都没有变化（剪贴板同步是新插件）。每个包都先用插件库的 `check.check_package` 检查真实归档，0 个错误、0 个警告；条目由 `tools/entry.py` 从 Release 附件生成（插件库 52cb1df），发布流程成功后核对线上 index：41 个插件，签名正确（ws-index-2026a），5 个插件的版本和 SHA256 都和本地包一致。

## 没有覆盖的


- 剪贴板同步和 Mac顺 的真机互通：协议按同一份测试向量核对过（`tst_shunsync`），海报那次是两台 Windows 引擎在同一台电脑上互连。
- 跨屏截图在用户的两块显示器上实测过（#409 那一轮），这次发布没有重测。
- 鼠标手势选中文字快捷键：在测试窗口和 Edge InPrivate 里由开发它的会话实测（读到文字、剪贴板原样放回、Win+V 条数不变），这次发布没有在 Win10 虚拟机里重测。
- 剪贴板看大图只在离屏的正式 QML 里验证（为了不动真实剪贴板），没在真实桌面上点过。
