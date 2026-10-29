# Win顺 插件开发

Win顺 的插件给搜索框加关键字命令、把结果混进列表。界面和插件都以普通用户权限运行，读取 NTFS 索引的是独立服务；插件崩了、慢了都不拖累主程序。三种写法，一份协议：

第一次做插件，可以在“设置 → 插件 → 插件库”点“创建插件”，从能运行的模板开始。“复制 AI 开发提示”会复制协议、SDK、验证和投稿要求；创建的目录也带有同一份 [DEVELOPMENT.md](../src/plugins-sdk/DEVELOPMENT.md)。插件库和[社区投稿入口](https://github.com/LingCore/WinShun-plugins)共用现有分发体系，安装后默认关闭。

| 写法 | 要写什么 | 适合 |
|---|---|---|
| **零代码** | 只有 `plugin.json` | 关键字开网址、开程序、跳文件夹、保存的搜索、固定清单 |
| **一次性脚本**（`once`） | 任何语言；查询从环境变量进，结果打印到 stdout | 几十行的小工具，偶尔用 |
| **常驻**（`resident`） | 任何语言；stdio 上一行一条 JSON-RPC | 每键都查的、要缓存的、PowerShell（它冷启动慢） |

随 Win顺 装好的插件在 `<安装目录>\plugins\`（默认全部关着，设置 → 插件 里打开）：`winshun.tools`（计算器、单位换算、颜色、时间戳、UUID、Base64、哈希；C++）、`winshun.kill`（结束进程；PowerShell）、`winshun.bookmarks`（浏览器书签；Python 一次性）、`winshun.translate`（翻译；Python 常驻，要自己填 API 密钥）、`winshun.semantic`（按意思找：`? 一句话` 按文档讲的内容找文件，用你选的嵌入模型——本机的 Ollama 或云服务的密钥；C++ 常驻，用 `winshun:content` 拿 Win顺 已经读出的文字）、`winshun.localsend`（局域网传输：同一网络里的手机、电脑互传文件和文字，兼容 LocalSend；`传` 或 `ls`，或文件右键菜单的"用局域网发送…"；C++ 常驻、eager，用到 `network:listen`、`notifications` 和 `files` 触发）、`winshun.web` 和 `winshun.go`（零代码）。每个都是一份能跑的示例，文件夹里有 README。

别人写的插件、以及分享你自己的插件：[WinShun-plugins](https://github.com/LingCore/WinShun-plugins)（设置 → 插件 → 插件库 里直接安装和更新：下载后先核对大小和 sha256，再走和压缩包一样的安装；也可以自己下载 `.wsplugin`，在“已安装”里从压缩包安装；投稿见那里的 CONTRIBUTING）。插件库只在打开这一页（十分钟内不重复取）、点“刷新”或安装时联网，上次的列表存在数据文件夹的 `plugin-index.json`。

English version: [plugins.en.md](plugins.en.md).

## 快速开始

插件 = 一个文件夹，放在 `%LOCALAPPDATA%\WinShun\plugins\<id>\`，里面一个 `plugin.json`。最快的路：设置 → 插件 → 高级 → 开发者模式 → **新建插件…**，选一种语言，模板连 SDK 文件一起生成好，打开开关就能用；改了脚本会自动重新加载，"消息记录"里能看到每一条往来。

下面三个例子各自五分钟。

### 1. 零代码：一个关键字开一个网址

`%LOCALAPPDATA%\WinShun\plugins\me.npm\plugin.json`：

```json
{
  "manifest": 1,
  "id": "me.npm",
  "name": "npm",
  "version": "1.0.0",
  "commands": [
    {"id": "npm", "keyword": "npm", "title": "搜 npm 包", "run": {"url": "https://www.npmjs.com/search?q=%s", "home": "https://www.npmjs.com/"}}
  ]
}
```

输入 `npm lodash`，Enter 打开搜索页；只输入 `npm `（带空格）打开首页。`run` 还可以是 `{"program": "C:\\…\\code.exe", "args": "%s"}`、`{"folder": "%USERPROFILE%\\Downloads"}`、`{"search": {"query": "ext:pdf %s", "scope": "files"}}`（等于在搜索框里输入它），或 `{"items": [{"id": …, "title": …, "open": {"path": …}}]}`（固定清单，输入的词用来筛选）。

### 2. Python 一次性：几行脚本

`me.hello\plugin.json`：

```json
{
  "manifest": 1,
  "id": "me.hello",
  "name": "Hello",
  "version": "1.0.0",
  "runtime": {"mode": "once", "program": "python", "script": "main.py"},
  "commands": [{"id": "hi", "keyword": "hi", "title": "打招呼", "hint": "名字"}]
}
```

`me.hello\main.py`（把 `winshun.py` 从 `src/plugins-sdk/` 或任何随包插件的文件夹复制过来）：

```python
import winshun

q = winshun.query()                      # q.text 是关键字后面的文字
winshun.reply([
    winshun.item("hi", f"你好，{q.text}", "按 Enter 复制", copy=q.text),
    winshun.item("len", f"{len(q.text)} 个字", valid=False),
])
```

每次查询起一个 Python 进程（几十毫秒）。不用 SDK 也行：从环境变量 `WS_QUERY` 读，往 stdout 打印 `{"items": [...]}`、一个数组，或者纯文本行（每行一条，Enter 复制）。

### 3. PowerShell 常驻：一个进程一直开着

`me.ps\plugin.json`：

```json
{
  "manifest": 1,
  "id": "me.ps",
  "name": "PS",
  "version": "1.0.0",
  "runtime": {"mode": "resident", "program": "powershell", "script": "main.ps1"},
  "commands": [{"id": "ps", "keyword": "ps", "title": "PowerShell"}]
}
```

`me.ps\main.ps1`（**带 BOM 的 UTF-8**；`WinShun.psm1` 复制到旁边）：

```powershell
Import-Module "$PSScriptRoot\WinShun.psm1"
Start-WinShunPlugin -Name 'me.ps' -OnQuery {
    param($q, $plugin)
    @(New-WinShunItem -Id 'hi' -Title "你好，$($q.text)" -Copy $q.text)
}
```

第一次用时 PowerShell 要几百毫秒启动，之后每次几毫秒；10 分钟不用自动退出。C++ 的写法见 `src/plugins-sdk/winshun.h` 开头和 `src/tools-plugin/`。

## SDK

三个文件在仓库的 `src/plugins-sdk/`，"新建插件…"会复制进插件文件夹；随包插件的文件夹里也有。它们只是把协议包起来，不用也可以。

| 文件 | 一次性 | 常驻 |
|---|---|---|
| `winshun.py` | `query()`、`settings()`、`item(...)`、`reply(items)`、`is_action()` / `action_request()` / `effects(...)` | `serve(on_query, on_action=None, on_settings=None)`；处理函数收到 `(q, plugin)`，`plugin.partial()`、`plugin.log()`、`plugin.notify_user()`、`plugin.toast()`、`plugin.progress()`、`plugin.refresh()`、`plugin.tool(name, **args)`、`plugin.settings` |
| `WinShun.psm1` | `Get-WinShunQuery`、`Get-WinShunSettings`、`New-WinShunItem`、`Send-WinShunReply`、`Send-WinShunEffects` | `Start-WinShunPlugin -OnQuery {…} -OnAction {…}`、`Send-WinShunPartial`、`Write-WinShunLog`、`Send-WinShunNotify`、`Send-WinShunToast`、`Send-WinShunProgress`、`Invoke-WinShunTool` |
| `winshun.h` | `winshun::onceQuery()`、`onceSettings()`、`once(items)`、`onceAction()` / `onceEffects()` | 派生 `winshun::Plugin`，重写 `query()`（和 `queryResult()`、`action()`、`settingsChanged()`、`initialized()`、`shuttingDown()`），`serve()`；成员 `partial()`、`log()`、`notifyUser()`、`toast()`、`progress()`、`refresh()`、`tool()`、`settings()`；这些成员可以在别的线程里调（发送有锁） |

effects 的辅助：Python `winshun.hide() / keep() / notify(text) / copy(text) / set_query(text) / open_path(path) / open_url(url) / replace(items)`；PowerShell `New-WinShunEffect -Hide / -Keep / -Notify / -Copy / -SetQuery / -OpenPath / -OpenUrl / -Replace`；C++ `winshun::effect::hide()` 等。

## manifest（plugin.json）字段

顶层：

| 字段 | 必需 | 说明 |
|---|---|---|
| `manifest` | 是 | 架构版本，现在是 `1`；比 Win顺 认识的新就装不上 |
| `id` | 是 | `作者.名字`，只许 `[a-z0-9._-]`，和文件夹名一致 |
| `name` | 是 | 显示名；字符串，或 `{"zh": …, "en": …}` 按界面语言取（下同"文字"） |
| `version` | 是 | 如 `1.2.0`；同 id 装更低版本时会问 |
| `description` | 否 | 一句话（文字） |
| `author`、`homepage`、`license` | 否 | 卡片上显示；`homepage` 是链接 |
| `icon` | 否 | 插件文件夹里的 PNG / JPG / ICO（深色主题有 `名字.dark.png` 就用它；**不支持 SVG**）；没有就用第一个命令的图标 |
| `minWinShun` | 否 | 低于它不启用 |
| `runtime` | 脚本 / 常驻必需 | 见下 |
| `permissions` | 否 | 见"权限" |
| `commands` | 是 | 至少一个 |
| `settings` | 否 | 见"设置项" |
| `ai` | 否 | `{"expose": true}`：允许命令成为 AI 的工具（用户还要在设置里打开），见"给 AI 用" |

`runtime`：

| 字段 | 说明 |
|---|---|
| `mode` | `once` 或 `resident` |
| `program` | `python`、`pwsh`、`powershell`、`node`，或插件文件夹里的 exe（相对路径）。找不到解释器时卡片上写"需要 Python 3"并给下载链接；找到的用不了时写明是哪个文件、为什么（见下面"找不到 Python"） |
| `script` | 入口脚本，默认 `main.py` / `main.ps1` / `main.js` |
| `args` | 附加参数（数组） |
| `cwd` | 工作目录，默认插件文件夹 |
| `env` | 额外环境变量（`WS_*` 不能覆盖） |
| `encoding` | stdout 的编码：`utf-8`（默认）或 `oem`（Windows PowerShell 5.1 不设编码时的输出；模板已经把输出设成 UTF-8，不需要） |
| `timeoutMs` | 一次查询的上限：常驻默认 1500，一次性默认 3000，最大 10000 |
| `debounceMs` | 一次性插件的防抖，默认 120（每键都起进程太浪费） |
| `idleExitSec` | 常驻插件空闲多久退出，默认 600；`0` 不退 |
| `start` | 常驻：`lazy`（默认，第一次用到时起）或 `eager`（Win顺 启动 10 秒后预起；推送型插件用） |
| `memoryMB` | 作业对象的内存上限，默认 512，最大 2048 |
| `sandbox` | `normal`（默认：中完整性，和你一样）或 `strict`（低完整性：写不了用户目录，只能写自己的数据文件夹；给不需要写文件的 exe 插件） |

`commands[]`：

| 字段 | 说明 |
|---|---|
| `id` | 命令 id，插件内唯一；设置项的键和 AI 工具名用它 |
| `title`、`description`、`hint` | 显示名、一句话、关键字模式下输入框的占位文字（都是文字） |
| `icon` | 见"图标" |
| `keyword` | 字符串或数组。输入**关键字 + 空格**进入关键字模式；单独输入关键字时列表第一行是这条命令的提示行（Enter 或 Tab 进入，↓ 到名字结果）；输入两个字以上、是关键字开头的（`uu` 对 `uuid`），也在应用之后、文件之前多一行（光标不动，按 Tab 就补全成 `uuid `；最多两行，候选太多就不出）；用户可以在设置里改 |
| `triggers` | 关键字以外的触发方式：`{"regex": "…", "minLength": 1, "maxLength": 64}`（整个输入匹配时，结果推进"全部"；`match` 是捕获组）、`{"push": {"minLength": 2, "maxItems": 3}}`（每键都发，常驻 + eager 才合适）、`{"files": {"kind": "any", "max": 1000, "extensions": ["jpg"], "title": {"zh": "用局域网发送…", "en": "Send over LAN…"}}}`（文件、文件夹结果的右键菜单里多一项 `title`，多选时对整个选择；`kind` 是 `file` / `folder` / `any`，`max` 默认 20、最多 1000，`extensions` 只管文件。选了它：一次 `action`，`trigger` 是 `files`、`action` 是 `files`、`files` 是这些完整路径，没有 item） |
| `mode` | `list`（默认）或 `instant`（单独输入关键字就执行，不列结果） |
| `actions` | `[{"id": "kill", "title": "结束", "key": "Enter"}, …]`：顺序决定 Enter / Ctrl+Enter / Shift+Enter；右键菜单列全部；不写就只有一个 `default` |
| `run` | 零代码专用：`url`（`%s` 是输入的文字，按网址规则编码；`home` 是单独输入关键字时开的）、`program`（`%VAR%` 会展开；`args` 里 `%s`）、`folder`、`search`（`{"query": "ext:pdf %s", "scope": "all | files | folders | content"}`）、`items`（item 数组，`open.path` 里的 `%VAR%` 会展开） |
| `cache` | `{"seconds": 60}`：同一命令同一文字的结果缓存多久 |
| `ai` | `{"description": "…", "parameters": {JSON Schema}}`：作为 AI 工具时的描述和参数；不写就用 `description` 加一个 `query` 字符串参数 |

未知字段忽略。校验错误是一句人话（"commands[0].keyword 含空格"），设置页原样显示。

## 协议

### 一次性（once）

每次查询起一个进程，stdin 关着。

进（环境变量）：`WS_QUERY`（关键字之后的文字）、`WS_TEXT`（整个输入）、`WS_COMMAND`、`WS_TRIGGER`（`keyword` / `regex` / `push`）、`WS_KEYWORD`、`WS_MATCH`（正则捕获组，JSON 数组）、`WS_PLUGIN_DIR`、`WS_DATA_DIR`（插件可写的数据文件夹 `<id>.data`）、`WS_SETTINGS`（JSON）、`WS_LOCALE`（`zh` / `en`）、`WS_THEME`（`light` / `dark`）、`WS_VERSION`。Enter 落到没有声明式 `open` 的行时再起一次进程做动作，另有 `WS_ACTION`（动作 id）和 `WS_ITEM`（那条 item 的 JSON）；从右键菜单选了 `files` 触发时 `WS_TRIGGER` 是 `files`、`WS_ACTION` 是 `files`，`WS_FILES` 是路径的 JSON 数组（整个数组最多约 32000 个字符，再多 Win顺 提示少选一些）。

出（stdout，UTF-8）：一个 JSON 对象 `{"items": [...], "rerun": 秒, "cache": 秒}`，或一个数组（就是 items），或**纯文本行**（每行一条，Enter 复制）。`rerun`：列表还在显示时（没有新的按键、窗口没收起）过这么多秒再问一次同样的查询，结果原位换上，键盘停在原来那一行（行的 `id` 要稳定）；常驻 0.5–60 秒，一次性 1–60 秒。适合进度、设备列表这类自己会变的列表（常驻插件还可以发 `winshun/refresh`，让 Win顺 马上再问，见下）。动作时打印 `{"effects": [...]}`，什么都不打印就是收起窗口。stderr 进 `plugins.log`。退出码非 0 = 失败（底栏提示）。

### 常驻（resident）

进程一直开着，stdin / stdout 上跑 JSON-RPC 2.0，一行一条，UTF-8。**stdout 只许写协议消息**，调试信息写 stderr。

Win顺 → 插件：

```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","winshun":"0.7.0","pluginDir":"…","dataDir":"…","locale":"zh","theme":"dark","settings":{…}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"1","name":"…"}}
{"jsonrpc":"2.0","id":2,"method":"query","params":{"queryId":17,"command":"kill","trigger":"keyword","keyword":"kill","text":"note","raw":"kill note","limit":200}}
←（可选，任意条）{"jsonrpc":"2.0","method":"query/partial","params":{"queryId":17,"items":[…],"replace":false}}
← {"jsonrpc":"2.0","id":2,"result":{"items":[…],"rerun":0,"cache":0}}
{"jsonrpc":"2.0","method":"cancel","params":{"queryId":17}}         （新的按键来了；可以不理，Win顺 按 queryId 丢）
{"jsonrpc":"2.0","id":3,"method":"action","params":{"command":"kill","action":"kill","item":{…原样…},"text":"note"}}
← {"jsonrpc":"2.0","id":3,"result":{"effects":[{"type":"notify","text":"已结束 notepad"},{"type":"hide"}]}}
{"jsonrpc":"2.0","id":4,"method":"action","params":{"command":"send","action":"files","trigger":"files","files":["C:\\…\\a.jpg","D:\\相册"],"item":{},"text":""}}   （右键菜单的 files 触发）
{"jsonrpc":"2.0","method":"settingsChanged","params":{"settings":{…}}}
{"jsonrpc":"2.0","id":5,"method":"shutdown"}  ← {"jsonrpc":"2.0","id":5,"result":{}} 然后退出
```

插件 → Win顺：

```
{"jsonrpc":"2.0","id":"p1","method":"winshun/tool","params":{"name":"search_files","arguments":{"query":"*.pdf","limit":20}}}
{"jsonrpc":"2.0","method":"winshun/log","params":{"level":"info","text":"…"}}        进 plugins.log 和开发者模式的记录
{"jsonrpc":"2.0","method":"winshun/notify","params":{"text":"…"}}                   底栏提示
{"jsonrpc":"2.0","method":"winshun/notify","params":{"toast":true,"title":"…","text":"…","click":[{"type":"setQuery","text":"传 "}]}}   系统通知
{"jsonrpc":"2.0","method":"winshun/progress","params":{"queryId":17,"text":"正在读书签…"}}   "正在等…"那行的文字
{"jsonrpc":"2.0","method":"winshun/refresh","params":{}}   显示着的列表自己变了：马上再问一次
```

`winshun/refresh`：显示着的列表自己变了（比如来了一个请求、一次传输结束了），不必等 `rerun` 的秒数，Win顺 马上再问一次同样的查询；两次查询之间至少隔 0.1 秒，连着发几条只多问一次（正在回答时来的，答完再问）。只在上一次回答要了 `rerun`、列表还开着时起作用，别的时候什么都不做；旧版 Win顺 不认识它，也不碍事。先写好下一次查询要读的状态，再发它。C++ SDK 是 `refresh()`，Python 是 `plugin.refresh()`。

系统通知（`toast`）要 `notifications` 权限，没有就当底栏提示。每个插件最多每 2 秒一条，等着的只留最新的；正文截到 200 个字。标题前面 Win顺 总会加上插件的名字，插件去不掉，免得被当成 Win顺 自己的通知：随 Win顺 装的插件是「局域网传输：…」，自己装的插件不管叫什么都套上「插件「名字」：…」；名字看不出字来（只有空格、填充符这类）时改用 id。名字最多 24 个字，连名字一共 63 个字；没有正文就拿标题当正文。看不见的字符（控制字符、改文字方向的字符、零宽字符、填充符）会被去掉。`click` 是点通知时做的 effects，只认三种、每种一个，别的（网址、`notify`、`hide`、`keep`、`replace`）都丢掉，日志里记一笔：`setQuery` 只能填这个插件自己的关键词（会先打开搜索框）；`open` 只能打开盘符开头的文件或文件夹（`C:\…`，不带参数和工作目录；网址、`\\服务器\共享` 这类路径不行，映射成盘符的网络盘照样能开），照样受 processes 权限约束；`copy` 要 `clipboard:write` 权限。Win顺 自己的通知还在时，插件的通知先等它过去（一般 6 秒，最多 8 秒；等了 60 秒还没轮到就不发）；点哪条通知只做那条自己的事，刚被别的插件的通知顶掉时点的，什么都不做。通知会留在操作中心：**别把文件名、聊天内容这类私人的字写进去**。

C++ SDK 的 `initialized()` 在回答完 `initialize` 后调一次：`eager` 插件在这里开始干活（监听、扫描），不必等第一次查询。`shuttingDown()` 在回答 `shutdown` 之前调：Win顺 一收到回答就结束进程，所以没写完的文件要在这里删掉、对方要在这里告诉；动作要快（Win顺 最多等 2 秒左右）。

`winshun/tool` 和 Win顺 给 AI 的 MCP 工具同名同参数：`search_files`、`search_content`、`recent_files`、`list_folder`、`get_file_info`、`open_item`、`show_in_folder`、`clipboard_history`、`clipboard_get`、`read_file_text`、`index_status`，另有只给插件的 `list_documents`（内容索引里有文字的文件，分页，带一个文件变了就变的 `stamp`）；按 manifest 的权限放行（`winshun:search`、`winshun:open`、`clipboard:read`；`read_file_text` 和 `list_documents` 要 `winshun:content`；`index_status` 总可以）。结果和 MCP 的 `structuredContent` 一样。

超时：每次查询一个计时器（`timeoutMs`），到点这次的结果作废；常驻插件连续 3 次超时当挂死，结束后退避重启（1 → 5 → 30 秒）；5 分钟内崩 3 次就停用，卡片上红字说明。空闲 `idleExitSec` 后收到 `shutdown`。每条响应最多 200 条结果，一行最多 16 MB。

### item 字段

```json
{
  "id": "pid-1234",                  // 稳定 id：原位更新、动作的目标；AI 的 _run 工具也靠它
  "title": "notepad.exe",            // 必需
  "subtitle": "PID 1234 · 38 MB",
  "subtitleAway": "按 ↑ 选中，再 Enter 结束",   // 可选：键盘在别的行上时，第二行换成这句（从头显示，放不下截掉后面）；选中这一行又显示 subtitle。给自己冒到键盘上面、没被选中的行（比如列表开着时来的请求）说怎么过去；AI 看不到它。PowerShell 的 New-WinShunItem 里叫 -AwaySubtitle（-Sub… 仍是 -Subtitle），C++ 是 withSubtitleAway
  "icon": "file:C:\\Windows\\notepad.exe",   // 见"图标"
  "score": 80,                       // 0–100；推送混排时和应用同一个尺度，没给按 50
  "match": "notepad 记事本",          // 零代码 items 用它筛选；别的插件自己筛
  "autocomplete": "kill notepad",    // Tab 填进输入框
  "open": {"path": "C:\\…", "args": "…", "cwd": "…"} 或 {"url": "https://…"},   // 声明式默认动作：Win顺 自己打开（普通权限），不往返；url 写 "winshun:settings" 则打开 Win顺 的设置、定位到这个插件的卡片（"还没有填密钥"这类行用）。没有 processes 权限的插件：path 只能是文件夹或非程序/脚本的文件、url 只能是 http/https/mailto，且不能带 args、cwd；否则不执行，底栏提示（见"权限"）
  "copy": "1234",                    // Ctrl+C 复制什么；没有就 title
  "preview": {"text": "…"},          // 悬停提示
  "actions": [{"id": "kill", "title": "结束", "key": "Enter"}],   // 覆盖命令级的
  "valid": true,                     // false：Enter 无效（提示行）
  "data": {"pid": 1234}              // 不透明，随 action 原样回来；AI 看不到它
}
```

effects（动作的回答，按顺序执行）：`hide`（收起窗口，默认）、`keep`（留着）、`setQuery {text}`、`notify {text}`、`copy {text}`、`open {path, args, cwd | url}`（和 item 的 `open` 同款，同样受 processes 权限约束；`url` 同样认 `winshun:settings`）、`replace {items}`（原地换成这些结果，做二级列表）。

### 长文字结果（#117）

标题本身就是内容的行（翻译、计算结果：没有 `open`、`search`、`setQuery`，写了 `copy`，`valid` 不是 false）若有一行放不下，列表下面会多出一块文字区，把当前行的全文换行显示（最多约 6 行，更长的在区内滚动）。区的高度按这批结果里最长的那条定，所以上下移动选择时窗口不会忽高忽低，只在结果变化时才变；窗口在屏幕可用区域内增高。选中的是别的行（比如打开网址的行）时，区是空的。文字可用鼠标选中，有选中时 Ctrl+C 复制选中的部分。会打开东西的行标题只是名字，照旧截断。不用加字段，行高、序号和键盘操作都不变；Enter、Ctrl+C 仍按 `copy`。

### 搜索面板的日期、大小与筛选（#88）

item 可选提供 `modified`、`created`（带时区的 ISO 8601 字符串或 Unix 秒）和 `size`（非负整数，字节；`0` 是空文件）。未提供或无效的字段留空；日期和大小未知的旧插件条目仍然显示。面板可显示、排序这些列，未知日期和大小排在最后。类型取标题扩展名，所在文件夹取 `open.path` 的父目录，来源是插件名称。

常驻查询的 `params.filters`、一次性查询的 `WS_FILTERS` JSON 以及三个 SDK 的 `q.filters` 提供当前面板条件：

```json
{"modifiedFrom":"2026-01-01T00:00:00Z","modifiedTo":"2027-01-01T00:00:00Z",
 "minSize":1048576,"maxSize":null,"kinds":["documents"],"extensions":["pdf","docx"],"folder":"C:\\Docs"}
```

时间下界包含、上界不包含，大小上下界都包含；`null`、空数组、空文件夹表示没有该项限制。普通搜索的 filters 是 `{}`。插件可按这些条件查询自己的数据源；Win顺 也会按返回的已知日期、大小、扩展名和本地打开路径筛选。Python 的 `item(..., modified=..., created=..., size=...)`、PowerShell 的 `New-WinShunItem -Modified ... -Created ... -Size ...`、C++ 的 `withModified / withCreated / withSize` 均可输出元数据。

`kinds` 可以包含多个类型，例如 `["pictures","videos"]`；对应的 `extensions` 是这些类型扩展名的并集。类型之间是 OR，仍与日期、大小、文件夹等条件同时生效；插件不要只读取数组第一项。

### 图标

`icon.png`（插件文件夹里的 PNG / JPG / ICO，相对路径；深色主题优先 `icon.dark.png`）、`ext:pdf`（扩展名的图标）、`file:C:\…`（某个文件的图标）、`app:<AUMID>`、`glyph:U+E8E5`（Segoe Fluent Icons 字形的编号；Win顺 自己用到的那些画成对应的 [Phosphor](https://phosphoricons.com) 图标，每台 Windows 都一样，其余用系统图标字体画，老 Win10 的字体里可能没有）、`color:#ff8800`（色块）、`data:image/png;base64,…`（≤ 64 KB）、`site:https://…`（网站自己的图标，和浏览器地址栏左边那个一样：Win顺 只向该网站首页取一次、存在本地、一个月更新；没取到前显示浏览器图标；网址的路径部分不会发出去）。插件文件夹以外的路径不认，SVG 不认。

## 权限

manifest 的 `permissions` 是数组：`["processes", "clipboard:read", {"files:read": ["%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Bookmarks"]}]`。打开开关时逐条列给用户，Windows 能强制的打勾，不能的标"仅声明"——说实话：

| 权限 | 含义 | Windows 能否强制 | 怎么做到 |
|---|---|---|---|
| （总是）不是管理员 | 插件永远以你的普通权限运行 | **是** | 令牌去掉 Administrators 组和特权；`normal` 中完整性，`strict` 低完整性；内存上限、作业对象，Win顺 退出时一起结束 |
| `processes` | 起子进程，或让 Win顺 打开程序 / 非网页链接 | **是** | 没有它：作业 `ActiveProcessLimit = 1`，`CreateProcess` 报 1816；且 Win顺 不替它执行开程序、脚本、非网页 URL 或带 args/cwd 的 `open`（item 和 open 效果、`open_item` 都一样，只能开文件夹、普通文件和 http/https/mailto，拦下时底栏提示）。有它：子进程在作业里，`open` 不受限 |
| `clipboard:read` / `clipboard:write` | 直接读写剪贴板 | **是** | 作业的 UI 限制；没有时 `GetClipboardData` 返回空（读到空或抛异常） |
| `ui` | 开自己的窗口、找别的窗口 | 部分 | 没有时作业限制桌面和 USER 句柄 |
| `winshun:search` / `winshun:open` / `winshun:history` | 调 Win顺 的工具 | **是** | `winshun/tool` 按权限放行 |
| `winshun:content` | 读 Win顺 从文档和文本文件里取出的文字（`list_documents`、`read_file_text`） | **是** | `winshun/tool` 按权限放行；不用自己解析 Word、PDF，也不用再读一遍硬盘 |
| `files:write` | 写你的文件 | 只在 `strict` 下 | `strict` 写不了用户目录，只能写 `<id>.data`；`normal` 和你自己一样能写，**只是声明** |
| `files:read` | 读哪些文件 | **否** | 只是告诉用户它读什么（中完整性本来就能读） |
| `network` | 联网 | **否** | Win顺 挡不住；卡片上写"Win顺 不能阻止它联网" |
| `network:listen` | 在局域网上开端口，让别的设备连进来：`{"network:listen": ["TCP 53317-53320", "UDP 53317"]}` | **否** | 只是告诉用户它开哪些端口；卡片上写"Win顺 不能阻止它开端口"。防火墙规则 Win顺 不替它加（随包的局域网传输由安装程序加） |
| `notifications` | 弹出系统通知（`winshun/notify` 的 `toast`） | **是** | 没有它，Win顺 把通知当底栏提示；有它也限速：每 2 秒最多一条 |

**新版本要更多权限，要再同意一次。** 你打开过的插件换了新版本（从插件库更新、装压缩包或文件夹，或者直接改了 `plugin.json`），只要它要的权限比你当初同意的多，就先停用：不启动，不回答查询，AI 用不了，也不发通知。卡片上会写明新版本多要了哪些权限。打开它的开关，会照常列出全部权限，并标出新增的几条；同意后照常使用，不同意就一直停着。权限没变或变少了，什么都不用做。随 Win顺 安装的插件跟着 Win顺 一起升级，照旧可信。只比较权限名：`files:read` 列的路径、`network:listen` 的端口变了，不会再问。

只安装你信得过来源的插件。

## 设置项

`settings[]`：`{"key", "type", "title", "hint", "default", "options"}`，`type` ∈ `text | password | bool | number | choice | folder | file | hotkey`。Win顺 用自己的控件画在卡片上，值存在 `WinShun.ini` 的 `[Plugin.<id>] Setting.<key>=`；**`password` 型存 Windows 凭据管理器**，不进 INI，不进日志（消息记录里打码）。`choice` 的 `options` 是字符串数组或 `[{"value": "deepl", "title": {"zh": …, "en": …}}]`。

插件看到的是合并后的 JSON（声明的默认值 + 用户的值）：一次性在 `WS_SETTINGS`，常驻在 `initialize` 的 `settings`，改了再发 `settingsChanged`。

## 给 AI 用

manifest 写 `"ai": {"expose": true}`，并在设置 → AI 助手 打开"AI 可以使用插件"后，每个命令成为 MCP 工具 `plugin_<id>_<command>`（id 里的点换成下划线：`plugin_winshun_tools_calc`）。AI 调用 = 一次 `query`（没运行就起它，等它 `initialize`），返回 `{items: [{id, title, subtitle, url?, path?, copy?}], returned}`，不含 `data`；`readOnlyHint` 为真。命令的 `ai.description`（英文，给模型看）和 `ai.parameters` 可以改描述和参数，默认是 `{query, limit}`。

动作默认**不**暴露；"允许 AI 执行插件动作"打开后再多一个 `plugin_<id>_<command>_run`（`destructiveHint`）：AI 先用查询工具拿到行的 `id`，再把同样的 `query` 和 `item_id`（可选 `action`）交给它，Win顺 重新查一次、找到那行、发 `action`，回 `{done, message, effects}`。每个插件卡片上还有"给 AI 用"开关；插件停用时 AI 调用会被告知"插件 X 已停用"。工具列表变了会通知已连接的 AI 客户端。

## 调试

- **开发者模式**（设置 → 插件 → 高级）：改了脚本自动重启常驻插件（底栏"已重新加载 X"）；"消息记录"记下每一条往来（时间、插件、方向 → ← stderr ·、方法、请求到响应的毫秒），点一条看整个消息；"打开 plugins.log"（`%LOCALAPPDATA%\WinShun\plugins.log`，插件的 stderr、协议错误、崩溃都在那）。
- **命令行**（源码编译出的 `build\release\wsbench.exe`，不用开界面）：

```powershell
wsbench --plugin D:\my.plugin --query "hi 你好" --runs 20                 # 校验 manifest、经 runner 起进程、打印结果 JSON 和耗时（冷启动 / 首次往返 / 稳态中位数），以及进程的完整性、是否在作业里
wsbench --plugin D:\my.plugin --query "hi 你好" --setting greeting=嗨    # 给声明的设置项一个值
wsbench --plugin D:\my.plugin --query "kill note" --action kill          # 对第一条结果执行动作，打印 effects
wsbench --launcher-sim --plugins D:\plugins "hi 你好" "#ff8800"          # 按启动器的合并逻辑逐键输入，看行怎么混进列表
wsbench --mcp-host --plugins D:\plugins                                 # 把插件当 AI 工具提供在测试管道上（WinShunMcp.exe --pipe …）
```

- 卡片上的状态行：就绪 / 运行中 · 内存 / 超时 n 次 / 崩溃，稍后重启 / 连续崩溃，已停用 / 无法使用：manifest 的错误。

## 常见问题

- **中文乱码。** 两头都按 UTF-8：Python 加了 `-X utf8`，自己打印就行；PowerShell 脚本**必须存成带 BOM 的 UTF-8**（Windows PowerShell 5.1 把没有 BOM 的 .ps1 按 ANSI 读，脚本里的中文全坏），并在开头设 `[Console]::OutputEncoding = [Text.Encoding]::UTF8`（`WinShun.psm1` 已经设了）。不想改脚本的老 PowerShell 脚本可以在 manifest 里写 `"encoding": "oem"`，但那只救得了 Latin 字符。
- **找不到 Python，或用 uv 装的 Python 一跑就"崩溃"。** Win顺 起的是 `python.exe` 本身。先看注册表里的登记（PEP 514，`HKCU/HKLM\Software\Python\<厂商>\<版本>\InstallPath`）：python.org 的 `PythonCore` 在前，uv 的 `Astral`、Anaconda 的等在后，各自挑最新的；再看 PATH 上的 `python.exe`（跳过 `WindowsApps` 里 Store 的转接程序，接着往后找）。插件的作业只许一个进程，凡是"自己再起一个 python.exe"的启动器都不能直接用，Win顺 替它们起真的那个：`py.exe` 不用；虚拟环境（venv、uv venv）的 `Scripts\python.exe` 换成它 `pyvenv.cfg` 里 `home` 下的 `python.exe`，并像启动器那样设 `__PYVENV_LAUNCHER__`，`sys.prefix` 和 site-packages 仍是这个虚拟环境的；uv 的 bin 文件夹里的 `python.exe`（`uv python install --default` 放到 PATH 上的那个）换成它指向的解释器。指向的解释器不在了（比如 uv 卸掉了那个版本），卡片上写出是哪个文件、为什么；装一个 python.org 的，或者 `uv python install` 重装一下就行，不用重启 Win顺（没找到时几秒后会再找）。
- **超时 / 没有响应。** 一次查询的预算是 `timeoutMs`（常驻 1500、一次性 3000，最大 10000）；慢的工作先发 `query/partial` 或 `winshun/progress`，大头放后台线程。起进程本身约 15 ms（Defender 的开销），Python 几十毫秒，PowerShell 几百毫秒——所以 PowerShell 用常驻。
- **常驻插件 print 了没反应。** 管道上 stdout 默认是块缓冲：每行后 flush（SDK 都做了；Python 还加了 `-u`）。
- **Enter 什么都不做。** 行有 `"valid": false`，或者命令没有 `actions` 而插件的 `action` 没回。有 `open` 的行由 Win顺 自己打开，不发 `action`。
- **想写文件。** 写 `WS_DATA_DIR`（`<id>.data`，卸载时问用户要不要一起删）；`strict` 沙箱的插件只能写那里。
- **图标不显示。** 只认 PNG / JPG / ICO，路径要在插件文件夹里；SVG 不支持。
- **关键字被占。** 和设置里"网页搜索"的关键字或别的插件撞了，用户设的赢，卡片上写明；改一个或让用户改。
- **插件目录能被改吗。** `normal` 插件就是以你的身份运行，你能改的它都能改——这是开发者模式"改了脚本自动重载"的前提。要防插件改自己，用 `strict`。
