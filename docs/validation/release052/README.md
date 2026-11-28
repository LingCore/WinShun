# 0.52.0 验证记录

正常推送（`-Push Normal`）。GitHub 网页登录仍不可用，全程用本机 `gh` 保存的令牌。

版本内容（0.51.0 之后）：

- main 上的提交：#430 终端程序文件那一行也开在当前文件夹、#475 设置搜索关键词、#500 内存（空闲让出、设置窗口关闭后归还、设置页面少三分之一对象；验证见 `docs/validation/issues-430-500`）、群聊反馈的“内容”行尾标签与对话框搜索框当前文件夹优先、#501 光标、#504 索引服务的修复命令、#503 合盖保持唤醒（系统开关 1.2.0）。
- 合并的分支 `fuzzy-favorite`（2 个提交）：始终模糊匹配时只是猜中的收藏不提前、猜中的结果排在名字对得上的结果之后（验证见 `docs/validation/chat-1127`）。合并时 `SearchEngine.cpp` 有一处冲突：两边各加了一个辅助函数（`wordsAsAnywhere`、`learnedFor`），两个都保留。

## 自动测试

合并后 `build\release` 全量构建通过。只跑了这次改动涉及的 21 项，全部通过：

`tst_mojibake`、`tst_encoding`、`tst_hotpages`、`tst_idletrim`、`tst_contentkinds`、`tst_settingsreopen`、`tst_settingsmemory`、`tst_settingslazy`、`tst_fuzzymatch`、`tst_searchcaret`、`tst_launcherfilters`、`tst_aisettings`、`tst_settings`、`tst_switches`、`tst_switchplugin`、`tst_core`、`tst_favorites`、`tst_commandlaunch`、`tst_updater`、`tst_scoopinstall`、`tst_dialogbarsuggestions`。

分支上记录的 `tst_core::relocatedPersonalFoldersKeepPriority` 失败，这次在合并后的 main 上通过。

`tools/verify-installer.ps1`：6 项全过（打包时自动运行）。

## 安装和升级

- `tools/verify-installed-upgrade.ps1 -Mode InApp`：本机装的 0.51.0 通过软件内更新升到 0.52.0，用时 51.7 秒，用的是增量包 `WinShun-0.52.0-x64-delta-from-0.51.0.zip`（8.3 MB）。更新后服务在运行，没有提升权限，自启任务不变，剪贴板历史的校验值不变，装好的 `WinShun.exe`（bb4b3a3c…）和打包目录里的相同。结果见 [inapp-upgrade.json](inapp-upgrade.json)。
- 增量包三个（从 0.51.0、0.50.0、0.49.0），脚本各自还原比对过，8.3–9.7 MB；安装包 42.7 MB。
- 安装程序、更新代码这一版没有改，没有跑便携版、向导和“只为我安装”的升级。

## 海报

没有重拍。这一版能看出变化的只有对话框旁的搜索框（先列对话框所在文件夹里的）和系统开关卡片上的一个开关。现有 `dialog.jpg` 里列出的结果都在对话框所在的文件夹里，和新的排序一致；重拍它要用全桌面实拍（`agent.ps1`，会接管桌面、换主题和壁纸），不值得为了同样的画面打扰正在用电脑的人。

## 插件库

用 `build\release` 里 ninja 记下的真实输入（各插件目标的源码、QML 和编译时读过的头文件）对照各插件上次发布的源码提交，有变化的 6 个升版重新发布：

| 插件 | 版本 | 变了什么 |
|---|---|---|
| 截图与录屏 winshun.capture | 1.1.42 | `CaptureApp.cpp`、`IdleTrim.cpp`（内存紧张时也让出，留常用部分，#500）、设置界面零件（Glyph、FlatButton、ToggleSwitch、SettingRow 等）、`Caret.qml`（#501）；安装包附带的是同一份源码 |
| 鼠标手势 winshun.gestures | 2.2.2 | 同上的界面零件、`IdleTrim.cpp`、`Caret.qml` |
| 图片文字搜索 winshun.ocr-search | 1.0.37 | 同上 |
| 开发工具 winshun.devtools | 1.0.12 | 界面零件、`IdleTrim.cpp`（没有用到带样式光标的输入框，说明里不写 #501） |
| 按意思找 winshun.semantic | 1.2.1 | `plugins-common/IdleTrim.h`：内存紧张时只留 32 MB 以内，其余让出 |
| 压缩包搜索 winshun.archive | 1.0.2 | 同上 |

这几个还编进了 `SearchEngine.cpp`、`ServiceLocation.cpp` 等共用文件，但它们用不到改过的那部分。其余 11 个插件编译进去的文件没有变化，不发。权限都和已发布的版本相同。

## 没有覆盖的

- #503 合盖：只用假的电源计划和电源计划的副本测过写入和还原，没有在笔记本上真的合上盖子。
- 对话框搜索框当前文件夹优先：有单元测试（`tst_dialogbarsuggestions`、`tst_core`），没有在真实的“打开”对话框里打字。
- #504：`tst_core` 核对了生成的修复命令，并在临时文件夹上用 `icacls` 设出类似的权限做了检查；没有在权限出问题的电脑上真的装服务。
- #501：`tst_searchcaret` 在 125% 下用真实启动器核对了四种外观，没有在反馈者的电脑上确认。

## 发布后核对

- `-Publish` 从 d56f8f32 重新构建打包：三个增量包与实测时逐字节相同；安装包、便携包重新生成（SHA256 变了），里面的 `WinShun.exe`（bb4b3a3c…）与实测装上的相同。之后用公开的安装包（303c2328…）跑 `-Mode Setup`：8.9 秒，0.52.0，未提权，服务在运行，剪贴板历史不变。
- GitHub Release 8 个附件，远端 SHA256 与 `SHA256SUMS.txt` 一致，标签指向 d56f8f32，是最新正式版。lcstp.com 两个接口发布约 9 分钟后都是 v0.52.0，大小和 digest 一致，第一个镜像是 cnb.cool，说明里有增量行、没有立即推送行（正常推送）。CNB 上的 `SHA256SUMS.txt` 下载回来与 GitHub 一致。Scoop e32b5076。
- 插件库 75ac820，线上 index 44 个插件，6 个新版本都在，签名有效（ws-index-2026a）。
- 关闭并回复 #430（通用 Command 模式没有做，已说明）、#475、#500、#501、#503、#504。
- 回收：分支 `fuzzy-favorite` 和它的工作树；`build/`、`dist/` 下 33 个旧的临时目录（删前确认没有目录链接、没有程序从里面运行），保留 `build\release`、`build\recovery` 和这一版的 `dist\release052`、`dist\release052pub`。
