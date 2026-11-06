# Win顺 0.21.0 发布验证

2026-11-06，在 Windows 11 x64、Qt 6.12.0、MSVC 2022 Release 环境验证。发布程序与插件包来自干净的提交副本（`git archive`）：`f5517b169e10169b96d16f17629d6937dc2d7322`，路径 `F:\wsrel021\src`（构建 `F:\wsrel021\build`，输出 `F:\wsrel021\out`）。之后的提交只有两张海报、截图文档的引用和本记录。

## 内容

0.20.0 以后合并到 main 的几轮工作：
- 三种界面风格（精修原生、品牌色彩、轻拟物质感），所有窗口和截图、图片文字搜索、休息提醒插件的窗口一起换。
- 设置里的“运行状况”页、托盘最上面一行的状态、超过 2 分钟的大活结束通知。
- 进程更少：随包的小工具和系统开关在插件进程里运行，进程有看得懂的名字。
- 内存：设置窗口只做看过的页；最小化的窗口不算在用；运行状况页不再逐页读堆；内容索引只看改过的文件，大文档一次一个；图片文字搜索和按意思找插件闲时归还内存。
- Issue：#82、#137、#150、#164、#182、#198、#201、#202（含 C++ SDK）、#203、#207、#214、#216、#225。
- README 改成概览，截图移到 `docs/截图.md`，英文细节移到 `docs/features.en.md`；“界面风格”一项补上英文翻译。

## 构建与回归

- 发布构建（`scripts/release.ps1`，不带 `-Publish`）全新配置、全新构建。配置后 `CMakeFiles/rules.ninja` 的 `msvc_deps_prefix` 为“注意: 包含文件:”，不是问号。
- 按项目规定只跑本次改动涉及的测试（`-Tests` 正则），44 项全部通过：`tst_activity`、`tst_plugin`、`tst_pluginhotkeys`、`tst_pluginupdates`、`tst_switchplugin`、`tst_tools`、`tst_semantic`、`tst_ocrsearch`、`tst_contentdetails`、`tst_settings`、`tst_settingsscreen`、`tst_settingsscroll`、`tst_settingscapturehotkey`、`tst_settingsreopen`、`tst_settingslazy`、`tst_settingsopenlink`、`tst_aisettings`、`tst_appearance`（1、1.25、1.5、2 倍）、`tst_capture`、`tst_capturekeys`、`tst_capturesettings`、`tst_captureui`（1～2 倍，软件和 D3D11 各一套，加空闲图形）、`tst_recording`、`tst_foldermenu`、`tst_keyrouter`、`tst_diagnostics`、`tst_explorertabs`、`tst_admindialoghint`、`tst_traymenu`、`tst_notificationturns`、`tst_scoopinstall`。
- 部署程序的 FileVersion / ProductVersion 为 `0.21.0`。`WinShun.exe` 的 SHA256：`D5A91E49C50329EEC0D0E3725A273C6C18BF2D2019095501E8FBBB22735B7C9A`。
- 英文翻译：用同一个 lupdate 项目对 `winshun_en.ts` 的副本更新，新增的 10 条全是“界面风格”一项，已翻译；截图插件和图片文字搜索插件的翻译没有缺。

## 实机验证

开发时在暂存目录加临时配置下实测，没有动正在运行的 Win顺（详见各轮记录）：
- **运行状况页**：总数和 PDH 读数相差约 2 MB；后台服务一行、悬停、“现在就归还”和归还标记、离开页面后停止读取都正常。
- **内存**：设置窗口冷打开多占约 23 MB（以前两百多 MB）；最小化后约 13 秒归还；运行状况页开着时不再每分钟把归还的内存读回来。
- **进程更少**：插件进程里同时跑小工具和系统开关，只有一个子进程，私有内存约 7.9 MB；`WINSHUN_PLUGINS_APART=1` 可以恢复分开运行。
- **内容索引**：增量一轮只看 47 个改过的文件、2 ms；第一次建内容索引，交替各跑三次，新版约 48.6 秒、主进程峰值约 504 MB（旧版约 52.7 秒、约 780 MB）。
- **Issue**：#82、#182、#164、#198、#201（微信输入法）、#202、#203（A1–A5、B1–B6、C）、#137、#207 在集成分支上实测通过；#214 用测试覆盖没有视频功能的 D3D11 设备。
- **发布程序本身**：用 `--preview-settings` 按 200%、D3D11 渲染了全部设置页（含外观页的三种风格和运行状况页），日志没有警告。

## 插件同步

逐个对比已发布包的源码提交与现在的插件，以及它编译进去的全部源文件（用构建目录的 ninja 输入和头文件依赖列出，再与提交差异求交）：

| 插件 | 已发布 | 源码提交 | 变化 | 本次版本 | 最低宿主 |
| --- | --- | --- | --- | --- | --- |
| winshun.capture | 1.1.2 | `ed7454a` | 自身多处、`src/capture`、主题、`IdleTrim`、`winshun.h` | 1.1.3 | 0.18.0 |
| winshun.ocr-search | 1.0.5 | `cb0e23d` | 自身、`src/core`、主题、`IdleTrim` | 1.0.6 | 0.21.0（构建时写入） |
| winshun.semantic | 1.0.2 | `8aaac9f` | `main.cpp`、CMake、`winshun.h` | 1.0.3 | 0.17.0 |
| winshun.break | 1.0.1 | `8aaac9f` | `Core.h`、`Ui.cpp`、`main.cpp`、`winshun.h` | 1.0.2 | 0.17.0 |
| winshun.explorer-tabs | 1.0.0 | `7bb3203` | 规则、`main.cpp`、`plugin.json`、`winshun.h` | 1.0.1 | 0.21.0（构建时写入） |
| winshun.bookmarks、translate、localsend | 2.0.1、2.1.0、1.0.3 | `8aaac9f` | 只有 `winshun.h` | 2.0.2、2.1.1、1.0.4 | 0.17.0 |
| winshun.web | 2.0.0 | — | 无 | 不变 | — |

权限都没有变。截图插件 1.1.3 用到 0.21.0 新增的 `open_url` 工具（#225）；旧宿主回答“Unknown tool”，二维码的“打开链接”只是没反应，和以前一样，所以不提高最低宿主。

| 包 | SHA256 |
| --- | --- |
| winshun.capture-1.1.3.wsplugin | `1ec6cf6ab1038124cefe66b0c2d49cd1c044756f597344fac843a5c21820b693` |
| winshun.ocr-search-1.0.6.wsplugin | `0b6c2e67af429290e433f996a1a2277a6b1faa09fc42909db531908a1b2bbe3b` |
| winshun.semantic-1.0.3.wsplugin | `63100ed54835d93c0cdc0484fca52a9bbdd99de0761de0608e9e843ed8b9e65f` |
| winshun.break-1.0.2.wsplugin | `56bfe72deb34ca5e9903b5a401d28f85b31355ecfa0a6c1f6ae86b10dc8d694a` |
| winshun.explorer-tabs-1.0.1.wsplugin | `5f54b13cc35b72d0f1558c491a614a4b06323cf4c53d31612a3539698aabd0c6` |
| winshun.bookmarks-2.0.2.wsplugin | `58de4ded919ad080c1e92df2ccd1228826793e67571d63148ed8f09c228f0a0f` |
| winshun.translate-2.1.1.wsplugin | `c1223c4d2a2ad94b28941475325d5e4a362782212e01749ff1b5357fa5e8c783` |
| winshun.localsend-1.0.4.wsplugin | `4fd178b19fe4e61ef1891d298b8845449a3628bed01eadae34bf50cf0a1be4a1` |

八个包都用插件库的 `check.py` 做了文件夹检查，只剩“本仓库不收 exe / dll”、单个文件和文件夹大小这类原生官方包本来就不适用的提示，没有别的错误。`plugin.json` 的版本和最低宿主见上表。

## 包及截图

标准安装包、便携 ZIP、完整离线安装包和增强 OCR ZIP 的 SHA256 见同版 `SHA256SUMS.txt`，内容如下：

```
7d6db064ca585a205769a3fc5f8eb9d43b5234059b7175dee271dfee7aed830e  WinShun-0.21.0-x64-setup.exe
33aa9a38b6a101d2bbe128ea11e13af465e1d4f0e462c794a6db4f415d38b96a  WinShun-0.21.0-x64-portable.zip
e0e857efb29e46b35f9554444aa768bb705dec5fcb86b61349c3a19ace147bbf  WinShun-0.21.0-x64-offline-setup.exe
ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e  WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip
```

便携 ZIP 只含发布文件和 `data/README.txt`，没有索引、设置或剪贴板数据。

海报：`appearance.jpg` 重拍（外观页最上面多了三种界面风格），新增 `health.jpg`（运行状况页，预览自带的演示读数）。两张都由发布程序的 `--preview-settings` 按 200%、D3D11、独立配置渲染，沿用方形品牌模板，见 `tools/screenshots/release021.py`。`settings.jpg` 是设置搜索的图，左边列表少了“运行状况”一项，其余不变，沿用。

## 验证范围

- **发布程序**只用预览模式和测试跑过，没有替换正在运行的 Win顺 做整机实测；各项功能的实测在集成分支的同一份代码上做。
- **三种界面风格**在 Windows 11 上检查；Windows 10 没有实测。
- **#214** 没有在真正缺视频功能的显卡上录过，只用测试模拟。
- **#225**（`open_url`）、**#216** 的通知、**#150** 的“按键被占用”提示没有实机触发过。
- **截图插件 1.1.3 在旧宿主（0.18～0.20）上**没有实测，只按代码确认 `open_url` 失败时不影响别的功能。
