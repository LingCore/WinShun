# 命令行 ws（设计，2026-10-24）

论坛用户的需求：像 Everything 的 `es` 那样在命令行里搜文件，AI 通过 skill 调用（比 MCP 省上下文）；能平替的话就卸掉 Everything，免得两个索引一起扫盘。

要求：代码现代化，用户体验好。

## 1. Everything 的 es 是怎么做的

| 方面 | es 的做法 | 我们取什么 |
|---|---|---|
| 程序 | 单独下载的小 exe（MIT），文档让用户自己拷进 `%LOCALAPPDATA%\Microsoft\WindowsApps`（已在 PATH 里） | 一个不带 Qt 的小 exe，安装包负责放进 PATH，用户不用拷 |
| 和主程序通信 | Everything 1.4：`WM_COPYDATA` 发给 Everything 的隐藏窗口，回答再用 `WM_COPYDATA` 发回 es 自己建的窗口（所以错误码里有“注册窗口类失败”“建监听窗口失败”）；Everything 1.5：命名管道 `\\.\pipe\Everything IPC`（`-ipc1/2/3` 选版本，`-instance` 选实例） | 命名管道，而且就用 MCP 那根（见第 2 节） |
| 主程序没运行 | 退出码 8 “找不到 Everything IPC 窗口” | 没运行就拉起来（不弹 UAC），开关关着才报错 |
| 数据库没加载完 | 默认**不等**，直接返回空结果；`-timeout` 才等 | 默认等一会儿并在 stderr 说明；绝不悄悄返回空 |
| 查询 | 参数拼成 Everything 的搜索语法 | 参数拼成 Win顺 的搜索语法（拼音、`ext:`、`!`、通配符、路径），另给常用的写成参数 |
| 输出 | 默认每行一个完整路径；`-size -dm` 等加列；`-csv -json -tsv -efu -m3u -txt`；`-export-*` 写文件；`-highlight` | 默认每行一个路径；`-l` 加大小和时间；`--json`；`-0` |
| 数量 | `-n` 最多几个，默认全部；`-viewport-offset` 分页 | `-n` 默认 100（见第 4 节） |
| 排序 | `-sort name/path/size/date-modified…`，默认按名字 | 默认按相关度（和启动器一样，最近打开的靠前），`--sort` 可换 |
| 退出码 | 0 成功，1–8 各种失败，9 无结果（要加 `-no-result-error`） | 照 grep：0 有结果，1 没结果，2 起是错误（第 6 节） |
| 其他 | `-get-folder-size`、`-get-result-count`、run count、journal、`-save-settings` | 第一版不做（第 11 节） |

es 只管文件名。我们多出来、对 AI 最有用的两样：**内容搜索**（Word/Excel/PPT/PDF 也能搜）和**读出文档文字**（AI 自己读不了 docx/pdf）。

## 2. 总体方案：ws 是一个一次性的 MCP 客户端

```
终端 / AI 代理
  └─ ws.exe（bin\ 下，无 Qt，静态 C 运行库，普通权限）
       └─ 命名管道 \\.\pipe\WinShun.Mcp.<用户 SID>   ← 和 WinShunMcp.exe 是同一根
            └─ McpServer → ToolHost（同一套 Policy、Visibility、排除文件夹、日志）
```

每次运行：连管道 → `initialize`（clientInfo 名字 `ws`）→ `tools/call` → 打印 → 退出。三条消息一次写出去，不等握手的回答。

为什么不另开一根管道、另造一套协议：

- **隐私开关不能被绕过。** 主要用户是 AI 代理（Claude Code、Codex 这类能跑命令的）。如果 ws 走一个不受 `[Mcp]` 开关管的 host，“不让 AI 看到的文件夹”、剪贴板开关对这些 AI 就全失效了：AI 跑一下 `ws` 就绕过去。所以 ws 必须走同一个 `ToolHost` 和同一套 `mcp::Policy`。
- **能看到的不多于调用者自己。** 管道连上后照常 `ImpersonateNamedPipeClient` 拿 ws 的令牌做 `AccessCheck`：普通终端看到普通权限能看的，管理员终端看到全部，和 MCP 一样。
- **协议只有一份。** 工具、参数校验、取消、排队、`mcp.log` 全部复用，`tst_mcp` 的测法直接适用。
- **代价很小。** 同一进程里多一次 `initialize` 往返，不到 1 ms。

注意：`App::applyMcp()` 在 `[Mcp] Enabled` 关着时会拆掉 `m_mcpServer`，管道就没了。ws 能不能用取决于这个开关，见第 7 节（待定）。

## 3. 命令

`ws <查询>` 等于 `ws find <查询>`，和 `es <查询>` 一样顺手。第一个词是下面的子命令时按子命令处理；真要搜叫 `recent` 的文件，写 `ws find recent` 或 `ws -- recent`。Skill 里一律写全子命令，不让模型碰这个歧义。

| 命令 | 做什么 | 背后的工具 | 常用参数 |
|---|---|---|---|
| `ws find <查询>` | 按名字找文件和文件夹 | `search_files` | `-n` 数量，`--files` / `--folders`，`--ext pdf,docx`，`--in <文件夹>`（可多次，`.` 是当前目录），`--sort match\|newest\|oldest\|name\|largest`，`-l` |
| `ws grep <文字>` | 按内容找（含 Word/Excel/PPT/PDF） | `search_content` | `-n`，`--in`，`--timeout 5s` |
| `ws recent` | 最近改过的文件 | `recent_files` | `--days 7`，`--ext`，`--in` |
| `ws text <文件>` | 读出文件里的文字（文档带 `[page 3]` 这类位置行） | `read_file_text` | `--from <字数>`，`--max <字数>` |
| `ws ls <文件夹>` | 从索引列文件夹（大文件夹也快） | `list_folder` | `-n`，`-l` |
| `ws open <查询或路径>` | 打开最匹配的那个（和启动器里按 Enter 一样） | `search_files`，打开由 ws 自己做 | `--reveal` 在文件夹里显示 |
| `ws clip` | 剪贴板历史（剪贴板开关开着才行） | `clipboard_history` | `-n`，`--kind text\|link\|path\|image` |
| `ws status` | Win顺 在不在、索引好了没、版本 | `index_status` | |
| `ws skill` | 打印给 AI 用的 SKILL.md；`--install claude` 装进 Claude Code | 本地 | |
| `ws help [命令]` | 帮助 | 本地 | |

几点说明：

- **查询原样交给搜索语法**，多个参数用空格拼起来（`ws find 周报 ext:docx`）。`--ext`、`--in`、`--files` 只是常用写法的参数版，拼进同一个请求，不另造规则。
- **`--in .` 很重要**：在项目里搜、在当前文件夹里搜，AI 最常用。相对路径由 ws 用自己的当前目录换成绝对路径再发。
- **`ws open` 由 ws 自己打开**（`ShellExecuteW`，就是调用者的权限），不经 `open_item`。能跑 `ws` 的程序本来就能 `start` 一个文件，没有新增的能力，所以不受“允许 AI 打开文件”开关限制；剪贴板历史是别的程序拿不到的东西，才受开关管。
  - 这等于把那个开关的意思收窄成“只管 MCP 客户端”：能跑命令的 AI 开关关着也能用 `ws open` 打开文件（它本来也能 `start`）。2026-10-24 定：就这样做，设置页那个开关的说明跟着改。
- **bash 会先展开通配符**：`ws find *.pdf` 在 bash 里会变成当前目录的文件名。帮助和 Skill 里都写“查询加引号”。PowerShell 和 cmd 不展开。
- 参数照常见习惯：`--长名`、`-短名`、`-n20` 和 `-n 20` 都认、`--` 之后不再当参数；`/?` 也认（Windows 用户的习惯）。
- 不认识的参数直接报错并给出最像的那个（“没有 --exts，是不是 --ext？”），不悄悄当成查询词。

## 4. 输出

原则：**stdout 只放数据，提示一律写 stderr**，管道里的数据永远干净；AI 拿到的就是最少的字。

- **默认（find / recent / ls）**：每行一个完整路径。
  - 输出到控制台时：文件名加粗、命中的字高亮、文件夹部分淡一点（虚拟终端序列，`SetConsoleMode` 打开 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`）；遵守 `NO_COLOR`，`--color auto|always|never`。
  - 重定向时：纯路径，没有颜色。
- **`-l`**：`大小  修改时间  路径`，大小按 KB/MB 对齐。只有这时才去磁盘读大小（见第 9 节）。
- **`grep`**：`路径:行号:片段`（和 ripgrep、编辑器的跳转格式一样）；文档写 `路径:p3:片段`（页）、`s2`（幻灯片）、`[表名]`（Excel）。
- **`text`**：文字原样输出；没读完时 stderr 写“还有 N 字，用 --from 12000 接着读”。
- **`--json`**：一个 JSON 数组（一行），字段和 MCP 工具的 `items` 一样。
  - 结果本来就是一次回来的，不需要流式输出，所以不用 JSON Lines。
  - 数组在 Python 的 `json.loads`、Node 的 `JSON.parse` 里一次就能解析，jq 用 `.[]` 展开。
  - PowerShell 5.1 和 7 的 `ConvertFrom-Json` 两种格式都能解析（2026-10-24 实测）。
- **`-0`**：用 NUL 分隔路径，给 `xargs -0` 这类用。
- **数量**：默认 100 条，`-n 20` 改，`-n 0` 表示能给多少给多少（上限 50 000，见第 9 节）。截断时 stderr 写一行“只列了前 100 个，用 -n 0 看全部，或把查询写细一点”。不报总数：被访问检查滤掉的不能计数（和 MCP 一样）。
  - 和 es 不同（es 默认全给）。这是为 AI 选的：随手一个 `ws report` 给出几千行会把上下文撑爆。脚本要全部就写 `-n 0`。

### 编码（中文 Windows 上最容易出事的地方）

- **输出到控制台**：一律 `WriteConsoleW`，任何代码页都对。
- **重定向（管道、写文件）**：默认 UTF-8，不带 BOM。Git Bash、Claude Code、Codex、Python（UTF-8 模式）、写文件都要这个。
- **例外：父进程是 PowerShell**（`powershell.exe` / `pwsh.exe`）。这时 ws 按控制台的输出代码页（`GetConsoleOutputCP()`，PowerShell 改 `[Console]::OutputEncoding` 时它跟着变）写；是 65001 就照样 UTF-8。
  - 理由：**和 PowerShell 自己的输出用同一种编码。** PowerShell 的 cmdlet 输出被重定向时也按这个代码页编码。用 PowerShell 跑命令的 AI 代理，只要它能正确读出 PowerShell 自己输出的中文，就能正确读出 ws 的。
  - 代码页里没有的字（生僻字、emoji）：普通输出换成 `?`，并在 stderr 提醒一次“有 N 个路径含当前代码页写不出的字”；`--json` 里把这些字写成 `\uXXXX`，不丢信息。
- 2026-10-24 实测（控制台代码页 936，一个程序分别写 UTF-8 和 GBK 的“D:\报告\周报.docx”）：

  | 去向 | PowerShell 7.6 | PowerShell 5.1 |
  |---|---|---|
  | 管道给 cmdlet（`\| % {…}`、赋值） | 只有 GBK 对 | 只有 GBK 对 |
  | 管道给外部程序（`\| rg`） | 原样传字节，写什么收到什么 | 两种都丢中文（5.1 按 `$OutputEncoding` 重新编码） |
  | `>` 写文件 | 原样写字节 | 按代码页解码后写 UTF-16，只有 GBK 对 |

  - 所以按代码页写，在最常见的用法（交给 cmdlet、5.1 写文件）上是对的。
  - 代价：PowerShell 7 里 `ws … > a.txt` 得到 GBK 文件，`ws … | rg 中文` 匹配不上。
  - 帮助里给 PowerShell 用户一个建议：在 profile 里加 `[Console]::OutputEncoding = [Text.UTF8Encoding]::new()`。之后一切都是 UTF-8，ws 跟着变。
- **`--encoding utf8|utf16|ansi|oem|console`** 和环境变量 `WS_ENCODING` 可以强制指定。
- 做好后测一轮，按**去向**分：控制台；管道给 cmdlet；管道给外部程序；写文件。
  - 父进程分 cmd、PowerShell 5.1、PowerShell 7、Git Bash、Claude Code 的 Bash；
  - 代码页 936 和 65001 各一次；
  - 文件名里放中文和一个 GBK 里没有的字（比如 𠀀）；
  - 读出实际字节比对。

## 5. 语言

- 提示和帮助跟 Win顺 的界面语言（读设置文件里的语言；读不到就用 `GetUserDefaultUILanguage`），`WS_LANG=en` 可改。
- `--json` 的字段名和退出码不随语言变。

## 6. 退出码

| 码 | 意思 |
|---|---|
| 0 | 成功，有结果 |
| 1 | 成功，但没有结果（和 grep、ripgrep 一样） |
| 2 | 用法不对（参数错、少了查询） |
| 3 | Win顺 没在运行，也没能启动 |
| 4 | Win顺 在运行，但 AI / 命令行连接关着（stderr 说明在哪打开） |
| 5 | 索引还没准备好（等到 `--wait` 用完） |
| 6 | 这个路径看不到（不存在、没权限、在“不让 AI 看到的文件夹”里，说法相同） |
| 7 | 其他错误（管道断了、Win顺 回了错误） |
| 130 | 按了 Ctrl+C（已向 Win顺 发取消） |

## 7. 没运行、没准备好、开关关着

- **先看开关**：ws 本来就读设置文件（拿界面语言），顺便读连接开关。关着就直接退出码 4，告诉用户在哪打开，不拉起 Win顺。否则新用户第一次敲 `ws 报告` 会先把 Win顺 拉起来，再白等 12 秒一根永远不会出现的管道。
- **Win顺 没在运行**（单实例互斥量不在）、开关开着：照中继的 `--start-winshun` 做法，跑开机自启的计划任务把它拉起来（不弹 UAC），stderr 写“Win顺 没在运行，正在启动…”，最多等 12 秒连上管道。`--no-start` 不拉。
- **在运行但没有管道**：设置文件和正在运行的 Win顺 对不上（刚改过开关、旧版本）。**绝不能**跑计划任务，那只会把启动器弹出来。直接退出码 4。
- **索引还在加载**：默认最多等 10 秒（`--wait 30s` 可改，`--wait 0` 不等），控制台上显示一行“正在加载索引 63%…”，好了就接着搜；等不到就退出码 5。es 默认不等，数据库没加载完就返回空结果，看起来和“没找到”一样，我们不学。
- 加载完、还在追变更日志时照常搜，stderr 提一句“索引正在更新，刚改过的文件可能还没有”。

**连接开关怎么管命令行：选方案 A**（2026-10-24 定）。B 等于让只读搜索默认对本机所有程序开放，而且“AI 连接”开关关掉也挡不住，和“默认关、用户点头才开”的原则不合；A 的代价只是第一次点一下，ws 和设置页都把路指清楚。

- **方案 A：和“允许 AI 连接”共用一个开关**，改名“允许 AI 助手和命令行连接”，默认照旧关。
  - 设置 → AI 助手里加“命令行”一栏：一个“启用”按钮，打开开关（和给客户端点“连接”自动打开开关一样）、便携版加进 PATH、可选装 Skill。
  - 关着时 `ws` 退出码 4，告诉用户在哪打开。
  - 好处：一个开关说清楚“谁能问 Win顺”。坏处：装好后第一次要点一下。
- **方案 B：命令行单独一个开关，默认开**，装好就能用，和 es 一样。
  - 但这等于任何能跑命令的 AI 默认就能搜文件名、读文档文字（只读、按用户自己的权限，仍然遵守“不让 AI 看到的文件夹”）。
  - 而且管道上分不清来的是 ws 还是冒充 ws 的程序，所以“AI 连接”开关关着也挡不住只读的搜索。要选 B，就得接受只读搜索默认对本机程序开放。

## 8. 安装和 PATH

- **`ws.exe` 单独放在 `<安装目录>\bin\`，只把这个文件夹加进 PATH**（和 VS Code 的 `bin\code.cmd` 一样）。
  - 绝不能把安装目录本身加进 PATH：里面有 Qt6*.dll、pdfium.dll，会被别的程序按 PATH 找 DLL 时误用。
  - 也因此 ws 不能依赖 Qt，必须是自带一切的单个 exe。
- **安装包**：“附加任务”页加一项“添加 ws 命令（在命令行里搜索）”，默认勾上。“所有用户”安装写系统 PATH，“只为我”安装写用户 PATH（Inno 的 `HKA` 根键 + `ChangesEnvironment=yes`），卸载时只删自己加的那一项。
- **便携版**：设置 → AI 助手 → 命令行里的按钮把 `bin` 加进用户 PATH（写 `HKCU\Environment`，再广播 `WM_SETTINGCHANGE`）。
- 不用 App Paths：cmd 和 PowerShell 找命令不看它。
- **已经开着的终端看不到新 PATH**：设置页和安装完成页写明“在新打开的终端里输入 ws 试试”；VS Code 的终端要重开 VS Code。设置页显示当前状态（在 PATH 里 / 不在 / 有别的 ws 抢在前面，后者列出那个程序的路径）。

## 9. Win顺 这边要改的

改动不大，都在 `src/mcp/`：

1. **认出命令行会话**：`initialize` 的 `clientInfo.name` 是 `ws` 时，会话标成命令行。名字可以冒充，但这里只影响下面几条格式和上限，不影响权限。日志和设置页的“打开记录”里显示“命令行”。
2. **命令行会话的上限放宽**：`search_files` / `list_folder` 等对命令行会话最多 50 000 条（MCP 仍是 200）。一行最多 16 MB（`kMaxLineBytes`）：50 000 条精简过的条目约 10 MB，留了余量。真有人要更多，再做分批发送。
3. **结果不再重复一遍**：MCP 的回答把结果放进 `structuredContent`，还在 `content` 里放一份同样的 JSON 文字。命令行会话只放 `structuredContent`，大结果省一半。
4. **不读盘拿大小**：现在 `fileItem()` 对每个结果都 `sizeOf(path)` 读一次盘。列几千个文件就是碰几千次盘（全机械硬盘的用户最怕这个，也正是论坛用户嫌“伤硬盘”的地方）。工具加参数 `size`（默认 true，MCP 行为不变），ws 只在 `-l` 或按大小排序时才要。
5. **管道客户端拆出不带 Qt 的一版**：`pipe::Client` 现在用 `QByteArray`，改成 `std::string` 的核心，中继在外面套一层，两边共用。`userSid()`、`defaultName()` 本来就是纯 Win32。
6. **“在不在运行”“拉起来”抽出来共用**：中继 `main.cpp` 里的 `winShunRunning()`、`runAutostartTask()`、`winShunExe()`、`startWinShun()` 本来就是纯 Win32，挪到 `src/mcp/Launch.{h,cpp}`，中继和 ws 都用。
7. **名字搜索不排在内容搜索后面**：
   - 现在 `ToolHost` 只有一个引擎，所有搜索排一个队，MCP 客户端和 ws 也共用这个队。
   - Claude Code 会同时跑几个命令：一个 `ws grep --timeout 5s` 会让同时发出的 `ws find`（本来几毫秒）跟着等 5 秒。
   - 改成两个引擎：名字搜索一个，内容搜索一个（第一次内容搜索时才建）。同类的照旧排队；名字搜索每个几毫秒，排队不明显。
   - 隐私要求的是“同一套 Policy”，不是“同一个引擎”，所以这样拆不影响第 2 节。MCP 客户端也一起受益。

## 10. ws.exe 的代码

放在 `src/cli/`，C++20（和项目一致），不带 Qt，静态 C 运行库（和 `WinShunSearch.exe`、runner 一样），启动约 10 ms。AI 会一次对话调很多次，启动快很要紧。

```
src/cli/
  main.cpp        入口：解析参数 → 找子命令 → 运行 → 退出码
  Commands.cpp/h  子命令表：名字、一句话说明、参数定义、帮助文字、把参数变成工具调用、把结果打印出来（表驱动，加命令只加一项）
  Args.cpp/h      参数解析：--长名 / -短名 / -n20 / -- / /?；拼写建议（编辑距离）
  Session.cpp/h   连管道、initialize、tools/call、等回答；Ctrl+C（SetConsoleCtrlHandler）发 notifications/cancelled 再退出
  Output.cpp/h    控制台还是重定向、编码（第 4 节）、颜色、高亮、-l 的对齐
  SKILL.md        给 AI 的说明，编进资源；ws skill 打印它
```

- JSON 用 runner 现成的 `src/plugin/runner/Json.h`（无 Qt）：ws 和 runner 都是 /MT，直接链 `wsplugin_lite`。
- 和中继共用的管道客户端核心、`Launch`：**源文件分别编进 ws 和中继，不做成一个静态库**。原因：ws 用静态 C 运行库（/MT），中继带 Qt 用 /MD，同一个库混着链接会报 LNK2038。
- 错误一律带着退出码往上返回（一个小的 `Result<T>`：值，或“码 + 给人看的一句话”），不用异常，也不在深处直接 `exit()`。
- 字符串内部一律 `std::wstring` / UTF-16，只在输出时按第 4 节转码。参数从 `CommandLineToArgvW` 取，不用 `argv`（ANSI 会丢中文）。
- 帮助文字和 SKILL.md 里提到的每个参数，测试都核对它在参数表里真的存在，免得两边说法对不上。

## 11. Skill（用户真正要的东西）

- **随 ws.exe 带一份 SKILL.md**（`ws skill` 打印），内容从 `ToolHost::instructions()` 改写：
  - 什么时候用：找文件、按内容找、读 docx/pdf 的文字、看最近改了什么；
  - 每个子命令一行例子；
  - 查询语法（拼音、`ext:`、`!`、引号）；
  - 要求加引号、用 `-n` 控制数量、按退出码判断。
  - 写成英文，控制在约 60 行以内，description 一句话写清触发条件。只在用到时才加载，这就是比 MCP（每次对话都要带上全部工具定义）省上下文的地方。
- **装进去**：
  - `ws skill --install claude` 写到 `%USERPROFILE%\.claude\skills\winshun\SKILL.md`；
  - 设置 → AI 助手的客户端列表里，Claude Code 那一行除了“连接”（MCP）再加“装 Skill”；
  - 别的支持 Skill 的客户端，查到它们的目录约定后再加。
- MCP 照旧保留：Claude Desktop 这类不能跑命令的客户端只能用 MCP。两条路背后是同一套工具。

## 12. 和 es 对照：第一版不做的

| es 有的 | 为什么先不做 |
|---|---|
| `-r` 正则、`-i` 大小写、`-w` 整词 | Win顺 的查询语法里没有，要先加到搜索引擎里（启动器也得用上），不能只给命令行开后门 |
| `-get-folder-size`、按大小排序很快 | Everything 的索引存了大小；Win顺 的索引每项 17 字节，不存大小，要读盘。`--sort largest` 能用，但大结果会慢 |
| `-export-efu/m3u` | 用的人少；要的话 `-0`、`--json` 加管道能做 |
| journal（`-j`） | `ws recent` 覆盖最常见的“最近改了什么” |
| `-instance`、`-ipc1/2/3`、`-save-settings` | 不需要 |

以后可以加的：`ws completion powershell`（Tab 补全）、`ws show <查询>`（打开启动器并填好查询）、`ws run <插件> <命令>`（插件命令，AiBridge 已经把它们做成了工具）、`--csv`。

## 13. 测试

- `tst_cli`：仿照 `tst_mcp` 驱动真的 `WinShunMcp.exe` 的做法，驱动真的 `ws.exe`：
  - 在临时索引上测每个子命令的输出、`--json`、`-0`、退出码；
  - 在线、没运行（`--no-start`）、开关关着（有互斥量没管道：不跑计划任务）、索引加载中；
  - Ctrl+C 取消。
- 编码一轮（第 4 节的矩阵），把实际字节读出来对比，不靠眼睛看。
- 性能：在真实索引（330 万项）上量 `ws find 报告` 从启动到退出的时间，目标 50 ms 以内；`-n 0` 列几万条的时间和峰值内存。
- 真实 AI 走一遍：Claude Code 装好 Skill，让它按内容找一个文档并读出文字。

## 14. 顺序

1. 服务端的改动（第 9 节，含名字搜索和内容搜索分开两个引擎）+ ws 的 `find` / `grep` / `text` / `status` + 编码 + 退出码 + 测试。
2. 安装包和 PATH、设置页“命令行”一栏、Skill 和“装 Skill”按钮。
3. `recent`、`ls`、`open`、`clip`、帮助的打磨。
4. 补全、`show`、插件命令（看有没有人要）。
