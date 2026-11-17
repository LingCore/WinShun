# 0.33.0 发布验证记录（2026-11-17）

内容：PR #336（WinGet 文档）、#345（小工具 1.2.0：换算、时间戳、哈希、文件右键复制 SHA-256）、#347（C++ SDK 改设置、开设置窗口）；#356 截图翻译快捷键、#352 界面大小“密集”、#354 面板内容单击选中 + F11 + Ctrl+Shift+1…5、#358 `winshun://` 链接、#348 文件夹菜单分列、#343 贴图原地编辑 + 按键卡片、#360 框选前 Tab 只框窗口、#359 插件数字设置滑块、#361 Tab 进文件夹 / Ctrl+Backspace 退出，以及发布前处理的 #362（对话框旁的搜索框位置）。

## #362 对话框旁的搜索框

- 原因：“自动”下方放不下“搜索框 + 两行结果”就换到侧边（截图 1 的“乱跳”），对话框最大化时侧边也放不下，搜索框压在对话框顶部（截图 2）。
- 改法（`93d29c6e`）：“自动”和“下方”只要放得下搜索框本身就贴在对话框下面，屏幕底部的任务栏那一条也算（这时置顶、最宽 800 逻辑像素），列表往上展开；连这都放不下才去侧边 / 上方。`tst_dialogbarvisibility` 改写了摆放规则的用例，新增 `barTopUnder` 用例。
- 实测（stage 副本 + WinForms 另存为对话框，2560×1600、150%、任务栏 72 px，读窗口位置）：居中 → 搜索框在对话框下方 6 px；对话框底边离工作区底 20 px、贴到工作区底、最大化 → 搜索框在任务栏那一条上，置顶；最大化时顶部地址栏不再被挡（截图核对）。宽度上限是之后加的一行，只跑了单元测试：第二次实测时前台被别的窗口占着，对话框拿不到前台，搜索框不出现，没有强抢。实测结束后 Program Files 的 Win顺 已恢复，开机任务、HKCU 里没有指向 stage 的项。

## 测试

- `scripts/release.ps1 -Tests '^(tst_dialogbarvisibility_gpu|tst_dialogbarvisibility_software|tst_foldermenu|tst_pinwindow|tst_settings|tst_winshunuri|tst_tools|tst_launchernavigation|tst_settingsopenlink)$'`：8 个通过；`tst_foldermenu` 第一次失败（108 秒，输出没留下），单独重跑通过。它用真实窗口和注入的光标，当时用户正在用电脑。打包随后用 `release.ps1`（不带测试）重新跑完。
- 插件库 `tools/test_*.py` 全过，`test_manifest_fields` 新增 number 范围用例。
- 各 issue 轮次在提交前的实测见对应记忆 / 提交说明（#358、#348、#343 2026-11-16/17 实测；#361/#360/#359 19/19）。

## 安装和升级

- `tools/verify-installer.ps1`（打包前自动运行）：5 项全过。
- `tools/verify-installed-upgrade.ps1 -Mode InApp`（经用户同意）：本机 Program Files 的正式版 0.32.0 经应用内更新真实升级到 0.33.0，50.7 秒；新版本以普通权限启动，索引服务在运行（同一路径），开机自启任务和搜索历史（SHA256 不变）保留；设置文件多了新版的默认项。

## 插件

- 对比基准 `9ae27471`（0.32.0 插件源码）：截图与录屏（capture-plugin 18 个文件）升 1.1.19；图片文字搜索（共用的 UiSize/Theme/UiScale/WinShunUri）升 1.0.18。
- semantic、localsend、break、explorer-tabs、bookmarks、translate、cloudbackup 只因 `winshun.h` 加了 `updateSettings` / `showSettings`。宿主只对清单里声明了 `settingsWindow` 的插件发 `showSettings`，这几个都没声明，行为不变，不升版不重发。web、go 没有变化。
- 小工具 1.2.0 随安装包分发（`plugins/winshun.tools/plugin.json`）。

## 打包

`scripts/release.ps1 -BuildDir build\run-latest -OutputDir dist\release0330`，从 `59524f2b` 构建（之后只改了文档、海报和本记录）。

| 文件 | SHA256 |
|---|---|
| WinShun-0.33.0-x64-setup.exe | 0fab9442aebd2eef50ac159b60a28aba66395f72d71777078286868f389901dc |
| WinShun-0.33.0-x64-portable.zip | cdcade589de346ebec4018fd36edee07fb2aa9eeb6af9ee6dd9f92dbd8ce47e3 |
| WinShun-0.33.0-x64-offline-setup.exe | 537f6a16e073e0a993d0758eae3d62dda23402c6aa9858500f3403f3a8b051c8 |
| WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip | ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e |

## 海报

重做 `foldermenu.jpg`（`tools/screenshots/release033.py`，发布程序 `--preview-results --style-windows --scale 2`，菜单和第一列各自加 Windows 11 圆角边）。其他变化没有新海报：贴图编辑、截图翻译快捷键、Tab 只框窗口（要真实桌面）、`winshun://`、密集、Tab 进文件夹、#362。

## 没有覆盖的

- 文件右键菜单只出现“复制 SHA-256”、卡片开关显示 / 隐藏 MD5 / SHA-1：没有在真实右键菜单里点过，靠 `tst_tools` 和 `files.if` 的单元测试。
- #362 宽度上限（800）没有实测；任务栏不在底部、自动隐藏时的回退（侧边 / 上方）没有实测。
- 干净的 Windows 10 虚拟机首次启动；Setup / Wizard / Portable 升级方式。
