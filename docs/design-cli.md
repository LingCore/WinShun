# 命令行 `ws` 设计与规范

`ws.exe` 是 Win顺 的独立命令行客户端（面向终端用户和通过 Skill 调用的 AI 代理）：通过命名管道复用 Win顺 内部 MCP 服务，支持按名称/拼音搜文件、全文搜索（含 Word/Excel/PPT/PDF）、读文档正文、列目录、查剪贴板以及管理插件。

## 1. 总体架构与安全边界

```
终端 / AI 代理
  └─ ws.exe（<安装目录>\bin\，C++20，无 Qt，静态 CRT /MT，普通权限）
       └─ 命名管道 \\.\pipe\WinShun.Mcp.<用户 SID>（与 WinShunMcp.exe 共用）
            └─ McpServer → ToolHost（统一 Policy、可见性、排除目录与 mcp.log）
```

- **单次会话**：连接管道后一次发出 `initialize`（`clientInfo.name = "ws"`）+ `tools/call`，打印结果即退出。
- **统一权限与隐私**：复用 `ToolHost`、`mcp::Policy` 和 `ImpersonateNamedPipeClient` + `AccessCheck`，不能绕过“不让 AI 看到的文件夹”及剪贴板开关；调用者看到的范围不多于自身令牌权限。
- **共用连接开关**：与“允许 AI 助手和命令行连接”（`[Mcp] Enabled`，默认关）共用一个开关。
- **服务端能力声明**：新版 Win顺 在 `initialize` 中返回 `capabilities.experimental.winshun = {"cli": 1}`，启用命令行会话专属上限（默认 100，`-n 0` 最高 50,000 条、行上限 16 MiB，`grep` 最高 2,000 条）、仅返回 `structuredContent`、默认不读盘查文件大小（仅 `-l` 或按大小排序时请求 `size: true`）以及结构化拒绝码。若旧版宿主无此声明且用户传了 `--in`，`ws` 直接以退出码 7 报错，避免返回未按目录筛选的误导结果。
- **命名引擎与内容引擎分离**：`ToolHost` 为名称搜索和内容搜索各持有一个 `SearchEngine`，并发 `ws grep` 不会阻塞几毫秒的 `ws find`。

## 2. 子命令与参数规则

`ws <查询>` 等价于 `ws find <查询>`；仅当首词精确命中子命令表时才按子命令解析。选项须放在 `--` 之前，`--` 之后一律作为查询词；支持 `--long`、`-s`、`-n20`、`/?`；未知选项直接报退出码 2 并给出拼写建议。

| 命令 | 作用 | 背后工具 | 常用参数 |
|---|---|---|---|
| `ws find <查询>` | 按名称/拼音搜文件与文件夹 | `search_files` | `-n`、`--files`/`--folders`、`--ext`、`--in`（可多次，`.` 转为绝对路径）、`--sort`、`-l` |
| `ws grep <文字>` | 全文搜索（含 Office/PDF 与 `regex:`） | `search_content` | `-n`、`--in`、`--timeout 5s`（最大 30s） |
| `ws recent` | 最近修改的文件 | `recent_files` | `--days 7`、`--ext`、`--in`（首次空结果自动带 `include_system`/`include_hidden` 重查一次并提示） |
| `ws text <文件>` | 读取文本/文档内容（带页/表位置） | `read_file_text` | `--from <字数>`、`--max <字数>`（默认 20,000，`0` 不限） |
| `ws ls <文件夹>` | 从索引列目录 | `list_folder` | `-n`、`-l` |
| `ws open <查询或路径>` | 打开匹配项（由 `ws` 自身执行） | `search_files` | `--reveal`（在文件夹中定位） |
| `ws clip` | 剪贴板历史（受剪贴板与 AI 开关控制） | `clipboard_history` | `-n`、`--kind text\|link\|path\|files\|image` |
| `ws plugin <动作>` | `list` / `search` / `install` / `enable` / `disable` / `remove` | `plugins_*`（仅对 `ws` 开放） | `--json` |
| `ws status` | Win顺 运行状态、版本与索引就绪度 | `index_status` | 无 `--wait`/`--no-start` |
| `ws skill` | 输出内置 `SKILL.md`；`--install claude` 写入 Claude Code | 本地资源 | `--install claude` |
| `ws help [命令]` | 中/英文帮助 | 本地 | |

- **`ws open` 安全校验**：
  - 传入已存在的本地路径时直接打开；通过查询获得结果时，发请求前先核验管道服务端进程（`GetNamedPipeServerProcessId`）确实是安装目录或 `ws.exe` 上级目录的 `WinShun.exe`（防管道名抢占），否则退出码 7。
  - 若 `ws` 自身以管理员身份运行，搜索结果的打开优先交给 Explorer 的 `IShellDispatch2::ShellExecute`（保持普通用户权限），并带 `SEE_MASK_FLAG_NO_UI` 避免弹窗卡住代理。
- **本地前置校验**：对 `ws text <文件夹>`、`ws ls <文件>` 或 `--in <文件>` 在本地直接返退出码 2；`--in` 目录不存在返退出码 6。

## 3. `ws plugin` 约定

- 五个 `plugins_*` 工具（`mcp::kPluginTools`）不出现在 `tools/list`，非 `ws` 客户端调用返回 Unknown tool。
- **可直接完成的操作**：安装本地包/目录（复用 `install::inspect` / `updates::replace`，新装默认关，覆盖已启用插件保持启用）、禁用插件、启用已授权插件。
- **必须用户本人确认的操作**：首次启用/权限升级/提权插件授权、卸载确认、从插件库按 ID 安装（走 `PluginGallery::installFor` 下载、SHA-256 校验与原生包确认）。此时 Win顺 弹出自身窗口并返回 `{state: "asking", ...}`，`ws` 不代答，立即以退出码 1 返回提示。
- 回答结构：`{state: "done"|"asking"|"failed", message, plugin?, plugins?, code?}`；退出码：`0` 完成、`1` 等待用户确认或 `search` 无结果、`6` 未找到、`7` 失败。
- 实现位置：`src/cli/Plugin.cpp`、`src/app/PluginCommands.*`。

## 4. 输出、编码与退出码

- **流分离与换行**：`stdout` 仅输出数据（统一 LF 换行），所有提示、进度与错误写 `stderr`（`ws:` 前缀），写 `stderr` 前先 flush `stdout`。`--json` 与 `-0`、`-l` 与 `-0` 互斥（退出码 2）。
- **格式**：
  - 默认：每行一个完整路径；控制台启用 VT 高亮（遵守 `NO_COLOR`、`--color auto|always|never`），重定向时无颜色。
  - `-l`：对齐的大小、修改时间与路径。
  - `grep`：`路径:行号:片段`（文档为 `p3`、`s2`、`[表名]`）。
  - `--json`：单行 JSON 数组（`find`/`recent`/`ls` 含 `{path, type, modified, size?}`；`grep` 含 `{path, modified, score, snippet, location}`；`text` 为对象 `{path, kind, text, chars, offset, cut, next_offset?}`）。
  - `-0`：NUL 分隔。
- **编码规则**：
  - 控制台输出：`WriteConsoleW`（不受代码页影响）。
  - 重定向默认：UTF-8 无 BOM（适配 Git Bash、Claude Code、Python、jq）。
  - 父进程为 `powershell.exe` / `pwsh.exe` 时：跟随 `GetConsoleOutputCP()`，与 PowerShell 自身管道/重定向解码一致；无法表示的字符替换为 `?` 并在 `stderr` 提示按行统计（`--json` 输出 `\uXXXX` 不丢字），代码页无法表示简体中文时提示语自动切换纯 ASCII 英文。
  - 可用 `--encoding utf8|utf16|ansi|oem|console` 或 `WS_ENCODING` 覆盖（多次指定取最后一个，非法值退出码 2）。
  - 管道对端提前关闭（`ERROR_NO_DATA` / `ERROR_BROKEN_PIPE` / `ERROR_PIPE_NOT_CONNECTED`）静默退出；磁盘满等真实写错报退出码 7。
- **语言**：跟随 Win顺 界面语言，可用 `WS_LANG=en` 覆盖；`--json` 字段与退出码与语言无关。

| 退出码 | 含义 |
|---|---|
| `0` | 成功且有结果（或 `ws plugin` 已完成） |
| `1` | 成功但无结果（或 `ws plugin` 等待用户在 Win顺 窗口确认） |
| `2` | 用法或参数错误 |
| `3` | Win顺 未运行且未能拉起 |
| `4` | Win顺 在运行，但 AI/命令行连接开关关闭（或 `ws clip` 对应剪贴板开关关闭；新版通过 `structuredContent.setting` 区分 `ai_clipboard` / `clipboard_history`） |
| `5` | 索引尚未就绪（超过 `--wait`，默认 10s） |
| `6` | 路径不可见（不存在、无权限或在 AI 排除目录中，提示口径一致） |
| `7` | 其他错误（管道断开、读取空闲超时、`too_large`、`queued` 内容搜索排队超时等） |
| `130` | `Ctrl+C` / `Ctrl+Break` 中断（已发 `notifications/cancelled`） |

## 5. 连接、自动拉起与超时策略

- **先连管道再看配置**：管道存在直接连接；连不上且读配置见连接开关关闭则返回 4 不拉起。
- **未运行自动拉起**：开关开启且无单实例互斥量时（除非 `--no-start`），通过共享命名互斥量避免并发重复拉起，由一个 `ws` 调用开机计划任务拉起 Win顺（不弹 UAC），最多等 12 秒连管道。
- **启动/切开关宽限**：Win顺 已在运行但管道未就绪时等待 2 秒（若检测到另一 `ws` 持 `<mutex>.Starting` 等满 12 秒，若本会话最老 `WinShun.exe` 启动不足 8 秒等到满 8 秒）。
- **索引等待无额外往返**：`index_status` 与业务调用合并发出；就绪直接取结果，未就绪则取消调用并按 `--wait` 轮询等待。
- **读超时**：按距上次收到字节的空闲计时，普通命令 120 秒，`status`/`--version` 10 秒。

## 6. 安装、PATH 与内置 Skill

- **独立 `bin\` 目录**：`ws.exe` 放在 `<安装目录>\bin\`，仅将该目录加入 PATH（绝不把含 Qt/PDFium DLL 的安装根目录加入 PATH）。安装包按安装模式写系统/用户 PATH 并随卸载清理；便携版可在 **设置 › AI 助手** 一键加入用户 PATH。
- **内置 Skill**：`skills/winshun/SKILL.md` 经 `resources/ws.rc` 编入 `ws.exe`。`ws skill --install claude` 使用 `FOLDERID_Profile`（或合法绝对路径 `CLAUDE_CONFIG_DIR`）原子写入 `.claude\skills\winshun\SKILL.md`，并附带 `ws.exe` 绝对路径回退说明（独立重试，不用 `||` 拼接以免触发工具权限拦截）。

## 7. 代码布局与测试

```
src/cli/
  main.cpp           wmain 入口（shell32/ole32 延迟加载）
  Run.*              全局参数、子命令分发、输出流与收尾
  Commands.* Args.*  表驱动子命令与参数解析、拼写建议
  Help.cpp Messages.h 中英文帮助与提示文案
  Config.* Connect.* 配置读取、管道连接、拉起与宽限等待
  Session.* Context.* JSON-RPC 会话、取消、索引等待与错误映射
  Interrupt.*        Ctrl+C / Ctrl+Break 控制台处理器
  Output.* Encoding.* Format.* Print.*  控制台/重定向编码、高亮与格式化
  Search.cpp Grep.cpp TextCommand.cpp Clip.cpp Plugin.cpp Status.cpp Skill.*  各子命令
src/mcp/PipeClient.* Launch.*  与 WinShunMcp.exe 共享的源文件（分别按 /MT 与 /MD 编译，避免 LNK2038）
```

- **测试入口**：`tst_cli`（参数/SKILL 一致性、全部子命令真实驱动、编码、取消与拒绝码）、`tst_plugincommands`、`tst_mcp::pluginToolsAreKeptForWs`。
- **测试隔离环境变量与隐藏参数**：`--pipe`、`--instance`、`WS_SETTINGS`、`WS_TEST_START`、`WS_TEST_OPEN`、`WS_TEST_ELEVATED`、`WS_TEST_IDLE_MS`、`WS_TEST_VERIFY`、`WS_TEST_ASSUME_CLI`、`CLAUDE_CONFIG_DIR`（均不污染用户的 Win顺 实例和 `~\.claude`）。
