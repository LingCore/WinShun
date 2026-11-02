# fx186: search preview with the mouse (#186, #174 part 2) and open at the matching line (#177)

## Automated (build\release, `ctest -R "tst_searchnavigation|tst_contentpane|tst_clipboardsizing"`, all pass)

- `tst_searchnavigation` (production Main.qml, Launcher, FilePreview, PreviewWindow, synthetic Qt events):
  - Space right after typing types a space; after an arrow key it toggles the preview; Shift+Space always types.
  - Middle click on a gallery tile previews that tile and opens nothing, the query is unchanged.
  - Hover shows nothing before 100 ms, shows the hovered tile after 250 ms; the current tile stays selected.
  - An arrow key brings the preview back to the current tile; a middle click on the shown tile closes it;
    hovering never opens a closed preview.
  - Text preview: a left drag from line 1 to line 3 picks across rows (`FilePreview::selectedText()` has the
    line breaks), the text does not move, the wheel still scrolls, the pick is gone when the preview closes.
- `tst_contentpane`: the editor table (`contentopen::arguments`) for the VS Code family, Sublime/Zed, JetBrains,
  Notepad2/3/4, EmEditor, gvim, and the refusals (unknown editor, line 0, a quote in the path).
  Needs the `WinShunExtract` target built.
- `tst_clipboardsizing` still passes (ClipRow / ClipboardPage changed: middle click).

## Open time of a text preview (1.3 MB, 24000 lines, same probe, preview window shown and settled)

| | loaded | shown |
|---|---|---|
| before (read-only `Text` delegates) | 74-86 ms | 496-560 ms |
| after (`TextEdit` delegates + pick layer) | 98 ms | 513 ms |

## Live (stage copy, own profile, real input, E:\wsi12-test\fx186)

- #177: content scope on a test folder (`Index/Scope=folders`, `Access=walk`, content index off, direct read),
  Enter on `notes.md` (match on line 37): `Code.exe --goto "E:\wsi12-test\fx186\notes.md:37"` was started.
- #186 (`live-middle-click-hover-drag-select.png`, PreviewWith=builtin): real middle click on row 3 (notes.md)
  opened the preview and opened no file; hovering row 4 switched the preview to notes2.txt while row 3 stayed the
  current row; a real left drag in the preview picked text over lines 1-3 (highlight, no flick, numbers aligned).
  Ctrl+C was not sent live (it would put text into Windows' clipboard history).
- Afterwards: the three HKCU SearchHere keys and the `\WinShun` task still point at C:\Program Files\WinShun.

## Not verified

- Sublime Text, Zed, EmEditor, Notepad2/3/4, JetBrains, gvim and Windsurf are not installed here; their syntax is
  from the official docs (Sublime: `subl file:line`, Zed: `zed file:line`, EmEditor: `/l line`, Notepad3/4: `/g line`,
  JetBrains: `--line N path`). Cursor is installed: its cli.js has the VS Code `--goto` option. Sublime's GUI exe
  `sublime_text.exe` and Zed's `Zed.exe` are assumed to take the same `file:line` as their command-line helpers.
- Trae is not in the table: not confirmed to be a VS Code fork with `--goto`.
