# 插件系统、AI 助手（MCP）与命令行架构

本文档说明 Win顺 当前插件运行时、官方插件生态、AI 助手（MCP）服务端与命令行工具 `ws` 的架构、进程与安全边界。插件清单字段与 JSON-RPC 协议规范见 [plugins.md](../plugins.md)，命令行规范见 [design-cli.md](../design-cli.md)。

## 1. 宿主与插件边界

### 核心能力与进程模型

文件搜索、内容搜索与剪贴板管理属于宿主核心（`WinShun.exe`）；截图与录屏、系统开关、网页搜索、图片文字搜索、语义搜索、开发工具、翻译、浏览器书签、局域网传输、剪贴板同步、云备份、休息提醒、鼠标手势、缩写展开、窗口置顶与资源管理器标签页均作为可选插件运行。文件夹菜单、文件对话框集成与拖放圆盘由宿主实现。剪贴板在原生 Start/Search 上叠加显示时，可使用独立 UIAccess 显示子进程（`WinShun.exe --clipboard-display`），历史库、监听与插件调用仍在普通主进程。

插件遵循“三层形态、一份协议”，且**绝不将插件代码载入 `WinShun.exe` 主进程**：

- **零代码插件**：无 `runtime`，在宿主进程内由 `ZeroCode` 或宿主能力标记（如 `winshun.web` 的 `webSearch: true`）直接求值。
- **一次性进程插件（`runtime.mode = once`）**：每次查询拉起独立进程，经 `WS_*` 环境变量传参、从 stdout 读结果。
- **常驻进程插件（`runtime.mode = resident`）**：经 stdio 承载按行 JSON-RPC 2.0 长连接，支持流式结果（`query/partial`）、动作执行与反向工具调用（`winshun/*`）。

### 沙箱与隔离机制

宿主统一由无 Qt、静态 CRT 的托管进程 `WinShunPluginHost.exe`（runner）派生并隔离插件：

- **令牌降权与继承控制**：普通权限宿主以 `CreateProcessW` 启 runner；管理员宿主优先取资源管理器非提权令牌（硬检查 `TokenIsElevated == 0` 且完整性低于高），异常时回退自身令牌经 `CreateRestrictedToken` 降至中完整性再 `CreateProcessAsUserW`。
- **作业与进程缓解**：runner 挂在 `KILL_ON_JOB_CLOSE` 主作业中随宿主退出，启用扩展点禁用与镜像加载缓解。每个插件以 `CREATE_SUSPENDED` + `STARTUPINFOEX`（仅继承三个标准句柄；不启 `PROHIBIT_DYNAMIC_CODE` 以兼容解释器 JIT）启动，挂入嵌套作业后再恢复：内存上限受清单 `memoryMB` 约束；未声明 `processes` 强制 `ActiveProcessLimit = 1`；未声明剪贴板权限施加剪贴板 UI 限制；未声明 `ui` 施加桌面等 UI 限制。
- **环境与 `strict` 沙箱**：子进程环境由 `CreateEnvironmentBlock`（基于 runner 令牌）叠加 `WS_*` 生成。声明 `sandbox: strict` 的插件降至低完整性运行，宿主通过 `labelLow` 为 `<id>.data` 写入可继承低完整性标签 `S:(ML;OICI;NW;;;LW)`，仅允许写自有数据目录。

### 提权插件（`elevated`）安全边界

面向需操作提权窗口的插件（如 `winshun.explorer-tabs`），遵循**“插件跟着主程序走”**：仅当宿主本身以管理员身份运行且用户显式确认后才提权，普通宿主下自动降回普通权限，**永不为插件弹 UAC**。`Manifest::parse` 限定 `elevated` 仅允许 `exe` 且禁止与 `strict` 并存。

- **双 runner 隔离**：宿主按需创建管理员 `Runner`（实例号自 `2^48` 起），管道 DACL 仅允许 SYSTEM/Administrators 且高完整性标签，`Host::connectRunner` 核对 `runnerFor(instance) == runner`。
- **防偷换受保护副本（`Elevation`）**：管理员插件绝不直接从用户可写目录启动，而从 `%ProgramFiles%\WinShun Elevated Plugins\<SID>\<id>-<路径哈希>\<树哈希>\` 的已确认副本启动（拒绝重解析点与硬链接，经 `svcloc::writableByNonAdmin` 校验且不自动修复异常目录），副本旁保存逐文件 SHA-256 的 `record.json`。
- **TOCTOU 防护与降级**：每次启动前比对原目录与 `record.json` 及 `Manifest ==`，校验通过至进程创建期间由 `elevation::Hold`（无 `FILE_SHARE_DELETE`）锁定目录句柄防替换；任一不符自动降级至普通 runner。
- **受限提权环境与 AI 隔离**：管理员 runner 使用 `sandbox::elevatedEnvironment` 剔除代码注入环境变量（`COR_*`、`NODE_OPTIONS`、`PYTHON*` 等），过滤可写 `PATH`，禁用 `WS_COMPONENTS_DIR`。宿主提权时 `elevated` 插件对 `AiBridge` 隐藏且 `Host::callCommand` 拒绝，防止普通权 MCP/`ws` 驱动管理员插件。

### 权限确认、动作策略与共享 Qt

- **权限确认与升级挂起**：启用前经 `PluginDialog` 逐项确认，记入 `Confirmed=`；升级新增权限时 `Catalog::setConfirmed` 将插件置入 `awaitingConsent` 直至重新授权。
- **声明式动作与通知策略**：`OpenPolicy` 对未声明 `processes` 的插件仅放行 `http`/`https`/`mailto` 链接及绝对路径的现有目录/非危险文件，永不以管理员身份运行插件结果；`ToastPolicy` 强制通知标题带插件名前缀，点击动作仅限白名单行为。
- **共享宿主 Qt 模式**：带 Qt Quick 界面的官方插件拆为“无 Qt 启动器 `*.exe` + 业务 DLL（`*App.dll` / `*Editor.dll`）”，经 `WS_APP_DIR` 加载宿主 Qt/PDFium/ONNX/OCR，QML 编译为 `--only-bytecode` 并由 `minWinShun` 锁定兼容版本。
- **随包 vs 插件库**：随包插件（`bundled_plugins`，`<安装目录>\plugins`，默认停用，含 `go`/`kill`/`tools`/`switches`）；插件库插件（`library_plugins`，发布至 `LingCore/WinShun-plugins`，用户安装至 `%LOCALAPPDATA%\WinShun\plugins`，同 ID 用户目录优先）。
- **插件库插件的附带副本**：`winshun.capture` 仍是插件库插件，但安装版与便携版另带其解包副本 `<安装目录>\library-plugins\winshun.capture`（`build.ps1 -Deploy` 从构建目录同名文件夹复制，CMake `install` 同步；不在 `plugins\` 下，`Catalog` 不把它当随包插件扫描）。首次引导选了截图时，`Onboarding::copyOfCapture` 找到它就经 `PluginSettings::installFrom` 直接装进用户插件目录并启用，不联网；没有副本、装不上（日志记原因）或用户目录已有不可用副本时才走插件库下载。装好后与插件库安装的完全相同，由后台插件更新照常升级。副本以原文件随包而不预先压缩：安装包的 LZMA2 固实压缩能与主程序里重复的内容一起去重，实测只让安装包大 1.16 MB（单独 zip 约 3.0 MB，且每版整包都变），增量更新也可按文件打补丁；安装器 `[InstallDelete]` 先清空 `library-plugins` 防旧文件残留。`WINSHUN_PLUGIN_COPIES` 可指定另一个副本目录（测试用，指向不存在的目录即视为无副本）。

## 2. 插件运行时架构

核心实现在 `src/plugin/`（`wsplugin`），运行器在 `src/plugin/runner/`（`WinShunPluginHost.exe`），SDK 在 `src/plugins-sdk/`（`winshun.py`、`WinShun.psm1`、`winshun.h`，同步编入资源 `:/sdk/`）。

| 模块 | 文件 | 核心职责 |
|---|---|---|
| `Manifest` | `Manifest.{h,cpp}` | 解析校验 `plugin.json`（拒 `.svg` 图标）、多语言文案、权限、运行模式、触发器、设置项与能力标记（`webSearch`/`systemSwitches`/`contentSearch`） |
| `Catalog` | `Catalog.{h,cpp}` | 扫描合并用户/随包插件，目录监视（清单重扫、脚本发 `scriptsChanged`），维护关键字/正则/推送/冲突索引与 `awaitingConsent` |
| `Install` / `Gallery` / `Updates` / `IndexSignature` | 对应 `.{h,cpp}` | 安装、升级替换与卸载（清 `<id>.data`/INI/凭据）；拉取插件库 `index.json` 并用 CNG 验内置公钥 ECDSA P-256 / SHA-256 签名（`index.json.sig`），仅验签通过允许静默更新 |
| `Secrets` | `Secrets.{h,cpp}` | `password` 类型设置仅存 Windows 凭据管理器（`WinShun/plugin/<id>/<key>`），**绝不写 INI**，`settingsOf()` 运行时合成 JSON |
| `Items` / `ZeroCode` / `Merge` / `Details` / `Programs` | 对应 `.{h,cpp}` | 结果与 `effects` 解析；零代码进程内求值；启动器合并配额（每插件 ≤3 行、总 ≤6 行）；解释器解析（Python 强制解析真实 `python.exe`，禁用再派生子进程的 `py.exe`） |
| `Runner` / `WinShunPluginHost` | `Runner.*`、`runner/*` | 单命名管道 `WinShun.Plugin.<SID>.<pid>.<seq>.<rand>` 多路复用实例（复用 `LineChannel`，16 MiB 单行上限），校验对端 PID 与 hello 密钥 |
| `InProcess` | `runner/InProcess.{h,cpp}` | 随包普通权限常驻插件 `winshun.tools`（静态编入 `wstools`）与 `winshun.switches`（单常驻 `QtLane` 线程加载 `WinShunSwitchesApp.dll`）直接在 runner 线程运行，减少常驻进程数；`WINSHUN_PLUGINS_APART=1` 可切回独立进程 |
| `Session` / `Host` | `Session.*`、`Host.*` | JSON-RPC 2.0 会话；GUI 线程门面：防抖、超时、崩溃退避、空闲退出、开发者模式追踪（密码打码）及 `callCommand()` 独立通道；`winshun/status` 经 `Host::reportFrom` 限长限项后存为 `State::report`（进程退出即清），`reportChanged` 只在插件页可见时刷新卡片（`PluginReport.qml`：一行状态、进度条、至多两个按钮），按钮经 `statusAction` 以 `trigger: "status"` 的 `action` 回传 |
| `Tools` | `Tools.{h,cpp}` | 插件反向 `winshun/tool` 主机：独立 `SearchEngine` + `mcp::ToolHost`，不看 `[Mcp] Enabled` 与 AI 排除目录，严格按清单权限放行；只给插件的分页全量工具 `list_documents`（`winshun:content`）与 `list_files`（`winshun:search`：按扩展名列名称索引里的本机文件，`longReadLock` 下按条目号游标翻页，远程根与低优先目录默认不列）不对 AI 客户端提供 |

- **统一停止与限额**：停用、移除、权限未确认与 runner 断开共用同一停止入口，释放排队查询、`once` 进程、热键、托盘与在途工具调用。`once` 默认防抖 120 ms、stdout 上限 **16 MiB**，通过 `lastQuery` 防同步取消后误建 rerun；查询超时默认 `resident` 1500 ms / `once` 3000 ms，连超 3 次重启；崩溃退避 `1 → 5 → 30` 秒，5 分钟内崩 3 次自动停用；常驻上限 8（`eager` 不计入淘汰）。
- **命令快捷键（#466）**：`[Plugin.<id>] Hotkey.<command>=`（`PluginConfig::hotkeys`，只存设了的）。`App::applyCommandHotkeys` 在 `applyPlugins`、目录变化与录制快捷键开始/结束时，把启用插件里有可用关键字的命令经 `PluginHotkeys`（属主 `:commands`，与插件 `register_hotkeys` 同一注册器，最多 32 个，只增删有变化的键）注册；与 Win顺 自己的键、剪贴板条目键、其他命令或他程冲突的原因经 `PluginSettings::setCommandHotkeyErrors` 写在卡片该行下。按下走 `App::commandHotkey`：`showLauncher("<第一个可用关键字> ")`，关键字仍在且启动器在前台时再按则关闭。
- **查询缓存**：`QCache` LRU，序列化数据预算 **2 MiB**，键含插件/命令/输入/筛选上下文，单调时钟过期，动态 rerun 不入缓存；设置、清单、权限、语言、动作与 refresh 触发失效。

## 3. 官方插件一览

当前仓库包含 **21 个官方插件**（4 个随包 + 17 个插件库）：

| 插件 ID | 源码目录 | 二进制 / 入口 | 分发与运行模式 | 核心职责与要点 |
|---|---|---|---|---|
| `winshun.go` | `plugins/winshun.go` | 无（零代码） | 随包 · 宿主内 | 快捷跳转示例，支持 `%VAR%` 展开 |
| `winshun.kill` | `plugins/winshun.kill` | `powershell` + `main.ps1` | 随包 · `resident` | 进程列表与终止（`kill`/`jc`）；支持 `ai.expose` |
| `winshun.tools` | `src/tools-plugin` | `WinShunTools.exe` / runner 进程内 | 随包 · `resident`（`eager`/`strict`） | 算式、单位、颜色、Base64/哈希/UUID/时间戳；默认随 runner 线程运行 |
| `winshun.switches` | `src/switches-plugin` | `WinShunSwitches.exe` + `*App.dll` | 随包 · `resident`（`eager`/进程内） | 7 项原生系统开关，共享宿主 Qt（详下） |
| `winshun.web` | `src/web-plugin` | 无（`webSearch: true`） | 插件库 · 宿主内零代码 | 网页搜索、自定义站点与图标、搜索建议（存 `[Plugin.winshun.web]`） |
| `winshun.capture` | `src/capture-plugin` + `src/capture` | `WinShunCapture.exe` + `*App.dll` | 插件库（安装包附带副本）· `resident`（`eager`/`normal`） | 截图、标注、贴图、长截图、录屏、演示白板与 OCR；详见 [capture-and-recording.md](capture-and-recording.md) |
| `winshun.ocr-search` | `src/ocr-search-plugin` | `WinShunOcrSearch.exe` + `*App.dll` | 插件库 · `resident`（`eager`/`strict`/`contentSearch`） | 图片/扫描 PDF/文档内嵌图本地 OCR 倒排索引（SQLite 默认 ≤512 MiB），隔离后台工作进程（单文件 60 s 无进展跳过） |
| `winshun.semantic` | `src/semantic-plugin` | `WinShunSemantic.exe`（无 Qt） | 插件库 · `resident`（`eager`/`strict`） | “按意思找”：经 `winshun:content` 复用内容索引，OpenAI 兼容 `/embeddings`，int8 mmap 向量段，首次前先估算并写入 `consent.txt`。`local` 服务（1.1.0）走 `winshun.localmodel` 的本机接口：每轮先 `GET /models` 取 `kind` 为 `embedding` 的那个模型名记入签名（换了嵌入模型即另建索引），没有时发一次探测请求拿到它自己说的原因；它的错误行 Enter 打开“本地模型”卡片 |
| `winshun.devtools` | `src/devtools-plugin` | `WinShunDevTools.exe` + `*App.dll` | 插件库 · `resident`（`lazy`/`normal`） | JSON 格式化/压缩/转义及共享宿主 Qt 的树形查看器 |
| `winshun.translate` | `src/translate-plugin` | `WinShunTranslate.exe`（无 Qt） | 插件库 · `resident`（`lazy`） | 翻译与 AI 词典（`tr`/`dict`）；异步 WinHTTP 单调截止时间（无逐请求 watchdog 线程，响应 ≤8 MiB）；服务回 401/403 时给 `setup` 行（“翻译服务的密钥不对或没有权限（接口返回 401）”，Enter 打开插件设置），402/429/5xx 给直说原因的普通错误行；宿主 `translate_text` 把这一行的标题作为 `message` 随状态 `service` 交回，仍会继续尝试下一个翻译插件（#444）。`local` 服务走 `winshun.localmodel` 的本机接口（不要密钥、不走代理；Ollama 等本机 OpenAI 兼容地址同样免密钥）：启动器里对聊天类服务按“文本+服务+目标语言”建后台任务，SSE 流式读入，查询最多等 1.5 s，没完就回已到的译文和 `rerun`，到字和结束时 `refresh()`；命令调用（`translate_text`、AI）仍一次请求、25 s 内回答 |
| `winshun.localmodel` | `src/localmodel-plugin` | `WinShunLocalModel.exe`（无 Qt）+ `llama\llama-server.exe`（llama.cpp b12359，Vulkan 版，构建时按 SHA256 下载） | 插件库 · `resident`（`eager`/`normal`，`memoryMB` 16384） | 本地模型：常驻的只有自写中转（`Relay`，`127.0.0.1:39281`，查 Host/Origin 防 DNS 重绑定与外站网页），收到要模型的请求才由 `Engine` 起 llama-server（随机端口、`--cache-ram 0`、`--ctx-checkpoints 2`、`-lv 4` 读设备与卸载层数），空闲超时或换模型就停；`/health`、`/v1/models` 不加载模型；显卡失败自动改 CPU，自己退出（多为内存）时卡片写明原因。推荐模型（Qwen3.5 2B、HY-MT1.5 1.8B）钉在 ModelScope 的提交上，多连接分块下载（`.part.map` 续传）后核对大小与 SHA-256；自选 .gguf 先读 GGUF 头拒绝 mmproj、LoRA 和选错种类的模型。两个 `Engine` 各带一个 llama-server：对话模型，和嵌入模型（`--embedding`，默认 Qwen3-Embedding 0.6B；`/v1/embeddings` 的请求归它，其余归对话模型），各自按需加载、空闲释放；`/v1/models` 用 `kind` 标出两者。嵌入模型在有请求要过它之后（`embed.wanted`）才出现在卡片上，只翻译的用户看不到。显存在 Windows 上也计入进程提交内存，所以上限放到 16 GB |
| `winshun.bookmarks` | `src/bookmarks-plugin` | `WinShunBookmarks.exe`（无 Qt） | 插件库 · `once` | Chrome/Edge/Firefox 书签与浏览历史（`bm`/`bh`），查完即退无常驻 |
| `winshun.localsend` | `src/localsend-plugin` | `WinShunLocalSend.exe`（无 Qt） | 插件库 · `resident`（`eager`/`normal`） | 局域网传输（LocalSend v2）：Schannel 自签证书 + CNG 持久密钥、组播 `224.0.0.167:53317` + `/24` 发现、`.wspart` 落盘与 MOTW |
| `winshun.sync` | `src/sync-plugin` | `WinShunSync.exe`（无 Qt） | 插件库 · `resident`（`eager`/`normal`） | 跨设备剪贴板同步（ShunSync v1，与 Mac顺 互通），详见 [shunsync-protocol.md](../shunsync-protocol.md) |
| `winshun.cloudbackup` | `src/cloudbackup-plugin` | `WinShunCloudBackup.exe`（无 Qt） | 插件库 · `resident`（`eager`） | 设置云备份（`winshun:backup`）：WebDAV 或私有 GitHub 仓库（拒传公开库），凭据不入备份 |
| `winshun.break` | `src/break-plugin` | `WinShunBreak.exe`（无 Qt） | 插件库 · `resident`（`eager`/`normal`） | 休息提醒：纯时钟调度、空闲/锁屏/全屏/勿扰感知、独立线程 Direct2D 分层窗口 |
| `winshun.gestures` | `src/gestures-plugin` | `WinShunGestures.exe` + `*Editor.dll` | 插件库 · `resident`（`eager`/`normal`） | 鼠标手势、触发角/摩擦边与划词动作（常驻无 Qt，编辑器共享宿主 Qt），详见 [design-gestures.md](../design-gestures.md) |
| `winshun.expander` | `src/expander-plugin` | `WinShunExpander.exe`（无 Qt） | 插件库 · `resident`（`eager`/`normal`） | 缩写展开：`WH_KEYBOARD_LL`/`WH_MOUSE_LL` + `ToUnicodeEx` + 输入法中文状态检测 + `SendInput` 替换 |
| `winshun.topmost` | `src/topmost-plugin` | `WinShunTopmost.exe`（无 Qt） | 插件库 · `resident`（`eager`/`normal`） | 窗口置顶：快捷键（宿主代注册 `register_hotkeys`，默认 `Win+Ctrl+T`）切换前台窗口的 `WS_EX_TOPMOST`，`置顶`/`top` 列出窗口；状态只认窗口自身的置顶位，`SWP_ASYNCWINDOWPOS` 不等无响应程序；提权窗口（UIPI）、无响应、已关闭都在窗口上的提示条（独立线程 Direct2D 分层窗口）写明；自己置顶的窗口有描边（`Border`，设置 `border`）：独立线程，每个窗口四条点击穿透的分层窗口（上下两条含圆角、左右两条，不建整窗大小的位图），画在 `DWMWA_EXTENDED_FRAME_BOUNDS` 以内、紧贴目标窗口之上的 Z 序；按目标线程挂 WinEvent 钩子跟随移动/缩放/最小化/隐藏，另有 250 ms 核对（置顶位被别人摘掉、Z 序、缩放变化）；圆角取 `DWMWA_WINDOW_CORNER_PREFERENCE`，最大化按工作区裁剪并取直角，铺满显示器（全屏）不画；停止时取消自己置顶过的窗口 |
| `winshun.archive` | `src/archive-plugin` | `WinShunArchive.exe`（无 Qt） | 插件库 · `resident`（`eager`/`strict`/`contentSearch`） | 压缩包内文件名搜索：经 `list_files` 向宿主分页取压缩包清单（不自己扫盘），后台线程（`THREAD_MODE_BACKGROUND_BEGIN`）用静态链接的 libarchive（`cmake/Libarchive.cmake`：libarchive + zlib + liblzma 按固定 SHA256 从源码包以 ExternalProject 构建）只读每个包的文件列表，按宿主给的大小+修改时间作戳增量重读；清单相同的包共享一份 `Contents`（同名同大小同时间的拷贝直接沿用、不再打开，搜索时同一份清单只扫一次、拷贝排在后面），名字常驻内存（去重后 ≤128 MiB，空闲时让出工作集）并原子落盘 `archives.bin`；正在读的包记在 `reading.txt`，进程死在某个包上时下次启动把它记为打不开、不再重读，直到它变了；行为 `details` 行，预览/解压时才把单个成员取到 `<id>.data`（预览 ≤32 MiB、4 s；打开 ≤1 GiB、8 s）；文件名加密、网盘占位、损坏与截断分别计数并显示在卡片状态上 |
| `winshun.explorer-tabs` | `src/explorer-tabs-plugin` | `WinShunExplorerTabs.exe`（无 Qt） | 插件库 · `resident`（`eager`/`elevated`） | Win11 22H2+ 资源管理器合并标签页与会话恢复；提权宿主经确认可合并管理员窗口 |

- **系统开关线程模型（`winshun.switches`）**：宿主仅留 `SwitchCommand` 语法解析、`places.txt` 与 GUI 异步缓存（`Switches`/`SwitchPlugin`）。插件内部三线程分离：① 插件事件线程（`RegistryWatch`、`PowerCreateRequest` 与保持唤醒定时器，严禁调 WinRT `.get()` 或全局广播）；② MTA `ComWorker` 开关线程（串行硬件读写，回调经 `SwitchRelay` 转回）；③ 独立 `detach` 广播线程（深色模式先同步刷本进程窗再 `HWND_BROADCAST` `ImmersiveColorSet` 配 `SMTO_ABORTIFHUNG`，退出不 `join` 防主消息循环死锁）。因全属当前用户会话与 HKCU，不经 LocalSystem 服务；崩溃或停用立即撤销电源请求，重启仅重读不重放写入。

## 4. AI 助手（MCP）与 `AiBridge`

MCP 服务端位于 `src/mcp/`（`wsmcp`），实现 MCP 2025-11-25（兼容 2025-06-18 / 2025-03-26 / 2024-11-05）。

- **命名管道与中继进程**：AI 客户端以 stdio 启普通权 `WinShunMcp.exe`（`src/mcp/relay/main.cpp`），透传至 `\\.\pipe\WinShun.Mcp.<用户 SID>`（DACL 仅本用户 + 中完整性标签，杜绝本地端口嗅探/DNS 重绑定）。服务端 `PipeServer` 每连接一线程，复用 `LineChannel`（16 MiB 单行上限，停止直接取消 I/O 不等客户端读空）。中继延迟建连、断线重连重放 `initialize`，宿主未运行时读 `mcp-tools.json` 应答 `tools/list`，带 `--start-winshun` 经计划任务无 UAC 拉起（自身提权且无任务时拒绝直起）。
- **双引擎车道与工作池**：`McpServer` 持独立名称/内容双 `SearchEngine` 车道互不阻塞；所有碰盘与收尾（访问检查、大小、正文与 JSON 编码）投递至 3 线程 `AsideWorkers`（SSD 4096 条/批并行预检父目录，HDD 活跃时 `QuietIo` 降级 I/O；长扫描先持 `m_scanMutex` 再取读锁防写饥饿）。单行受 `kAnswerBudget` 预算约束（CLI 15 MiB，AI 客户端三分之一），按条截断并置 `truncated` / `next_cursor`。
- **分级门控（`Policy` / `Gate`）**：`[Mcp] Enabled` 默认关；开启仅露只读工具，其余受独立开关门控并广播 `tools/list_changed`：`Gate::Open`（普通权打开/定位）、`Gate::Clipboard`（需 AI 与历史双开，截图拷至 `%TEMP%\WinShun\mcp\`）、`Gate::Screenshot`（仅 AI 可用，弹 `ScreenshotConfirm.qml` 须鼠标点允许、30 秒超时、隐藏后两次 `DwmFlush` 抓图）、`Gate::Documents`、`Gate::Settings`（`AiSettings` 默认拒绝白名单，`[Mcp]`/插件/共享/备份路径永不可写，`SettingsConfirm.qml` 确认 + `SettingsNotice.qml` 可安全撤销）。
- **权限与防绕过安全边界**：首包 `ImpersonateNamedPipeClient` 拷贝调用者令牌供工作线程做 `AccessCheck`（**永不用宿主提权令牌**），不可见项视同不存在。路径入参先拦 `\\?\`/`\\.\`/`\??\`、排除目录与本机回环 UNC，再 `OPEN_NO_RECALL` 句柄取 `GetFinalPathNameByHandleW` 复核排除与权限，`read_file_text` 经 `ReOpenFile` 消 TOCTOU；非已索引远程根的任意 UNC 触盘前拒访（防 NTLM 泄漏）。每次调用记入 `mcp.log`（绝不记剪贴板正文）。
- **`AiBridge`**：`[Mcp] Plugins` 开启时把声明 `ai.expose` 且启用的插件命令映为 `plugin_<id>_<command>`（去私有 `data`），`PluginActions` 开启再加 `_run`，走 `Host::callCommand` 与启动器隔离；提权宿主下隐藏并拒绝 `elevated` 插件。

## 5. 命令行 `ws` 与 Skills

- **`ws.exe`（`src/cli/`）**：无 Qt、静态 CRT、部署于 `<安装目录>\bin`（仅此目录进 `PATH`），作为一次性 MCP 客户端（`clientInfo.name = "ws"`）复用管道、`ToolHost`、权限与 `mcp.log`。服务端 `capabilities.experimental.winshun.cli = 1` 启用高上限、仅 `structuredContent` 与结构化错误码。`ws open` 发包前经 `GetNamedPipeServerProcessId` 验证对端为合法 `WinShun.exe`；`ws plugin` 由 `src/app/PluginCommands` 承接，需用户决策的操作弹窗并返回退出码 1。详见 [design-cli.md](../design-cli.md)。
- **Skills（`skills/`）**：提供 `winshun`（编入 `ws.exe`）、`winshun-plugin-dev`、`winshun-dev` 三个 Skill，支持 `npx skills add LingCore/WinShun` 与 `ws skill --install claude`（设置页以普通用户权限执行）。

## 6. 相关测试入口

- **插件运行时与安全**：`tst_plugin`、`tst_plugincommands`、`tst_pluginhotkeys`、`tst_pluginupdates`、`tst_pluginsdk`、`tst_movedplugins`、`tst_pluginreadme`、`tst_pluginconfirm`、`tst_pluginhomepage`、`tst_pluginkeywords`、`tst_pluginsliders`。
- **MCP 与命令行**：`tst_mcp`、`tst_aisettings`、`tst_cli`、`tst_commandline`、`tst_shelllaunch`。
- **官方插件回归**：`tst_switchplugin`、`tst_switches`、`tst_tools`、`tst_shunsync`、`tst_semantic`、`tst_localsend`、`tst_ocrsearch`、`tst_devtools`、`tst_cloudbackup`、`tst_explorertabs`、`tst_gestures`、`tst_expander`、`tst_topmost`、`tst_break`、`tst_translate`、`tst_bookmarks`、`tst_archive`。
- **探针与预览**：`wsbench --plugin` / `--launcher-sim` / `--mcp-host` / `--switches`、`plugin-probe --parent`、`WinShun.exe --preview-plugins` / `--preview-ai-cards`。
