#pragma once

// Renders settings pages to PNG files without showing anything, for checking
// their layout in both themes and scalings without touching the Win顺 that
// is running. Modes:
//
//   WinShun.exe --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog] [--page ai]
//     The 插件 page on its own (PluginsPreview.qml) over made-up plugins
//     (PluginSettings::loadPreview), or the AI 助手 page.
//     --menu card --pointer-check checks pointer hit/highlight alignment
//     through repeated menu-row changes, including first/last-row jumps.
//
//   WinShun.exe --preview-settings <folder> [--dark] [--scale 1.5] [--tree]
//     [--corners round|square] [--page 1]: one appearance for a selected page.
//     [--web-form] [--compact]: exercise adding/editing web shortcuts, focus,
//     validation and scrolling at 920x720 or the minimum 760x420 window.
//     Add --renderer d3d11 for the GPU path (default: software).
//     --combo-popup checks the action and font menus' corners, control gap,
//     inner padding and delegate width, and captures both open menus.
//     --style-picker checks all four styles, keyboard/pointer selection,
//     persistence, and the minimum/default/wide settings layouts.
//     --glass-controls checks liquid sliders, drag preview versus persistence,
//     keyboard/accessibility, reset, visibility and narrow/default layouts.
//     --composited-frame shows Windows 10's square frame on any OS.
//     --wsl-folders checks WSL folder identity and renders stopped/running
//     cards; --english and --compact cover translation and minimum size.
//     --backup-settings checks the real backup worker and success/failure
//     feedback with automatic backups off; --compact uses 760x420.
//     --plugin-recovery checks the recovery row's busy, offline and retry
//     states; --english --compact also checks the minimum translated layout.
//     --preview-hotkeys checks the real shortcut recorder, persistence,
//     cancellation, clearing and conflicts; --compact covers minimum width.
//     The whole settings window (the real SettingsWindow.qml over a real
//     SettingsEditor, WindowFrame and Updater on a temporary profile): every
//     page and every tab of it as <folder>\pNN-tT.png, the window as tall
//     as the page needs, then the settings search driven with a few words
//     (search-<word>.png, and where Enter would go, checked against the
//     page it should be). The log is <folder>\preview.log.
//
//   WinShun.exe --preview-results <folder> [--dark] [--scale 1.5] [--size compact|standard|roomy]
//     The real result model and rows over made-up folders, with D3D11 native
//     text; hint modes and local Qt modifier events. --rows-source <folder>
//     compares a saved ResultRow.qml and IconNumber.qml from before an edit;
//     --picked shoots rows 2 and 5 picked (the highlight of rows not current).
//     --file-metadata --gallery-image <demo.png> also exports the production
//     search panel and resized clipboard with demo entries for screenshots.
//     --file-metadata --file-gallery checks the real Main.qml gallery with
//     generated temporary PNGs, real name searches and shell thumbnails,
//     keyboard navigation, selection, mode switching, zoom and minimum width.
//     --drop-pie-check checks the ordinary-user helper's real OLE registration,
//     favorite refresh and shutdown on a temporary profile, with no file drag.
//     --drop-pie-hit [--renderer software|d3d11] [--unrevealed] shows the
//     real pie window at the middle of the primary screen for under a second
//     (never active, no input sent, not while a mouse button is held: exit 5)
//     and asks WindowFromPoint whether every point over it is its own (GitHub
//     #150); --unrevealed as it waits for a drag, nothing of it drawn. Exit 0
//     all of them, 4 some not, 6 not measured (hidden meanwhile, or not the
//     renderer asked for). drop-pie-hit.json has the counts. No file drag.
//     --scope-memory checks that the scope the user chose (a tab, Tab, a
//     chip) is kept in state.ini and comes back in a new Launcher set up as
//     App sets it up, and that a prefix typed or the scope set from inside
//     is not kept (GitHub #263); scope-memory.json lists what failed.
//     It also exercises configurable preview keys through Main.qml and the
//     real FilePreview window, including held Space and Shift+Space input.
//     --preview-hotkeys runs this production key/preview check on its own.
//     --search-navigation also checks the production result menus in the
//     search box and panel: arrows, Ctrl+O, caret/selection, IME, and Enter.
//     --search-workload checks automatic vs explicit content searches over
//     real temporary OOXML/text files, records app/extractor CPU time, and
//     checks the continuation in the box, panel and gallery without busy waits.
//     --content-pane checks all five detail tabs with real text/spreadsheet
//     fixtures, selection races, filtering, report scopes and native preview.
//     --foreground-memory measures private bytes, working sets, object counts,
//     search and panel-opening times using Main.qml and 2,000 fixture names.
//     --require-lazy-panel also checks deferred creation and reuse;
//     --preload-panel makes the panel ahead, hidden, as the app does. Windows
//     stay offscreen and inactive; no global input or working-set trimming.
//
//   WinShun.exe --preview-ai-cards <folder> [--dark] [--scale 1.5]
//     The screens for AI changing settings (SettingsConfirm.qml, SettingsNotice.qml)
//     over made-up changes: confirm.png, confirm-always.png, notice.png, notice-undone.png.
//
//   The screen capture's previews are the plugin's (WinShunCapture.exe,
//   src/capture-plugin/CapturePreview.h).
//
//   WinShun.exe --probe-runtime <folder> [--with-clipboard]
//     --clipboard-shell-probe <SearchHost.exe|native> checks both clipboard
//     hotkey handlers, foreground preservation and flyout avoidance. The fixture
//     is tst_keyrouter.exe renamed SearchHost.exe; native uses the real Start
//     menu. Neither mode writes the system clipboard or changes global hotkeys.
//     [--runtime-plugins <folder>] [--settle-ms 3000] [--renderer d3d11]
//     --live [--style …] [--dark] keeps it running on the real desktop at the
//     screen's own scale, beside the user's Win顺, with a few made-up clips,
//     taking commands from <folder>\cmd.txt until <folder>\stop exists
//     (runLive in Preview.cpp): a real-window audit without stopping theirs.
//     A separate production startup probe: real App::start, temporary profile,
//     process-local HKCU redirection, no global shortcuts/service/shell setup.
//     --scope-restore-check <all|files|content> [--clear-on-open] writes that
//     scope into the profile's state.ini first and checks the started App's
//     launcher is in it (in 全部 with 每次打开时清空搜索 on): runtime.json's
//     scope_restore (GitHub #263).
//     Optional clipboard recording reads but never changes the clipboard;
//     --onboarding-probe fresh|later|apply|legacy|capture|decline|offline|incompatible|retry
//     checks first-run gating and choices in the real guide. Capture cases use
//     WINSHUN_PLUGIN_INDEX local fixtures; approval still uses PluginDialog.
//     runtime.json exports resource and lifecycle checks. Plugin fixtures are
//     --web-plugin-check checks the real optional web provider's keyword/name/
//     pinyin search, disable/enable and uninstall/reinstall on this profile;
//     --web-suggest-url <loopback URL with %s> adds a live suggestion fixture.
//     copied to the profile, enabled there, and stopped on exit. The reusable
//     tools/verify-plugin-recovery.ps1 drives recovery and permission cases,
//     including native hotkey readiness, with local packages and test data.
//     --folder-typing opens a test-owned Explorer window over temporary files,
//     exercises the real App/QML search, avoidance and focus-preserving selection,
//     checks same-count result replacements, cancellation and nested results,
//     the box in the file list's corner once that window is too large to keep
//     clear of (GitHub #203), and closes only that fixture.
//     --file-dialog-window <HWND> --file-dialog-pid <PID>
//     --file-dialog-sample <fixture.blend> --file-dialog-state <observer.json>
//     Opt-in real dialog-bar search/selection test against an isolated Blender
//     fixture. Saves dialog-with-search.png and requires independent UI state.
//
// In the static previews, nothing of the user's is read or written (a temporary profile), and
// nothing that reaches the running Win顺 is started: no persistent hotkeys, hooks,
// clipboard listener, taskbar box, pipe server or tray icon. The
// environment the rendering needs (the scale factor, the software renderer)
// is set before QGuiApplication by prepare(); run() does the rest once the
// application exists. The exit code is the number of QML warnings, so that
// a check script can tell.

#include <QStringList>

namespace ws::preview {

bool wanted(const QStringList& args); // one of the preview flags is among them
void prepare(const QStringList& args); // before QGuiApplication
void note(const QString& line); // a line into the log, from the start on
int run(const QStringList& args); // after it; the exit code

} // namespace ws::preview
