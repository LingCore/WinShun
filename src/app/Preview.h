#pragma once

// Renders settings pages to PNG files without showing anything, for checking
// their layout in both themes and scalings without touching the Win顺 that
// is running. Modes:
//
//   WinShun.exe --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog] [--page ai]
//     The 插件 page on its own (PluginsPreview.qml) over made-up plugins
//     (PluginSettings::loadPreview), or the AI 助手 page.
//
//   WinShun.exe --preview-settings <folder> [--dark] [--scale 1.5] [--tree]
//     [--corners round|square] [--page 1]: one appearance for a selected page.
//     [--web-form] [--compact]: exercise adding/editing web shortcuts, focus,
//     validation and scrolling at 920x720 or the minimum 760x420 window.
//     Add --renderer d3d11 for the GPU path (default: software).
//     --wsl-folders checks WSL folder identity and renders stopped/running
//     cards; --english and --compact cover translation and minimum size.
//     --backup-settings checks the real backup worker and success/failure
//     feedback with automatic backups off; --compact uses 760x420.
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
//     compares a saved ResultRow.qml and IconNumber.qml from before an edit.
//     --file-metadata --gallery-image <demo.png> also exports the production
//     search panel and resized clipboard with demo entries for screenshots.
//     --file-metadata --file-gallery checks the real Main.qml gallery with
//     generated temporary PNGs, real name searches and shell thumbnails,
//     keyboard navigation, selection, mode switching, zoom and minimum width.
//
//   WinShun.exe --preview-ai-cards <folder> [--dark] [--scale 1.5]
//     The screens for AI changing settings (SettingsConfirm.qml, SettingsNotice.qml)
//     over made-up changes: confirm.png, confirm-always.png, notice.png, notice-undone.png.
//
//   WinShun.exe --preview-capture-tools <folder> [--scale 1.5] [--dark] [--renderer d3d11]
//     The real annotation model, canvas, handles and style panel over a
//     synthetic document. --preview-results also accepts --file-metadata.
//
//   WinShun.exe --preview-long-capture <folder> --renderer d3d11 [--scale 1.5]
//     [--up|--both|--auto|--interactive] [--copy|--save|--pin|--cancel] [--edge] [--english] [--dark]
//     Generated scrolling content through the real Capture/LongCapture WGC
//     path. Checks complete output, fixed bars, immediate finish and cleanup.
//     Interactive mode accepts real wheel input; Enter finishes, Esc cancels.
//
// Nothing of the user's is read or written (a temporary profile), and
// nothing that reaches the running Win顺 is started: no persistent hotkeys, hooks,
// clipboard listener, taskbar box, pipe server or tray icon. The
// environment the rendering needs (the scale factor, the software renderer)
// is set before QGuiApplication by prepare(); run() does the rest once the
// application exists. The exit code is the number of QML warnings, so that
// a check script can tell.
// Long-capture preview temporarily owns Enter/Esc and returns its failed
// verification count. Its auto target is a separate generated fixture.

#include <QStringList>

namespace ws::preview {

bool wanted(const QStringList& args); // one of the preview flags is among them
void prepare(const QStringList& args); // before QGuiApplication
void note(const QString& line); // a line into the log, from the start on
int run(const QStringList& args); // after it; the exit code
int runLongCapture(const QStringList& args); // generated scroll fixture through real WGC

} // namespace ws::preview
