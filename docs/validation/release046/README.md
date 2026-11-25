# 0.46.0 验证记录

发布提交 6cf40a82，正常推送（`-Push Normal`，用户没有要求立即推送）。内容是 0.45.0 之后 main 上的全部提交（#426、#433、#442、#452 的诊断、#454–#460，内容搜索里写文件类型、拼音缩写的电源命令、录屏编码参数、新插件窗口置顶、C++ SDK 的代注册快捷键），加上发版时合并的 `capture-fullscreen-overlay`（71df06d2：全屏商店应用上由索引服务拉起截图表面）和发版中修好的截图 `Enter` 问题（be8dd01a）。构建期间另一个会话提交的窗口置顶 1.1.0（60a9dcb3，只改插件和文档）也在这个版本里。

## 自动测试

只跑了这次改动涉及的测试：

- 合并 `capture-fullscreen-overlay` 后（`build\release046`）：`tst_aisettings`、`tst_recording`、`tst_settings`、`tst_capture`、`tst_capturesurface`、`tst_service`、`tst_plugin`，7 组全部通过。
- 截图焦点修复后：`tst_capture`、`tst_capturekeys`、`tst_capturesettings`、`tst_capturesurface`，4 组全部通过（修复所在的构建和正式发布那次各跑一遍）。
- `tools/verify-installer.ps1`：发布前打包和正式发布两次都是 6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。

## 全屏商店应用上的截图表面（71df06d2）

按用户的决定**未实测**就并入：没有在真实的全屏媒体播放器上跑过服务拉起的 UIAccess 截图表面，也没有测中文输入、服务不可用时的回退。依据只有它自带的 `tst_capturesurface` 和 `tst_service` 的新用例。旧宿主（0.45.0）上截图插件 1.1.36 收到 `Unknown tool` 会按 `old-host` 回退到 1.1.35 的做法，这一点只读过代码，没有在 0.45.0 上运行。发布说明里写明了“未在真实全屏播放器上实测”。

## 截图保存夹具与 `Enter`（d1d0f80b、be8dd01a）

发版前按上次的教训跑截图插件自带的保存夹具（`WinShunCapture.exe --preview-capture-tools <dir> --save-options`），它通过真实遮罩点保存按钮、另存为、覆盖确认、保存菜单。保存相关全过，但后面 5 项失败。查明是夹具自 588d46b6（2026-11-03）起就过时：`Esc` 关菜单后焦点留在下拉箭头上，夹具接着按的 `Down` 又打开了菜单，之后连锁失败；在 588d46b6 上编出来同样失败，不是这次的回归。改夹具（d1d0f80b）。

顺带发现一个真实的老问题：遮罩窗口是复用的，上一次截图点过“保存”或下拉箭头，下一次截图按 `Enter` 会打开另存为或保存菜单，而不是复制。修复（be8dd01a）：每次截图开始时把键盘焦点交回遮罩的按键项。夹具里加了两条固定检查（用过保存菜单后、点过保存后，下一次 `Enter` 都是复制且不弹对话框）和一条 `Tab` 仍能走到工具栏的检查。未修复时这两条失败，修复后 31/31 通过（`build\release046` 里的发布构建再跑一遍也是 31/31）。工具栏的“贴图”“识别文字”按钮没有用真实鼠标点，以 `tst_capture` 的 `qmlEnumKeysAreUnique` 为准。

## 安装和升级

- `tools/verify-installed-upgrade.ps1 -Mode InApp -Feed dist\release046 -Version 0.46.0`：本机装的 0.45.0 走软件内更新到 0.46.0，下载的是 `WinShun-0.46.0-x64-delta-from-0.45.0.zip`（8 457 416 字节），用时 50.7 秒。更新后服务在运行，开机任务、设置和历史保留（历史 SHA256 不变）。结果见 `inapp-upgrade.json`。
- 这次测的是发布前那次打包（e2c01ac6 起的代码）。之后又加了截图焦点修复（只在截图插件里）和海报用的预览探针（`WinShun.exe` 里只有 `--preview-results --name-filter-poster` 这一条预览路径不同），所以本机装着的 `WinShun.exe`（EA08EF7E…）和正式包里的（C59F96A9…）不是同一份；程序的行为相同，版本号都是 0.46.0，本机不会再收到这个版本的更新提示。

## 海报

`tools/screenshots/release046.py`，用 0.46.0 的 `WinShun.exe --preview-results <dir> --name-filter-poster --scale 2`（离屏，f740be83 加的预览：一组像样的办公文件，搜“报告”，开头是 `2026`、结尾是 `.docx`、不包含 `草稿 旧`）。

- `name-filter.jpg`（新）：搜索面板的“名称”筛选，8 行 2026 年的 Word 报告。放在 `docs/截图.md` 的“搜文件”一组，快捷筛选旁边。

没有新海报的：全屏播放器上截图、截图快捷键、贴图顶边拖动、内容搜索的文件类型、文件夹窗口里 `Tab` 补全、插件卡片进度、窗口置顶、Logo 开关、不感兴趣、内存。

## 插件库

按各插件已发布版本的源码提交和这次比对编译进去的文件（`ninja -t inputs` 加 `#include` 闭包）：

| 插件 | 版本 | 最低 Win顺 | 说明 |
|---|---|---|---|
| 截图与录屏 winshun.capture | 1.1.36 | 0.18.0 | 全屏商店应用上的截图表面、截图快捷键（#455）、贴图顶边拖动（#456）、录屏编码参数、`Enter` 修复、内存预留上限；源码 f9f4d2ae，安装包附带的是同一份 |
| 窗口置顶 winshun.topmost | 1.0.0 → 1.1.0 | 0.45.0 | 新插件（4ab4898b），发布中又出了 1.1.0：置顶窗口描边（6cf40a82） |
| 按意思找 winshun.semantic | 1.0.6 | 0.17.0 | 卡片上的进度、暂停（#458）；卡片进度要 0.46.0 |
| 图片文字搜索 winshun.ocr-search | 1.0.31 | 0.46.0 | 共用的内存预留代码（#459） |
| 开发工具 winshun.devtools | 1.0.6 | 0.46.0 | 同上 |
| 鼠标手势 winshun.gestures | 2.1.4 | 0.41.0 | 同上，加新 SDK |
| 剪贴板同步、休息提醒、缩写展开、局域网传输、资源管理器标签页、云备份、浏览器书签、翻译 | 1.0.1、1.1.3、1.0.1、1.0.10、1.1.6、1.0.2、2.2.2、2.2.6 | 不变（资源管理器标签页由 CMake 写成 0.46.0） | 只因 SDK 头文件（代注册快捷键）重新构建，用起来没有变化 |

网页搜索没有变化，不发。条目由 `tools/entry.py` 从 Release 附件生成，`check.py --online` 0 个错误（4 条警告是社区插件之间的关键字冲突，早就有）。插件库提交 a803d87、cbec131、3f38346；`tools/registry.py` 登记了 `winshun.topmost`。发布后下载线上 index 核对，各插件的版本、最低版本和大小与本地一致。

## 发布

`scripts/release.ps1 -Publish -Push Normal`（`dist\release046pub2`，构建目录 `build\release046`）。第一次（`dist\release046pub`）在构建中途因为另一个会话往 main 提交了 60a9dcb3 被脚本拦下（“The release commit changed during the build”），没有建 Release；通知其他会话后重新跑。

- 标签 v0.46.0 指向 6cf40a82，最新版本接口返回 v0.46.0。
- 8 个附件（安装包、便携包、离线安装包、增强 OCR 组件、3 个增量包、SHA256SUMS.txt）：GitHub 上每个附件的大小和 SHA256 都和 `SHA256SUMS.txt` 一致。
- CNB 镜像：8 个文件，脚本下载回来校验通过。
- Scoop：2074564c（Scoop: winshun 0.46.0），已推送。
- lcstp.com：两条回答（安装版、便携版）都是 v0.46.0，文件名、大小、SHA256 和 `SHA256SUMS.txt` 一致，第一个镜像是 cnb.cool，正文有增量那行、没有立即推送那行。
- 回复并关闭 #426、#433、#442、#454、#455、#456、#457、#458、#459、#460。#452 只加了诊断记录，留着（另一个会话已在下一版修）。

## 没有覆盖的

- 全屏商店应用上的截图表面整条路径（见上）。
- 录屏编码参数在 AMD、Intel 显卡上的效果（这台是 NVIDIA；测量见 `docs/validation/recording-encoder-params`）。
- 名称筛选、内容搜索的文件类型、文件夹窗口 `Tab` 补全、拼音电源命令、插件卡片进度、Logo 开关、不感兴趣、贴图顶边拖动、内存预留，以各自提交里的测试和预览检查为准，这次发布没有在真实桌面上逐个重测。
- 窗口置顶 1.1.0 的描边只在 Windows 11、150% 上测过（`docs/validation/topmost-plugin`）。
