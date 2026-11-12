# Win顺 插件开发

官方原生可选插件的最低宿主版本各自写在清单的 `minWinShun` 里（比如截图 0.18.0、资源管理器标签页 0.19.0），由主仓库发布脚本生成 `.wsplugin`，继续在现有 WinShun-plugins 仓库分发，条目为 `bundled: false`。内容提供者在“全部”里的前几行排在第一屏（前 3 条排在前 5 个名称结果之后），从 Win顺 0.19.0 起。官方原生包继续按该仓库的入口白名单分发；社区 exe/dll 包按本文的未审核原生类别收录（待配套版本发布）。已发布同版本包不得替换内容，改动时升插件版本，详见插件仓库的[维护者说明](https://github.com/LingCore/WinShun-plugins/blob/main/CONTRIBUTING.md#维护者)。

Win顺 的插件给搜索框加关键字命令、把结果混进列表。界面和插件一般都以普通用户权限运行（只有带 `elevated` 的插件，在 Win顺 以管理员身份运行、且你确认过时，才以管理员身份运行，见“权限”），读取 NTFS 索引的是独立服务；插件崩了、慢了都不拖累主程序。三种写法，一份协议：

第一次做插件，可以在“设置 → 插件 → 插件库”点“创建插件”，从能运行的模板开始。“复制 AI 开发提示”会复制协议、SDK、验证和投稿要求；创建的目录也带有同一份 [DEVELOPMENT.md](../src/plugins-sdk/DEVELOPMENT.md)。插件库和[社区投稿入口](https://github.com/LingCore/WinShun-plugins)共用现有分发体系。新插件点“安装并启用”，需要权限时先征求同意；拒绝后保留安装、保持关闭。更新保留原来的开关，本地压缩包安装不自动启用。

| 写法 | 要写什么 | 适合 |
|---|---|---|
| **零代码** | 只有 `plugin.json` | 关键字开网址、开程序、跳文件夹、保存的搜索、固定清单 |
| **一次性脚本**（`once`） | 任何语言；查询从环境变量进，结果打印到 stdout | 几十行的小工具，偶尔用 |
| **常驻**（`resident`） | 任何语言；stdio 上一行一条 JSON-RPC | 每键都查的、要缓存的、PowerShell（它冷启动慢） |

随 Win顺 装好的插件在 `<安装目录>\plugins\`（默认全部关着，设置 → 插件 里打开）：`winshun.tools`（计算器、单位换算、颜色、时间戳、UUID、Base64、哈希；C++）、`winshun.kill`（结束进程；PowerShell）、`winshun.go`（零代码）。

升级后原来开着的功能如果没能恢复，“已安装”页会保留原因，提供“重试恢复”和本地包安装，原来的设置和数据保留。

Win顺 自己的另外几个插件不随安装包，在插件库里，要用时装上（设置 → 插件 → 插件库；源码在 `src\<名字>-plugin\`，清单和 README 在其中的 `package\`）：`winshun.capture`（截图与录屏；C++，用 Win顺 自己的 Qt，见下面“有自己窗口的常驻插件”）、`winshun.cloudbackup`（云备份：把设置备份自动传到 WebDAV 或 GitHub 私有仓库，`cloud` 立即备份或恢复；C++ 常驻、eager，用 `winshun:backup` 让 Win顺 做出备份、恢复备份，自己只管传文件，GitHub #276）、`winshun.semantic`（按意思找：`? 一句话` 按文档讲的内容找文件，用你选的嵌入模型——本机的 Ollama 或云服务的密钥；C++ 常驻，用 `winshun:content` 拿 Win顺 已经读出的文字）、`winshun.localsend`（局域网传输：同一网络里的手机、电脑互传文件和文字，兼容 LocalSend；`传` 或 `ls`，或文件右键菜单的"用局域网发送…"；C++ 常驻、eager，用到 `network:listen`、`notifications` 和 `files` 触发）、`winshun.break`（休息提醒：每 20 分钟小休、隔一次大休，提前一个小条，到点调暗所有显示器显示倒计时；`休息` 或 `break`；C++ 常驻、eager，只要 `ui`，自己画窗口、休息时用键盘钩子拦按键）、`winshun.explorer-tabs`（资源管理器标签页：新开的文件夹窗口并成已开着的资源管理器窗口里的一个标签页，像 ExplorerTabUtility；按住 Ctrl+Shift 打开就还是新窗口；`标签页` 或 `tabs` 看状态、暂停；C++ 常驻、eager，要 `ui` 和 `elevated`（Win顺 以管理员身份运行、确认过时，也能并以管理员身份运行的窗口），用 ShellWindows 事件和 WinEvent 钩子，不往资源管理器里注入东西；关键词命令只为能暂停，主要功能不靠输入）、`winshun.translate`（翻译：`tr 句子`，用你自己的 DeepL 或 OpenAI 兼容接口，要自己填 API 密钥；C++ 常驻，`debounceMs` 300，先推一行"翻译中…"）、`winshun.bookmarks`（浏览器书签：`bm 词` 搜 Chrome 和 Edge 的书签；C++ 一次性）、`winshun.ocr-search`（图片文字搜索：`ocr 词` 搜图片、扫描 PDF 和文档内嵌图片里的文字，也在“内容”和“全部”里出结果；C++ 常驻，插件包的最低版本是构建它的 Win顺 版本）、`winshun.web`（网页搜索：不带程序的声明式插件，网站在插件设置里管理，见下面“网页搜索”）。以前随安装包、你又开着的，升级后 Win顺 自动从插件库装上，设置照旧。每个都是一份能跑的示例，有 README。

别人写的插件、以及分享你自己的插件：[WinShun-plugins](https://github.com/LingCore/WinShun-plugins)（设置 → 插件 → 插件库 里直接安装和更新：下载后先核对大小和 sha256，再走和压缩包一样的安装；也可以自己下载 `.wsplugin`，在“已安装”里从压缩包安装；投稿见那里的 CONTRIBUTING）。插件库在打开这一页（十分钟内不重复取）、点“刷新”或安装时联网；开着“自动更新”、又装了插件库里的插件时，每天还会在后台检查一次更新。上次的列表存在数据文件夹的 `plugin-index.json`。

**别人写的插件由作者维护。** 用着有问题，在插件卡片的“…”菜单点“向作者反馈问题”：打开作者仓库的 Issues（插件库登记的 `repo`；不是从插件库装的，按插件的 `homepage`：GitHub 仓库就开它的 Issues，别的网址就开那个网址）。插件没写主页时，“主页”打开插件库登记的仓库。Win顺 的 Issues 只管 Win顺 本身和官方插件（官方插件的“向作者反馈问题”就是 Win顺 的 Issues）。投稿时 `repo` 写你自己的仓库，并开着 Issues。

**网页搜索**现在统一为插件库中的 `winshun.web` 2.0（源码和清单在 `src/web-plugin/package`）。它使用声明式宿主能力，不运行额外进程；展开插件卡片管理网站和搜索建议。旧内置网页搜索与已启用零代码示例的配置一次性合并，卸载保留配置。新用户默认不启用，旧用户通过现有插件恢复流程安装，离线时保留数据并可重试。

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

零代码路径以 `%USERPROFILE%\Desktop`、`Documents`、`Downloads`、`Pictures`、`Music`、`Videos` 开头时，按 Windows 已知文件夹解析到当前设置的位置，支持迁移到其他盘、OneDrive 和网络目录，也支持后接子路径。`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup` 同样读取 Windows 当前的启动文件夹。`run.folder`、`run.program`、工作目录和固定清单的 `open.path`、含变量的副标题使用相同规则。直接填写的绝对路径按填写的地址打开。

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

`me.hello\main.py`（把 `winshun.py` 从 `src/plugins-sdk/` 复制过来，或者用 设置 → 插件 里的"新建插件…"选 Python 模板）：

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

三个文件在仓库的 `src/plugins-sdk/`，"新建插件…"会复制进插件文件夹；用到它的随包插件（`winshun.kill` 的 `WinShun.psm1`）文件夹里也有。它们只是把协议包起来，不用也可以。

| 文件 | 一次性 | 常驻 |
|---|---|---|
| `winshun.py` | `query()`、`settings()`、`item(...)`、`reply(items)`、`is_action()` / `action_request()` / `effects(...)` | `serve(on_query, on_action=None, on_settings=None, on_details=None)`；处理函数收到 `(q, plugin)`，`plugin.partial()`、`plugin.log()`、`plugin.notify_user()`、`plugin.toast()`、`plugin.progress()`、`plugin.refresh()`、`plugin.tool(name, **args)`、`plugin.settings` |
| `WinShun.psm1` | `Get-WinShunQuery`、`Get-WinShunSettings`、`New-WinShunItem`、`Send-WinShunReply`、`Send-WinShunEffects` | `Start-WinShunPlugin -OnQuery {…} -OnAction {…} -OnDetails {…}`、`Send-WinShunPartial`、`Write-WinShunLog`、`Send-WinShunNotify`、`Send-WinShunToast`、`Send-WinShunProgress`、`Invoke-WinShunTool` |
| `winshun.h` | `winshun::onceQuery()`、`onceSettings()`、`once(items)`、`onceAction()` / `onceEffects()` | 派生 `winshun::Plugin`，重写 `query()`（和 `queryResult()`、`action()`、`settingsChanged()`、`initialized()`、`shuttingDown()`、`appearanceChanged()`、`details()`），`serve()`；成员 `partial()`、`log()`、`notifyUser()`、`toast()`、`progress()`、`refresh()`、`tool()`、`settings()`；这些成员可以在别的线程里调（发送有锁；`tool()` 在别的线程里调时由 `serve()` 所在的线程把回答交给它，可以给一个 `canceled` 标志，插件要停时置位，调用马上返回错误） |

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
| `author`、`homepage`、`license` | 否 | 卡片上显示；`homepage` 是链接：要是 `http://` 或 `https://` 开头的完整网址，卡片“…”菜单和插件库条目上才会有「主页」（别的写法不当链接，也不会被打开）；不是从插件库装的插件，“向作者反馈问题”也按它找地方 |
| `icon` | 否 | 插件文件夹里的 PNG / JPG / ICO（深色主题有 `名字.dark.png` 就用它；**不支持 SVG**）；没有就用第一个命令的图标 |
| `minWinShun` | 否 | 低于它不启用 |
| `runtime` | 脚本 / 常驻必需 | 见下 |
| `permissions` | 否 | 见"权限" |
| `commands` | 是 | 至少一个 |
| `settings` | 否 | 见"设置项" |
| `ai` | 否 | `{"expose": true}`：允许命令成为 AI 的工具（用户还要在设置里打开），见"给 AI 用" |
| `trayMenu` | 否 | 托盘菜单里一个子菜单的标题（文字），见下面"有自己窗口的常驻插件" |
| `pictures` | 否 | `["pin", "edit"]`：剪贴板历史里的图片、结果里的图片文件，右键菜单的"贴到屏幕""编辑"交给这个插件，见下面"有自己窗口的常驻插件" |
| `settingsWindow` | 否 | `true`：插件有自己的设置窗口，卡片上多一个"设置"按钮，见下面"有自己窗口的常驻插件" |

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
| `debounceMs` | 防抖：打字停下这么久才查询。一次性插件默认 120（每键都起进程太浪费）；常驻插件默认 0（每键都问），每次查询都联网的设上它（翻译用 300），停顿时进程照样先起来 |
| `idleExitSec` | 常驻插件空闲多久退出，默认 600；`0` 不退 |
| `start` | 常驻：`lazy`（默认，第一次用到时起）或 `eager`（启用且配置、权限应用完成后异步启动；用于快捷键、后台接收和推送，不再固定等待 10 秒） |
| `memoryMB` | 作业对象的内存上限，默认 512，最大 2048 |
| `sandbox` | `normal`（默认：中完整性，和你一样）或 `strict`（低完整性：写不了用户目录，只能写自己的数据文件夹；给不需要写文件的 exe 插件） |

`commands[]`：

| 字段 | 说明 |
|---|---|
| `id` | 命令 id，插件内唯一；设置项的键和 AI 工具名用它 |
| `title`、`description`、`hint` | 显示名、一句话、关键字模式下输入框的占位文字（都是文字） |
| `icon` | 见"图标" |
| `keyword` | 字符串或数组。输入**关键字 + 空格**进入关键字模式；单独输入关键字时列表第一行是这条命令的提示行（Enter 或 Tab 进入，↓ 到名字结果）；输入两个字以上、是关键字开头的（`uu` 对 `uuid`），也在应用之后、文件之前多一行（光标不动，按 Tab 就补全成 `uuid `；最多两行，候选太多就不出）；提示行只在“全部”范围出，“文件”“文件夹”里只有文件和文件夹；用户可以在设置里改 |
| `triggers` | 关键字以外的触发方式：`{"regex": "…", "minLength": 1, "maxLength": 64}`（整个输入匹配时，结果推进"全部"；`match` 是捕获组）、`{"push": {"minLength": 2, "maxItems": 3}}`（每键都发，常驻 + eager 才合适）、`{"files": {"kind": "any", "max": 1000, "extensions": ["jpg"], "title": {"zh": "用局域网发送…", "en": "Send over LAN…"}}}`（文件、文件夹结果的右键菜单里多一项 `title`，多选时对整个选择；`kind` 是 `file` / `folder` / `any`，`max` 默认 20、最多 1000，`extensions` 只管文件。选了它：一次 `action`，`trigger` 是 `files`、`action` 是 `files`、`files` 是这些完整路径，没有 item） |
| `mode` | `list`（默认）或 `instant`：单独输入关键字、不用加空格，就向插件发 `query`，它返回的结果排在列表最前面，后面照常是按这个词找到的文件和程序（关键字后面加了空格，列表才只剩这个命令的结果；GitHub #242）。打开窗口、改东西这类动作仍然放在 `action` 里（用户按 Enter 时）：`query` 在用户打字时就会来，他可能只是想找名字里有这几个字母的文件 |
| `contentSearch` | `true`：作为内容检索提供者，必须有 `runtime`；收到 `trigger: "content"`，见下面“内容检索提供者” |
| `actions` | `[{"id": "kill", "title": "结束", "key": "Enter"}, …]`：顺序决定 Enter / Ctrl+Enter / Shift+Enter；右键菜单列全部；不写就只有一个 `default` |
| `run` | 零代码专用：`url`（`%s` 是输入的文字，按网址规则编码；`home` 是单独输入关键字时开的）、`program`（`%VAR%` 会展开；`args` 里 `%s`）、`folder`、`search`（`{"query": "ext:pdf %s", "scope": "all | files | folders | content"}`）、`items`（item 数组，`open.path` 里的 `%VAR%` 会展开） |
| `cache` | `{"seconds": 60}`：同一命令同一文字的结果缓存多久 |
| `ai` | `{"description": "…", "parameters": {JSON Schema}}`：作为 AI 工具时的描述和参数；不写就用 `description` 加一个 `query` 字符串参数 |

未知字段忽略。校验错误是一句人话（"commands[0].keyword 含空格"），设置页原样显示。

### 查询、缓存与停止

同一个有进程的插件，每次搜索只接收一个启动器查询：关键字优先，其次是清单顺序中的第一个正则触发，再其次是 push。零代码命令不占进程查询槽。这样重叠触发不会把已进入的关键字列表取消掉。

命令的 `cache.seconds` 和查询结果的 `cache` 都会复用相同查询的完整结果；结果给出的正数优先，最长一天。命令、触发类型、原始输入、筛选条件、捕获组和结果限额分别参与缓存。缓存只在内存中，按序列化数据计全宿主最多 2 MiB，满时淘汰最近最少使用的项；设置、清单、权限、语言变化，以及执行动作或收到刷新通知时清空。带 `rerun` 的动态列表不缓存。

关闭、移除插件或等待新增权限时，会停止排队查询、一次性进程和常驻进程；过期结果不再执行。常驻动作也使用 `timeoutMs`，超时只失败一次、丢弃迟到回答，不再永久占用常驻名额。一次性插件累计 stdout 最多 16 MiB，不能靠很多短行绕过限额；关键词列表的分批结果合计最多 500 项，匿名项的标识跨批连续，相同显式 ID 只保留第一项。

插件 ID 的 `.data` 结尾留给数据文件夹，不用于新插件；更新使用独立临时备份，保留其他插件和已有数据目录。旧版本留下的数据目录若含另一个插件的 `plugin.json`，删除数据会停止并说明冲突。

## 原生插件的共享 Qt 契约（最低 0.22.0）

原生插件可显式复用 Win顺 的 Qt，省去包内重复分发的运行时。缺省行为不变，未声明的插件仍自行部署依赖。

```json
{
  "minWinShun": "0.22.0",
  "runtime": {
    "mode": "resident",
    "program": "my-tool.exe",
    "qt": "host",
    "qtVersion": "6.12.0"
  }
}
```

- 只支持 Windows x64、MSVC、Release 动态链接 Qt；主次版本必须与宿主一致。可只写 `6.12`，写 `6.12.1` 则要求宿主补丁版本至少为 1。同一主次版本的后续补丁可用；宿主升级主次版本后，插件需更新契约并重新验证。
- 保证 Qt Core、Gui、Qml、Quick，以及 QML `QtQuick`、`QtQuick.Controls.Basic`、`QtQuick.Layouts`；其他模块不在此契约内。插件仍独立进程、独立 QML 引擎，主要减少下载和磁盘成本，不承诺同比减少内存。
- runner 在 `CreateProcess` 前为这个子进程配置 `PATH`、Qt 插件路径和 QML 导入路径，提供 `WS_QT_DIR`、`WS_QT_QML_DIR`，默认 Controls 样式为 Basic。普通静态导入 Qt 的 exe 也可直接启动，无需 `/DELAYLOAD`。不会修改系统、用户或 Win顺 主进程的环境变量。
- 不要在包内混入另一版 Qt DLL、平台插件、QML 模块或覆盖共享路径的 `qt.conf`；Windows 优先搜索 exe 自身目录。自有的非 Qt 依赖照常放入插件目录。
- 启动前检查版本及宿主部署是否完整；不兼容时显示具体原因和更新/重装建议，不尝试启动后再等待崩溃。
- 带 `elevated` 的插件也能用共享 Qt（0.23.0 起，GitHub #255；清单写 `"minWinShun": "0.23.0"`，更早的 Win顺 会拒绝这种清单）。以管理员身份运行时，runner 只认它自己所在的文件夹，也就是以管理员身份运行的 Win顺 自己加载 Qt 的那个文件夹，不会因此多加载任何代码：安装版在 Program Files 里，普通权限改不了；便携版的文件夹本来就决定了以管理员身份运行的 Win顺 跑什么。插件自己的 exe 和 DLL 照旧来自确认过的副本（见“权限”里的 `elevated`）。
- 社区 exe/dll 包自动按“未审核的原生插件”收录（插件库的工具认出包里的 exe/dll 就标上，不再拒收）。插件库标明审核状态，安装及每次更新需确认；签名或 SHA256 校验只证明分发完整性，不代表代码已审核。

契约参考：[Qt Windows 部署](https://doc.qt.io/qt-6/windows-deployment.html)、[Qt 插件版本检查](https://doc.qt.io/qt-6/deployment-plugins.html)、[Windows DLL 搜索顺序](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)。

## 协议

### 一次性（once）

每次查询起一个进程，stdin 关着。

进（环境变量）：`WS_QUERY`（关键字之后的文字）、`WS_TEXT`（整个输入）、`WS_COMMAND`、`WS_TRIGGER`（`keyword` / `regex` / `push`）、`WS_KEYWORD`、`WS_MATCH`（正则捕获组，JSON 数组）、`WS_PLUGIN_DIR`、`WS_DATA_DIR`（插件可写的数据文件夹 `<id>.data`）、`WS_SETTINGS`（JSON）、`WS_LOCALE`（`zh` / `en`）、`WS_THEME`（`light` / `dark`）、`WS_VERSION`。Enter 落到没有声明式 `open` 的行时再起一次进程做动作，另有 `WS_ACTION`（动作 id）和 `WS_ITEM`（那条 item 的 JSON）；从右键菜单选了 `files` 触发时 `WS_TRIGGER` 是 `files`、`WS_ACTION` 是 `files`，`WS_FILES` 是路径的 JSON 数组（整个数组最多约 32000 个字符，再多 Win顺 提示少选一些）。

出（stdout，UTF-8）：一个 JSON 对象 `{"items": [...], "rerun": 秒, "cache": 秒}`，或一个数组（就是 items），或**纯文本行**（每行一条，Enter 复制）。`rerun`：列表还在显示时（没有新的按键、窗口没收起）过这么多秒再问一次同样的查询，结果原位换上，键盘停在原来那一行（行的 `id` 要稳定）；常驻 0.5–60 秒，一次性 1–60 秒。适合进度、设备列表这类自己会变的列表（常驻插件还可以发 `winshun/refresh`，让 Win顺 马上再问，见下）。动作时打印 `{"effects": [...]}`，什么都不打印就是收起窗口。stderr 进 `plugins.log`。退出码非 0 = 失败（底栏提示）。

### 常驻（resident）

进程一直开着，stdin / stdout 上跑 JSON-RPC 2.0，一行一条，UTF-8。**stdout 只许写协议消息**，调试信息写 stderr。

Win顺 → 插件：

```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","winshun":"0.7.0","pluginDir":"…","dataDir":"…","locale":"zh","theme":"dark","appearance":{…},"settings":{…}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"1","name":"…"}}
{"jsonrpc":"2.0","id":2,"method":"query","params":{"queryId":17,"command":"kill","trigger":"keyword","keyword":"kill","text":"note","raw":"kill note","limit":200}}
←（可选，任意条）{"jsonrpc":"2.0","method":"query/partial","params":{"queryId":17,"items":[…],"replace":false}}
← {"jsonrpc":"2.0","id":2,"result":{"items":[…],"rerun":0,"cache":0}}
{"jsonrpc":"2.0","method":"cancel","params":{"queryId":17}}         （新的按键来了；可以不理，Win顺 按 queryId 丢）
{"jsonrpc":"2.0","id":3,"method":"action","params":{"command":"kill","action":"kill","item":{…原样…},"text":"note"}}
← {"jsonrpc":"2.0","id":3,"result":{"effects":[{"type":"notify","text":"已结束 notepad"},{"type":"hide"}]}}
{"jsonrpc":"2.0","id":4,"method":"action","params":{"command":"send","action":"files","trigger":"files","files":["C:\\…\\a.jpg","D:\\相册"],"item":{},"text":""}}   （右键菜单的 files 触发）
{"jsonrpc":"2.0","id":6,"method":"details","params":{"item":{…原样…},"text":"invoice"}}   （内容详情、预览要看不是文件的行，见下面“内容详情与预览”）
← {"jsonrpc":"2.0","id":6,"result":{"text":"From: …\nSubject: …\n\n…"}}  或  {"result":{"file":"C:\\…\\<id>.data\\details\\mail.msg"}}
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

系统通知（`toast`）要 `notifications` 权限，没有就当底栏提示。每个插件最多每 2 秒一条，等着的只留最新的；正文截到 200 个字。标题前面 Win顺 总会加上插件的名字，插件去不掉，免得被当成 Win顺 自己的通知：随 Win顺 装的插件是「局域网传输：…」，自己装的插件不管叫什么都套上「插件「名字」：…」；名字看不出字来（只有空格、填充符这类）时改用 id。名字最多 24 个字，连名字一共 63 个字；没有正文就拿标题当正文。看不见的字符（控制字符、改文字方向的字符、零宽字符、填充符）会被去掉。`click` 是点通知时做的 effects，只认三种、每种一个，别的（网址、`notify`、`hide`、`keep`、`replace`）都丢掉，日志里记一笔：`setQuery` 只能填这个插件自己的关键词（会先打开搜索框）；`open` 只能打开盘符开头的文件或文件夹（`C:\…`，不带参数和工作目录；网址、`\\服务器\共享` 这类路径不行，映射成盘符的网络盘照样能开），照样受 processes 权限约束；`reveal {path}`（同样只认盘符开头的路径）在文件管理器里打开它所在的文件夹并选中它，只是选中、不启动程序，不要 processes 权限，文件已经不在了就打开它所在的文件夹。`open` 和 `reveal` 合起来只做排在前面的那一个：想兼容不认识 `reveal` 的旧版 Win顺，就写 `[{"type":"reveal","path":文件}, {"type":"open","path":文件夹}]`，旧版会丢掉 `reveal`、照旧打开文件夹；`copy` 要 `clipboard:write` 权限。Win顺 自己的通知还在时，插件的通知先等它过去（一般 6 秒，最多 8 秒；等了 60 秒还没轮到就不发）；点哪条通知只做那条自己的事，刚被别的插件的通知顶掉时点的，什么都不做。通知会留在操作中心：**别把文件名、聊天内容这类私人的字写进去**。

C++ SDK 的 `initialized()` 在回答完 `initialize` 后调一次：`eager` 插件在这里开始干活（监听、扫描），不必等第一次查询。`shuttingDown()` 在回答 `shutdown` 之前调：Win顺 一收到回答就结束进程，所以没写完的文件要在这里删掉、对方要在这里告诉；动作要快（Win顺 最多等 2 秒左右）。

`winshun/tool` 和 Win顺 给 AI 的 MCP 工具同名同参数：`search_files`、`search_content`、`recent_files`、`list_folder`、`get_file_info`、`open_item`、`show_in_folder`、`clipboard_history`、`clipboard_get`、`read_file_text`、`index_status`，另有只给插件的 `list_documents`（内容索引里有文字的文件，分页，带一个文件变了就变的 `stamp`）；按 manifest 的权限放行（`winshun:search`、`winshun:open`、`clipboard:read`；`read_file_text` 和 `list_documents` 要 `winshun:content`；`index_status` 总可以）。结果和 MCP 的 `structuredContent` 一样。

超时：每次查询一个计时器（`timeoutMs`），到点这次的结果作废；常驻插件连续 3 次超时当挂死，结束后退避重启（1 → 5 → 30 秒）；5 分钟内崩 3 次就停用，卡片上红字说明。空闲 `idleExitSec` 后收到 `shutdown`。每条响应最多 200 条结果，一行最多 16 MB。

### 有自己窗口的常驻插件（#138）

截图插件 `winshun.capture` 就是这样一个插件：它自己开窗口、自己注册快捷键，Win顺 只在几个地方把事情交给它。下面这些都要常驻（`runtime.mode` 是 `resident`）并要 `ui` 权限，否则 manifest 校验不过。

```
{"jsonrpc":"2.0","method":"appearance","params":{"locale":"zh","theme":"dark","themeSetting":"system","corners":"round","uiStyle":"brand","uiSize":"standard","transparency":"on","renderer":"d3d11","nativeLook":{"style":"brand","dark":true,"rounded":true,"menuRadius":10,"background":"#ff1d1d20","text":"#fff2f2f5","accentFill":"#ffa8402c","…":"…"}}}
{"jsonrpc":"2.0","id":6,"method":"trayMenu","params":{}}
← {"jsonrpc":"2.0","id":6,"result":{"items":[{"id":"capture","title":"开始截图\tCtrl+Alt+A"},{"title":"延时截图","children":[{"id":"delay3","title":"3 秒","checked":true}]},{},{"id":"closePins","title":"关闭全部贴图","enabled":false}]}}
{"jsonrpc":"2.0","method":"menu","params":{"id":"capture"}}
{"jsonrpc":"2.0","id":7,"method":"picture","params":{"action":"pin","file":"C:\\…\\123.png","clipId":123,"project":"C:\\…\\123.wsedit","screenshot":true}}
{"jsonrpc":"2.0","id":8,"method":"showSettings","params":{}}
插件 → {"jsonrpc":"2.0","method":"winshun/hotkeys","params":{"keys":["Win+Shift+S","Ctrl+Alt+A"]}}
```

插件库列表、插件包及旧插件自动恢复的公开 GitHub 下载统一先尝试两个加速镜像，再回退原站；HTTP 成功但列表无效、包大小或 SHA256 不符也会换源。取消会停止后续来源。列表格式和权限确认流程不变，本地包、自定义服务及带凭据的链接不会被自动转发到这些镜像。

增强 OCR 由主程序按需安装，剪贴板与截图插件共用。宿主提供 `WS_COMPONENTS_DIR`，指向当前用户或便携配置的 `components`；不要往该目录写插件私有数据。`show_ocr_setup` 应用工具需要已有的 `ui` 权限，只打开设置中的安装入口，不替插件联网或自动下载。截图仍兼容程序旁边的旧版 / 离线版识别文件。

- **外观**：`initialize` 的 `appearance` 和之后的 `appearance` 通知，是 Win顺 的界面语言和"外观"设置（主题设置、圆角、界面大小、透明效果、渲染方式）。插件的窗口照着画，就和 Win顺 一个样子；变了才发，只发给有 `ui` 权限的插件。通知里还有 `theme`，是正在用的主题（`light` / `dark`）：主题设成“跟随系统”时，Windows 换了深浅色设置没变，也会发。`winshun.h` 收到后 `locale()`、`theme()`、`appearance()` 就是新的，再调 `appearanceChanged()`。
  - `uiStyle` 是界面风格（`fluent` / `brand` / `tactile`）。`nativeLook` 是这个风格算好的颜色和圆角，给不用 Win顺 QML 自己画界面的插件照着画（休息提醒就是这样）；旧版 Win顺 没有这一项，Win顺 刚启动、界面还没建好的那一下也可能没有，没有就按自己原来的样子画。颜色是 `#AARRGGBB` 字符串；圆角是逻辑像素，已经按"圆角/直角"设置算过（直角时都是 0）；`rounded` 是圆角设置本身，插件自己算的胶囊形（高度的一半）直角时也要变成 0；`dark` 是这套颜色对应的深浅色。字段：`style` `dark` `rounded`；圆角 `cardRadius` `controlRadius` `largeControlRadius` `smallControlRadius` `menuRadius` `menuItemRadius` `menuPadding`；颜色 `background` `layer` `page` `windowEdge` `divider` `text` `subtext` `faint` `hover` `pressed` `tagFill` `control` `controlHover` `controlBorder` `inputFocused` `menuBackground` `menuHover` `menuEdge` `accent`（字、图标、线）`accentFill`（白字底）`accentLine` `accentText`（彩色底上的字）`textSelection` `caret`（风格自己的光标色，没有时透明）`danger` `dangerFill`。开着透明效果时 `layer`、`hover` 等可能是半透明的，画在不透明的底上要先叠到 `background` 上。以后可能加字段，不认识的忽略。
- **托盘子菜单**（`trayMenu`）：右键按下托盘图标时，Win顺 发 `trayMenu`，松开时菜单弹出，所以要马上回答（几毫秒）；赶不上的，下次弹菜单时用。子菜单在剪贴板那几项下面，标题是 manifest 的 `trayMenu`。`items` 每项 `{id, title, checked, enabled, children}`，`{}` 是分隔线，最多两层；`title` 里 `\t` 后面的字靠右显示（快捷键）；没有项就不出这个子菜单。用户选了一项，Win顺 发 `menu` 通知，`id` 是那一项的。
- **图片**（`pictures`）：用户在剪贴板历史的图片上、结果里的图片文件上选"贴到屏幕"（`pin`）或"编辑"（`edit`），Win顺 发 `picture`。`file` 是图片文件；来自剪贴板历史时还有 `clipId`，有标注工程时有 `project`，`screenshot` 表示它是截的图。这些文件是 Win顺 的：读，不要改、不要删。几个插件都声明同一项时，交给第一个开着的。
- **设置窗口**（`settingsWindow`）：卡片上的"设置"按钮发 `showSettings`；插件没在运行时按钮提示稍后再试。
- **前台**：`action`（用户在结果上按了 Enter 或点了动作，一次性插件为此起的进程也算）、`menu`、`picture`、`showSettings` 是用户刚点的，Win顺 发之前允许插件把自己的窗口放到前台（`AllowSetForegroundWindow`）；AI 和命令行发来的 `_run` 不算，插件别为它们抢前台。常驻插件的进程是在用户第一次打出关键字时起的，那时搜索框在前台：**起进程时别让任何窗口拿到前台，隐藏的也不行**，否则搜索框会当成用户点了别处而收起。Tk 会激活它建的第一个窗口（`withdraw()` 过的也一样），Python + Tk 的插件要把建窗口那段包在 `LockSetForegroundWindow(1)` … `LockSetForegroundWindow(2)` 里（`try/finally`），等 `action` 来了再显示。Win顺 发出的请求按插件的 `timeoutMs` 等回答，等不到就当失败。
- **快捷键**（`winshun/hotkeys`）：插件自己用 `RegisterHotKey` 注册的快捷键，变了就发一次全部（`[]` 是都放了）。Win顺 就把它们让出来：接管 Win+S 时的 Win+Shift+S、剪贴板的 Ctrl/Alt+数字。进程结束等于发了 `[]`。插件启动时 Win顺 可能还占着其中一个，所以注册失败也照样报上，过一会儿（300 毫秒）再注册一次。

另有只给插件、不给 AI 的工具（照样用 `winshun/tool` 调，按权限放行）：

支持宿主代注册热键时，`initialize.capabilities.hostHotkeys` 为 true。有 `ui` 权限的常驻插件可先用 `winshun/hotkeys` 报告希望保留的键，再调用 `register_hotkeys`，以全部 `keys` 替换本插件的注册（最多 32 项，`[]` 释放）。返回 `{errors: {快捷键: Windows错误码}}`；成功项不在 errors 中，不支持的键记为 87（`ERROR_INVALID_PARAMETER`），不影响其余键注册。按下后收到 `hotkey` 通知 `{key}`，宿主同时允许插件激活自己的窗口。录制快捷键期间释放，结束后重新注册；停用、退出和崩溃由宿主清理。未声明该能力的旧宿主继续走插件自行注册的路径。这样截图插件保持普通权限，F1 等单键的注册可沿用主程序已有的权限级别；不会把插件提权，也不能让普通权限宿主越过更高权限的前台窗口。

| 工具 | 权限 | 参数 | 做什么 |
|---|---|---|---|
| `register_hotkeys` | `ui` | `keys` | 代注册本插件的全局快捷键；支持录制器接受的组合及 PrintScreen，返回逐键的注册错误 |
| `clipboard_add_image` | `clipboard:write` | `file`、`project`（可选）、`copy`（默认 true）、`screenshot`、`origin`（可选） | 把插件数据文件夹里的一张图（和标注工程）放进剪贴板历史；`copy` 为 false 时只记下、不放到剪贴板。Win顺 读完就删掉这两个文件。回 `{kept}`：剪贴板历史或图片记录关着、暂停着时是 false。`origin` 是至多 96 UTF-8 字节、不含 NUL 的图片来源标识，随实际图片一同写入剪贴板；截图插件用它保留最近截图的可编辑标注，不依赖异步调用返回时的剪贴板序号 |
| `clipboard_copy` | `clipboard:write` | `text` | 文字放到剪贴板（最多 100 万字），像平常复制一样进历史 |
| `clipboard_copy_entry` | `clipboard:write` | `id` | 剪贴板历史里的一条重新放到剪贴板，并移到最前 |
| `show_launcher` | `ui` | `text` | 打开搜索框并填上这段文字 |
| `web_search` | `winshun:open` | `text` | 用 Win顺 的默认网页搜索搜这段文字 |
| `open_url` | `winshun:open` | `url` | 用默认浏览器（或邮件程序）以用户的普通权限打开一个 `http`、`https` 或 `mailto` 链接；`open_item` 只开文件和文件夹。0.21.0 起 |
| `backup_export` | `winshun:backup` | `file`、`unless`（可选） | 把现在的设置做成一份备份（和“导出备份”一样，不含这台电脑的备份位置、索引位置），写到 `file`（要在插件的数据文件夹里）。回 `{hash, when, written}`：`hash` 只跟内容有关，和 `unless` 一样时不写文件、`written` 是 false。0.26.0 起 |
| `backup_restore` | `winshun:backup` | `file` | 用 `file`（插件数据文件夹里的一份备份）恢复：先校验，把现在的设置在本机 `backups` 里存一份，然后 Win顺 重启换上它，和“从备份恢复”一样。校验不过或另一个备份正在进行时回错误，什么都不换。让用户确认之后再调。0.26.0 起 |

### 系统开关提供者

随包的 `winshun.switches` 是可关闭的 C++ 常驻插件，新配置默认关闭，旧配置按原有“搜索系统设置”选择迁移一次。它复用主程序的 Qt，只在插件进程中创建硬件监听、COM 工作线程和电源请求。停用或崩溃会释放保持唤醒，重启不恢复。主程序只保留异步界面缓存；关闭时，系统设置搜索仍能打开对应页面。

清单声明 `"systemSwitches": true`，必须是 resident、`start: "eager"`、`idleExitSec: 0`，且包含 `ui` 和 `system:settings` 权限。主程序优先选择已启用的 `winshun.switches`，否则选择目录中第一个已启用的提供者。此能力只使用以下七个 id：`theme`、`hidden-files`、`file-extensions`、`bluetooth`、`wlan`、`volume`、`keep-awake`。

- 请求 `systemSwitches/read`：`{"id":"volume"}`，返回状态对象。
- 请求 `systemSwitches/write`：`{"id":"volume","intent":{"kind":"set-level","value":30}}`，返回 `{"ok":true,"wrote":true,"error":"","after":状态对象}`。`kind` 为 `on`、`off`、`set-level`、`level-only`、`step-level`、`awake-for` 或 `awake-until`；宿主先按显示的状态解析切换，不发送 `toggle` 或 `open-settings`。音量是 0–100，时长是分钟，时刻是午夜起的分钟数。
- 通知 `systemSwitches/changed`：`{"ids":["volume"]}`，宿主合并读取。只有声明此能力、权限齐全且已启用的插件通知才会生效。

状态对象字段：`availability`（0 未知、1 可用、2 无设备、3 拒绝、4 被阻止、5 不支持），`on`（-1 未知、0 关、1 开、2 混合），`level`（-1 或 0–100），`note`、`error`、`openInstead`（字符串，后者只能为空或 `ms-settings:` 页面），`mixedToggleOn`、`carriesInternet`（布尔），可选 `until`（Unix 毫秒时间）。非法响应降级到 Windows 设置，不重放失败或崩溃时的写入。完整校验与映射见 `src/core/SwitchWire.h`；C++ SDK 仍可发送这些 JSON-RPC 方法，无须绑定 Qt。

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
  "details": true,                   // 不是文件的行：内容详情和预览要看它时，Win顺 发 details 来问（只对常驻插件；见“内容详情与预览”）
  "data": {"pid": 1234}              // 不透明，随 action 原样回来；AI 看不到它
}
```

effects（动作的回答，按顺序执行）：`hide`（收起窗口，默认）、`keep`（留着）、`setQuery {text}`、`notify {text}`、`copy {text}`、`open {path, args, cwd | url}`（和 item 的 `open` 同款，同样受 processes 权限约束；`url` 同样认 `winshun:settings`）、`replace {items}`（原地换成这些结果，做二级列表）、`reveal {path}`（在文件管理器里选中这个文件或文件夹，只认盘符开头的路径；新版 Win顺 才有，旧版不认识会跳过）。

### 长文字结果（#117）

标题本身就是内容的行（翻译、计算结果：没有 `open`、`search`、`setQuery`，写了 `copy`，`valid` 不是 false）若有一行放不下，列表下面会多出一块文字区，把当前行的全文换行显示（最多约 6 行，更长的在区内滚动）。区的高度按这批结果里最长的那条定，所以上下移动选择时窗口不会忽高忽低，只在结果变化时才变；窗口在屏幕可用区域内增高。选中的是别的行（比如打开网址的行）时，区是空的。文字可用鼠标选中，有选中时 Ctrl+C 复制选中的部分。会打开东西的行标题只是名字，照旧截断。不用加字段，行高、序号和键盘操作都不变；Enter、Ctrl+C 仍按 `copy`。

### 搜索面板的日期、大小与筛选（#88）

item 可选提供 `modified`、`created`（带时区的 ISO 8601 字符串或 Unix 秒）和 `size`（非负整数，字节；`0` 是空文件）。未提供或无效的字段留空；日期和大小未知的旧插件条目仍然显示。面板可显示、排序这些列，未知日期和大小排在最后。类型取标题扩展名，所在文件夹取 `open.path` 的父目录，来源是插件名称。

常驻查询的 `params.filters`、一次性查询的 `WS_FILTERS` JSON 以及三个 SDK 的 `q.filters` 提供当前面板条件：

```json
{"modifiedFrom":"2026-01-01T00:00:00Z","modifiedTo":"2027-01-01T00:00:00Z",
 "minSize":1048576,"maxSize":null,"kinds":["documents"],"extensions":["pdf","docx"],"folder":"C:\\Docs"}
```

时间下界包含、上界不包含，大小上下界都包含；`null`、空数组、空文件夹表示没有该项限制。普通搜索也可能附带内容检索及排除条件；忽略不认识的键。插件可按这些条件查询自己的数据源；Win顺 也会按返回的已知日期、大小、扩展名和本地打开路径筛选。Python 的 `item(..., modified=..., created=..., size=...)`、PowerShell 的 `New-WinShunItem -Modified ... -Created ... -Size ...`、C++ 的 `withModified / withCreated / withSize` 均可输出元数据。

`kinds` 可以包含多个类型，例如 `["pictures","videos"]`；对应的 `extensions` 是这些类型扩展名的并集。类型之间是 OR，仍与日期、大小、文件夹等条件同时生效；插件不要只读取数组第一项。

### 内容检索提供者

在命令中声明 `"contentSearch": true`，可同时保留 `keyword`。宿主只在“内容”，或已开启内容检索的“全部”中派发 `query`，此时 `trigger` 为 `content`，`text` 为内容关键词；文件／文件夹搜索和独占的关键词列表不派发。关键词查询仍按原协议执行。此接口在 0.17.0 之后的版本才有；旧宿主不派发内容查询，只能用命令关键词。官方图片文字搜索插件打包时把 `minWinShun` 设为构建它的主程序版本。

`filters` 除面板条件外，还包含 `contentEnabled`、`contentWords`、`contentPick`（选定目录）、`excludedExtensions`、`excludedFileNames`（通配符）。提供者应按整个文件验证关键词，返回稳定 id、原文件的 `open.path`、日期／大小及命中摘要。内容结果在“内容”里排在原生结果之后；在“全部”里，所有提供者的前 3 行排在前 5 个名称结果之后（最近、收藏和与输入完全同名的应用之后），其余仍在最后；重复应答或名称列表增长时，已有行的位置不变。每个提供者最多 200 行，不占普通推送的 3／6 行配额；已有原生文件结果会去重。空结果返回空数组，避免把设置提示混进内容列表。可参考 [图片文字搜索插件](../src/ocr-search-plugin/package/README.md)。

### 内容详情与预览（#202）

“内容”面板右侧的内容详情（摘要、匹配、文本、预览、报告）和预览窗（Alt+P、空格）也显示插件的行。

**会打开文件的行**：`open.path` 是普通文件（盘符或 `\\服务器\共享` 开头的完整路径，没有 `args`、`cwd`）时，窗格和预览就把这一行当成这个文件，和文件自己的行一样：匹配高亮、文本、预览，摘要和报告也算它（列表里已有同一个文件时不重复）。内容检索提供者的行本来就是这样，不用改。在窗格或预览里点“打开”和在这一行按 Enter 一样，照样受 `processes` 权限约束。

**不是文件的行**（一封邮件、一条记录）：常驻插件在 item 里写 `"details": true`。内容详情开着（匹配、文本、预览页）或预览窗开着、选中这一行时，Win顺 发一次请求：

```
{"jsonrpc":"2.0","id":7,"method":"details","params":{"item":{…原样…},"text":"invoice"}}
← {"jsonrpc":"2.0","id":7,"result":{"text":"From: …\nSubject: …\n\n…"}}
← {"jsonrpc":"2.0","id":7,"result":{"file":"C:\\Users\\…\\<id>.data\\details\\mail.msg"}}
```

`item` 是这一行的 item（和 `action` 里的一样），`text` 是输入的文字（和 `action` 的一样；内容搜索里是内容关键词）。回答到了，匹配、文本、预览照常显示，标题是这一行的标题。

- `text`：最多用前 1,000,000 个字符，多的截掉，窗格标明“仅显示部分内容”。预览页提示到“文本”页看；预览窗显示开头的文字。
- `file`：必须是插件自己数据文件夹（`initialize` 的 `dataDir`，即 `<id>.data`）里的普通文件。Win顺 打开它看它真正在哪里：相对路径、`..`、共享、设备路径、文件夹、指到文件夹外面的符号链接或 junction、还有别的硬链接的文件，都不认。读取照样受内容大小上限约束；预览用系统的预览处理程序（比如 .msg 用 Outlook 装的那个）。文件别马上删：同一行再选中时 Win顺 直接用上次的回答。
- 两个都给时先用 `file`，`file` 不合格再用 `text`。都没有、回答出错（包括 SDK 回的“方法不存在”）或超过 `timeoutMs` 没回答时，窗格写“无法获取详情”，界面不等它；再选中这一行、或列表换了新结果时重问。
- 节奏：键盘停下 120 毫秒才问；一次只有一个请求在路上，答完再问最新选中的那一行，过时的回答不会顶掉当前行（它留着备用）。回答按插件、item 和文字记在内存里（合计约 16 MB）。插件没在运行时不会为此启动它，直接显示“无法获取详情”。
- 只认常驻插件；一次性和零代码插件的 `details` 忽略。同时有 `open.path` 和 `"details": true` 的行，以 `details` 为准。窗格里点“打开”或双击匹配行，等于在这一行按 Enter。摘要另外写“插件结果（不是文件，不计入报告）：N”。
- 内容检索提供者（`contentSearch`）的结果原本只保留有 `open.path` 的行；声明了 `"details": true` 的行在“内容”里也保留（排序、去重照旧），别的没有文件的行（设置提示之类）仍不显示。
- 不要新权限：Win顺 只读插件自己数据文件夹里的文件，显示给用户，不交给别的程序。
- 兼容：宿主会发 `details` 时，`initialize` 的 `capabilities.details` 是 true。旧版 Win顺 不认识 `details` 字段，照常显示这一行，不会来问，不用为它写 `minWinShun`。
- SDK：Python `item(..., details=True)`，`serve(on_query, on_details=…)`，`on_details(item, text, plugin)` 返回 `{"text": …}`、`{"file": …}` 或一个字符串（就是文字）；PowerShell `New-WinShunItem -Details`，`Start-WinShunPlugin -OnDetails { param($d, $plugin) … }`（`$d.item`、`$d.text`），返回值同上；C++ `Item(...).withDetails()`，重写 `std::optional<Json> details(const Json& item, const std::string& text)`。

### 图标

`icon.png`（插件文件夹里的 PNG / JPG / ICO，相对路径；深色主题优先 `icon.dark.png`）、`ext:pdf`（扩展名的图标）、`file:C:\…`（某个文件的图标）、`app:<AUMID>`、`glyph:U+E8E5`（Segoe Fluent Icons 字形的编号；Win顺 自己用到的那些画成对应的 [Phosphor](https://phosphoricons.com) 图标，每台 Windows 都一样，其余用系统图标字体画，老 Win10 的字体里可能没有）、`ph:heart`（随程序带的 Phosphor 2.1.2 图标名，全套可用；`ph-fill:heart` 换成实心 Fill 字重；名字见 `third_party/phosphor/metadata/regular.css`，不认识的显示成拼图图标；不依赖 Windows 版本。0.25.0 起可用，更早的 Win顺 不认识这种写法，会改用命令或插件的图标；只用于命令和结果条目的图标，清单最外层的 `icon` 仍然是文件）、`color:#ff8800`（色块）、`data:image/png;base64,…`（≤ 64 KB）、`site:https://…`（网站自己的图标，和浏览器地址栏左边那个一样：Win顺 只向该网站首页取一次、存在本地、一个月更新；没取到前显示浏览器图标；网址的路径部分不会发出去）。插件文件夹以外的路径不认，SVG 不认。

## 权限

manifest 的 `permissions` 是数组：`["processes", "clipboard:read", {"files:read": ["%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Bookmarks"]}]`。打开开关时逐条列给用户，Windows 能强制的打勾，不能的标"仅声明"——说实话：

| 权限 | 含义 | Windows 能否强制 | 怎么做到 |
|---|---|---|---|
| （总是）不是管理员 | 插件以你的普通权限运行；唯一的例外是下面的 `elevated` | **是** | 令牌去掉 Administrators 组和特权；`normal` 中完整性，`strict` 低完整性；内存上限、作业对象，Win顺 退出时一起结束 |
| `elevated` | 跟随 Win顺 的管理员权限：Win顺 以管理员身份运行时，这个插件也以管理员身份运行，能操作以管理员身份打开的窗口（比如以管理员身份运行的文件管理器）。Win顺 不是管理员时，它和别的插件一样用普通权限，**从不为插件弹 UAC**（这时插件卡片上有一行说明和“以管理员身份重新启动”，搜索框里搜“管理员”也有同一项；想每次启动都以管理员身份运行，在“搜索范围 › 磁盘 › 索引方式”里选“管理员直读”） | **是** | 只给自带 exe 的插件（`runtime.program` 是 exe；python、node、pwsh、powershell 的解释器或模块可能在你自己能改的文件夹里），不能和 `sandbox: "strict"` 一起用。以管理员身份运行的不是插件文件夹本身，而是你确认过的那份副本（见下）；作业、内存上限照旧 |
| `processes` | 起子进程，或让 Win顺 打开程序 / 非网页链接 | **是** | 没有它：作业 `ActiveProcessLimit = 1`，`CreateProcess` 报 1816；且 Win顺 不替它执行开程序、脚本、非网页 URL 或带 args/cwd 的 `open`（item 和 open 效果、`open_item` 都一样，只能开文件夹、普通文件和 http/https/mailto，拦下时底栏提示）。有它：子进程在作业里，`open` 不受限 |
| `clipboard:read` / `clipboard:write` | 直接读写剪贴板 | **是** | 作业的 UI 限制；没有时 `GetClipboardData` 返回空（读到空或抛异常） |
| `ui` | 开自己的窗口、找别的窗口 | 部分 | 没有时作业限制桌面和 USER 句柄，并禁用传统扩展点；有此权限的插件保留传统输入法扩展点，仍受其他进程缓解策略和作业限制 |
| `system:settings` | 修改 Windows 用户设置（主题、资源管理器、无线电、音量、保持唤醒） | **否** | 普通用户权限，仅声明；用于系统开关提供者，不授予管理员权限 |
| `winshun:search` / `winshun:open` / `winshun:history` | 调 Win顺 的工具 | **是** | `winshun/tool` 按权限放行 |
| `winshun:backup` | 取得 Win顺 的设置备份，并用备份恢复设置（`backup_export`、`backup_restore`）。备份里有全部设置、收藏和装的插件；恢复会换掉现在的设置并重启 Win顺。0.26.0 起，更早的 Win顺 不认识它，会拒绝整个插件，清单里同时写 `"minWinShun": "0.26.0"` | **是** | `winshun/tool` 按权限放行；文件只能在插件自己的数据文件夹里 |
| `winshun:content` | 读 Win顺 从文档和文本文件里取出的文字（`list_documents`、`read_file_text`） | **是** | `winshun/tool` 按权限放行；不用自己解析 Word、PDF，也不用再读一遍硬盘 |
| `files:write` | 写你的文件 | 只在 `strict` 下 | `strict` 写不了用户目录，只能写 `<id>.data`；`normal` 和你自己一样能写，**只是声明** |
| `files:read` | 读哪些文件 | **否** | 只是告诉用户它读什么（中完整性本来就能读） |
| `network` | 联网 | **否** | Win顺 挡不住；卡片上写"Win顺 不能阻止它联网" |
| `network:listen` | 在局域网上开端口，让别的设备连进来：`{"network:listen": ["TCP 53317-53320", "UDP 53317"]}` | **否** | 只是告诉用户它开哪些端口；卡片上写"Win顺 不能阻止它开端口"。防火墙规则 Win顺 不替它加（随包的局域网传输由安装程序加） |
| `notifications` | 弹出系统通知（`winshun/notify` 的 `toast`） | **是** | 没有它，Win顺 把通知当底栏提示；有它也限速：每 2 秒最多一条 |

**`elevated`：以管理员身份运行前要确认一次。** 插件文件夹（`%LOCALAPPDATA%\WinShun\plugins\<id>`、开发中的文件夹）是你自己能写的，所以任何以普通权限运行的程序都能偷换里面的 exe 或塞一个 DLL，等以管理员身份运行的 Win顺（常常是开机自启的“管理员直读”）去启动它。所以 Win顺 从不以管理员身份运行插件文件夹里的东西：

- Win顺 以管理员身份运行时，你在插件卡片上点“确认”（或在这时打开开关、同意权限），Win顺 在后台做一份副本，放进 `%ProgramFiles%\WinShun Elevated Plugins\<你的 SID>\`：Program Files 里普通用户建不了文件夹，这些文件夹由 Win顺 建，属主是 Administrators，只有管理员和 SYSTEM 能打开，旁边记下每个文件的 SHA-256。之后插件以管理员身份运行时，exe、工作目录、`WS_PLUGIN_DIR`、清单（`runtime` 的程序、参数、环境变量）都只来自这份副本，而且副本的清单必须和 Win顺 现在读到的完全一样。
  - **插件库里的插件**（带签名的列表里有它）：副本从插件库重新下载、按签名列表核对过的包做，包里的文件必须和装着的一模一样，否则不确认（“可能被改过，从插件库重新安装它”）；列表里的版本和装着的不一样时，先更新再确认。
  - **自己装的、开发中的插件**：副本就是点“确认”那一刻插件文件夹里的文件。插件文件夹本身不能是链接（junction、符号链接），里面也不能有链接或硬链接；开发中要以管理员身份测试，用真正的文件夹，不要链接。
- 每次启动前，Win顺 把插件文件夹和记录比一遍：一样，就以管理员身份运行副本；不一样（没确认过、文件改过、多了或少了文件、有链接、清单变了，或者存副本的文件夹不对：是链接、属主不是管理员、普通用户能改），就照常从插件文件夹以普通权限运行，卡片上写明原因。文件被改过时卡片写“插件的文件被改过，和你确认时不一样……不确定是谁改的就别确认”。改过的代码绝不会悄悄以管理员身份运行；比对只决定跑“确认过的副本（管理员）”还是“插件文件夹（普通权限）”，判断错了也不会让没确认过的文件以管理员身份运行。
- 以管理员身份运行的 Win顺 从插件库更新确认过的插件时，只在**带签名的列表**（签名核对过，不是开发用的本地文件）里有同一个包（版本、大小、SHA-256 都一样）时，才把内存里的这份字节解压成新的确认副本，更新后不用再点；比确认过的版本旧的包不收。插件库页面自己的列表不验签名，单凭它不算数。以普通权限运行的 Win顺 装的更新、自己装的压缩包或文件夹，都要在 Win顺 以管理员身份运行时再确认一次。
- 以管理员身份运行的插件拿到的环境变量是干净的：只有系统的变量（`HKLM`），去掉会让程序加载代码的（`COR_*`、`DOTNET_*`、`NODE_OPTIONS`、`PYTHON*`、`_NT_SYMBOL_PATH` 等），`PATH` 用系统的、只留普通权限改不了的文件夹（装在你用户文件夹里的 Python、VS Code 之类即使在系统 PATH 里也去掉），再加上你的几个文件夹（`USERPROFILE`、`APPDATA`、`LOCALAPPDATA`）和 `WS_*`；`TEMP`/`TMP` 是副本旁边受保护的文件夹。`HKCU\Environment` 里你自己设的变量不会传给它；也没有 `WS_COMPONENTS_DIR`（共享组件文件夹普通权限能改）。
- Win顺 以管理员身份运行时，带 `elevated` 的插件不给 AI 和命令行用（它们的请求来自普通权限的程序）。
- 插件自己要小心的：`WS_DATA_DIR`、`WS_SETTINGS` 和插件设置都是普通权限能改的，当作不可信的输入；不要从数据文件夹加载代码；找不到的 DLL 会按系统的 PATH 去找，需要的 DLL 都放在插件包里（Qt 可以用 Win顺 的，见“原生插件的共享 Qt 契约”）。

清单这样写：`"permissions": ["elevated", "ui"]`，`"runtime": {"mode": "resident", "program": "bin\\tool.exe"}`。老版本的 Win顺 不认识 `elevated`，会整个拒绝这个插件，所以同时写上 `"minWinShun"`。

**新版本要更多权限，要再同意一次。** 你打开过的插件换了新版本（从插件库更新、装压缩包或文件夹，或者直接改了 `plugin.json`），只要它要的权限比你当初同意的多，就先停用：不启动，不回答查询，AI 用不了，也不发通知。卡片上会写明新版本多要了哪些权限。打开它的开关，会照常列出全部权限，并标出新增的几条；同意后照常使用，不同意就一直停着。权限没变或变少了，什么都不用做。随 Win顺 安装的插件跟着 Win顺 一起升级，照旧可信。只比较权限名：`files:read` 列的路径、`network:listen` 的端口变了，不会再问。

**插件更新。** 设置 → 高级 → 更新里的“自动更新”开着时（默认），Win顺 每天从插件库的列表（带签名，经 `index.json.sig` 核对）查一次你装的插件。权限、读的路径、开的端口都没变的新版本，会在插件没在运行时直接换上；正在运行的先放在一边，下次启动 Win顺 时换上，也可以在插件库点“立即更新”。要新权限的新版本从不自动装：开着的插件会收到一条托盘通知（每个版本一次），“设置”里“插件”条目上的数字和托盘菜单的“N 个插件有新版本…”指向插件库里筛出的可更新项；点“更新并允许”，一步装上新版并同意新权限，保持开启；点“取消”则旧版照常用，这个版本不再计数、不再通知。插件库页的“全部更新”只更新不需要新权限的插件。列表没签名或签名不对、开发者模式、指向源码的开发中副本，都只提示，不自动替换。

只安装你信得过来源的插件。

## 设置项

`settings[]`：`{"key", "type", "title", "hint", "default", "options"}`，`type` ∈ `text | password | bool | number | choice | folder | file | hotkey`。Win顺 用自己的控件画在卡片上，值存在 `WinShun.ini` 的 `[Plugin.<id>] Setting.<key>=`；**`password` 型存 Windows 凭据管理器**，不进 INI，不进日志（消息记录里打码）。`choice` 的 `options` 是字符串数组或 `[{"value": "deepl", "title": {"zh": …, "en": …}}]`。

插件看到的是合并后的 JSON（声明的默认值 + 用户的值）：一次性在 `WS_SETTINGS`，常驻在 `initialize` 的 `settings`，改了再发 `settingsChanged`。

## 给 AI 用

manifest 写 `"ai": {"expose": true}`，并在设置 → AI 助手 打开"AI 可以使用插件"后，每个命令成为 MCP 工具 `plugin_<id>_<command>`（id 里的点换成下划线：`plugin_winshun_tools_calc`）。AI 调用 = 一次 `query`（没运行就起它，等它 `initialize`），返回 `{items: [{id, title, subtitle, url?, path?, copy?}], returned}`，不含 `data`；`readOnlyHint` 为真。命令的 `ai.description`（英文，给模型看）和 `ai.parameters` 可以改描述和参数，默认是 `{query, limit}`。

动作默认**不**暴露；"允许 AI 执行插件动作"打开后再多一个 `plugin_<id>_<command>_run`（`destructiveHint`）：AI 先用查询工具拿到行的 `id`，再把同样的 `query` 和 `item_id`（可选 `action`）交给它，Win顺 重新查一次、找到那行、发 `action`，回 `{done, message, effects}`。每个插件卡片上还有"给 AI 用"开关；插件停用时 AI 调用会被告知"插件 X 已停用"。工具列表变了会通知已连接的 AI 客户端。

## 调试

- **开发者模式**（设置 → 插件 → 高级）：改了脚本自动重启常驻插件（底栏"已重新加载 X"）；"消息记录"记下每一条往来（时间、插件、方向 → ← stderr ·、方法、请求到响应的毫秒），点一条看整个消息；"打开 plugins.log"（`%LOCALAPPDATA%\WinShun\plugins.log`，插件的 stderr、协议错误、崩溃都在那）。
- **`ws plugin`**（Win顺 自带的命令行，先在 设置 → AI 助手 里启用命令行）：`ws plugin install <插件文件夹或 .wsplugin>` 把它装上（新装的先关着；盖在开着的版本上直接换成新的，改完再装一次就行），`ws plugin enable <id>` 打开（第一次要你在 Win顺 的窗口里确认权限），`ws plugin list --json` 看它的状态（`state`、进程的 `status`、`error`：manifest 的错、缺程序、上次崩溃）。AI 助手也这样装上试。
- **让 AI 写**：`npx skills add LingCore/WinShun --skill winshun-plugin-dev` 给 Claude Code、Codex、Cursor 等装上写插件的 Skill（协议要点、上面这个循环、发布步骤），不用再手动复制开发提示。
- **wsbench**（源码编译出的 `build\release\wsbench.exe`，不用开界面）：

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
- **想写文件。** 写 `WS_DATA_DIR`（`<id>.data`，卸载时问用户要不要一起删）；`strict` 沙箱的插件只能写那里；它的 `TEMP`／`TMP` 指向 `<id>.data\tmp`（0.21 起；用户自己的临时文件夹低完整性写不了，SQLite 这类要临时文件的库会报 “unable to open database file”）。
- **图标不显示。** 只认 PNG / JPG / ICO，路径要在插件文件夹里；SVG 不支持。
- **关键字被占。** 和设置里"网页搜索"的关键字或别的插件撞了，用户设的赢，卡片上写明；改一个或让用户改。
- **插件目录能被改吗。** `normal` 插件就是以你的身份运行，你能改的它都能改——这是开发者模式"改了脚本自动重载"的前提。要防插件改自己，用 `strict`。

### 网页搜索宿主能力

`webSearch: true` 为官方 `winshun.web` 保留：不声明 `runtime`，`commands` 为空，网站来自插件设置。宿主提供通用网址解析、结果渲染和懒加载编辑器；停用或缺少插件时不提供结果、不占用关键词、不请求建议。旧版官方零代码清单在内存中兼容为这一能力，不重写用户文件。插件包构建时将 `minWinShun` 设为当前主程序版本。

## C++ 异步查询

C++ SDK 提供 `queryAsync(const Query&, std::function<void(Json)>)`，默认同步调用 `queryResult`。异步实现应在协议线程快照设置和语言，每个请求恰好回复一次，并处理 `cancel(queryId)`。在 `shuttingDown()` 和派生类析构函数中取消并等待工作线程退出。请求尚未结束时，工作线程可用 `partial()` 推送结果。线上协议保持兼容。
