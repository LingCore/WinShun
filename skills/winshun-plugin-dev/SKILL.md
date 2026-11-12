---
name: winshun-plugin-dev
description: Write, test and publish a plugin for WinShun (Win顺), the Windows launcher and file search app - a keyword command in its search box that lists results and runs actions. Use it when the user wants to make, change, debug, package or publish a WinShun plugin (开发、调试、发布 Win顺 插件), or asks what a plugin can do.
---

# Developing a WinShun plugin

A plugin is a folder with a `plugin.json` (the manifest). It adds keyword commands to WinShun's search box: the user types the keyword and some text, the plugin answers with rows, and Enter runs a row's action. Three ways to write one, the same protocol for all:

| Kind | You write | Good for |
|---|---|---|
| zero-code | only `plugin.json` | a keyword that opens a URL, a program or a folder, a saved search, a fixed list |
| `once` script | any language; the query comes in environment variables, rows go to stdout | small tools used now and then |
| `resident` | any language; JSON-RPC 2.0 on stdio, one message a line | every-keystroke queries, caches, PowerShell (slow to start) |

Pick the simplest kind that does the job. Do not guess the protocol: read the reference first.

## Read these before writing code

- Protocol, manifest fields, permissions, settings, examples: https://raw.githubusercontent.com/LingCore/WinShun/main/docs/plugins.md (English: `docs/plugins.en.md`)
- Manifest JSON Schema (put it in `"$schema"` for editor checks): https://raw.githubusercontent.com/LingCore/WinShun-plugins/main/schema/plugin.schema.json
- SDK files, copied next to your script: `winshun.py`, `WinShun.psm1`, `winshun.h` from https://github.com/LingCore/WinShun/tree/main/src/plugins-sdk
- Publishing rules: https://github.com/LingCore/WinShun-plugins/blob/main/CONTRIBUTING.md

## The smallest plugin

```json
{
  "$schema": "https://raw.githubusercontent.com/LingCore/WinShun-plugins/main/schema/plugin.schema.json",
  "manifest": 1,
  "id": "me.hello",
  "name": {"zh": "打招呼", "en": "Hello"},
  "version": "1.0.0",
  "runtime": {"mode": "once", "program": "python", "script": "main.py"},
  "commands": [{"id": "hi", "keyword": "hi", "title": "打招呼", "hint": "名字"}]
}
```

```python
import winshun                      # winshun.py from the SDK, beside this file

q = winshun.query()                 # q.text: what was typed after the keyword
winshun.reply([winshun.item("hi", f"你好，{q.text}", "Enter 复制", copy=q.text)])
```

The id is `author.name` (lower-case letters, digits, dots, hyphens, underscores) and never changes; the folder has the id's name once installed.

## Install and try it: the loop

Work in a folder of its own that holds only the plugin's files (everything in it is copied: no `.git`, virtual environments or build output). WinShun's `ws` command installs it; it needs "Allow AI assistants and the command line to connect" turned on in WinShun's Settings > AI assistants (exit code 4 says so).

1. `ws plugin install <folder>`: puts the folder in place. A new plugin stays off.
2. `ws plugin enable <id>`: the first time, WinShun shows the user what the plugin asks for, in its own window, and this command exits 1. Ask the user to confirm there; do not try to answer for them.
3. The user types the keyword in WinShun's search box. You cannot type there yourself: say exactly what to type and what should appear.
4. `ws plugin list --json`: the plugin's `state` (on, off, waiting, unusable), its program's `status` (ready, running, crashed...) and `error` (a bad manifest, a missing program, the last crash).
5. `%LOCALAPPDATA%\WinShun\plugins.log`: the plugin's stderr, protocol errors and crashes. Read it after every failed try.
6. After a change, run `ws plugin install <folder>` again: it replaces the installed copy, and a plugin that was on stays on. A version that asks for more permissions waits for the user's yes again (`ws plugin enable <id>`).

Without `ws` (an older WinShun): the user picks the folder in Settings > Plugins > Install from a folder. Settings > Plugins > Advanced > Developer mode shows every message between WinShun and the plugin and restarts a resident plugin when its script changes.

## Rules that keep a plugin correct

1. A query only answers. It never deletes, starts programs, changes the clipboard or opens windows: the user's Enter does (the `action`). Typing must have no side effects.
2. stdout carries protocol JSON only, UTF-8; diagnostics go to stderr. A resident plugin writes one JSON-RPC message per line and flushes. PowerShell scripts are saved as UTF-8 with a BOM.
3. Ask only for the permissions the code uses, and say why in the README. Do not ask for `elevated` unless the plugin must act on windows that run as administrator.
4. Data goes under `WS_DATA_DIR`; secrets are `password` settings (kept in Windows Credential Manager), never in source, plain settings or logs.
5. Give rows stable ids, readable titles and subtitles, and a clear row for "nothing found" and for errors. Never return made-up results when a request failed.
6. Network calls have timeouts and honour cancel. Nothing heavy is reloaded per keystroke.
7. Check the real round trip in WinShun (steps 3 to 5), not only the script run by hand.

## Publish

1. Put the plugin in a public GitHub repository of its own: `plugin.json`, `README.md`, `LICENSE`, the code and the SDK files at its top level.
2. Bump `version`, tag `v<version>`, and attach `<id>-<version>.wsplugin` (a zip of the folder's files) to that release. The plugin library's `template/release.yml` workflow builds and attaches it for you.
3. Open a pull request to https://github.com/LingCore/WinShun-plugins that adds one file, `plugins/<id>.json`, generated with its `tools/entry.py <release asset URL>`. With `"autoUpdate": true` later versions are picked up without another pull request.
4. A package with an exe or dll is listed as "unreviewed native": WinShun asks the user at every install and update. It needs public source and `minWinShun` 0.22.0 or later.

When done, tell the user what changed, what you verified in WinShun, and what is still unverified.
