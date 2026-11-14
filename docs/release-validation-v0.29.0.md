# Win顺 0.29.0 发布验证

2026-11-14，在 Windows 11 x64、Qt 6.12.0、MSVC 2022 Release 环境完成构建和相关验证。构建源码为 `b8284349`（版本号、发布说明与文档），构建目录 `build/release029`，交付文件在 `dist/release029`。此后的提交只更新海报、截图页和本记录。

本版接在 0.28.0（`b2afb537`）后面：四个贡献者的 PR（#318、#323、#317、#312 的“运行”部分）和四个 issue（#325、#128、#328、#61）。各自的验证记录在 [PR 那一轮](validation/prs-318-323-317-312-20261114/README.md)、[#325 和 #128](validation/issues-325-128-20261114/README.md)、[#328 和 #61](validation/issues-328-61-20261114/README.md)。发布时本机主工作目录里另有未提交的改动（字体选择框、截图插件的若干文件），不属于本版，没有纳入。

## 构建与相关回归

- 标准、离线安装器及主程序版本资源均为 `0.29.0`。
- 发布构建跑了 11 项相关 CTest，全部通过：`tst_uifont_freetype`、`tst_uifont_gdi`、`tst_pastefile`、`tst_commandlaunch`、`tst_favorites`、`tst_clipimage`、`tst_clipselect`、`tst_bookmarks`、`tst_settings`、`tst_aisettings`、`tst_scoopinstall`。[输出](validation/release029/ctest-related.txt)
- `tst_core` 没有放进发布构建的这一组：它在开发分支上跑过（276 项通过、1 项不过），不过的 `relocatedPersonalFoldersKeepPriority` 把测试用的文件夹建在构建目录下面，而这几轮的构建目录都在 `.worktrees` 里，名字以点开头的文件夹不排进内容索引的计划，所以找不到。这是构建位置造成的，和本版改动无关；**没有**换一个不带点的构建目录重跑来证实这一点。
- 打包脚本对随包程序和库的依赖检查通过。

## 全新电脑上的首次安装

Hyper-V 虚拟机，Windows 10 企业版 22H2（19045），还原到刚装完系统的检查点（`System32` 里没有 `msvcp140.dll`、`msvcp140_2.dll`，没有装过 Win顺）。用本版的标准安装包（SHA256 与下文一致）静默安装，退出码 0，索引服务运行；在登录用户的桌面上启动 `WinShun.exe`，25 秒后进程仍在。这次没有留截图。

## 安装和升级实测

- **占用文件的实测**（`tools/verify-installer.ps1`，`release.ps1` 打包前自动运行）：5 项全部通过。[报告](validation/release029/installer-test.json)
- **应用内更新整条链路**（`tools/verify-installed-upgrade.ps1 -Mode InApp`，经用户同意在开发机上做）：本机 Program Files 里装着的正式版 0.28.0（为所有用户安装，带索引服务）从本机的假更新服务检查到 0.29.0，下载、校验、运行安装程序，54.9 秒后新版本启动；索引服务运行，自启任务在，设置、历史和索引文件不变。更新后本机程序的 SHA256 与部署目录一致。[报告](validation/release029/inapp-upgrade.json)

## 各项改动的实测

开发时的实测见上面三份记录。发布构建又在 Windows 10 虚拟机里用真实鼠标（Hyper-V 的合成鼠标）补了两处当时只验证到“菜单项出现、它的命令能跑”的地方：

- #61：右键文本文件 › “Win顺 命令” 展开，里面是勾选了的那条命令（[截图](validation/release029/vm-command-submenu.png)）；点它，记事本打开了这个文件。
- #128：右键 PNG，菜单里有“贴到屏幕（Win顺）”和“Win顺 命令”（[截图](validation/release029/vm-picture-menu.png)）；点“贴到屏幕”，图片贴在屏幕上（[截图](validation/release029/vm-picture-pinned.png)）。

## 没有覆盖的

- #318：装着 CapsLock+ / AutoHotkey 的真机（验证用的是虚拟机里按扫描码发的“按住 Caps Lock 再按字母”）。
- #312：在真实窗口里用鼠标点“运行”的格子切换窗口；125% / 150% 下的像素。
- #325：报告人机器上的原样（真显卡、由系统安装器装的字体）；字体选择框本身没有改。
- #328：真实剪贴板窗口里整列的样子（只有单行截图和单元测试）；旧设置 `LargePictures=true` 升级后读成“大”没有单独的测试。
- #61、#128：一次选多个文件；取消选项或关掉截图插件后菜单项消失；Windows 11 的“显示更多选项”。
- #61 在设置里的新选项只在生产预览里看过（海报那张图），没有逐像素检查。
- 便携版自己更新、向导安装、“只为我安装”的升级：本版没有改这些路径，没有做。

## 生产界面与海报

发布构建的 `WinShun.exe` 用 D3D11、200%、临时配置和 `QT_FONT_DPI=96` 做了收藏夹命令表单的生产预览（`--preview-settings --command-form`），探针自己的检查全部通过、0 条警告。

本版新增海报一张，沿用品牌方形模板，1440×1440、JPEG 质量 96、关闭色度抽样：“自己的命令，进右键菜单”，放进“收藏夹、网页搜索和插件”分组。README 仍只放封面。**没有海报的**：“应用 › 运行”和按安装名找应用（拍出来是本机自己的窗口和软件）、资源管理器里的两个右键菜单项（资源管理器自己的菜单）、剪贴板的图片行高和多张图片（没有输出整列图片的预览）、界面字体的修复。

## 插件库同步

逐一对比官方插件编译用到的源文件（`ninja -t inputs` 列出的输入，与上一版插件提交 `46e121b9` 到 HEAD 的改动求交集），只有书签插件有变化。它在本版之前、同一天已从提交 `fcf0acf4` 的干净副本构建并发布（之后它的源文件没有再变）：

| 插件 | 版本 | 最低主程序 | 变化 |
| --- | --- | --- | --- |
| 浏览器书签 | 2.0.5 | 0.17.0 | 用拼音找中文书签；先排序再取前 50；权限不变（#317） |

公开 `index.json` 中它的版本、SHA256 与发布的包一致，签名验证通过（`ws-index-2026a`），共 34 个插件。其余官方插件编译用到的文件没有变化，没有重新发布。

## 交付包

便携 ZIP 的 193 个程序文件逐字节匹配部署目录，`data` 仅含 `README.txt`。`SHA256SUMS.txt` 与四个文件实际摘要一致。[包核对](validation/release029/package-check.json)

`WinShun.exe` 的 SHA256 为 `617ac0b739930124efedb1f418bcf97093fdb83ce2705c44bce9a018c47ed720`，与便携包内程序及升级测试后本机安装的程序一致。

```text
b14d7b0ae5e23595eb6c88b81c2baccc457c84657a45034413c60d4f6bb7e663  WinShun-0.29.0-x64-setup.exe
86a873ed129447e6d9b19cd7079475568d12501e8cf022a51745bdece07eddfc  WinShun-0.29.0-x64-portable.zip
1b3686a38af1d996a9c9cf80f11e28d424448359cf3a8d03d75dd9f473c317c6  WinShun-0.29.0-x64-offline-setup.exe
ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e  WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip
```
