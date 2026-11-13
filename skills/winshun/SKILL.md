---
name: winshun
description: Find files on this Windows PC by name (pinyin too) or by the text inside them (Word, Excel, PowerPoint and PDF included), read a document's text, or list recently changed files, with WinShun's ws command; also list, find, install and switch WinShun's plugins. Use it whenever the user asks to find, locate or read a file on this computer, or to install or manage a WinShun (Win顺) plugin.
---

# WinShun on the command line: ws

`ws` asks WinShun (Win顺), which keeps an index of every file and folder name on the local drives. A name search takes milliseconds across millions of files: use it instead of a recursive `dir`, `Get-ChildItem` or `find` to locate files anywhere on the PC.

## Commands

- `ws find "quarterly report" -n 20`: files and folders by name (`ws "report"` is the same)
- `ws grep "invoice 2024" -n 20`: files containing the words, as `path:line:text`; documents give `p3` (page), `s2` (slide), `[Sheet1]r12` (row). `"a|b"` finds either word; a word starting with `regex:` is a regular expression matched within a line (`ws grep "regex:invoice-\d{4}"`), fast when it has some fixed text in it
- `ws text "D:\Docs\plan.docx"`: the text of a file or document (docx, xlsx, pptx, pdf); a long one in parts with `--from <offset>`
- `ws recent --days 7 --ext docx,xlsx`: files changed lately, newest first
- `ws ls "D:\Projects"`: a folder's entries, from the index
- `ws open "plan" --ext docx`: opens the best match as a double-click would; only when the user asks to open something
- `ws clip -n 5`: the clipboard history, when the user allowed it
- `ws status`: whether WinShun runs and its index is ready
- `ws plugin list`, `ws plugin search translate`, `ws plugin install <id | package.wsplugin | folder>`, `ws plugin enable <id>` (also `disable`, `remove`): WinShun's plugins, see below

## Query syntax (find)

- All words must match, in the name or in a parent folder's name; Chinese names also match pinyin (`bg`, `baogao` for 报告).
- `"exact phrase"`; `!word` excludes; `*.pdf` and `?` are wildcards; `ext:pdf,docx`; `folder\name`.
- `--in <folder>` keeps to a folder (repeatable; `--in .` is the current one); `--files` or `--folders`; `--ext pdf,docx`.
- `--sort newest` (or `oldest`, `name`, `largest`…); by default the best matches come first.
- People describe a file by what it is about: when nothing fits, try synonyms, shorter stems, Chinese and English (报销, 费用, reimbursement), then `ws grep`.

## Plugins

- `ws plugin search <words>` lists the plugin library (id, version, whether it is installed, `unreviewed` for packages nobody reviewed); `ws plugin install <id>` installs one, `ws plugin enable <id>` turns it on.
- Exit code 1 from install, enable or remove means WinShun is asking the user in its own window (the permissions of a plugin turned on for the first time, an unreviewed package, a removal). Tell the user to answer there, never answer for them, then check with `ws plugin list`.
- Install only what the user asked for; say which permissions a plugin asks for (`--json` lists them).

## Output

- One full path per line on stdout; hints go to stderr and start with `ws:`.
- `-n 20` limits the results (default 100; `-n 0` gives as many as allowed). Keep it small.
- `-l` adds size and modified time; `--json` prints one JSON array (`path`, `type`, `modified`, and `size` with `-l`); `-0` separates paths with NUL.
- `--timeout 15s` lets grep search longer (default 5s, at most 30s); `--wait 30s` waits for an index that is still loading.

## Rules

- Always quote the query: bash expands `*`. Options go before `--`; after it everything is the query.
- Exit codes: 0 found, 1 nothing found, 2 wrong usage, 3 WinShun is not running, 4 off in WinShun's settings: ask the user to turn on the switch stderr names, 5 the index is not ready, 6 the path cannot be seen, 7 other errors (stderr says which; if another content search kept `ws grep` from starting, run it again).
- Show paths to the user as they are.
