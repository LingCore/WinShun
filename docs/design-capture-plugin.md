# 截图与录屏拆成插件（GitHub #138）

2026-10-29。#138 说 Win顺 什么都往里塞；用户认为说得对：有些东西不该塞进主程序，而该做成 C++ 插件，而且不随安装包自带，要用的人从插件库装。

## 范围

| 东西 | 结果 |
|---|---|
| 截图与录屏（`src/capture`，约 2.6 万行，加上截图的 QML 和 App 里的接线） | 拆成插件 `winshun.capture`，放进插件库 |
| 局域网传输 `winshun.localsend`、按意思找 `winshun.semantic` | 本来就是 C++ 插件，从安装包里拿掉，放进插件库 |
| 休息提醒（插件库 #7） | 另一个会话在做，照同样规矩：不自带 |
| 剪贴板历史 | 留在主程序：和搜索框是一个窗口，默认开，用的人多；关掉就不占资源 |
| 计算器、结束进程、书签、翻译、web、go | 留着自带：一共约 600 KB，是启动器的本分，也是写插件的示例 |

## 1. 进程：插件自己一个进程，用 Win顺 的 Qt

- 和别的插件一样由 runner 起，有作业、有权限：`ui`、`clipboard:read`、`clipboard:write`、`files:write`、`notifications`。截图崩了不拖垮搜索框；没装的人进程里一行截图代码都没有。
- 常驻、`eager`、`idleExitSec: 0`：快捷键一按就要出遮罩，不能等进程冷启动。
- **Qt 用 Win顺 安装目录里那份**，不另带：少下载约 40 MB，代码页和 Win顺 共享。做法：`WinShunCapture.exe` 是不带 Qt 的小启动器（静态 C 运行库），读宿主给的 `WS_APP_DIR`，`SetDefaultDllDirectories` + `AddDllDirectory` 把那个目录加进 DLL 搜索路径（不搜 PATH、不搜当前目录），再加载同目录的 `WinShunCaptureApp.dll`；后者导入 Qt，由加载器在 Win顺 目录里找到。Qt 的插件（`platforms\qwindows.dll` 等）和 QML 模块也从那里取（`QCoreApplication::setLibraryPaths`、`addImportPath`）。
- 只用 Qt 的公开 API（`src/capture` 没有 `private/` 头文件）；QML 只编成字节码、不编成 C++（`qmlcachegen --only-bytecode`），因为编成的 C++ 调的是 QML 的私有接口。这样 Win顺 升级到更新的 Qt 6，旧插件照样跑；字节码版本对不上时 QML 引擎从源码重新编译。
- 识字的 onnxruntime.dll 和 `ocr\` 里的模型同样在 Win顺 目录：插件里的 `PaddleOcr` 直接用那个目录，不经主程序转一道。
- `WS_APP_DIR` 是通用的：宿主给每个插件都设，别的插件用不用随意。

## 2. 插件里有什么，主程序留什么

插件 `WinShunCaptureApp.dll`：
- `wscapture` 静态库（不变）和现在编进 WinShun 的 QML 模块里的截图类：`Capture`、`CaptureSession`、`CaptureImage`、`CapturePointer`、`AnnotationModel`、`AnnotationCanvas`、`CaptureEditor`、`CaptureOcr`、`LongCapture`、`Recorder`、`PinManager`、`PinWindow`。
- 截图的 QML：`CaptureOverlay`、`EditWindow`、`Annotate*`、`OcrPanel`、`RecordingPanel`、`LongCapturePanel`、`KeyCap`，以及设置窗口（现在的 `CaptureCard` 的内容）。
- 它们用到的公共 QML（`Theme`、`Glyph`、`HoverTip`、`TitleBar`、`WindowEdge`、`FlatButton`、`ToggleSwitch`、`DropMenu`、`HotkeyRecorder`、`InputBox`、`Tag` 等）和背后的 C++（`SystemTheme`、`WindowFrame` 等）**同一份源文件同时编进两个模块**。插件的 QML 模块 URI 也叫 `WinShun`：两个进程各有各的，QML 文件一个字不用改，公共控件只有一份源码。
- `App` 里现在的截图接线（`applyCapture`、`pins()`、`editor()`、`captureOcr()`、快捷键、PrtScn 接管、托盘项）搬进插件的 `CaptureApp`，改成跟宿主说协议。

主程序留下：
- 截图历史 `jt`：它列的是剪贴板历史里标了"截图"的图片，数据在主程序。插件开着时关键词才有效（和现在"截图开着"一样）。
- 剪贴板图片的识字（`PaddleOcr`）。
- `PrintScreen.cpp` 的恢复：老版本接管过 PrtScn、而截图插件没装上时，主程序把 PrtScn 还给截图工具，免得这个键按了没反应。

## 3. 协议的新东西

插件 → Win顺（`winshun/tool`，只给插件）：
- `clipboard_add_image {file, project, copy}`（要 `clipboard:write`）：截图进剪贴板历史、标"截图"、带可再编辑的项目；`copy: false` 是 Ctrl+S / F3 那种只记历史不动剪贴板。图片是插件数据文件夹里的临时文件，主程序读进来后删掉。照旧看剪贴板历史开没开、暂停没有。
- `show_launcher {text}`：识别文字并搜索，把字填进搜索框。
- `web_search {text}`：识别文字并网页搜索，用 Win顺 设置的搜索引擎。
- 做的时候又加了 `clipboard_copy {text}`（取色、识别出的字进剪贴板和历史）和 `clipboard_copy_entry {id}`（编辑窗口里没改的图，把历史里那一条重新放上剪贴板）；`clipboard_add_image` 多一个 `screenshot`（标“截图”）。
- `winshun/hotkeys {keys}`（通知）：插件自己注册的快捷键。Win顺 让出 Win+Shift+S（它接管 Win+S 时自己占着）和剪贴板的 Ctrl/Alt+数字；进程结束就收回。

Win顺 → 插件（manifest 声明，宿主照着画）：
- ~~`"menus"`~~ 做的时候拆成了两样：
  - `"trayMenu": 文字`：托盘菜单里一个子菜单。右键按下托盘图标时宿主问 `trayMenu`，插件回当时的项（`{id, title, checked, enabled, children}`，两层），松开时菜单弹出；选了发 `menu {id}`。比 manifest 里写死的项好：贴图有几张、延时几秒、有没有放弃的标注，都是那一刻的。
  - `"pictures": ["pin", "edit"]`：剪贴板历史的图片、截图历史、搜索结果里的图片文件上的“贴到屏幕”“编辑”，发 `picture {action, file, clipId, project, screenshot}`。文件是 Win顺 的，插件只读。
  - 这些和 `showSettings` 发之前，宿主 `AllowSetForegroundWindow(插件的 pid)`（不是 `ASFW_ANY`），插件的窗口才到得了前台。
- `appearance`（`initialize` 里一份，变了再发通知，只给有 `ui` 权限的）：Win顺 的界面语言和外观设置，插件的窗口照着画。
- `"settingsWindow": true`：卡片上的"设置"按钮发 `showSettings`，插件开自己的设置窗口（选项太多，宿主的 `settings[]` 画不下）。

插件自己做：注册快捷键（`WM_HOTKEY` 给它前台权）、接管 PrtScn 并在 `shutdown` 时还回去、录屏时的键盘鼠标钩子、系统通知（`winshun/notify` toast，点了打开保存的文件夹）。

## 4. 设置与迁移

- 插件的设置存它数据文件夹里的 `capture.ini`，键名和现在 `[Capture]` 一样。
- 升级后第一次启动（`MovedPlugins`）：`[Capture] Enabled=true` 的人，从插件库装上 `winshun.capture`，把 `[Capture]` 整节抄进 `capture.ini`，`[Plugin.winshun.capture] Enabled=true`、`Confirmed=` 它要的权限（截图是用户自己打开的，同一个功能，不再问一遍）。
- 装不上（离线）而 PrtScn 被接管着：主程序先把 PrtScn 还回去，下次启动再试。
- AI 改截图设置的那几项（`AiSettings` 的 `Capture/*`）去掉：插件的设置 AI 改不了。

做成的样子：`App::moveCapture` 在启动时（插件主机建好之后、应用快捷键之前）调 `moved::moveCaptureSettings`：`capture.ini` 已经有了就不覆盖；`Enabled` 和其余键从 WinShun.ini 删掉，只留 `Keyword`（截图历史的关键词，还是 Win顺 的，`AiSettings` 里也只剩它），所以不会搬两次。`PrintScreenBefore` 跟着搬给插件，由插件关掉时写回；插件没开时主程序自己写回（`App::restorePrintScreen`）。插件以 `moved::capturePermissions()` 打开（和 plugin.json 的权限一样，`tst_movedplugins` 核对），十秒后由 `installMovedPlugins` 从插件库装上，和局域网传输、按意思找一样。

## 5. 先后

0. 探针（`src/capture-plugin/Probe.cpp`）：插件进程在 runner 的作业里加载 Win顺 的 Qt、开 QML 窗口、截屏、注册快捷键、低级键盘钩子、打开剪贴板。全过了才往下。
1. 插件程序：把截图的 C++、QML 和接线搬进 `src/capture-plugin`，`capture.ini`，协议客户端，设置窗口。
2. 主程序：删掉截图（`App`、`Settings`、`SettingsEditor`、`AiSettings`、`Preview`、托盘、QML），加 §3 的工具和 `menus` / `settingsWindow`，PrtScn 兜底。
3. 迁移、测试、文档、截图海报；真机测试（插件库装、升级迁移、快捷键、PrtScn、贴图、编辑剪贴板图片、录屏、识字）。

## 做到哪了

- 阶段 0–2 已提交（`e3219f6` 协议、`af68bca` 插件、`1034520` 主程序去掉截图），阶段 3 的迁移 `9eef3f1`，文档随后。
- 验证过：插件自检 5/5（`wsbench --plugin … --query "cap __selftest" --app-dir …`）；`WinShunCapture.exe --preview-capture-tools`、`--toolbar-drag` 0 警告；`WinShun --preview-settings` 12 页 0 警告；ctest 89/89。
- 还没做：真机测试（插件库装、升级迁移、托盘子菜单、贴图和编辑剪贴板图片、设置按钮、Win+Shift+S 交接、PrtScn、录屏、长截图、识字），插件包和插件库索引的发布。

## 未定 / 风险

- 开着截图、接管了 PrtScn 的人升级后插件一直装不上（离线）：`PrintScreenBefore` 已经搬进 `capture.ini`，主程序不会替它还，PrtScn 是 Windows 老的“整屏复制到剪贴板”（不是没反应），插件装上后照常接管。

- 作业的内存上限：manifest 的 `memoryMB` 最多 2048；长截图和 4K 录屏可能更大，要实测后定（必要时放宽上限）。
- 插件 eager 在 Win顺 启动 10 秒后才起：开机后头 10 秒按截图快捷键没反应。
- 截图历史（另一个会话正在做，`CaptureHistory`）提交后跟着搬进插件。
