# 剪贴板与全局输入架构

本文档记录 Win顺 当前的全局输入触发、剪贴板监听存储、无焦点交互、UIAccess 显示与跨权限粘贴设计及线程/安全边界。

## 1. 全局触发与前台切换

### 1.1 Raw Input 双击检测与热键

- **`KeyListener`（Raw Input 独立线程）**：异步监听键鼠，避免常驻 `WH_KEYBOARD_LL` 阻塞输入或超时被摘除。
- **`DoubleTapDetector`**：`firstPressTime` 经 `Tap::firstPress` 供防误关判断；单侧抬起丢失超 `lostReleaseMs`（1.5 s）后另一侧按新击计；慢钩子拖延首击超 `maxPressMs`（300 ms）且松开到第二击 ≤ 16 ms（`kQueuedGapMs`）、同键、鼠标未动仍有效（记 `queued_ms`）；未触发经 `takeMiss` 记 `double-tap-near-miss`。
- **`FolderMenu::hotkeyPressed`**：`[Launcher] FolderMenuHotkey` 经 `RegisterHotKey` 触发，工作线程查目录，`menuhotkey::anchor`/`Toggle` 管混合 DPI 定位与防重入；Win顺 在前先还焦或收起启动器，弹出复用 `KeyRouter`。

### 1.2 服务 UIAccess 监听与 `TapTeller`

- **跨权限监听**：普通主进程在管理员窗前受 UIPI 限制收不到 Raw Input。`ServiceKeyListener` 后台维系 `WinShunService.exe` 租约，服务以会话用户 UIAccess 令牌启 `service/InputListener.cpp`（权限见 4.2 表）；助手仅向 `WinShun.KeyListener` 发心跳、`keyproto::kTap`/`tapSpan` 并调 `AllowSetForegroundWindow`（不传文本），设硬工作集下限（+1 MB，上限 16 MB）。
- **`TapTeller` 双路容错**：本地 Raw Input 与助手并行识别，助手 120 ms（`kBackupMs`）未报则用本地备份（`own-backup`），`toldAlready` 去重；≤ 1 s 直用，1–3 s 需无后续输入且前台未换，> 3 s 丢弃记 `double-tap-dropped`；管理员窗在前仅靠助手。

### 1.3 游戏抑制、前台切换、防误关与录制

- **`GameGuard` / `PointerWatch`**：全屏藏指针或被捕获（`hwndCapture`）抑制双击；`hiddenForTyping` 排除打字藏指针（前 100 ms 同窗非修饰键首次按下、未按鼠标且未动），查自 Raw Input 线程 `KeyPresses`（10 s / 256 条），隐藏与双击双判。
- **`win::bringToFront` 与失焦**：先查 `AllowSetForegroundWindow`，不能则发 `kOwnInput` 空鼠标输入再激活，弃 `AttachThreadInput`；1.5 s 内失焦由 `App::logLauncherLostFront` 记录抢焦者与输入态。
- **`ShortcutAgain.h` 与预热**：揭罩 200 ms 内重复触发仅 `ShortcutAction::Raise`（`DialogBar::toggleFocus` 未接入）。剪贴板启动 1.5 s 预绘，唤醒 5 s / 6 s 预热启动器与剪贴板；`IdleTrim` 据电源事件与时钟差（≥ 10 s）暂缓回收（`held: resumed`），`up` 回调护无焦点窗。
- **`ShortcutCapture` 与 `platform/WinV`**：`ShortcutCapture` 录热键时独立线程临时挂 `WH_KEYBOARD_LL`（吞 `Win`、放媒体键，失败退 Qt）。`WinV` 写 `DisabledHotkeys`（`V`）并在后台按原普通权限重启 `explorer.exe`（令牌复制或 `sihost.exe`），`TaskbarCreated` 后重注 `Win+V`，关闭/卸载移回自写 `V`。

## 2. 剪贴板监听、存储与识别

### 2.1 监听、自写隔离与隐私

- **`ClipboardWatcher`**：独立线程 + 隐藏窗调 `AddClipboardFormatListener`，等 40 ms 聚合；无文字才读图避延迟渲染，解码转 PNG 在同线程。粘贴写入同在监听线程记序号，跳过自身通知（仅置顶，合并粘贴不入库）、不回读防争锁，附 `WinShun.ClipboardEntry`。
- **隐私标记**：忽略 `ExcludeClipboardContentFromMonitorProcessing`、`Clipboard Viewer Ignore`（免开剪贴板）与 `CanIncludeInClipboardHistory == 0`，支持进程黑名单。

### 2.2 异步写库与 SQLite（`ClipStore`）

- **线程边界**：内存列表（前 64K 字符）仅主线程访问；SQLite 与图片读写交单写库线程 `ClipStore::Writer` 串行队列。写操作先改内存、单调分配不复用 ID 入队，读盘同队列保序；落盘前 `imagePending` 为真，完成经 `pictureSaved` 刷新、失败经 `lost` 回滚，退出排空。
- **SQLite 回收**：仅用 Qt SQLite 驱动，WAL（`journal_size_limit` ≤ 1 MB）+ `auto_vacuum=FULL`（旧库写库线程 `VACUUM` 迁移），删除提交即截断空闲页；图片原格式存盘，撤销期保留。

### 2.3 图片原格式、动图与公式

- **`ClipImage` / `ClipPreview`**：处理 GIF/PNG/JPEG/BMP-DIB，先验尺寸再解码；静态按像素、动图按原始字节去重，`imageFormat`/`animated` 兼容旧 PNG。动图写入给原格式、原文件与首帧 PNG/DIB；`ClipPreview` 仅载当前动图、离屏暂停、关闭卸载。
- **公式与嵌入对象**：独立公式（`MathML` + EMF）与嵌入对象（`Embedded Object`/`Equation.*` `Embed Source`）存图片（缺图 `imageFromEmf` 2× 渲染），原序打包 `formats`（`packClipFormats`，超 16/32 MB 退普通复制），线性文本存 `Clip::searchText`（优先 `ocr`，`Shift+Enter` 粘），排除回链、OLE 私有格式与单 `Embed Source`。Word 同给文字与 `\mmath` RTF 作富文本并存 `look` 及 `documentFormatName`。

### 2.4 本地 OCR 与镜像下载

- **`TextRecognizer`**：低优先级线程顺序识别（新图优先、空闲释放），写 `clips.ocr` 供 `ClipMatcher` 拼音搜索，启动 20 s 补扫、切引擎重扫。优先离线 `PaddleOcr`（PP-OCRv6 tiny + ONNX Runtime CPU，共享 `dataDir()/components/ppocr-v6-ort1.30.0-x64`）；日/韩在前、未装或失败退 `Windows.Media.Ocr`。
- **`OcrFiles` / `OcrPackage`**：`OcrFiles` 仅本地解析；`OcrPackage` 主程序持有、`std::jthread` 析构前退出，按 `resources/ocr/package.json` 验路径/大小/SHA256 全过才原子发布 `.ready`；截图插件经 `WS_COMPONENTS_DIR` 与 `show_ocr_setup` 共享，不增联网权。
- **`Download`**：`src/core/Download.*` 统一 GitHub 镜像序（`github-mirrors.txt`：`gh-proxy.com` → `ghfast.top` → 原站）与慢源/校验回退，不套用于带凭据/查询参数 URL、API 或任意站点；`cmake/Download.cmake` 共享同源与 SHA256 替换。

## 3. 剪贴板窗口、无焦点按键路由与 UIAccess 显示

### 3.1 窗口、排序、预览与定位

- **窗口与排序**：`ClipboardWindow.qml`（内嵌 `ClipboardPage.qml`）与启动器解耦共用云母主题。`pinnedFirst`（`state.order`，默认关）控固定置顶；分组内按 `clips.rank` 排，`ClipModel::setRows` 发 `rowsMoved`。非分组页 `Ctrl+A`/`RemoveAll`（`listedIds`）护固定与分组项，支持 `Ctrl+Z`。单击展预览（`state.preview`/`previewWidth`，优先占 `Placement::roomRight`）。
- **挂接与光标**：挂 Win顺 框用 `Placement::attach`（`boxside::above` → `Placement.above`、`clipboardBelow`、`DialogJump::setCompanions`），失焦排队复查隐藏。外部用 `Placement::attachNative`/`win::TextCaret`（`GetGUIThreadInfo` → `OBJID_CARET` → UIA `TextPattern2`），后两级 MTA 线程限 100 ms（UIA 300 ms，Chrome/Firefox 禁 UIA），失败退鼠标下 4 px。上方且开 `flipAbove` 按 `BottomToTop` 倒序贴光标。

### 3.2 无焦点路由（`KeyRouter`）与钉住

- **`KeyRouter`**：外部打开用 `_q_showWithoutActivating` + `WindowFrame::setNoActivate`（`WS_EX_NOACTIVATE`/`MA_NOACTIVATE`/`SC_MOVE`）保宿主焦点。开窗期独立线程挂 `WH_KEYBOARD_LL` 与鼠标观察，先 `CallNextHookEx` 让位改键工具，再经 `ToUnicodeEx`（标志 4）派给 `scopedFocusItem`（中文单按 `Shift` 触发 `shiftTapped`）。
- **掩码与钉住**：放行 `Win`/`Alt+Tab`/媒体键/`kOwnInput`；截留组合或单松 `Shift`/`Alt` 前补 `vkE8`（`Ctrl` 不补），收起留钩 ≤ 2 s 吞抬起；仅需 IME、开外部目标、Shell 弹出层或装钩失败才激活。钉住（`Clipboard::pinned`）置顶常驻，收起转 `releaseClipboard` 停 `KeyRouter`，双击实时取前台作 `m_target`，热键/点框经 `engageClipboard` 临时接管，不存盘。

### 3.3 UIAccess 独立显示进程（`ClipboardDisplay`）

- 叠放原生 Start/Search 上方启 `WinShun.exe --clipboard-display`，提前进专用入口仅载剪贴板 QML、预览与菜单；监听、库、插件与粘贴全留普通主进程。
- 两端经限长管道（`ClipboardDisplayProtocol.h`，`kMaxFrame` = 1 MiB）同步视图与白名单操作；图标由普通进程渲染 PNG，显示进程不载 Shell 图标提供器。交互窗 PID 由同会话显示进程核对（Session 0 无法解析），双端互核 PID，白名单环境防 Qt/QML 劫持；异常或便携版回退普通窗避开 Shell。

## 4. 粘贴、连按松开粘贴与文件导出

### 4.1 常规粘贴与自身输入框

- **`platform/Paster.*`（`ws::paste`）**：等修饰键松开（`modifiersDown`），监听线程写多格式；曾激活则 `activate` 还焦等 60 ms（无焦点收起直发），经 `sendPasteKeys` 发 `Ctrl+V`（mintty/PuTTY 发 `Shift+Insert`，见 `PasteKeys.h`/`PasteInput.h`）；已按修饰键只发主键；剪切文件再粘改复制。
- **`TextField.cpp`**：呼出前记 `Qt::ImEnabled` 选区，写剪贴板后文本经 `QInputMethodEvent`（`Selection` + `commitString`）写入支持 `Ctrl+Z`；文件填路径，不粘图。`App::eventFilter` 将单行框（`isOneLine`）多行粘贴去空行合成空格单行。

### 4.2 管理员代按粘贴与服务 UIAccess 安全边界

- **`platform/ServicePaste.*`**：普通权限发管理员窗 `SendInput` 受 UIPI 丢弃（`integrity::higherThanOurs`）。开 `Clipboard/PasteElevated`（默认开）时，后台单连接调 `servicepaste::paste`（`OpenPaste{HWND}`），服务启 `--paste-keys <hwnd>`（限 3 s），`InputListener.cpp::pasteInto` 仅按 `PasteKeys.h` 键序即退；目标须本会话可见顶层 High 窗且仍在最前台，管理员“打开”框贴文件不走代按。开关不防同用户进程内注入。
- **助手加固（`launchSessionHelper`）**：会话助手用非管理员用户令牌（`TokenIsElevated=0`）+ UIAccess + 高完整性运行（非 SYSTEM、无写他进程内存权），`helperEnvironment` 锁定系统目录、`Path`、`TEMP`/`TMP` 并删 `__COMPAT_LAYER` 防 `HKCU\Environment` 劫持。
- **提权对话框导航（`ServiceDialogNav`）**：`OpenDialogNav`（`--navigate-dialog`）双端字符串校验目录与单文件名（`isValidFolderRequest`/`isValidFileName`），服务不以 SYSTEM 碰路径；助手直调 `System32\UIAutomationCore.dll` `DllGetClassObject` 建 `CUIAutomation8`（绕过 `HKCU\Software\Classes`），本地化名经 `SHGetLocalizedName` + `svcloc::pathProtected` 以数据文件加载，除旧式无地址栏/AutoCAD 复核目录填 `\` 确认还原原名外不代按确定。

| UIAccess 能力 | 协议 / 能力位 | 请求方认证边界 | 并发与托管 | 降级 |
|---|---|---|---|---|
| 管理员双击监听 | `kCapabilityKeyListener` | 交互用户 ACL（禁低完整性/AppContainer/远程）+ 同会话 | 租约 + 心跳 | 本地 Raw Input |
| 提权对话框跳转 | `OpenDialogNav` / `kCapabilityDialogNav`、`kCapabilityDialogNavName` | `mayNavigateDialogs`（同目录或 `--installed-from`；其余 `kCapabilityDialogNavBesideOnly`） | 每 PID 限 1（`ERROR_BUSY`），Job | 复制路径 |
| 管理员代按粘贴 | `OpenPaste` / `kCapabilityPasteRelay` | 同 `mayNavigateDialogs`（非本份隐藏能力位，回 `ERROR_ACCESS_DENIED`） | 每 PID 限 1，限 3 s | 仅复制提示 |
| UIAccess 独立显示 | `OpenClipboardDisplay`（小版本 5） | `requesterIsWinShun`（同目录且路径/所有者受保护 `WinShun.exe`） | 租约 + Job + 双端 PID + 白名单环境 | 普通独立窗 |

### 4.3 文件对话框粘贴与图片导出（`ClipFiles`）

- **文件对话框**：文件列表贴非管理员 `filedialog::Kind::Open`/`Legacy` 时，延迟 `ComWorker` 调 `setFileName`，`clipfiles::fileNames` 填带引号完整路径，不代按“打开”也不发 `Ctrl+V`。
- **`ClipFiles` / `DragOut`**：拖出与资源管理器粘贴统一在 `clipboard\dragged\<id>\` 生成含时间与 ID 的 PNG 副本（留至退出，崩溃残留启动清理）；多选按 `targets` 顺序整批准备（任一失败中止），左键仅复制，拖放停 `KeyRouter` 供 `Esc` 取消；各入口共用 `writeFor`。

### 4.4 按住快捷键连按，松开粘贴（`HoldToStep`）

- **交互与状态**：开 `[Clipboard] HoldToStep`（默认关）后，按住修饰键连按主键下移（忽略重复与方向键），全松粘贴；未步进满 `kHoldMs`（1 s）贴首项；`Esc` 取消。`HotkeySteps.h` 分 `Watch`（钩子线程按 `Sides` 产 `Note`）与 `Hold`（GUI 线程按序配对 `WM_HOTKEY`）。
- **`HoldWatch` 与 `PasteKeys/Guard`**：按住期于 `THREAD_PRIORITY_HIGHEST` 线程挂 `WH_KEYBOARD_LL`（不等 GUI、不写日志），吞 `Esc` 与主键重复，含 `Alt`/`Shift`/`Win` 时异步补 `vkE8`（`kOwnMask`），`drain()` ≤ 2 s。松开发键经 `paste::Guard`/`guardPaste`（`pastekeys::Guard`）协调手部按键（中途再按拦合成抬起并经 `agrees` 复核，无关修饰键先补 `vkE8` 抬起再按回，待 `pastePassing` 完卸钩）。
- **范围与兜底**：仅新开外部普通窗且首条 `WM_HOTKEY` ≤ 50 ms（`kArmWithinMs`）时装钩；管理员窗在前且开 `PasteElevated` 时面板拿焦走 `OwnWatch` + `checkClipboardHold` 经服务代按（未开或自身框退 `m_browseHotkey`）。`Clipboard` 用 `m_heldId`/`heldRow` 锚定；50 ms `Hold::poll` 管 1 s 提示与 150 ms 静默兜底。

## 5. 已知边界与相关测试入口

### 5.1 已知边界

- **多选与终端**：不支持图/文混贴或混拖，无“依次粘贴”；非 mintty/PuTTY 终端需手动粘贴。
- **自绘与跨权限**：不暴露光标/无障碍/UIA 的自绘程序退鼠标定位；无服务、旧服务或属另一份 Win顺 时，管理员双击、代按粘贴与提权跳转降级，Start/Search 退普通窗；未响应“幽灵”窗无法抢焦；管理员旧式框贴文件仅复制不代填。
- **热键与来源**：接管 `Win+V` 期间退出则原生 `Win+V` 暂失效；`ShortcutAgain` 未接入 `DialogBar::toggleFocus`；源仅给静态图不联网补动图；无可用 OCR 引擎留空待下次重试。

### 5.2 诊断与测试入口

- **诊断键**：`trigger`（`tap_from`/`listener_ms`/`queued_ms`）、`double-tap-near-miss`、`double-tap-dropped`、`idle-trim`、`key-helper`、`clipboard-hold-end`/`skipped`、`Launcher: lost the front ...`。
- **CTest（输入/剪贴板）**：`tst_core`（`hotkeySteps`/`pasteKeys`/`pasteGuard`）、`tst_keylistener`、`tst_keyrouter`、`tst_winkeys`、`tst_admintaphint`、`tst_service`、`tst_clipboard`、`tst_cliplifecycle`、`tst_clipselect`、`tst_clipimage`、`tst_clipfiles`、`tst_pinclipboard`、`tst_clipboarddisplay`。
- **CTest（粘贴/OCR/下载）**：`tst_paster`、`tst_servicepaste`、`tst_servicedialognav`、`tst_pastefile`、`WINSHUN_TEST_NATIVE_DIALOG=1 tst_filepaste`（默认跳过）、`tst_ocr`、`tst_ocrpackage`、`tst_download`、`tst_download_cmake`。
- **实时验证夹具**：`docs/validation/issue277-20261111/`（管理员代按粘贴）、`docs/validation/issue287-20261111/`（按住连按粘贴）；其余早期验证与审计记录存于 Git 历史。
