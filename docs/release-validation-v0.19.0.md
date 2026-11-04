# Win顺 0.19.0 发布验证

2026-11-04，在 Windows 11 x64、Qt 6.12.0、MSVC 2022 Release 环境验证。发布程序与插件包来自干净的 `b35ebac3c6c287963dfa92f1ea3cb08bb3e3030a` 提交副本（`git archive`，路径 `F:\wsrel019\src`）。之后的提交只有本记录。

## 合并

本版合入四组改动。合并前已在各自分支上实测：
- 设置窗口提速。
- 演示白板、截图按键（#187、#189）。
- 插件跟随管理员权限与资源管理器标签页（论坛反馈一轮）。
- 图片文字搜索召回一轮与 Scoop 安装。

合并时 `docs/pitfalls.md` 两边各在末尾加了一节，两节都保留。编译发现 Scoop 的卸载清理还在用 #190 之前带标题参数的右键菜单接口，已改正（`356b283`）。

## 构建与回归

- 使用 `scripts/release.ps1 -BuildDir E:\wsrel019\build -OutputDir E:\wsrel019\out` 构建。按项目规定，只跑本次改动涉及的测试，不跑全部 CTest。
- 合并后跑了 17 项测试，全部通过：`tst_settingsreopen`、`tst_settingslazy`、`tst_settingsopenlink`、`tst_settings`、`tst_plugin`、`tst_pluginupdates`、`tst_movedplugins`、`tst_explorertabs`、`tst_updater`、`tst_scoopinstall`、`tst_ocrsearch`、`tst_ocr`、`tst_capturekeys`、`tst_capturesettings`、`tst_capture`、`tst_previewplacement`、`tst_pluginhotkeys`。
- 补拍演示白板海报时，发现激光笔拖尾在每个点上留一个洞：点的圆和连接两点的色带方向相反，按 WindingFill 互相抵消。已修复（`dd73eac`）。新增断言在修复前失败、修复后通过（`b35ebac`，`tst_lasertrail`）。
- 部署程序的 FileVersion / ProductVersion 均为 `0.19.0`。`WinShun.exe` 的 SHA256 为 `cfe0d2cde06e40fcee53291aa7cf3f893d152a475e72c676a62d0f002fb64c5d`。
- 用发布出来的程序跑一次隔离的 `--probe-runtime … --settings-reopen-check`，通过。重新打开设置时，第一帧在 4–8 ms 内画出。

## 插件同步

逐个对比已发布包的源码提交（`8aaac9f`，图片文字搜索为 `d777472`）与本次插件，以及它编译进去的共享源文件：

| 插件 | 版本 | 最低宿主 | 变化 |
| --- | --- | --- | --- |
| winshun.capture | 1.1.0 | 0.18.0 | 演示白板，按键可改，WASD 指针，按键提示，两行工具条，卡片式设置，共用 OCR 读取，激光拖尾修复 |
| winshun.explorer-tabs | 1.0.0（新） | 0.19.0 | 新插件，`elevated`、`ui` 权限 |
| winshun.ocr-search | 1.0.3 | 0.19.0（构建时写入） | 图标按像素网格绘制，窗口图标 |
| bookmarks、break、localsend、semantic、translate、web | 不变 | — | 编译进去的文件没有变化，不重发 |

三个新包用插件库 `check.check_package(..., official_native=True)` 检查，均为零错误、零警告。归档内没有反斜杠路径。插件库同步更新了 `tools/check.py` 和 schema，使其识别 `elevated`（规则与 `Manifest.cpp` 一致），并在 `tools/registry.py` 中把资源管理器标签页登记为官方原生包。

| 包 | SHA256 |
| --- | --- |
| winshun.capture-1.1.0.wsplugin | `a0f1e253f70e779cbc735c5026308531ec6721d9c0907388d3d8fba66b4c5a02` |
| winshun.explorer-tabs-1.0.0.wsplugin | `7f3347a247cd637d0f4853495c68058ba7babad0c3b1a26e9e04772d8e4f01a4` |
| winshun.ocr-search-1.0.3.wsplugin | `81f898caa8c8f0aa04039c9b33b886d64090011b569be4aebfb25bf4f0bc851b` |

## 包及截图

标准安装包、便携 ZIP、完整离线安装包和增强 OCR ZIP 的 SHA256 见同版 `SHA256SUMS.txt`。便携 ZIP 里的数据文件只有 `data/README.txt`。自带插件为 go、kill、switches、tools。OCR ZIP 按固定清单核对了 6 个文件。

使用 `tools/screenshots/release019.py` 和隔离预览补拍了三张海报：
- 两行截图工具条（替换 `capture.jpg`）。
- 截图按键页（新增 `capture-keys.jpg`）。
- 演示白板（新增 `presenter.jpg`）。

演示白板用新的 `--preview-presenter`，走生产代码的 Presenter、笔迹窗口和工具条，离屏渲染白板，不采集桌面。海报沿用方形品牌模板，1440×1440、质量 96、无色度抽样，README 保持 `width="49%"` 双列。截图所用程序来自激光修复前的同一分支构建（`WinShun.exe` `b5288126…`）；三张海报不含激光笔，界面与发布版相同。未变化的功能沿用已核验的海报。

## 验证范围

- **演示白板**：只在单显示器 150% 缩放下实测。多显示器、100% 缩放，以及和录屏、会议软件同时使用，都没有覆盖。
- **资源管理器标签页**：在测试文件夹中实测。资源管理器先加载“主文件夹”，开成标签页的中位时间约 1.65 秒。正式模式下用户自己的资源管理器窗口没有测。
- **管理员插件**：AI / `ws` 的拒绝路径只有单元测试。
- **Scoop**：清单在 Release 发布后才更新为本版。
- **#190**：英文 Windows 分支只有单元测试，本机是中文 Windows。
- **安装包**：完成了构建、元数据和归档校验。没有用覆盖安装去动本机现有的安装和服务。
