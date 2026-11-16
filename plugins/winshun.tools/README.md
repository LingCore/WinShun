# 小工具 · Tools

随 Win顺 安装的 C++ 常驻插件（`WinShunTools.exe`，用 `winshun.h` 单头文件 SDK 写的，源码在仓库的 `src/tools-plugin/`）。它跑在**严格沙箱**里：低完整性、不能起子进程、不碰剪贴板——Enter 复制是经由 Win顺 做的。

| 输入 | 得到 |
|---|---|
| `= 1+2*3`、`= 2^10`、`= sqrt(2)`、`= 80*15%`、`= 5!`、`= 0xff` | 结果；整数还给出十六进制、二进制、八进制 |
| `1+2*3`（不带 `=`，含运算符即可） | 结果直接混进"全部"的列表 |
| `= 12 inch to cm`、`12 inch to cm`、`100 f to c`、`1.5 GB in MB`、`3 斤 换成 kg`、`2 小时 to 分钟` | 换算；数据大小同时给出按 1024 和按 1000 的 |
| `= 12 inch` | 换成本类的每种单位 |
| `#ff8800`、`rgb(255, 136, 0)`、`hsl(32, 100%, 50%)`、`color #f80` | HEX、RGB、HSL、HSV 写法，行首是色块 |
| `ts`、`ts 1700000000`、`ts 1700000000.5`、`ts 2024-01-02 15:04:05`、直接输入 `1700000000` | 时间戳 ⇄ 日期（秒、毫秒、微秒、纳秒都认），本地（带时区）、UTC、ISO 8601 |
| `uuid ` | 新的 UUID v4，四种写法 |
| `b64 你好`、`b64 5L2g5aW9` | Base64 编码 / 解码 |
| `md5 文字`、`sha1 文字`、`sha256 文字`、`sha384 文字`、`sha512 文字`、`sha256 D:\下载\setup.exe`、`\\服务器\共享\setup.exe` | 哈希（文件最大 64 MB） |

文件结果（搜索框里的文件行）的右键菜单里有**复制 SHA-256**：点一下，这个文件的摘要就进了剪贴板（文件最大 64 MB）。**复制 MD5**、**复制 SHA-1** 在本插件卡片上打开，不想要的项也在那里关掉。

每一行按 Enter 复制。单位换算认英文、符号和中文名：mm cm m km inch ft yd mile 里 尺 寸、mg g kg t lb oz 斤 两、°C °F K、B KB MB GB TB（KiB MiB GiB 也认，严格按 1024）、ms s min h day week、m² km² ha 亩 acre、mL L m³ gal cup。KB/MB/GB 按 Windows 习惯算 1024，结果里同时给出按 1000 的那一行。

**给 AI 用**：设置 → AI 助手 → "AI 可以使用插件"打开后，这些命令成为 MCP 工具 `plugin_winshun_tools_calc` 等；AI 只能查询，不会替你复制。

---

A resident C++ plugin shipped with Win顺 (`WinShunTools.exe`, written on the single-header SDK `winshun.h`; source in `src/tools-plugin/`). It runs in the **strict sandbox** (low integrity, no child processes, no clipboard; Enter copies through Win顺): `=` calculator and unit conversion (also without the keyword; `5!!`, `50%`, `0xff` too), `color` notations (`#ff8800` works on its own), `ts` timestamps (seconds, milliseconds, microseconds, nanoseconds), `uuid`, `b64`, `md5` / `sha1` / `sha256` / `sha384` / `sha512` of text or a file; a file result's right-click menu has **Copy SHA-256**, which puts that file's digest on the clipboard (up to 64 MB); **Copy MD5** and **Copy SHA-1** are switched on, and unwanted items off, on the plugin's card. With "AI may use plugins" on, the commands are MCP tools (`plugin_winshun_tools_calc` …).
