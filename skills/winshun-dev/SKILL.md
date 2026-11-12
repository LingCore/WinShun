---
name: winshun-dev
description: Work on WinShun (Win顺) itself - the Windows launcher and file search app in the LingCore/WinShun repository (C++20, Qt 6 Quick/QML, CMake, MSVC). Use it when building, changing, testing or reviewing code in a clone of that repository (开发 Win顺 本体、改代码、修 bug、提 PR). For writing a plugin, use winshun-plugin-dev instead.
---

# Working on WinShun

Run everything from the root of a clone of https://github.com/LingCore/WinShun. Windows only. The repository's own documents are the reference; this skill says where to look and what trips people up. Read `AGENTS.md` first: it holds the project's standing rules.

## Build

Needs Visual Studio 2022 with the C++ workload (MSVC x64), CMake 3.25+, Ninja, and Qt 6.8 or later (6.12 recommended, the MSVC 2022 64-bit kit). The first configure downloads PDFium, ONNX Runtime and OCR models, so it needs the network.

```powershell
./scripts/build.ps1                                   # Release into build\release
./scripts/build.ps1 -Tests 'tst_core|tst_cli'         # build, then only these tests (a ctest -R regex)
./scripts/build.ps1 -BuildDir build\<task> -DeployDir dist\<task>\WinShun   # a copy you can run, with Qt's DLLs
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64   # else %QTDIR%, else the newest under C:\Qt
```

- Compile from PowerShell, not through Git Bash. Task builds go in `build/<task>/`, deployments in `dist/<task>/`, a separate checkout in `.worktrees/<task>/`; clean up what you created.
- Output: `build\<dir>\WinShun.exe`, the other programs beside it, `bin\ws.exe`, and every `tst_*.exe`.
- New C++ and QML files are listed by hand in `src/app/CMakeLists.txt` (and the other folders' lists). Nothing is globbed but the themes.

## Run without disturbing the user's copy

WinShun is single-instance: a second start, and `--quit`, `--toggle` or `--settings`, go to the copy already running. Never quit or replace a running WinShun unasked; it may be the user's, with unsaved work.

- `WinShun.exe --preview-settings <folder>`, `--preview-results <folder>`, `--preview-plugins <file.png>`: render the real UI offscreen into pictures, on a temporary profile, beside any running copy. Add `--dark`, `--scale 1.5`, `--english`, `--style fluent|brand|tactile|liquid`, `--renderer d3d11` (needed to judge exact pixels). The exit code counts QML warnings; `preview.log` is beside the pictures. Many `tst_*` tests are such runs (`tests/CMakeLists.txt`).
- `--profile <folder>` keeps settings and data in that folder, for a test copy; it still cannot run beside another copy, and it repoints the Explorer menu's registry entries at itself unless its ini has `ExplorerMenu=false` under `[Launcher]` (`docs/pitfalls.md`).
- From a build folder Qt's `bin` must be on `PATH`; a `-DeployDir` copy needs nothing.

## Test

- Qt Test, one program per `tests/tst_<topic>.cpp`, each registered by hand in `tests/CMakeLists.txt`.
- Run only the tests your change touches, never the whole suite: `ctest --test-dir build\<dir> --output-on-failure -R '<regex>'`, or one function directly: `build\<dir>\tst_core.exe <function>`.
- Write the failing test first for a bug, then fix it. Some tests skip unless elevated, and say so.
- Anything in `installer/`, or the code that starts Setup, is checked by a real install and upgrade: `tools/verify-installer.ps1`.

## Where things are

| Folder | What |
|---|---|
| `src/core` | search engine, NTFS index, content index, clipboard store (Qt Core only) |
| `src/app` | the UI and Windows integration; `qml/` (QML, `themes/`), `platform/` (Win32), `i18n/` |
| `src/service`, `src/stub`, `src/updater` | index service, taskbar button, update helper (no Qt) |
| `src/mcp`, `src/cli` | the MCP server inside WinShun and its relay; `ws.exe` |
| `src/plugin`, `src/plugins-sdk`, `src/*-plugin`, `plugins/` | plugin system, SDK, official plugins |
| `src/extract`, `src/capture` | document text readers; screenshot and recording |
| `tests`, `tools`, `scripts`, `installer`, `docs` | tests, probes and checks, build and release scripts, Inno Setup, documents |

| Task | Read |
|---|---|
| where code lives and why it is built so | `docs/architecture.md` |
| before touching QML, DPI, windows, builds, live tests | `docs/pitfalls.md` (search it for the area you edit) |
| colours, styles | `docs/colors.md`, `src/app/qml/themes/README.md` |
| plugins, protocol, SDK | `docs/plugins.md`, `src/plugins-sdk/DEVELOPMENT.md` |
| command line, AI tools | `docs/design-cli.md` |
| releasing | `docs/releasing.md` |
| what users are told | `README.md`, `docs/使用说明.md`, `docs/features.en.md` |

## Conventions

- C++20, `/W4`, `.clang-format` (WebKit base, 120 columns, `Type* p`). Strings are `u"..."_s`: `QT_NO_CAST_FROM_ASCII` is on. `namespace ws`, members `m_name`, constants `kName`.
- Comments are English prose that say why, not what. UI text is Chinese in `tr()` / `qsTr()`; its English goes in `src/app/i18n/winshun_en.ts` (`cmake --build <dir> --target update_translations`, then fill in every `unfinished` entry).
- Commits: English, sentence case, no prefix convention, the issue as `(#123)`, one feature per commit, a body that says what and why and what was not tested.
- A user-visible change updates the user guide (`docs/使用说明.md`) and, where they describe it, the README files and `docs/features.en.md`.

## UI rules

- Compact, modern, consistent with what is there. Colours only from `Theme.qml`; four styles, each light and dark.
- Function icons come from the bundled Phosphor set: `Glyph { iconName: "heart" }`. Reuse the icon that already means the same thing.
- Windows keep DWM's own corners, edge and shadow: never draw them.
- Everything must hold at 100, 125, 150 and 175 % scaling. Hairlines use `Theme.edge(dpr)`, positions `Theme.snap`; check real pixels (`--renderer d3d11 --scale 1.5`), not by eye.
- Never `scale` a `Glyph` or text (use `Glyph.inkScale`); `sourceSize` is logical pixels for `image://` and SVG, physical for PNG and JPG.
- A QML file must not have the name of a C++ `QML_ELEMENT` class; a line starting with `(` or `[` joins the line before it.

## Before you say it is done

Build it, run the tests the change touches, and look at the result in a preview or a deployed copy when it is UI. Report what you verified and what you did not.
