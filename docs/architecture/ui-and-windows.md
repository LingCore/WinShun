# 界面、窗口与系统集成架构

本文说明 Win顺 的渲染管线、主搜索窗口、搜索面板、预览、收藏与设置界面，以及文件对话框、文件管理器、资源管理器和任务栏集成的当前架构设计与边界约定。

## 1. 渲染、字体、主题与语言

- **D3D11 + FreeType**：内置阿里巴巴普惠体 3.0 常规字库（`fonts/`，无 hinting）。FreeType 将中文横笔画对齐像素网格，但 Qt 软件渲染器会裁剪 FreeType 溢出笔画，因此 FreeType 必须配 D3D11；省内存模式（`software`）切回 GDI。`Settings::defaultRenderer` 按内存择优（≥8 GB 默认 D3D11）。
- **字体加载与防退化**：Qt 合成粗体（`Theme.fontStrong` 用 `Font.Bold`，FreeType 不合成 `DemiBold`）；非 ASCII 路径先读入内存再 `addApplicationFontFromData`；界面避免普惠体缺失符号防触发系统字体回退；`main()` 首句调用 `_setmaxstdio(8192)` 防 FreeType 耗尽默认 512 句柄。
- **主题与圆角/直角**：颜色令牌与 4 套风格（`fluent`/`brand`/`tactile`/`liquid`）见 [colors.md](../colors.md) 与 [themes/README.md](../../src/app/qml/themes/README.md)。`[Appearance] Corners` 仅在 Windows 11 生效（Win10 恒为直角）；`WindowEffects::setWindowCorners()` 加锁枚举全部已登记顶层窗口（含菜单、提示与非 Qt 预览页）统一设 `DONOTROUND` 或恢复原 DWM 偏好；任务栏框经 `Look` 消息同步。
- **窗口预热（Prewarm）**：启动 1 s（启动器）、1.5 s（剪贴板）、2 s（拖放圆盘）后隐身无焦点绘制首帧并隐藏，消除首次呼出冷延迟。
- **多语言（i18n）**：源码中文走 `tr()`/`qsTr()`，英文译文在 `src/app/i18n/winshun_en.ts`（`--target update_translations`），切换动态 `QQmlEngine::retranslate()` + C++ `retranslate()` 无需重启。

## 2. 主搜索框、搜索面板与预览

### 2.1 主搜索框（`Launcher`）

- **输入区内 Logo 与拖动**：主搜索、面板、剪贴板与对话框搜索栏统一内置程序 Logo；左侧 Logo 与顶部横条由 `WindowFrame::addHandle` 登记为拖动把手，`WM_NCLBUTTONDOWN` 经 `DragDetect` 区分拖动与点击。
- **文件类型多选与大小**：`selectedFileTypes` 合并多选扩展名并与 `ext:` 取交集（文件夹与其他类型互斥）；`Results/ShowFileSize` 复用 `ResultModel::SizeRole` 不访盘，面板文件夹列由 `FolderSizes` 后台遍历内存索引显示 `≈`。
- **拖拽出窗口（`dragout::drag`）**：`SHCreateDataObject` 构造完整 PIDL 列表并写 `CF_HDROP` 与 `Preferred DropEffect = Copy`；左键只允许复制/快捷方式（防同盘误移），右键允许移动以弹出目标拖放菜单；管理员拖给普通目标不受 UIPI 阻断；网络共享先探测 1.5 s 防卡界面。
- **系统右键菜单（`platform/ShellContextMenu`）**：GUI 队列异步弹出 `IContextMenu3`（`Shift` 带 `CMF_EXTENDEDVERBS`），以独立隐藏窗 `WinShun.MenuOwner` 作为外壳命令对话框属主防止启动器隐藏带走确认框；`shellcontext::watchKeys` 绕过中文 IME `VK_PROCESSKEY`。
- **普通权限打开边界**：管理员模式默认委托 Explorer `IShellDispatch2::ShellExecute` 打开文件/应用（防继承提权），仅 `Ctrl+Shift+Enter` 走 `runas`；无 Explorer 时用桌面外壳令牌启动，绝不静默提权。

### 2.2 钉住（`Ctrl+T`）、长文件名提示与对称调宽

- **钉住**：`Launcher::pinned` 设置顶且失焦/打开结果不收起（`Esc` 仍可收起，不写配置）；失焦时再次触发呼出就地拉回前台不重算位置；从任务栏打开后钉住则把输入区迁回启动器。
- **长文件名提示与调宽**：`Elide.cpp` 保留文件名尾部做中间省略；`NameTip.qml` 为独立无焦点透传置顶窗（防被低窗裁切），`FileFacts` 后台补读创建时间（跳过 UNC）。`WindowFrame::setSideSizing` 在左右 6 逻辑像素返 `HTLEFT`/`HTRIGHT`（不加 `WS_THICKFRAME` 防 Aero Snap），`Placement::beginSizing` 围绕中心对称调宽并存 `state.ini`。

### 2.3 异步缩略图（`ThumbnailProvider`）

- `QQuickAsyncImageProvider`（`image://thumbnail`），独立 4 低优先级线程、LIFO 优先当前视口，排除文件夹、应用、网络/映射盘与云端占位文件（防触发下载）；优先查系统缓存（`SIIGBF_INCACHEONLY`），自有 48 MB 位图缓存 + 2 万条负缓存；大图视图经 `GalleryThumbnailRole` 独立出图。

### 2.4 搜索面板（`Ctrl+E`）、列管理、大图与内容详情

- **同窗切换**：复用窗口与状态；展开加 `WS_THICKFRAME`（`WM_NCCALCSIZE` 返回 0 保持无边框），收回先 DWM cloak 并待 `afterNextFrame` 再 uncloak 消闪烁。
- **筛选、快捷筛选与列**：大小与创建时间常驻 `FileIndex` 平行数组，排序筛选零访盘；`QuickFilter` 多选 AND（组内 `|` 为 OR）经 `Request::conditions` 传引擎不污染输入框；`PanelColumns` 管理 8 列显隐/顺序/宽度（名称列不可隐藏），每页 256 行续载；`Gallery` 共用 `ResultModel`，输入焦点留在搜索框保 IME 正常。
- **内容详情与报告（`ContentDetails` / `ContentReport`）**：`ContentDetails` 将 `DocText` 转为共享只读文本与行偏移，仅在面板内容范围激活，按文件身份+大小+时间缓存（总预算 32 MiB）；5 个子页 `Loader` 懒加载，`Ctrl+Shift+1…5` 切页；内嵌文档预览复用 `PreviewHost`。`ContentReport` 基于不可变快照用独立 `SearchEngine` 后台生成纯文本/CSV（防公式注入）/HTML/最小 OOXML `.xlsx`（miniz，无 Excel COM）及 GDI 打印。

### 2.5 文件预览（`Alt+P`、`FilePreview`、`PreviewHost`）

- **调度**：`PreviewWith=auto` 优先调用运行中的 QuickLook（命名管道）或 Seer（`WM_COPYDATA` + 200 ms 节流），否则启内置无焦点工具窗 `PreviewWindow.qml`（`PreviewPlacement.h` 优先左右空侧）。读取走 `ComWorker` 通道池（单文件阻塞 >2 s 切新线程，最多 4 条）。
- **隔离 STA 预览宿主（`platform/PreviewHost`）**：严格以 `CLSCTX_LOCAL_SERVER` 进程外加载系统 `IPreviewHandler`（防崩溃拖垮主进程），独占 STA 线程建无父无属主 `WS_EX_NOACTIVATE` 弹出浮层叠在预览区上；GUI 仅异步操作；10 s 卡死则丢弃线程并本轮拉黑该处理器；对 `Zone.Identifier` ZoneId 3/4 网传文件拒交 `IPreviewHandler`。

## 3. 收藏夹、自定义命令、网页搜索与设置

- **收藏夹与列头排序（`Favorites`）**：独立存于 `favorites.json`（原子写盘，损坏留 `.bad`，高版本只读；v1 普通 / v2 含命令 / v3 含自定义图标），启动器、对话框与拖放圆盘共用。关键词优先级：**网页 > 收藏 > 插件**。设置页四列排序前自动保存 `manualOrder` 供恢复默认；向管理员设置窗拖入走 `WM_DROPFILES`（`FileDropTarget`）。
- **自定义命令与图标**：`Favorite::Kind::Command` 先展开环境变量再将用户输入作为单一参数替换 `{query}`（不经 cmd/PowerShell，搜索与导入绝不执行）。图标存 ≤128×128 PNG data URL，解码与提取在 `QThreadPool` 异步执行。
- **网页搜索（`winshun.web`）**：官方零代码宿主插件（`webSearch: true`，配置存 `[Plugin.winshun.web]`），拒绝 `file:` 协议。站点图标（`SiteIcons`）后台经 WinHTTP 拉取，严禁 WIC 解码（仅 Qt PNG、手写边界校验 ICO 与 `SvgIcon`——限 64 KB、拦外部引用、禁动画），按对比度自动加反色底板；搜索建议（`WebSuggester`）150 ms 防抖、1.5 s 超时、2 分钟缓存。
- **首次引导（`OnboardingWindow`）**：`Settings::load` 先读 `Setup/Completed`，未完成时 `App::start` 仅创建两页纯草稿引导窗，**不初始化**索引/剪贴板/插件；`Settings::saveChecked` 落盘后才进入 `startRuntime`。
- **设置搜索与开机自启**：`SettingsSearch` 直接匹配各 `SettingRow` 绑定文本，无静态清单维护负担；内置功能卡片（`BuiltinCard.qml`）统一开关可选内置模块。开机自启统一用登录计划任务“WinShun”（普通模式 LUA，管理员直读才用最高权限）。

## 4. 文件对话框集成

### 4.1 无注入原则、线程与权限边界

- **无 DLL 注入**：完全依靠 WinEvent、窗口消息、COM/Shell 与 UIA 驱动外部对话框。
- **钩子与后台线程**：`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 留在 GUI 线程（拖动/缩放期间隐藏 `DialogBar` 防拖慢）；跨进程查询与对话框操控均在独占 STA `ComWorker` 顺序执行（`DialogJump` 1 条，`DialogBar` 2 条——建议与路径补全隔离）。
- **UIPI 边界**：普通权限遇到管理员对话框时不发跳转消息、不注册 `Ctrl+G`、不自动跳转、不调整大小（无索引服务助手时仅提供搜索与复制路径；有服务助手时经 `DialogNav` 代理）。

### 4.2 `DialogJump`、`FileDialog` 与 `DialogBar`

- **`DialogJump`**：仅在同权限受支持对话框在前台时按需 `RegisterHotKey(Ctrl+G)`；自动跳转（默认关）跟踪“文件管理器 → 对话框”切换并防重复跳回，隐藏复用型对话框（如 Qt/WPS）时重置已见状态；WPS 冷启动 UIA 探测去重且未就绪不永久缓存，按需单次重试。
- **`FileDialog`**：支持现代 `#32770` 地址栏（无地址栏回退文件名框）、XP 传统对话框、`SHBrowseForFolder`（`VirtualAllocEx` + `BFFM_SETSELECTIONW`）、WPS `KcfdFileDialog`（UIA）、Inno `TSelectFolderForm`（仅写框不回车）、AutoCAD/AutoCAD LT、Bandizip/7-Zip/WinRAR 路径框、用户自定义规则（`dialogs.json`）及 Blender 官方扩展命名管道。Tessoa 接管后的自绘打开/另存为窗口由 `isTessoaPicker` 认出但不属任何 `kind`（无法驱动）。完整兼容清单见 [dialog-compatibility.md](../dialog-compatibility.md)。
- **`DialogBar`**：无焦点置顶工具窗，**不挂外部对话框为 owner**（防合并输入队列），通过 `win::followDialog` 维持 z 序；`roomAlong` 优先下方、不足放左右，必要时异步调整对话框尺寸腾位（每框仅一次）；支持 `Ctrl+Shift+D` 仅搜当前目录与路径实时补全。转不过去的窗口（无服务的管理员对话框、Tessoa 自己的窗口）进入只复制模式：`copyOnlyHint` 决定提示语，选中结果只复制路径；`App::adminDialogFront` / `App::tessoaPickerFront` 负责日志、诊断记录与一次性通知。

### 4.3 `FileManagers` 支持矩阵

`Ctrl+G`、自动跳转与建议按 Z 序枚举 6 类文件管理器，打开/定位统一以**普通权限**执行：

| 文件管理器 | 读取当前文件夹 | 打开 / 定位 | 关键边界 |
|---|---|---|---|
| **资源管理器** | `IShellWindows` + `IShellBrowser` 取 `SIGDN_FILESYSPATH` | `IShellDispatch2` / `SHOpenFolderAndSelectItems` / `Navigate2` | 跳过主页、此电脑、搜索等虚拟视图 |
| **Total Commander** | `WM_USER+50` 查焦点栏与路径栏文本 | `/O /T /S /L="路径"`（定位加 `/P`） | 仅读左右两栏当前视图 |
| **Directory Opus** | 普通权执行 `dopusrt.exe /info <xml>,paths` | `dopusrt /acmd Go … NEWTAB=findexisting,tofront` | 无窗口时不查 |
| **XYplorer** | 串行 `WM_COPYDATA` 发 `::` 脚本收全标签；无应答回退地址栏 `Edit`（含常见特殊目录名），提权/隐藏地址栏回退窗口标题 | `seltab` / `selectitems` / `focus 'XY'` | 匹配 `XYplorer*.exe`（含 Free/64 位/便携，排除 `XYcopy.exe`）；管理员主程序需 `ChangeWindowMessageFilterEx` |
| **Pulse** | `FILE_SHARE_DELETE` 读 `session.json` 按位置匹配 | `pulse.exe <路径>` | 会话文件每 ~3 s 刷盘 |
| **OneCommander** | `ComWorker` UIA 读 `CurrentPathGet` + `Tabs*.json` | `-o <路径> -newtab` / UIA `CurrentPathSet` | 不碰剪贴板与焦点 |

## 5. 资源管理器与任务栏集成

- **直接打字搜索（`TypeToSearch`）**：高优先级、关闭省电节流的单低级键盘钩子线程，仅在前台为受支持窗口时挂钩，同步轻判断、暂存按键并在启动器获焦后带 `kOwnInput` 按序重放（保 IME 组字）。覆盖：
  - **开始菜单**（`SearchHost` 等，发 `Esc` 关闭开始菜单）；
  - **资源管理器**（焦点为 `SHELLDLL_DefView\DirectUIHWND` 且非菜单；`FolderTyping` 支持 `first`/`only`/`level`，`Ctrl+Shift+D` 轮换；打开文件夹原标签页 `Navigate2` 跳转）；
  - **Directory Opus**（焦点 `dopus.filedisplay`，仅拦字母与无 Shift 数字）；
  - **OneCommander**（钩子初筛 + `ComWorker` UIA 确认 `AutomationId="flb"`，2 s 焦点缓存防侧栏打字卡顿）。
- **文件夹菜单（`FolderMenu`）**：空白处左键双击 / 中键（`FolderViewHit`）或全局热键（`FolderMenuHotkey`）触发，无焦点弹窗，按键由 `KeyRouter(Policy::Menu)` 路由。
- **拖放圆盘（`DropPie` / `DropPieHelper`）**：按住文件或拖放途中按扫描码 `0x29` 键触发，每页 8 个文件夹收藏，`Tab` 翻页（动态热键）；管理员主进程自动交由普通权限辅助进程 `DropPieHelper` 运行。`PieDrop` 替换 Qt `IDropTarget`（不用 `IDropTargetHelper`），优先读 `CFSTR_SHELLIDLIST` 防压缩软件同步解压阻塞；磁盘文件走独立 STA `IFileOperation`（支持撤销，返 `DROPEFFECT_NONE` 防源误删），虚拟项转交目标文件夹原生 `IDropTarget`。
- **粘贴为文件（`PasteAsFile`）**：普通权限 `WinShunSearch.exe` 实现，提供右键背景菜单与 `TypeToSearch` 资源管理器/桌面 `Ctrl+V` 拦截（仅 `IsClipboardFormatAvailable` 判断有文本/图片且无文件），自动识别类型与编码落盘并进入 `SVSI_EDIT` 改名。
- **任务栏集成（`TaskbarSearchBox` & `WinShunSearch.exe`）**：
  - `TaskbarSearchBox` 为 `Shell_TrayWnd` 的 Direct2D 分层子窗（真实输入与 IME 仍在启动器，同步绘制与光标定位），固定停靠在任务栏空端、三档自适应（完整框 / 图标+标签 / 仅图标），兼容 StartAllBack / ExplorerPatcher；采用独立窗口线程、MTA UIA 测量线程（事件驱动防抖）、`DwmFlush` 节拍线程与主线程四线程隔离。
  - `WinShunSearch.exe` 为无 Qt 普通权限固定按钮进程，经中完整性命名事件切换启动器；`Win+S` 接管期间代注册 `Win+Shift+S` 并以普通权限拉起 `ms-screenclip:///`。

## 6. 相关测试与预览入口

- **单元测试**：`tst_displaygeometry`、`tst_filedialog`、`tst_dialogrules`、`tst_pathbox`、`tst_dialognav`、`tst_piegeometry`、`tst_pielabels`、`tst_pastefile`、`tst_foldermenu`、`tst_searchtyping`、`tst_taskbarroom`、`tst_favorites`、`tst_websearch`、`tst_websuggest`、`tst_svgicon`、`tst_contentdetails`、`tst_shelllaunch`、`tst_settings`。
- **离屏预览与探针**：`WinShun.exe --preview-settings`、`--preview-results`（含 `--file-gallery`、`--content-pane`、`--drop-pie-pages`、`--style-buttons`、`--search-logo`、`--dialog-rule-settings`）、`--probe-runtime`（含 `--onboarding-probe`、`--settings-motion-check`），均应带 `--renderer d3d11`。
