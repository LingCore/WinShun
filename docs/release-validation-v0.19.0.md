# Win顺 0.19.0 发布验证

2026-11-04，在 Windows 11 x64、Qt 6.12.0、MSVC 2022 Release 环境验证。发布程序与插件包来自干净的 `7bb3203986262f972d8fcd1e5165c07239773da4` 提交副本（`git archive`，路径 `F:\wsrel019b\src`）。之后的提交只有本记录。

## 合并

本版合入六组改动。合并前已在各自分支上实测：
- 设置窗口提速。
- 演示白板、截图按键（#187、#189）。
- 插件跟随管理员权限与资源管理器标签页（论坛反馈一轮）。
- 图片文字搜索召回一轮与 Scoop 安装。
- 安装程序：不是管理员的账户默认“只为我安装”（自写的安装方式选择，`installer/InstallMode.iss`）。
- 没装索引服务时，管理员窗口在前台让双击 Ctrl 失灵的一次性提示（`src/app/AdminTapHint.h`）。

合并时 `docs/pitfalls.md` 两边各在末尾加了一节，两节都保留。编译发现 Scoop 的卸载清理还在用 #190 之前带标题参数的右键菜单接口，已改正（`356b283`）。

## 构建与回归

- 使用 `scripts/release.ps1 -BuildDir E:\wsrel019b\build -OutputDir E:\wsrel019b\out` 构建。按项目规定，只跑本次改动涉及的测试，不跑全部 CTest。
- 合并后跑了 17 项测试，全部通过：`tst_settingsreopen`、`tst_settingslazy`、`tst_settingsopenlink`、`tst_settings`、`tst_plugin`、`tst_pluginupdates`、`tst_movedplugins`、`tst_explorertabs`、`tst_updater`、`tst_scoopinstall`、`tst_ocrsearch`、`tst_ocr`、`tst_capturekeys`、`tst_capturesettings`、`tst_capture`、`tst_previewplacement`、`tst_pluginhotkeys`。后加的两项合入后，再跑 `tst_admintaphint`、`tst_settings`、`tst_settingsreopen`、`tst_settingslazy`、`tst_settingsopenlink`、`tst_aisettings`、`tst_lasertrail`，7 项全部通过。
- 补拍演示白板海报时，发现激光笔拖尾在每个点上留一个洞：点的圆和连接两点的色带方向相反，按 WindingFill 互相抵消。已修复（`dd73eac`）。新增断言在修复前失败、修复后通过（`b35ebac`，`tst_lasertrail`）。
- 部署程序的 FileVersion / ProductVersion 均为 `0.19.0`。`WinShun.exe` 的 SHA256 为 `412c828251a907cf449a0fb6eb13ae3e648e2d08edcf0ea610200f2d5cfd1014`。
- 用发布出来的程序跑一次隔离的 `--probe-runtime … --settings-reopen-check`，通过。重新打开设置时，第一帧在 4–8 ms 内画出。

## 安装方式选择

Inno 自带的“为所有用户 / 只为我”对话框只按脚本写死的 `PrivilegesRequired` 决定默认项，不看账户是不是管理员，而且在 `[Code]` 之前出现。改为 `PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=commandline`，在 `InitializeSetup` 里自己决定，顺序如下：
1. 命令行给了 `/ALLUSERS` / `/CURRENTUSER`，照办。软件内更新总带其中一个，行为不变。
2. 只在 HKLM 或只在 HKCU 有安装记录，沿用原来的方式。
3. 否则弹自己的两键对话框：管理员账户“为所有用户”在前，标准账户“只为我”在前。
4. 选了“为所有用户”，就以 `/ALLUSERS` 重新运行自己，由 Inno 提权；拿不到管理员权限时，改问要不要只为自己安装。

用只做判断的测试安装包 `installer/tests/InstallModeProbe.iss`（独立 AppId，不动本机安装）测了：
- **本机管理员账户**：16 种情况，覆盖无记录、HKCU、HKLM、两处都有、静默、两种开关、软件内更新的命令行、拒绝提权、带空格参数、`/LOG`、`/LANG`。
- **临时本地标准用户**：默认项、静默、HKLM 存在、真实提权弹出密码框后取消。测完删除了账户和配置文件夹。

结果全部符合预期。没测：正式安装包在本机不带参数的真实安装（会以所有用户方式覆盖现有安装），以及提权后的安装向导是否在最前。

## 管理员窗口提示

没装索引服务、Win顺 不是管理员时，真正以管理员身份运行的程序（完整性 0x3000 且令牌已提升）在最前面停留 1.5 秒，托盘提示一次：双击 Ctrl 在这里收不到，改用组合快捷键；便携版还提示可以安装索引服务。每个程序只提示一次，总共最多 3 个程序，每次运行最多一次，记在 `[Launcher] AdminTapHinted`。

在真实程序里测了（独立部署和配置，隐藏服务监听）：首次提示、同一程序不再提示、普通窗口不提示、关闭双击不提示、测试副本自己提权时不提示；点击后分别打开快捷键行和便携版的“安装索引服务”行。测试中发现并修好两处：安装版没有安装按钮却提示去装；点击打开时没有滚到那一行。没测：提示气泡在屏幕上的样子，以及旧服务不报 Alt / Shift 的情况。

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
| winshun.capture-1.1.0.wsplugin | `44f5cbcc9b99da781ba07210b303485ae0e87573dafb8c0846609a15bedfea3e` |
| winshun.explorer-tabs-1.0.0.wsplugin | `873a832b5e705ce2af2ccb2e011ae5551974f9d5fea7ae69dff3f504168312e2` |
| winshun.ocr-search-1.0.3.wsplugin | `6fde2cb3f793ec6ccd85c780e77e6075e6a2c11734c64c01f49fab463def567d` |

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
