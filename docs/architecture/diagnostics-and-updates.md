# 运行诊断、配置、更新与验证入口

本文汇总 Win顺 当前实现中的配置与数据目录布局、运行诊断与隐私边界、应用内更新链路、命令行与验证/基准入口，以及参考性能基线。发布操作流程见 [releasing.md](../releasing.md)，常见排障见 [pitfalls/installer-build-and-testing.md](../pitfalls/installer-build-and-testing.md)。

## 1. 配置、数据目录与便携模式

### 1.1 目录模式与文件布局

Win顺 在读取任何设置之前按以下优先级确定目录：

1. **`--profile <目录>`**：隔离模式（用于截图脚本、测试与探针），配置存 `<目录>\WinShun\WinShun.ini`，其余数据直接存 `<目录>`；重启时自动透传绝对路径。
2. **便携模式**：未指定 `--profile` 且 `WinShun.exe` 同级存在 `data\` 目录时，`Settings::portableFolder()` 将其作为 profile（设置位于 `data\WinShun\WinShun.ini`）。发布脚本为便携 ZIP 预置 `data\README.txt`，安装包不含该目录。
3. **标准安装模式**：配置存 `%APPDATA%\WinShun\WinShun.ini`，运行数据存 `%LOCALAPPDATA%\WinShun\`。

| 文件 / 子目录 | 默认位置（便携/`--profile` 均在数据目录内） | 职责与说明 |
|---|---|---|
| `WinShun.ini` | `%APPDATA%\WinShun\` | 全部用户设置，支持手改即时生效；缺失键补写默认值 |
| `state.ini` | `%LOCALAPPDATA%\WinShun\` | 窗口几何与易失状态（`Placement`、`SavedWindowSize`、上次范围/预览展开等） |
| `favorites.json` | `%LOCALAPPDATA%\WinShun\` | 收藏夹（启动器与对话框共用）；损坏时旁存 `favorites.json.bad` |
| `dialogs.json` | `%LOCALAPPDATA%\WinShun\` | 自绘文件对话框规则（`DialogRuleStore`），监听变更热重载 |
| `update.ini` | `%LOCALAPPDATA%\WinShun\` | 更新检查时间、跳过版本、`RolloutSlot`、`PendingVersion`、`Usage/LastReport` |
| `index.bin` 与 `content\` | 默认在数据目录，可由 `[Index] Folder` 迁移 | 文件名索引快照、`content\*.grams`（内容索引段）与 `content\*.texts`（文档正文） |
| `clipboard\` | `%LOCALAPPDATA%\WinShun\clipboard\` | `clipboard.db`、`images\`、`projects\`、OCR 崩溃哨兵文件 `reading-picture`；手动备份为单个 ZIP（`ClipBackupFiles`），还原时先解到旁边的 `clipboard.restore\`，下次启动在打开历史前整体换入 |
| `WinShun.log` / `.old.log` | `%LOCALAPPDATA%\WinShun\` | 运行警告/错误日志（`WINSHUN_LOG_INFO` 开 info 级）；`RepeatFilter` 折叠 10 秒内超 3 次重复消息 |
| `diagnostics\` | `%LOCALAPPDATA%\WinShun\diagnostics\` | `events*.jsonl`、`crash.txt` 与固定导出包 `WinShun-diagnostics.zip` |
| `Updates\` | `%LOCALAPPDATA%\WinShun\Updates\` | 下载暂存 `*.partial`、增量展开目录、更新助手副本与 `update.log` |

### 1.2 `WinShun.ini` 节区总览

具体键名、类型、默认值与迁移逻辑均以 **`src/app/Settings.h`** 与 **`src/app/Settings.cpp`** 为唯一事实来源：

| INI 节区 | 管理内容 |
|---|---|
| `[Setup]` | 首次启动引导完成状态与待安装项标记 |
| `[Launcher]` | 呼出方式（双击修饰键/热键/游戏与全屏暂停/排除应用）、渲染器（`d3d11`/`software`）、文件管理器、文件对话框跳转/搜索框、文件夹打字与空白处菜单 |
| `[Results]` / `[Panel]` | 排序与起始列表、优先文件类型、导航（数字键/字母直达/打开热键）、预览、默认终端、搜索面板列与空输入行为 |
| `[Favorites]` | 拖放圆盘（`DropPie`）开关与热键占用提示标记 |
| `[FileTypes]` / `[QuickFilters]` | 文件类型筛选条与搜索面板侧栏自定义快捷筛选 |
| `[Appearance]` | 主题、界面风格（`fluent`/`brand`/`tactile`/`liquid`）、圆角、毛玻璃、语言、界面大小、托盘图标风格、自定义字体、最大行数 |
| `[Proxy]` / `[Update]` / `[Backup]` | WinHTTP 代理；自动检查更新与匿名用户统计开关；配置备份目录 |
| `[Index]` | 排除路径/名称/类型/模式、盘符与可移动盘、索引方式（`Access`）与范围（`Scope`/`Folders`）、自定义索引位置（`Folder`） |
| `[Content]` | 内容搜索与文档提取开关、纯文本扩展名、四类文件大小上限、内容索引开关 |
| `[Clipboard]` | 历史开关、Win+V/自定义/纯文本粘贴热键、`PlainByDefault=false`（默认关：开后点击/双击/`Enter`/数字键/快速粘贴键粘贴文字去掉格式，`Shift` 反向，`Clipboard::pastesPlain` 唯一判定）、保留策略、图片/截图收录策略、排除应用、布局与交互 |
| `[Taskbar]` | Win+S 接管、开始菜单打字、任务栏搜索框样式与位置、托盘左右/中键动作 |
| `[Shares]` / `[Network]` | 网络共享路径数组；核对周期、并发数与 NAS/macOS 专用排除项 |
| `[Mcp]` | AI MCP 与命令行总开关、打开/剪贴板/截图/插件/改设置分级权限、AI 排除目录 |
| `[Plugins]` / `[Plugin.<id>]` | 开发者模式、常驻上限；各插件启用状态、已确认权限、自定义关键字、AI 暴露与设置项（`password` 类只存 Windows 凭据管理器，不写 INI） |
| `[WebSearch]` / `[Capture]` / `[PasteFile]` | 旧版网页搜索兼容键（现存 `[Plugin.winshun.web]`）；截图历史关键词 `jt`（插件配置在 `capture.ini`）；粘贴为文件开关 |

- **自定义索引位置（`[Index] Folder`）**：`indexfolder::onExternalDisk` 仅接受内置硬盘（排除可移动盘、USB/SD/MMC/1394 总线及支持热插拔的外置控制器）；`IndexService::moveTo` 后台校验空间、复制刷盘、原子切换并清理旧文件，目标卷离线时临时回退默认目录。机械盘上的内容索引经 `IndexService::prewarmSoon` 以 `IoPriorityHintLow` 限速预读，内容查询先 `pinIds` 再释放文件索引读锁。
- **便携版迁移与单实例锁顺序（`src/app/main.cpp`）**：先解析路径与 `storedRenderer()` → 获取单实例互斥量 **`Local\WinShun.Instance`**（已存在则 `WM_COPYDATA` 转发并退出）→ **拿到锁之后**才执行 `copyIntoPortable()`（首次启动从 `%APPDATA%`/`%LOCALAPPDATA%` 复制配置与数据，跳过日志与 Qt 缓存）与 `Backup::applyRestored()`。绝不可将 `copyIntoPortable()` 移到单实例锁之前，否则会与运行中的安装版争抢打开文件。

## 2. 运行诊断与隐私边界

### 2.1 `DiagnosticLog` 轮换与导出白名单

`ws::diagnostics::Log`（`src/core/DiagnosticLog.*`）仅由主实例初始化，写入 `<数据目录>\diagnostics\` 下的 UTF-8 JSONL：

- **容量预算**：固定 3 份 `events.jsonl` / `events.1.jsonl` / `events.2.jsonl`，单文件 ≤ **1 MiB**，单条 ≤ **8 KiB**（超长自动截断并保持合法 JSON）。写入前检查轮换，轮换失败放弃写入；写入与 `snapshot()` 共锁无交错。初始化、写入与导出时清理超过 **7 天**未更新的这 3 份日志及固定诊断包，不触碰其他文件。
- **后台导出**：设置 › 常规导出按钮在后台 `std::jthread` 调用 `Diagnostics::exportArchive`（`src/app/Diagnostics.*`），通过 `QSaveFile` 原子替换固定路径 `WinShun-diagnostics.zip`。

| 压缩包路径 | 来源与裁剪预算 |
|---|---|
| `events*.jsonl` | 主实例 `DiagnosticLog::snapshot()`（每份 ≤1 MiB） |
| `app/WinShun.log` / `.old.log` | 各取末尾 **1 MiB** |
| `app/plugins.log` | 取末尾 **256 KiB**（插件输出可能含路径） |
| `capture/events*.jsonl` | `plugins/winshun.capture.data/diagnostics/`（每份 ≤1 MiB） |
| `service/service.log` / `.old.log` | 索引服务目录，各取末尾 **1 MiB**（不可读时列于 `unavailable_logs`） |
| `summary.json` | 版本/OS/权限、`plugins`（仅 id/version/enabled）、`display`、索引与磁盘状态；`memory`（导出时的“运行状况”分块：各进程、主进程按用途（含归还时留下的 `kept`）和按页类型，以及句柄、GDI/USER 对象、峰值与运行时长，`HealthMonitor::snapshot`）、`idle_trim`（内存没归还的原因：唤醒后保留、后台在忙、有窗口在用，及上次归还） |

> **隐私底线（永不记录、永不入包）**：`WinShun.ini`、系统凭据、剪贴板正文/图片载荷/哈希、文件索引原件、文档正文、搜索词与历史、MCP/插件对话、窗口标题、OCR 文字、原始按键内容、屏幕事件中的显示器名称。

### 2.2 显示渲染、卡顿与崩溃保护

- **显示自检（`DisplayDiagnostics` / `TextCheck` / `DisplaySetup`）**：记录 `ui-font`、`screens`、`graphics`/`graphics-error`（含 `capture-overlay-<n>`）、`power`、`fonts-changed`（2 秒节流）与 `font-database-changed`。`TextCheck` 每 10 分钟最多自检一次最近使用的 6 个高亮字体（对固定样例 `textcheck::sample()` 按横坐标比对字形编号）。若自检正常但屏幕错字，故障在渲染线程字形纹理缓存层。导出时 GUI 线程采集 `display`，后台 `platform/DisplaySetup` 补充 `monitors`、`graphics_settings`、`driver_settings` 与近 14 天 `display_events`（含 Display 4101 TDR）。
- **`UiStallWatch` 无锁取栈**：主程序每 2 秒探测（空闲 1 分钟暂停、排除睡眠），2 秒未应答立即写 `ui-stall`（≤48 帧，仅 x64）；截图插件在遮罩期间每 100 ms 探测、300 ms 触发，首次采样同步抓其余线程去重调用栈分段写入 `ui-stall-threads`。仅对 Windows 系统目录模块附加 4 KB 内最近导出函数名。**硬约束**：暂停目标线程期间仅拷贝 ≤256 KB 栈顶即刻放行，离线回溯，暂停期间绝不分配内存、查表或写日志。
- **`GraphicsLoss` 与 `DisplayPulse`**：截图遮罩期间统计 `sceneGraphInvalidated`（`graphics_lost`）与光标屏 `D3DKMTWaitForVerticalBlankEvent` / `DwmFlush` 最大间隔；`DisplayPulse` 析构不 `join`（防驱动等待阻塞退出）。
- **`CrashRecord`（`src/app/platform/CrashRecord.*`）**：拦截 `abort()`/`std::terminate` 与未处理 SEH，**仅用栈缓冲与 Win32 API** 写 `diagnostics\crash.txt`（原因、异常、线程、`activity`、≤64 帧），下次启动转为 `app-crash` 并删除。`qFatal` 直接 `__fastfail` 不经 `abort()`，记录在 `WinShun.log`。
- **剪贴板 OCR 崩溃守卫（`Clipboard::takeCrashedReading`）**：读图前写 `clipboard\reading-picture`，完后删；重启残留时首次后置重试，连续两次永久跳过并记 `clipboard-picture-not-read`。

### 2.3 诊断事件速查表

| 事件名 | 来源 | 核心内容与频控 |
|---|---|---|
| `app-start` / `app-crash` | `main.cpp` / `CrashRecord` | 启动环境与权限；上次闪退原因/线程/`activity`/调用栈 |
| `app-message-repeated` | `RepeatFilter` | 10 秒内重复超 3 次的 `WinShun.log` 折叠计数 |
| `content-*` | `ContentIndexer` | 轮次起止、限速、直读/回退、30 秒吞吐、最久 8 项活动槽、>3 秒慢读（每轮前 8 次） |
| `ui-font` / `screens` / `graphics` / `graphics-error` / `power` / `fonts-changed` / `font-database-changed` / `text-check` | `DisplayDiagnostics` / `TextCheck` | 字体、屏幕参数、显卡驱动、场景图错误、睡眠与字形自检 |
| `capture` / `capture-stall` / `capture-cursor-hidden` | `Capture` | 截图耗时、结果、GUI 阻塞 >150 ms（每次 ≤5 条）、`graphics_lost`、脉冲间隔 |
| `ui-stall` / `ui-stall-end` / `ui-stall-threads` | `UiStallWatch` | 界面无响应调用栈、CPU 耗时、`busy` 及截图时其余线程栈 |
| `trigger` | `TriggerJournal` | 呼出全链路（`source`/`queue_ms`/`action`/`front`/`seen_ms`/`faults`/`trimmed`/`helper`/`raw_input`/`tap_from`/`queued_ms`）；显示分段 `show_ms`（界面线程同步准备）/`arm_ms`（到 `FrameReveal::arm`）/`frame_ms`（`arm` 到揭开遮罩），200 ms 兜底揭开带 `timeout: true`；`idle_s` 距上次显示或隐藏，`private_mb`/`working_mb`/`ws_delta_mb` 为按下时内存与到显示时的工作集增量；显示后落盘，异常/慢/超时 ≤30/h，正常前 5 条后每 10 分钟 1 条 |
| `idle-trim` / `key-helper` | `IdleTrim` / `ServiceKeyListener` | 空闲内存交还（归还结束后一条：归还前 `private_mb`、保留热页 `keep_mb`（`HotPages`）、归还后仍驻留 `stayed_mb`，中途被唤醒为 `stopped`）/`held: resumed`；UIAccess 按键助手状态 |
| `double-tap-near-miss` / `double-tap-dropped` | `KeyListener` / `TapTeller` | 双击未成立原因与干扰类别、延迟/让位丢弃（合计 ≤30/h，不记具体键） |
| `clipboard-picture-not-read` / `clipboard-key-routing-*` | `Clipboard` / `KeyRouter` | OCR 连崩跳图尺寸；剪贴板按键转发与未接管原因统计 |
| `clipboard-hidden` | `App::hideClipboard` | 剪贴板面板每次隐藏的原因 `why`（`asked`/`inactive`/`clicked-away`/`routed-front-moved`/`lent-front-moved`/`display-lost-focus`）、显示了多久 `shown_ms`、是否转发按键、打开时前台是否全屏/独占全屏、目标窗口是否已最小化或消失；不记窗口标题与程序名 |
| `clipboard-kept` | `App::routedForeground` | 面板盖住全屏游戏、游戏因此退出全屏并最小化时面板不收起而接过焦点（#452）：`shown_ms`、是否独占全屏 `over_exclusive`、是否拿到前台 `front`（拿不到则照旧收起，随后有 `clipboard-hidden`） |

## 3. 检查更新、增量包与发布链路

完整发布步骤、插件库同步与实测门禁见 [docs/releasing.md](../releasing.md)。

- **检查与推送（`Updater` / `core/Release`）**：WinHTTP 请求 `https://lcstp.com/updates/winshun/stable.json`（每 10 分钟，支持 `ETag` / `304`；便携版加 `&package=portable`；`WINSHUN_UPDATE_FEED` 仅允许本机 HTTP 测试）。推送模式由发布说明隐藏标记控制：`<!-- winshun-push now -->` 为立即推送（`Push::Now`，每日提醒）；默认为正常推送（`Push::Normal`，按 `update.ini` 固定随机槽 `RolloutSlot` 在 24 小时内分批提醒，最多每 3 天一次）。
- **信任链与防篡改（`UpdatePackage` / `HeldFile`）**：无 Authenticode 签名，信任链建立在 **HTTPS 直连 `lcstp.com` + SHA-256 + PE 版本资源匹配**。优先镜像源再原站，单步 30 秒超时、低速切源、上限 400 MB；执行前由 `HeldFile` 禁止写删锁定句柄并复算 SHA-256 至安装/解包结束。
- **增量更新（`UpdateDelta` / `tools/delta`）**：发布脚本对最近至多 3 个兼容旧版用 zstd level-19 `--patch-from` 生成增量包（<60% 全量体积且经完整还原逐文件校验），清单通过隐藏注释 `<!-- winshun-delta 1 ... -->` 随同一可信链下发；客户端逐文件验旧哈希与新哈希，任一失败静默回退全量包（`WINSHUN_UPDATE_NODELTA` 可关闭增量）。
- **按安装形态执行**：
  - **安装版（`AllUsers` / `CurrentUser`）**：普通权限启动 `setup.exe /SILENT …`（`AllUsers` 由 Inno Setup 自弹 UAC 保持 `[Run]` 普通权限），伴随 `WinShunUpdate.exe --watch-setup` 兜底重启旧版；安装脚本先将 `WinShunMcp.exe` 改名再退中转防锁 DLL，被占用文件改名 `.wsold` 挪开。
  - **便携版（`Portable`）**：严格路径/重解析点校验解压至同卷 `.winshun-update-new`，复制 `WinShunUpdate.exe` 至 `Updates\` 持 `Local\WinShun.Updating` 交换顶层项（不动 `data`），3 分钟内未见新版窗口则自动回滚。
  - **Scoop（`Scoop`）**：高于便携版优先识别，经 `current` 联接稳定路径，以 `-EncodedCommand` 调 `scoop update winshun`；`pre_uninstall` 仅在真卸载时跑 `--uninstall-cleanup`。
- **提权交接（`ElevationWait`）**：管理员直读更新后先等 `Local\WinShun.Updating` 释放与窗口关闭（上限 60 秒，否则发通知询问）再以管理员重启；更新前写 `PendingVersion`，下次启动核对并清理残留包。
- **匿名统计（`core/UsageReport`）**：每日至多上报一次 `SHA-256("gleaning.install.v1:winshun:" + MachineGuid)` + 版本/OS，预览模式不上报。

## 4. 命令行参数与常用验证/基准入口

### 4.1 `WinShun.exe` 参数

- 常规：`--background`（托盘静默启动）、`--toggle`、`--query <文字>`、`--settings`、`--quit`。
- 隐藏/内部：`--settings-at <页,标签|indexAccess>`（转发给已运行实例经 `SettingsCommand.h` 编为 `settings:<位置>`）、`--profile <目录>`、`--take-autostart` / `--had-autostart`、`--uninstall-cleanup`、`--startup-unelevated`、`--dump <文件>`。

### 4.2 离屏预览与基准命令

所有 `--preview-*` / `--probe-runtime` 均使用临时配置，不触碰用户运行中的 Win顺（退出码通常为 QML 警告数）：

```powershell
# 离屏 UI 预览与生产启动探针
build\release\WinShun.exe --preview-settings build\<task>\settings --dark --scale 1.5 --renderer d3d11
build\release\WinShun.exe --preview-plugins build\<task>\plugins.png --dark --scale 1.5 --dialog
build\release\WinShun.exe --preview-results build\<task>\results --dark --scale 1.5 --renderer d3d11
build\release\WinShun.exe --probe-runtime build\<task>\runtime --with-clipboard

# 按需单元测试（只跑本次改动涉及的套件）
ctest --test-dir build\release --output-on-failure -R '<regex>'

# wsbench 基准与子系统探针
build\release\wsbench.exe                                          # 索引加载与搜索耗时
build\release\wsbench.exe --plugin build\release\plugins\winshun.kill --query "kill 记事本" --action kill
build\release\wsbench.exe --extract --sandbox --threads 8          # 文档提取基准
build\release\wsbench.exe --mcp-host --plugins build\release\plugins

# ASan 与模糊测试（VS 开发者命令行，覆盖 MFT/USN/编码/文档解析）
cmake --preset asan; cmake --build --preset asan; ctest --preset asan
build\asan\wsfuzz.exe -max_total_time=300
```

## 5. 参考性能基线

> 基线条件：i9-14900HX、4 本地盘、MSVC 2022 Release / Qt 6.12、约 316–400 万条索引；普通电脑（50–100 万文件）文件名索引约占 15–30 MB。

| 指标 | 参考基线 | 关键条件 |
|---|---|---|
| 首次全盘建索引 | **~2.6 s**（4 盘并行 MFT） | 含 4.4 GB C 盘 MFT，不含落盘 |
| 启动加载 / 保存快照 | 加载 **~0.15 s** + USN 数十 ms；保存 **~0.26 s** | ~329 万项，快照 ~27 MB，30 分钟定期保存 |
| 文件名搜索 | 常规 **2–7 ms**；单字母 **11–14 ms**；5 万项大列表 **~43 ms** | 400 万项快照，16 线程；已取消路径排序 <0.01 ms |
| 内容搜索 / 内容索引 | 命中索引 **1–10 ms**；直读建索引（开 Defender）**27–34 s** | ~34 万文本 + ~5,800 文档；索引段 75 MB + 文本 17 MB |
| 文档文本提取 | 8 进程 **12–16 s**（5,800+ 份）；PDF 均 ~45 ms | 含副本去重与 `WinShunExtract` 段堆优化 |
| 窗口呼出延迟 | 热启 **~35 ms**；首次 **~60 ms** | 启动阶段预渲染 |
| 内存占用 | 条目元数据 **25 B/项**；329 万项索引 ~110 MB；GUI 常驻 **~120–140 MB** | `FileIconProvider` 像素预算 8 MiB |
