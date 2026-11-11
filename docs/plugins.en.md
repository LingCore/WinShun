# Writing plugins for Win顺 (WinShun)

Plugins add keyword commands to the search box and mix their rows into the list. The interface and plugins run with normal user rights (only a plugin with `elevated` runs as administrator, and only while Win顺 does and after you confirm it; see Permissions); a separate service reads the NTFS index. A slow or crashing plugin never holds the launcher up. Three ways to write one, a single protocol:

For a new plugin in the Library, choose **Install and enable** and review its permissions. Declining keeps it installed and off. Updates preserve the existing switch; installing a local archive does not automatically enable it. If an upgrade cannot restore a previously enabled feature, the Installed tab keeps the reason and offers Retry and local package installation without discarding its settings or data.

| Kind | What you write | For |
|---|---|---|
| **Zero-code** | `plugin.json` only | a keyword that opens a URL or a program, jumps to a folder, runs a saved search, or lists fixed items |
| **Once** (a process per query) | any language; the query comes in environment variables, the rows go to stdout | small tools, used now and then |
| **Resident** | any language; JSON-RPC lines on stdio | anything queried on every key, anything with state, PowerShell (slow to start) |

The plugins that come with Win顺 are in `<install dir>\plugins\` (all off until you turn them on under Settings → Plugins): `winshun.tools` (calculator, units, colours, timestamps, UUIDs, Base64, hashes; C++), `winshun.kill` (end a process; PowerShell), `winshun.go` (zero-code).

Win顺's other own plugins are not in the setup but in the plugin library, to install when you want them (Settings → Plugins → Library; sources in `src\<name>-plugin\`, the manifest and README in its `package\`): `winshun.capture` (screenshots and recording; C++ on Win顺's own Qt, see "Resident plugins with windows of their own" below), `winshun.semantic` (search by meaning: `? a sentence` finds documents by what they are about, with the embedding model you pick — Ollama on this PC or a cloud service's key; C++, resident, takes the text Win顺 already read through `winshun:content`), `winshun.localsend` (LAN transfer: phones and computers on the same network send each other files and text, works with the LocalSend app; `传` or `ls`, or 用局域网发送… on a file's menu; C++, resident and eager, uses `network:listen`, `notifications` and the `files` trigger), `winshun.break` (break reminder: a short break every 20 minutes and a long one every other time, a small bar before, then every monitor dimmed with a countdown; `休息` or `break`; C++, resident and eager, only `ui`, draws its own windows and holds the keyboard with a hook during a break), `winshun.explorer-tabs` (Explorer tabs: a folder window that opens while another File Explorer window is there becomes a tab of that window, as ExplorerTabUtility does; hold Ctrl+Shift for a window; `标签页` or `tabs` shows the state and pauses it; C++, resident and eager, `ui` and `elevated` (with Win顺 running as administrator and the copy confirmed, it merges administrator windows too), ShellWindows events and a WinEvent hook, nothing injected into Explorer; the keyword is only there to pause it), `winshun.translate` (`tr sentence` with your own DeepL or OpenAI-compatible API key; C++, resident, `debounceMs` 300, a "Translating…" row first), `winshun.bookmarks` (`bm words` searches Chrome and Edge bookmarks; C++, once), `winshun.ocr-search` (image text search: `ocr words` finds words in pictures, scanned PDFs and pictures inside documents, and also answers in Content and All; C++, resident; the package needs the WinShun version that built it) and `winshun.web` (web search: a declarative plugin with no program, sites managed in its settings, see Web search below). One that came with an earlier Win顺 and that you had on is installed from the library after the upgrade, settings kept. Each is a working example with a README.

Plugins by other people, and a place to share yours: [WinShun-plugins](https://github.com/LingCore/WinShun-plugins) (install and update them right from Settings → Plugins → Library: each download is checked against the list's size and sha256, then installed as an archive would be; or download a `.wsplugin` yourself and install it from the archive on the Installed tab; see its CONTRIBUTING to submit one). The Library goes online when its tab is opened (at most once in ten minutes), when Refresh is pressed, or to install; with Automatic updates on and plugins from the library installed, it also checks for updates once a day in the background. The last list is kept in the data folder as `plugin-index.json`.

**Plugins by others are maintained by their authors.** When one misbehaves, choose "Report a problem to the author" in its card's "…" menu: it opens the Issues of the author's repository (the `repo` the library lists; for a plugin not installed from the library, its `homepage`: a GitHub repository's Issues, or the page itself). A plugin without a homepage gets its listed repository as Homepage. Win顺's own Issues are for Win顺 and its official plugins (theirs point there). When you submit a plugin, set `repo` to your own repository and keep its Issues open.

The Chinese version, [plugins.md](plugins.md), is the reference; this one follows it.

Each official native library plugin states its minimum host in its manifest's `minWinShun` (capture 0.18.0 and Explorer tabs 0.19.0, for example). Their `.wsplugin` packages are built by the main repository's release script and published on the existing WinShun-plugins repository, with `bundled: false`. The first-screen rule for a content provider's rows in All (the first three after the first five name rows) applies from WinShun 0.19.0. Registry validation permits only their named executables and capture's named DLL from that repository; community submission rules are unchanged. Never replace a published package with different contents: bump the plugin version. See the registry's [maintainer instructions](https://github.com/LingCore/WinShun-plugins/blob/main/CONTRIBUTING.md#维护者).

**Web search** is now the optional `winshun.web` 2.0 library plugin (`src/web-plugin/package`). It declares a host capability and starts no extra process. Its card manages sites and suggestions. Upgrades merge the former built-in settings and enabled legacy sample keywords once; uninstalling keeps the settings. Fresh profiles start disabled; existing profiles use plugin recovery, retaining their data if the library is offline.

## Quick start

A plugin is a folder under `%LOCALAPPDATA%\WinShun\plugins\<id>\` with a `plugin.json`. Fastest: Settings → Plugins → Advanced → Developer mode → **New plugin…**, pick a language, and a template is written with its SDK file; turn the switch on and type its keyword. Edits to the script reload it; the message log shows every line that goes back and forth.

### 1. Zero-code: a keyword that opens a URL

`%LOCALAPPDATA%\WinShun\plugins\me.npm\plugin.json`:

```json
{
  "manifest": 1,
  "id": "me.npm",
  "name": "npm",
  "version": "1.0.0",
  "commands": [
    {"id": "npm", "keyword": "npm", "title": "Search npm", "run": {"url": "https://www.npmjs.com/search?q=%s", "home": "https://www.npmjs.com/"}}
  ]
}
```

`npm lodash` and Enter opens the search; `npm ` alone (with the space) opens the home page. `run` may also be `{"program": "C:\\…\\code.exe", "args": "%s"}`, `{"folder": "%USERPROFILE%\\Downloads"}`, `{"search": {"query": "ext:pdf %s", "scope": "files"}}` (as if typed into the box) or `{"items": [{"id": …, "title": …, "open": {"path": …}}]}` (a fixed list the words filter).

Zero-code paths beginning with `%USERPROFILE%\Desktop`, `Documents`, `Downloads`, `Pictures`, `Music` or `Videos` resolve through Windows known folders to their current locations, including other drives, OneDrive and network shares. Subpaths are supported. `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup` also resolves to the current Windows Startup folder. This applies to `run.folder`, `run.program`, working directories, and fixed items' `open.path` and subtitles containing variables. Use an explicit absolute path to target a specific location.

### 2. Python, once: a few lines

`me.hello\plugin.json`:

```json
{
  "manifest": 1,
  "id": "me.hello",
  "name": "Hello",
  "version": "1.0.0",
  "runtime": {"mode": "once", "program": "python", "script": "main.py"},
  "commands": [{"id": "hi", "keyword": "hi", "title": "Say hello", "hint": "a name"}]
}
```

`me.hello\main.py` (copy `winshun.py` from `src/plugins-sdk/` or start from the Python template of "New plugin…" under Settings → Plugins):

```python
import winshun

q = winshun.query()                      # q.text: the words after the keyword
winshun.reply([
    winshun.item("hi", f"Hello, {q.text}", "Enter copies", copy=q.text),
    winshun.item("len", f"{len(q.text)} characters", valid=False),
])
```

A Python process per query (tens of milliseconds). Without the SDK: read `WS_QUERY`, print `{"items": [...]}`, a bare array, or plain text lines (one row each; Enter copies).

### 3. PowerShell, resident: one process that stays

`me.ps\plugin.json`:

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

`me.ps\main.ps1` (**UTF-8 with a BOM**; `WinShun.psm1` beside it):

```powershell
Import-Module "$PSScriptRoot\WinShun.psm1"
Start-WinShunPlugin -Name 'me.ps' -OnQuery {
    param($q, $plugin)
    @(New-WinShunItem -Id 'hi' -Title "Hello, $($q.text)" -Copy $q.text)
}
```

PowerShell takes a few hundred milliseconds to start the first time, then a few milliseconds per query; it exits after 10 idle minutes. For C++ see the head of `src/plugins-sdk/winshun.h` and `src/tools-plugin/`.

## The SDK

Three files in `src/plugins-sdk/`; "New plugin…" copies the right one into the plugin's folder, and a bundled plugin that uses one carries its own (`winshun.kill`, `WinShun.psm1`). They only wrap the protocol: using them is optional.

| File | Once | Resident |
|---|---|---|
| `winshun.py` | `query()`, `settings()`, `item(...)`, `reply(items)`, `is_action()` / `action_request()` / `effects(...)` | `serve(on_query, on_action=None, on_settings=None, on_details=None)`; handlers get `(q, plugin)` with `plugin.partial()`, `plugin.log()`, `plugin.notify_user()`, `plugin.toast()`, `plugin.progress()`, `plugin.refresh()`, `plugin.tool(name, **args)`, `plugin.settings` |
| `WinShun.psm1` | `Get-WinShunQuery`, `Get-WinShunSettings`, `New-WinShunItem`, `Send-WinShunReply`, `Send-WinShunEffects` | `Start-WinShunPlugin -OnQuery {…} -OnAction {…} -OnDetails {…}`, `Send-WinShunPartial`, `Write-WinShunLog`, `Send-WinShunNotify`, `Send-WinShunToast`, `Send-WinShunProgress`, `Invoke-WinShunTool` |
| `winshun.h` | `winshun::onceQuery()`, `onceSettings()`, `once(items)`, `onceAction()` / `onceEffects()` | derive from `winshun::Plugin`, override `query()` (and `queryResult()`, `action()`, `settingsChanged()`, `initialized()`, `shuttingDown()`, `appearanceChanged()`, `details()`), call `serve()`; members `partial()`, `log()`, `notifyUser()`, `toast()`, `progress()`, `refresh()`, `tool()`, `settings()`, which other threads may call too (sending is locked) |

Effect helpers: Python `winshun.hide() / keep() / notify(text) / copy(text) / set_query(text) / open_path(path) / open_url(url) / replace(items)`; PowerShell `New-WinShunEffect -Hide / -Keep / -Notify / -Copy / -SetQuery / -OpenPath / -OpenUrl / -Replace`; C++ `winshun::effect::hide()` and the rest.

## The manifest (plugin.json)

Top level:

| Field | Required | Meaning |
|---|---|---|
| `manifest` | yes | schema version, `1`; a newer one than Win顺 knows is refused |
| `id` | yes | `author.name`, `[a-z0-9._-]` only, the folder's name |
| `name` | yes | a string, or `{"zh": …, "en": …}` picked by the interface language ("text" below) |
| `version` | yes | e.g. `1.2.0`; installing a lower version over a higher one asks |
| `description` | no | one sentence (text) |
| `author`, `homepage`, `license` | no | shown on the card; `homepage` is a link |
| `icon` | no | a PNG / JPG / ICO in the plugin folder (`name.dark.png` for the dark theme; **no SVG**); else the first command's icon |
| `minWinShun` | no | not enabled below it |
| `runtime` | once / resident | see below |
| `permissions` | no | see "Permissions" |
| `commands` | yes | at least one |
| `settings` | no | see "Settings" |
| `ai` | no | `{"expose": true}`: the commands may become AI tools (the user still has to allow it); see "For AI" |
| `trayMenu` | no | the title (text) of a submenu in the tray menu; see "Resident plugins with windows of their own" below |
| `pictures` | no | `["pin", "edit"]`: Pin to screen and Edit on the pictures of the clipboard history and picture files among the results go to this plugin; see "Resident plugins with windows of their own" below |
| `settingsWindow` | no | `true`: the plugin has a settings window of its own, and its card a Settings button; see "Resident plugins with windows of their own" below |

`runtime`:

| Field | Meaning |
|---|---|
| `mode` | `once` or `resident` |
| `program` | `python`, `pwsh`, `powershell`, `node`, or an exe in the plugin folder (relative path). A missing interpreter puts "needs Python 3" with a download link on the card; one found that cannot run is named, with the reason (see "Python not found" below) |
| `script` | the entry script; default `main.py` / `main.ps1` / `main.js` |
| `args` | extra arguments (array) |
| `cwd` | working folder; default the plugin folder |
| `env` | extra environment variables (`WS_*` cannot be overridden) |
| `encoding` | stdout's encoding: `utf-8` (default) or `oem` (Windows PowerShell 5.1 without an encoding set; the templates set UTF-8) |
| `timeoutMs` | one query's budget: resident 1500, once 3000, at most 10000 |
| `debounceMs` | asks only once typing pauses this long: once plugins default to 120; resident ones to 0 (every key), and one that goes online for every query sets it (Translate: 300) — the process still starts at the first key |
| `idleExitSec` | resident: idle seconds before exit, default 600; `0` never |
| `start` | resident: `lazy` (default, on first use) or `eager` (started asynchronously once enabled, configured and permitted; for hotkeys, background receiving and push, without a fixed ten-second delay) |
| `memoryMB` | the job's memory limit, default 512, at most 2048 |
| `sandbox` | `normal` (medium integrity, as you) or `strict` (low integrity: cannot write your folders, only its own data folder; for exe plugins that write nothing) |

`commands[]`:

| Field | Meaning |
|---|---|
| `id` | unique within the plugin; the settings keys and the AI tool name use it |
| `title`, `description`, `hint` | the name, one sentence, the placeholder in keyword mode (all text) |
| `icon` | see "Icons" |
| `keyword` | a string or an array. **Keyword + space** enters keyword mode; the keyword alone puts the command's hint row first (Enter or Tab enters, ↓ reaches the names found); two or more characters a keyword begins with (`uu` for `uuid`) add a row after the apps, before the files (the keyboard stays where it is; Tab completes to `uuid ` from any row; two rows at most, none when too many keywords qualify); the user may change it |
| `triggers` | beyond the keyword: `{"regex": "…", "minLength": 1, "maxLength": 64}` (the whole input matches: rows are pushed into 全部; `match` carries the groups), `{"push": {"minLength": 2, "maxItems": 3}}` (every key; resident + eager only), `{"files": {"kind": "any", "max": 1000, "extensions": ["jpg"], "title": {"zh": "用局域网发送…", "en": "Send over LAN…"}}}` (an entry `title` in the menu of file and folder rows, for the whole selection when several are selected; `kind` is `file` / `folder` / `any`, `max` 20 by default and 1000 at most, `extensions` apply to files only. Chosen: one `action` with `trigger` `files`, `action` `files` and `files` the full paths, no item) |
| `mode` | `list` (default) or `instant`: the keyword alone, with no space after it, sends `query` to the plugin and lists what it returns. Opening a window or changing anything still belongs in `action` (the user pressed Enter): `query` arrives while the user types, and they may be looking for a file with those letters in its name |
| `contentSearch` | `true`: a content-search provider; requires `runtime` and receives `trigger: "content"`. See “Content-search providers” below. |
| `actions` | `[{"id": "kill", "title": "Kill", "key": "Enter"}, …]`: the order gives Enter / Ctrl+Enter / Shift+Enter; the menu lists all; none means one `default` |
| `run` | zero-code only: `url` (`%s` is the text, URL-encoded; `home` for the keyword alone), `program` (`%VAR%` expanded; `%s` in `args`), `folder`, `search` (`{"query": "ext:pdf %s", "scope": "all | files | folders | content"}`), `items` (item objects; `%VAR%` in `open.path` expanded) |
| `cache` | `{"seconds": 60}`: how long the same text's answer is reused |
| `ai` | `{"description": "…", "parameters": {JSON Schema}}` for the AI tool; default: `description` plus a `query` string |

Unknown fields are ignored. A validation error is a sentence, shown as is on the settings page.

## Community native review

Community packages containing executables or DLLs can be submitted with the registry's `--unreviewed-native` flow and `review: "unreviewed-native"`. They require WinShun 0.22.0 or newer. The library shows their review state, and installation and every update require individual user consent. Users can view the publisher's source page and retain the installed version when declining an update. These packages use individual update actions. Publishers retain the unreviewed marker when posting later versions.

## Shared host Qt runtime

Since WinShun 0.22.0, ordinary-privilege MSVC x64 Release executables can request the host Qt runtime in their manifest.

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

The host and plugin require the same Qt major and minor version. A third component is the minimum patch version, and `6.12` accepts that major/minor line. The contract provides Core, Gui, Qml, Quick, QtQuick, Basic Controls and Layouts. Each plugin has its own process and QML engine. Plugins requesting administrator privileges supply their own runtime.

Before creating the child process, the runner prepares its DLL, platform-plugin and QML paths and supplies `WS_QT_DIR` and `WS_QT_QML_DIR`. Ordinary static Qt imports work. Keep the package's own non-Qt dependencies beside its executable. The runner checks version and deployment requirements before starting it. Revalidate the plugin when the host changes Qt major/minor versions.

### Queries, caching and stopping

A plugin process receives one launcher query per search: a keyword takes precedence, followed by the first matching regex in manifest order, then push. Zero-code commands do not occupy that process query slot. Overlapping triggers therefore cannot cancel the keyword list the user entered.

Both command `cache.seconds` and result `cache` reuse complete answers. A positive result value takes precedence, for at most one day. The command, trigger, raw input, filters, captures and result limit are part of the key. This is an in-memory LRU cache with a host-wide 2 MiB budget measured as serialized data. Settings, manifests, permissions, language changes, actions and refresh notifications clear it. Live lists with `rerun` are not cached.

Disabling, removing or withholding consent for a plugin stops queued queries, once processes and resident processes. Stale results cannot execute. Resident actions also use `timeoutMs`; a timeout fails once, forgets late replies and releases the resident slot. Once stdout is limited to 16 MiB in total, including many short lines. Keyword partial batches share a 500-item limit and continuous anonymous identities; duplicate explicit IDs keep the first item.

The `.data` ID suffix is reserved for data folders. Updates use a separate temporary backup and preserve neighboring plugins and existing data directories. If a legacy data folder contains another plugin's `plugin.json`, removing data stops with an explanation.

## The protocol

### Once

A process per query; stdin is closed.

In (environment): `WS_QUERY` (after the keyword), `WS_TEXT` (the whole input), `WS_COMMAND`, `WS_TRIGGER` (`keyword` / `regex` / `push`), `WS_KEYWORD`, `WS_MATCH` (regex groups, a JSON array), `WS_PLUGIN_DIR`, `WS_DATA_DIR` (the plugin's writable `<id>.data`), `WS_SETTINGS` (JSON), `WS_LOCALE` (`zh` / `en`), `WS_THEME` (`light` / `dark`), `WS_VERSION`. For an action on a row without a declarative `open`, the process is started again with `WS_ACTION` (the action id) and `WS_ITEM` (the row as JSON); for a `files` trigger chosen on the menu `WS_TRIGGER` and `WS_ACTION` are `files` and `WS_FILES` is the paths as a JSON array (some 32,000 characters at most; with more, Win顺 asks for fewer).

Public GitHub downloads of the library index, plugin packages and upgrade recovery try two download mirrors before the origin. Invalid index content, wrong package size or SHA256 also trigger failover; cancellation stops further sources. The index format and permission prompts are unchanged. Local packages, custom services and credential-bearing URLs are not automatically forwarded to these mirrors.

Enhanced OCR is installed on demand by the main app and shared with the capture plugin. The host provides `WS_COMPONENTS_DIR`, the current user or portable profile’s `components` folder; do not store private plugin data there. The `show_ocr_setup` app tool requires the existing `ui` permission and only opens the installation setting; it does not download automatically or grant plugins network access. Legacy and offline recognition files beside the app remain supported.

Out (stdout, UTF-8): one JSON object `{"items": [...], "rerun": seconds, "cache": seconds}`, a bare array (the items), or **plain text lines** (one row each; Enter copies). `rerun`: while the list is still shown (no new key, the window still up), the same query is asked again after that many seconds and its rows replace the old ones in place, the keyboard staying on its row (keep the rows' `id`s stable); 0.5–60 seconds resident, 1–60 once. For lists that change by themselves: progress, devices nearby (a resident plugin can also send `winshun/refresh` to be asked again at once, see below). For an action, `{"effects": [...]}`; nothing printed means hide. stderr goes to `plugins.log`. A non-zero exit code is a failure (shown in the footer).

### Resident

One process, JSON-RPC 2.0 lines on stdin / stdout, UTF-8. **Only protocol messages may go to stdout**; debug output goes to stderr.

Win顺 → plugin:

```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","winshun":"0.7.0","pluginDir":"…","dataDir":"…","locale":"zh","theme":"dark","appearance":{…},"settings":{…}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"1","name":"…"}}
{"jsonrpc":"2.0","id":2,"method":"query","params":{"queryId":17,"command":"kill","trigger":"keyword","keyword":"kill","text":"note","raw":"kill note","limit":200}}
← (optional, any number) {"jsonrpc":"2.0","method":"query/partial","params":{"queryId":17,"items":[…],"replace":false}}
← {"jsonrpc":"2.0","id":2,"result":{"items":[…],"rerun":0,"cache":0}}
{"jsonrpc":"2.0","method":"cancel","params":{"queryId":17}}         (a newer key came; may be ignored, Win顺 drops by queryId)
{"jsonrpc":"2.0","id":3,"method":"action","params":{"command":"kill","action":"kill","item":{…as given…},"text":"note"}}
← {"jsonrpc":"2.0","id":3,"result":{"effects":[{"type":"notify","text":"Ended notepad"},{"type":"hide"}]}}
{"jsonrpc":"2.0","id":4,"method":"action","params":{"command":"send","action":"files","trigger":"files","files":["C:\\…\\a.jpg","D:\\Photos"],"item":{},"text":""}}   (a files trigger from the menu)
{"jsonrpc":"2.0","id":6,"method":"details","params":{"item":{…as given…},"text":"invoice"}}   (content details or the preview showing a row that is no file; see "Content details and preview" below)
← {"jsonrpc":"2.0","id":6,"result":{"text":"From: …\nSubject: …\n\n…"}}  or  {"result":{"file":"C:\\…\\<id>.data\\details\\mail.msg"}}
{"jsonrpc":"2.0","method":"settingsChanged","params":{"settings":{…}}}
{"jsonrpc":"2.0","id":5,"method":"shutdown"}  ← {"jsonrpc":"2.0","id":5,"result":{}} then exit
```

Plugin → Win顺:

```
{"jsonrpc":"2.0","id":"p1","method":"winshun/tool","params":{"name":"search_files","arguments":{"query":"*.pdf","limit":20}}}
{"jsonrpc":"2.0","method":"winshun/log","params":{"level":"info","text":"…"}}        plugins.log and the developer log
{"jsonrpc":"2.0","method":"winshun/notify","params":{"text":"…"}}                   the footer
{"jsonrpc":"2.0","method":"winshun/notify","params":{"toast":true,"title":"…","text":"…","click":[{"type":"setQuery","text":"ls "}]}}   a system notification
{"jsonrpc":"2.0","method":"winshun/progress","params":{"queryId":17,"text":"Reading bookmarks…"}}   the "waiting…" line
{"jsonrpc":"2.0","method":"winshun/refresh","params":{}}   the list shown changed by itself: ask again now
```

`winshun/refresh`: the list shown changed by itself (a request came, a transfer ended), so there is no need to wait out `rerun`: Win顺 asks the same query again now, at least 0.1 seconds after the last one, and several in a row get one more query (one that comes while a query is being answered, right after the answer). It only acts while the last answer asked for `rerun` and the list is still open, else it does nothing; older versions of Win顺 ignore it. Write the state the next query reads first, then send it. In the C++ SDK it is `refresh()`, in Python `plugin.refresh()`.

A system notification (`toast`) needs the `notifications` permission; without it, it is a footer notice. One per plugin every 2 seconds at most, the newest of those waiting; the text is cut to 200 characters. Win顺 always puts the plugin's name before the title, which the plugin cannot leave out, so it never passes for one of Win顺's own: "LAN transfer: …" for one that came with Win顺, and `Plugin “<name>”: …` for one you put in, whatever it calls itself; its id when the name shows nothing (only spaces, fillers and the like). The name is 24 characters at most, 63 with the title; with no text, the title is the text too. Characters that do not show (controls, those that turn the direction of the words, zero width ones, fillers) are taken out. `click` holds the effects of a click on it, of three kinds only and one of each; the rest (web links, `notify`, `hide`, `keep`, `replace`) are dropped, with a line in the log: `setQuery` only with this plugin's own keyword (it opens the search box first); `open` only of a file or folder whose path starts with a drive letter (`C:\…`, no arguments or working folder; never a web link or a path such as `\\server\share`, though a network drive mapped to a letter opens), held to the processes permission as ever; `reveal {path}` (also a drive letter's path only) opens the item's folder in the file manager with it selected: it only selects, starts no program and needs no processes permission, and when the file is gone its folder opens. Of `open` and `reveal` only the first is done: for older versions of Win顺, which do not know `reveal`, send `[{"type":"reveal","path":file}, {"type":"open","path":folder}]`; they drop the `reveal` and open the folder; `copy` with the `clipboard:write` permission. While a notification of Win顺's own is up, a plugin's waits until it goes (usually 6 seconds, 8 at most; one still waiting after 60 seconds is dropped); a click on a notification only does what that one said, and a click right after another plugin's replaced it does nothing. Notifications stay in the Action Center: **keep file names, messages and other private words out of them**.

The C++ SDK's `initialized()` runs once, after `initialize` was answered: an `eager` plugin starts its work there (listening, scanning) instead of at the first query. `shuttingDown()` runs before `shutdown` is answered: Win顺 ends the process as soon as it has the answer, so half-written files are deleted and peers told there, quickly (Win顺 waits about 2 seconds at most).

`winshun/tool` takes the same names and arguments as Win顺's MCP tools: `search_files`, `search_content`, `recent_files`, `list_folder`, `get_file_info`, `open_item`, `show_in_folder`, `clipboard_history`, `clipboard_get`, `read_file_text`, `index_status`, and, for plugins only, `list_documents` (the files whose text the content index holds, a page at a time, each with a `stamp` that changes when the file does); gated by the manifest's permissions (`winshun:search`, `winshun:open`, `clipboard:read`; `winshun:content` for `read_file_text` and `list_documents`; `index_status` always). The result is the MCP `structuredContent`.

Timeouts: one timer per query (`timeoutMs`); a late answer is dropped. A resident plugin that misses three in a row is restarted with backoff (1 → 5 → 30 s); three crashes in five minutes turn it off, with the reason on its card. After `idleExitSec` it gets `shutdown`. At most 200 rows per answer, 16 MB per line.

### Resident plugins with windows of their own (#138)

The screenshot plugin, `winshun.capture`, is one: it opens its own windows and registers its own shortcuts, and Win顺 hands it a few things. All of these need a resident plugin (`runtime.mode` `resident`) with the `ui` permission, or the manifest does not pass.

```
{"jsonrpc":"2.0","method":"appearance","params":{"locale":"en","theme":"dark","themeSetting":"system","corners":"round","uiStyle":"brand","uiSize":"standard","transparency":"on","renderer":"d3d11","nativeLook":{"style":"brand","dark":true,"rounded":true,"menuRadius":10,"background":"#ff1d1d20","text":"#fff2f2f5","accentFill":"#ffa8402c","…":"…"}}}
{"jsonrpc":"2.0","id":6,"method":"trayMenu","params":{}}
← {"jsonrpc":"2.0","id":6,"result":{"items":[{"id":"capture","title":"Take a screenshot\tCtrl+Alt+A"},{"title":"Delayed","children":[{"id":"delay3","title":"3 seconds","checked":true}]},{},{"id":"closePins","title":"Close all pins","enabled":false}]}}
{"jsonrpc":"2.0","method":"menu","params":{"id":"capture"}}
{"jsonrpc":"2.0","id":7,"method":"picture","params":{"action":"pin","file":"C:\\…\\123.png","clipId":123,"project":"C:\\…\\123.wsedit","screenshot":true}}
{"jsonrpc":"2.0","id":8,"method":"showSettings","params":{}}
plugin → {"jsonrpc":"2.0","method":"winshun/hotkeys","params":{"keys":["Win+Shift+S","Ctrl+Alt+A"]}}
```

- **Look**: `appearance` in `initialize`, and the `appearance` notification after it, are Win顺's interface language and Appearance settings (theme setting, corners, interface size, transparency, renderer), for the plugin's windows to look like Win顺's. Sent when they change, to plugins with the `ui` permission only. The notification also has `theme`, the theme in use (`light` / `dark`): with the theme set to follow the system it comes when Windows turns dark or light, though the settings stay the same. `winshun.h` then has the new ones in `locale()`, `theme()` and `appearance()` and calls `appearanceChanged()`.
  - `uiStyle` is the interface style (`fluent` / `brand` / `tactile`). `nativeLook` is that style's colours and corners, worked out, for plugins that draw their windows without Win顺's QML (as the break reminder does). An older Win顺 sends none, nor may Win顺 in the moment it starts, before its windows are made: draw as before then. Colours are `#AARRGGBB` strings; radii are logical pixels, with the corners setting applied (all 0 when square); `rounded` is that setting itself, for pill shapes a plugin works out (half the height), which go square too; `dark` is the theme the colours are for. Fields: `style` `dark` `rounded`; radii `cardRadius` `controlRadius` `largeControlRadius` `smallControlRadius` `menuRadius` `menuItemRadius` `menuPadding`; colours `background` `layer` `page` `windowEdge` `divider` `text` `subtext` `faint` `hover` `pressed` `tagFill` `control` `controlHover` `controlBorder` `inputFocused` `menuBackground` `menuHover` `menuEdge` `accent` (text, icons, lines) `accentFill` (under white words) `accentLine` `accentText` (words on a coloured fill) `textSelection` `caret` (the style's caret colour, transparent where it has none) `danger` `dangerFill`. With transparency on, `layer`, `hover` and others may be translucent: lay them on `background` before drawing on an opaque surface. Fields may be added; ignore the ones you do not know.
- **Tray submenu** (`trayMenu`): as the right button goes down on the tray icon, Win顺 sends `trayMenu`, and the menu opens as it comes up, so answer at once (a few milliseconds); a late answer is used the next time. The submenu is under the clipboard's entries, titled by the manifest's `trayMenu`. Each of `items` is `{id, title, checked, enabled, children}`, `{}` a line, two levels at most; what follows a `\t` in `title` is shown on the right (a shortcut); no items, no submenu. The entry chosen comes as the `menu` notification with its `id`.
- **Pictures** (`pictures`): Pin to screen (`pin`) or Edit (`edit`) chosen on a picture of the clipboard history or a picture file among the results is sent as `picture`. `file` is the picture; from the clipboard history there is also `clipId`, `project` when it has an annotation project, and `screenshot` when it is a screenshot. These files are Win顺's: read them, never change or delete them. When several plugins claim the same verb, the first one turned on gets it.
- **Settings window** (`settingsWindow`): the card's Settings button sends `showSettings`; when the plugin is not running, the button says to try again in a moment.
- **Foreground**: `menu`, `picture` and `showSettings` are what the user just chose, and Win顺 lets the plugin bring its window to the front (`AllowSetForegroundWindow`) before sending them. Win顺 waits the plugin's `timeoutMs` for an answer to what it asks, then takes it as failed.
- **Shortcuts** (`winshun/hotkeys`): the shortcuts the plugin registers itself (`RegisterHotKey`), all of them each time they change (`[]`: none). Win顺 leaves those to it: Win+Shift+S while it holds Win+S, the clipboard's Ctrl/Alt+digits. The process ending counts as `[]`. Win顺 may still hold one as the plugin starts, so report the ones that failed too and register again a moment (300 ms) later.

And tools for plugins, not for AI (called with `winshun/tool` as the others, held to the permissions):

When `initialize.capabilities.hostHotkeys` is true, a resident plugin with `ui` permission can report its reserved keys with `winshun/hotkeys`, then call `register_hotkeys` with all its `keys` (up to 32; `[]` releases them). The response is `{errors: {shortcut: WindowsErrorCode}}`, omitting successful keys; an unsupported key is 87 (`ERROR_INVALID_PARAMETER`) and the other keys still register. A press arrives as `hotkey` with `{key}`, with foreground activation allowed. Release while recording a shortcut and register again afterwards; the host cleans up on disable, exit and crash. Older hosts retain the plugin's own registration path. Registration uses the host's existing integrity level while the plugin keeps its normal rights. An ordinary host still cannot override a higher-integrity foreground application with an unmodified function key.

| Tool | Permission | Arguments | What it does |
|---|---|---|---|
| `register_hotkeys` | `ui` | `keys` | replaces this plugin's global shortcuts, accepting recorder-supported shortcuts and PrintScreen; returns per-key registration errors |
| `clipboard_add_image` | `clipboard:write` | `file`, `project` (optional), `copy` (default true), `screenshot` | puts a picture in the plugin's data folder (and its annotation project) into the clipboard history; with `copy` false it is recorded but not put on the clipboard. Win顺 deletes both files once read. Answers `{kept}`: false while the clipboard history or its pictures are off or paused |
| `clipboard_copy` | `clipboard:write` | `text` | text onto the clipboard (at most a million characters), into the history as any copy |
| `clipboard_copy_entry` | `clipboard:write` | `id` | an entry of the clipboard history onto the clipboard again, moved to the top |
| `show_launcher` | `ui` | `text` | opens the search box with this text |
| `web_search` | `winshun:open` | `text` | searches the web for this text with Win顺's default web search |
| `open_url` | `winshun:open` | `url` | opens an `http`, `https` or `mailto` link in the default browser (or mail program) with the user's normal rights; `open_item` opens files and folders only. Since 0.21.0 |

### Item fields

```json
{
  "id": "pid-1234",                  // stable: in-place updates, the target of actions, what the AI's _run tool names
  "title": "notepad.exe",            // required
  "subtitle": "PID 1234 · 38 MB",
  "subtitleAway": "Press ↑ to pick it, then Enter ends it",   // optional: the second line while the keyboard is on another row (shown from its start, its end cut if it does not fit); on the row, `subtitle` again. For a row that came in above the keyboard by itself (a request while the list is open): how to get there. Not shown to the AI. PowerShell's New-WinShunItem calls it -AwaySubtitle (so -Sub… is still -Subtitle); C++: withSubtitleAway
  "icon": "file:C:\\Windows\\notepad.exe",   // see "Icons"
  "score": 80,                       // 0–100; the apps' scale when pushed into 全部; 50 when absent
  "match": "notepad 记事本",          // zero-code items filter on it
  "autocomplete": "kill notepad",    // Tab puts it in the box
  "open": {"path": "C:\\…", "args": "…", "cwd": "…"} or {"url": "https://…"},   // declarative default action: Win顺 opens it (normal rights), no round trip; the url "winshun:settings" opens Win顺's settings at this plugin's card (for a "no API key yet" row). Without the processes permission a path must be a folder or a non-program/non-script file, a url must be http/https/mailto, and args/cwd are not allowed; otherwise it is not carried out and the footer says so (see Permissions)
  "copy": "1234",                    // Ctrl+C; else the title
  "preview": {"text": "…"},          // the hover tip
  "actions": [{"id": "kill", "title": "Kill", "key": "Enter"}],   // overrides the command's
  "valid": true,                     // false: Enter does nothing (a message row)
  "details": true,                   // a row that is no file: content details and the preview ask the plugin with `details` (resident plugins only; see "Content details and preview")
  "data": {"pid": 1234}              // opaque; comes back with the action; never shown to the AI
}
```

Effects (an action's answer, in order): `hide` (default), `keep`, `setQuery {text}`, `notify {text}`, `copy {text}`, `open {path, args, cwd | url}` (the same as an item's `open`, and held to the processes permission the same way; `url` takes `winshun:settings` too), `replace {items}` (a second-level list), `reveal {path}` (the file or folder selected in the file manager; a drive letter's path only; new in this version, older ones skip it).

### Long text results (#117)

A row whose title is the payload (a translation, a calculation: no `open`, `search` or `setQuery`, a `copy` given, `valid` not false) that is too long for its one line makes a text area appear under the list, showing the current row's full text wrapped (about 6 lines, then it scrolls inside). The area is as tall as the longest such text among the current results needs, so the window does not change height as the selection moves, only when the results change; it grows within the screen's work area. On any other row (say one that opens a URL) the area is empty. The text can be selected with the mouse, and Ctrl+C copies the selection. A row that opens something keeps its elided title: there the title is only a name. No new field: row heights, number badges and keyboard behaviour stay as they are, and Enter/Ctrl+C still use `copy`.

### Search-panel metadata and filters (#88)

Items may include `modified` and `created` (ISO 8601 with a time zone, or Unix seconds) and `size` (a nonnegative integer in bytes; zero means an empty file). Missing or invalid values stay blank. Older items without metadata remain visible. Unknown dates and sizes sort last in either direction. Type comes from the title's extension, Folder from the parent of `open.path`, and Source from the plugin's name.

Resident queries receive `params.filters`; once queries receive JSON in `WS_FILTERS`. All three SDKs expose `q.filters`:

```json
{"modifiedFrom":"2026-01-01T00:00:00Z","modifiedTo":"2027-01-01T00:00:00Z",
 "minSize":1048576,"maxSize":null,"kinds":["documents"],"extensions":["pdf","docx"],"folder":"C:\\Docs"}
```

The date interval includes its lower bound and excludes its upper bound. Both size bounds are inclusive. Null bounds, empty arrays and an empty folder mean no restriction. Ordinary searches may also include content and exclusion settings; ignore unknown keys. Plugins can apply these conditions to their data source. WinShun also filters known returned metadata, extensions and local open paths. Output metadata with Python `item(..., modified=..., created=..., size=...)`, PowerShell `New-WinShunItem -Modified ... -Created ... -Size ...`, or C++ `withModified / withCreated / withSize`.

`kinds` may contain several categories, such as `["pictures","videos"]`. `extensions` is their combined extension list. Categories use OR, while date, size and folder conditions still apply together. Plugins should read the entire array.

### Content-search providers

Declare `"contentSearch": true` on a command; a `keyword` may coexist. A host built with this capability sends `query` with `trigger: "content"` only in Content or content-enabled All. File/folder searches and exclusive keyword lists do not dispatch providers. `text` contains the content query. Keyword queries keep their existing protocol and remain available in older hosts. Hosts up to 0.17.0 have no content providers; the official image text search plugin sets `minWinShun` to the host version it is built with.

In addition to panel filters, `filters` includes `contentEnabled`, `contentWords`, `contentPick` (selected folder), `excludedExtensions` and `excludedFileNames` (wildcards). Verify terms across the entire file and return stable IDs, the original `open.path`, date/size metadata and a matching snippet. In Content the rows follow native results; in All, the first three provider rows (across providers) sit after the first five name rows (past recent and favorite rows and the app named exactly as typed) and the rest stay last, with existing rows keeping their place when an answer repeats or the name list grows. Up to 200 provider rows per provider, without consuming the ordinary 3/6 push quota; existing native file paths are deduplicated. Return an empty array for no matches, keeping setup hints out of content results. See the [image text search plugin](../src/ocr-search-plugin/package/README.md).

### Content details and preview (#202)

The content details pane of the Content panel (Summary, Hits, Text, Preview, Report) and the preview window (Alt+P, Space) show plugin rows too.

**A row that opens a file**: when `open.path` is a plain file (a full path starting with a drive letter or `\\server\share`, with no `args` or `cwd`), the pane and the preview treat the row as that file, just like the file's own row: highlighted hits, text and preview, and the summary and report count it (a file already listed is not counted twice). Content-search providers' rows already qualify; nothing to change. Open in the pane or the preview does what Enter on the row does, held to the `processes` permission as usual.

**A row that is no file** (a mail, a record): a resident plugin sets `"details": true` on the item. When content details (Hits, Text or Preview tab) or the preview window is open on that row, Win顺 sends one request:

```
{"jsonrpc":"2.0","id":7,"method":"details","params":{"item":{…as given…},"text":"invoice"}}
← {"jsonrpc":"2.0","id":7,"result":{"text":"From: …\nSubject: …\n\n…"}}
← {"jsonrpc":"2.0","id":7,"result":{"file":"C:\\Users\\…\\<id>.data\\details\\mail.msg"}}
```

`item` is the row's item (as in `action`), `text` the words typed (as in `action`; the content words in a content search). Once answered, hits, text and preview show as usual, titled with the row's title.

- `text`: the first 1,000,000 characters are used; the pane says when the rest was cut. The Preview tab points to the Text tab; the preview window shows the start of the text.
- `file`: must be a plain file in the plugin's own data folder (`dataDir` in `initialize`: `<id>.data`). Win顺 opens it to see where it really is; relative paths, `..`, shares, device paths, folders, symbolic links or junctions leading out of the folder, and files with other hard links are refused. Reading is held to the content size limits as usual; the preview uses the system preview handler (Outlook's for .msg, say). Don't delete the file at once: Win顺 reuses the answer when the row is selected again.
- Both given: `file` first, `text` if the file does not qualify. Neither, an error (including an SDK's "method not found") or no answer within `timeoutMs`: the pane says it could not get the details, and nothing waits for it; asked again when the row is selected again or new results come.
- Pace: asked after a 120 ms pause in the keys; one request at a time, the newest selected row asked next when it is answered; a late answer never replaces the current row (it is kept for later). Answers are kept in memory by plugin, item and text (about 16 MB in all). A plugin not running is not started for this: the pane says the details could not be had.
- Resident plugins only; `details` is ignored on once and zero-code plugins. A row with both `open.path` and `"details": true` goes by `details`. Open in the pane, or a double click on a hit, is Enter on the row. The summary adds "plugin results (not files, not in the report): N".
- A content-search provider's rows used to be kept only with an `open.path`; a row with `"details": true` now stays in Content too (ordered and deduplicated as before), while other rows with no file (setup hints and the like) still do not show.
- No new permission: Win顺 only reads files from the plugin's own data folder, to show them to the user.
- Compatibility: `initialize.capabilities.details` is true when the host sends `details`. Older hosts ignore the `details` field and show the row as before, never asking; no `minWinShun` needed for it.
- SDKs: Python `item(..., details=True)`, `serve(on_query, on_details=…)` with `on_details(item, text, plugin)` returning `{"text": …}`, `{"file": …}` or a string (the text); PowerShell `New-WinShunItem -Details` and `Start-WinShunPlugin -OnDetails { param($d, $plugin) … }` (`$d.item`, `$d.text`), returning the same; C++ `Item(...).withDetails()` and an override of `std::optional<Json> details(const Json& item, const std::string& text)`.

### System switch providers

The bundled `winshun.switches` is an optional native C++ resident plugin, off for fresh profiles. An existing profile migrates its system-search preference once. Device listeners, the COM worker and the power request live only in the plugin process; disabling or crashing ends keep-awake, and restart never restores it. Without a provider, system search still opens Windows Settings pages.

Declare `"systemSwitches": true`, resident mode with `start: "eager"`, `idleExitSec: 0`, and both `ui` and `system:settings` permissions. The host prefers enabled `winshun.switches`, then the first enabled alternative. Known IDs are `theme`, `hidden-files`, `file-extensions`, `bluetooth`, `wlan`, `volume` and `keep-awake`.

- `systemSwitches/read` request: `{"id":"volume"}`; returns a state object.
- `systemSwitches/write` request: `{"id":"volume","intent":{"kind":"set-level","value":30}}`; returns `{"ok":true,"wrote":true,"error":"","after":STATE}`. Kinds: `on`, `off`, `set-level`, `level-only`, `step-level`, `awake-for`, `awake-until`. The host resolves toggles from the displayed state; `toggle` and `open-settings` are rejected. Volume uses 0–100, duration uses minutes, and a time uses minutes after midnight.
- `systemSwitches/changed` notification: `{"ids":["volume"]}`. Accepted only from an enabled provider with the required permissions; the host coalesces reads.

A state has numeric `availability` (0 unknown, 1 ready, 2 no device, 3 denied, 4 blocked, 5 unsupported), `on` (-1 unknown, 0 off, 1 on, 2 mixed), `level` (-1 or 0–100); strings `note`, `error`, `openInstead` (empty or an `ms-settings:` page); booleans `mixedToggleOn`, `carriesInternet`; and optional `until` (Unix milliseconds). Invalid replies fall back to Windows Settings. Failed or interrupted writes are never replayed. See `src/core/SwitchWire.h` for validation. Any C++ SDK plugin can send these JSON-RPC messages without Qt.

### Icons

`icon.png` (PNG / JPG / ICO in the plugin folder, relative; `icon.dark.png` preferred in the dark theme), `ext:pdf`, `file:C:\…`, `app:<AUMID>`, `glyph:U+E8E5` (a Segoe Fluent Icons code point; those Win顺 uses itself are drawn as the matching [Phosphor](https://phosphoricons.com) icon, the same on every Windows, the rest with the system icon font, which an older Windows 10 may lack), `ph:heart` (a named icon from the bundled Phosphor 2.1.2 set; `ph-fill:heart` uses the Fill weight; names are in `third_party/phosphor/metadata/regular.css`; an unknown name draws the puzzle icon; independent of the Windows version), `color:#ff8800` (a swatch), `data:image/png;base64,…` (≤ 64 KB), `site:https://…` (the site's own icon, as a browser shows it: Win顺 asks the site's home page for it once, keeps it and renews it monthly; the browser's icon until then; nothing of the address past the host is sent). Paths outside the plugin folder and SVG are refused.

## Permissions

`permissions` is an array: `["processes", "clipboard:read", {"files:read": ["%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Bookmarks"]}]`. Turning a plugin on lists them: what Windows enforces is ticked, the rest is marked "declared only". Honestly:

| Permission | Meaning | Enforced | How |
|---|---|---|---|
| (always) not administrator | the plugin runs with your normal rights; `elevated` below is the only exception | **yes** | the token loses the Administrators group and the privileges; `normal` medium integrity, `strict` low; memory limit, a job that ends with Win顺 |
| `elevated` | follow Win顺's administrator rights: while Win顺 runs as administrator the plugin does too, and can drive windows opened as administrator (an elevated file manager). While Win顺 is not elevated it runs with normal rights like any plugin; **never a UAC prompt for a plugin** | **yes** | only for a plugin's own exe (`runtime.program` an exe: an interpreter or its modules may sit in a folder you can write to), not with `sandbox: "strict"`. What runs as administrator is the copy you confirmed, not the plugin folder (see below); job and memory limit as usual |
| `processes` | child processes, or having Win顺 open a program / a non-web link | **yes** | without it the job's `ActiveProcessLimit = 1` (`CreateProcess` fails with 1816), and Win顺 will not carry out an `open` of a program, a script, a non-web URL, or one with args/cwd on its behalf (item and `open` effects, and `open_item`, all alike: only folders, ordinary files and http/https/mailto, with a footer notice when refused); with it the children are in the job and `open` is unrestricted |
| `clipboard:read` / `clipboard:write` | the clipboard directly | **yes** | job UI restrictions; `GetClipboardData` returns nothing without it |
| `ui` | own windows, other windows | partly | without it the job restricts the desktop and USER handles, and legacy extension points are disabled; UI plugins retain legacy input-method extension points while other process mitigations and job limits still apply |
| `system:settings` | Windows user settings: theme, Explorer, radios, volume and keep-awake | **no** | declared only, with normal user rights; required for system switch providers; does not grant administrator rights |
| `winshun:search` / `winshun:open` / `winshun:history` | Win顺's tools | **yes** | `winshun/tool` is gated |
| `winshun:content` | the text Win顺 took out of documents and text files (`list_documents`, `read_file_text`) | **yes** | `winshun/tool` is gated; no Word or PDF parsing of its own, and the disk is not read again |
| `files:write` | writing your files | `strict` only | `strict` cannot write your folders, only `<id>.data`; `normal` writes as you do: **declared only** |
| `files:read` | which files it reads | **no** | a declaration for the user (medium integrity reads anyway) |
| `network` | going online | **no** | Win顺 cannot stop it; the card says so |
| `network:listen` | opening ports on the local network for other devices: `{"network:listen": ["TCP 53317-53320", "UDP 53317"]}` | **no** | a declaration of the ports for the user; the card says Win顺 cannot stop it. Win顺 adds no firewall rule for it (the installer adds one for the bundled LAN transfer) |
| `notifications` | system notifications (`toast` in `winshun/notify`) | **yes** | without it Win顺 shows them as footer notices; with it, one every 2 seconds at most |

**`elevated`: confirmed once before it runs as administrator.** A plugin's folder (`%LOCALAPPDATA%\WinShun\plugins\<id>`, a folder in development) is yours to write, so any program with your normal rights could swap its exe or drop a DLL beside it, waiting for an elevated Win顺 (often started at logon in direct-read mode) to start it. So Win顺 never runs the plugin folder itself as administrator:

- While Win顺 is elevated, Confirm on the plugin's card (or turning it on and allowing its permissions then) makes a copy in the background, in `%ProgramFiles%\WinShun Elevated Plugins\<your SID>\`: ordinary users cannot make folders in Program Files, Win顺 makes these with Administrators as owner, only administrators and SYSTEM can open them, and each file's SHA-256 is recorded beside the copy. The elevated plugin's exe, working folder, `WS_PLUGIN_DIR` and manifest (`runtime`'s program, args and env) come from that copy alone, and the copy's manifest must be exactly the one Win顺 reads now.
  - **A plugin of the Library** (the signed list has it): the copy is made from its package, downloaded again and checked against the signed list, whose files must be exactly the installed ones, else nothing is confirmed ("it may have been changed; reinstall it from the Library"); when the list has another version than the installed one, update first.
  - **A plugin installed by hand or in development**: the copy is the plugin folder's files at the moment of the click. The folder must not itself be a link (junction, symbolic link) and must hold no links or hard links; to test a plugin in development elevated, use a real folder, not a link.
- Before every start Win顺 compares the plugin folder with the record. The same: the copy runs as administrator. Different (never confirmed, a file changed, a file added or gone, a link, the manifest changed, or the store not what it must be: a link, not owned by administrators, changeable by ordinary users): it runs from its folder with normal rights, and its card says why; changed files read "The plugin's files were changed... If you don't know who changed them, don't confirm". Changed code never runs as administrator without you; the comparison only decides between the confirmed copy (as administrator) and the plugin folder (normal rights), so even a wrong answer never runs unconfirmed files as administrator.
- An elevated Win顺 updating a confirmed plugin from the Library unpacks the bytes into a new confirmed copy only when the **signed list** (its signature checked, not the unsigned local file of development) has the very same package (version, size, SHA-256); no click needed then. A package older than the confirmed version is refused. The Library tab's own list is not signed and does not count on its own. Updates installed by an unelevated Win顺 and zips or folders you install yourself are confirmed again once Win顺 runs elevated.
- An elevated plugin gets a clean environment: the system's variables only (`HKLM`), less those that make programs load code (`COR_*`, `DOTNET_*`, `NODE_OPTIONS`, `PYTHON*`, `_NT_SYMBOL_PATH` ...), the system `PATH` with only the folders normal rights cannot write to (a Python or VS Code in your user folder is left out even when the system PATH has it), your folders (`USERPROFILE`, `APPDATA`, `LOCALAPPDATA`) and `WS_*`; `TEMP`/`TMP` is a protected folder beside the copy. Your own variables in `HKCU\Environment` do not reach it, nor does `WS_COMPONENTS_DIR` (the shared components folder is changeable with normal rights).
- While Win顺 is elevated, a plugin with `elevated` is not offered to AI assistants or the command line (their requests come from programs with normal rights).
- Up to the plugin: `WS_DATA_DIR`, `WS_SETTINGS` and its settings can be changed with normal rights, so treat them as untrusted input; never load code from the data folder; a DLL not found beside the exe is looked for along the system PATH, so ship every DLL you need.

In the manifest: `"permissions": ["elevated", "ui"]`, `"runtime": {"mode": "resident", "program": "bin\\tool.exe"}`. An older Win顺 does not know `elevated` and refuses the whole plugin, so set `"minWinShun"` too.

**More permissions need your OK again.** You turned on a plugin, and then a newer version arrives: an update from the 插件库, a zip or folder installed over it, or an edited `plugin.json`. If the newer version asks for any permission you did not allow, the plugin pauses. It is not started, answers no queries, is not offered to the AI and sends no notifications. Its card names the new permissions. Turn its switch on: the usual list of all its permissions appears, with the new ones marked. Allow them and it runs again; until you do, it stays off. The same or fewer permissions change nothing. Plugins installed with Win顺 come with its updates and stay trusted. Only permission names are compared: other `files:read` paths or `network:listen` ports are not asked about again.

**Plugin updates.** With Automatic updates on (Settings → Advanced → Updates, the default), WinShun checks the plugins you installed against the Library list once a day; the list is signed and verified through `index.json.sig`. A newer version whose permissions, read paths and ports are unchanged is put in place at once when the plugin is not running; a running one is kept aside and swapped in at the next start (or press Update now in the Library). A version that asks for new permissions is never installed by itself: for a plugin that is on you get one tray notification per version, the number on the Plugins entry in Settings and the tray line "N plugin updates available…" lead to the Library filtered to updates, and Update and allow installs the new version and accepts its new permissions in one step, leaving the plugin on. Cancel keeps the installed version running, and that version is no longer counted or announced. The Library's Update all only does the updates that need no new permission. An unsigned or wrongly signed list, developer mode, and a copy in development (a link to its source) are only announced, never replaced.

Install plugins only from sources you trust.

## Settings

`settings[]`: `{"key", "type", "title", "hint", "default", "options"}`, `type` ∈ `text | password | bool | number | choice | folder | file | hotkey`. Drawn with Win顺's own controls on the card; stored in `WinShun.ini` as `[Plugin.<id>] Setting.<key>=`; **`password` goes to Windows Credential Manager**, never into the INI or the logs (masked in the message log). `choice` options are strings or `[{"value": "deepl", "title": {"zh": …, "en": …}}]`.

The plugin sees the merged JSON (declared defaults under the user's values): `WS_SETTINGS` for once plugins, `initialize`'s `settings` for resident ones, then `settingsChanged`.

## For AI

With `"ai": {"expose": true}` in the manifest and "Allow AI to use plugins" on under Settings → AI assistants, every command becomes the MCP tool `plugin_<id>_<command>` (dots to underscores: `plugin_winshun_tools_calc`). A call is one `query` (the plugin is started if it is not running) returning `{items: [{id, title, subtitle, url?, path?, copy?}], returned}`, without `data`; `readOnlyHint` is true. `ai.description` (English, for the model) and `ai.parameters` override the description and the schema (default `{query, limit}`).

Actions are **not** exposed unless "Allow AI to run plugin actions" is on; then `plugin_<id>_<command>_run` (`destructiveHint`) takes the same `query`, the row's `item_id` and an optional `action`; Win顺 queries again, finds the row, sends `action`, and answers `{done, message, effects}`. Each card has a "For AI" switch; a plugin that is turned off answers "plugin X is turned off". Connected clients are told when the tool list changes.

## Debugging

- **Developer mode** (Settings → Plugins → Advanced): a changed script restarts its resident plugin; the message log shows every line (time, plugin, direction, method, milliseconds to the response), click one for the whole message; "open plugins.log" (`%LOCALAPPDATA%\WinShun\plugins.log`: stderr, protocol errors, crashes).
- **Command line** (`build\release\wsbench.exe` from a source build; no window):

```powershell
wsbench --plugin D:\my.plugin --query "hi 你好" --runs 20                 # validate the manifest, start through the runner, print the rows and the timings (cold start, first round trip, steady median) and the process's integrity level and job
wsbench --plugin D:\my.plugin --query "hi 你好" --setting greeting=hey    # a declared setting's value
wsbench --plugin D:\my.plugin --query "kill note" --action kill          # run an action on the first row, print the effects
wsbench --launcher-sim --plugins D:\plugins "hi 你好" "#ff8800"          # the launcher's merge, key by key
wsbench --mcp-host --plugins D:\plugins                                 # serve the plugins as AI tools on the test pipe (WinShunMcp.exe --pipe …)
```

## FAQ

- **Garbled Chinese.** UTF-8 on both ends: Python gets `-X utf8`; a PowerShell script **must be saved as UTF-8 with a BOM** (Windows PowerShell 5.1 reads a bare .ps1 in the ANSI code page) and set `[Console]::OutputEncoding = [Text.Encoding]::UTF8` (`WinShun.psm1` does). `"encoding": "oem"` in the manifest rescues old scripts, Latin characters only.
- **Python not found, or a Python installed with uv "crashes" at once.** Win顺 starts `python.exe` itself. It looks at the registrations first (PEP 514, `HKCU/HKLM\Software\Python\<company>\<tag>\InstallPath`): python.org's `PythonCore` first, then uv's `Astral`, Anaconda's and the others, the newest in each; then for `python.exe` on PATH (skipping the Store's alias under `WindowsApps` and going on). A plugin's job allows one process, so a launcher that starts python.exe as a child cannot run as it is, and Win顺 starts the real one instead: `py.exe` is never used; a venv's `Scripts\python.exe` (venv or uv venv) becomes the `python.exe` in its `pyvenv.cfg`'s `home`, with `__PYVENV_LAUNCHER__` set as the launcher sets it, so `sys.prefix` and site-packages are still the venv's; the `python.exe` in uv's bin folder (the one `uv python install --default` puts on PATH) becomes the interpreter it points at. When that interpreter is gone (uv uninstalled the version, say), the card names the file and why; install Python from python.org, or run `uv python install` again. No restart needed: after a miss Win顺 looks again a few seconds later.
- **Timeouts.** A query's budget is `timeoutMs` (resident 1500, once 3000, at most 10000); send `query/partial` or `winshun/progress` early and do the heavy work on a thread. Starting a process costs about 15 ms (Defender), Python tens of milliseconds, PowerShell hundreds: hence resident PowerShell.
- **A resident plugin prints and nothing happens.** stdout on a pipe is block-buffered: flush after each line (the SDKs do; Python also gets `-u`).
- **Enter does nothing.** The row has `"valid": false`, or the command declares no `actions` and the plugin did not answer `action`. A row with `open` is opened by Win顺 itself; no `action` is sent.
- **Writing files.** Write to `WS_DATA_DIR` (`<id>.data`; uninstalling asks whether to delete it); `strict` plugins can write nowhere else.
- **Icons missing.** PNG / JPG / ICO inside the plugin folder only; no SVG.
- **Keyword taken.** It clashed with a Web search keyword or another plugin's; the user's own wins and the card says so. Pick another.
- **Can the plugin folder be changed?** A `normal` plugin runs as you and can change what you can — that is what developer mode's reload relies on. Use `strict` to keep a plugin from changing itself.

### Web search host capability

`webSearch: true` is reserved for official `winshun.web`: no `runtime`, an empty `commands` array, and sites from plugin settings. The host supplies URL handling, result rendering and a lazily loaded editor. An absent or disabled provider contributes no results or keywords and requests no suggestions. Legacy official URL manifests are adapted in memory without rewriting user files. Packaging sets `minWinShun` to the current host version.

## Asynchronous C++ queries

The C++ SDK supports `queryAsync(const Query&, std::function<void(Json)>)`. Its default calls `queryResult` synchronously. An asynchronous implementation snapshots settings and locale on the protocol thread, replies exactly once, handles `cancel(queryId)`, and joins workers in `shuttingDown()` and the derived destructor. A worker can send `partial()` results while the request is pending. The wire protocol is unchanged.
