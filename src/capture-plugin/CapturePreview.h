#pragma once

// The screen capture's previews, for checking it without Win顺 or the
// plugin host (they were WinShun.exe's before the screenshot became a
// plugin, GitHub #138). WinShunCapture.exe with one of the flags and
// --app-dir <Win顺's folder> (or WS_APP_DIR) runs one and exits:
//
//   WinShunCapture.exe --preview-capture-tools <folder> [--scale 1.5] [--dark] [--renderer d3d11]
//     The real annotation model, canvas, handles and style panel over a
//     synthetic document. --toolbar-drag: the capture toolbar's grip
//     dragged over a made-up screen (--interactive: on the screen, for a
//     real mouse).
//   --preview-capture-settings <folder> [--english] [--dark] [--scale 1.5]
//     Real settings QML at minimum width; checks the full-screen toggle,
//     binding and persistence in a temporary profile. Registers no hotkeys.
//     --pin-image scrolls the real pin shortcut row into view and checks its bounds.
//
//   WinShunCapture.exe --preview-recording <folder> --renderer d3d11 [--scale 1.5] --auto
//     A real Capture -> selection -> Recorder -> WGC -> MP4 run over a
//     window of its own. --gif, --small-file, --high-quality, --clicks,
//     --no-cursor, --no-countdown, --skip-countdown, --system-audio, --english.
//     --ui-only: options, keyboard, short viewport and screen-edge checks;
//     --failure-ui: invalid save folder, actual worker validation failure and retry.
//
//   WinShunCapture.exe --preview-long-capture <folder> --renderer d3d11 [--scale 1.5]
//     [--up|--both|--auto|--interactive] [--copy|--save|--pin|--cancel] [--edge] [--english] [--dark]
//     Generated scrolling content through the real Capture/LongCapture WGC
//     path. Checks complete output, fixed bars, immediate finish and cleanup.
//     Interactive mode accepts real wheel input; Enter finishes, Esc cancels.
//     --preceding-capture first copies a regular capture locally, checking that
//     a following long copy cannot inherit its clipboard identity. No system clipboard writes.
//
// --preview-ocr <folder> checks the missing-component panel and installation signal.
//
// Nothing of the user's is read or written, and no hotkey, hook or
// clipboard listener is set up. The log is <folder>\preview.log. The exit
// code is the number of QML warnings (4: a picture not saved or a check
// failed), so that a check script can tell.

#include <QStringList>

namespace ws::captureplugin::preview {

bool wanted(const QStringList& args); // one of the preview flags is among them
void prepare(const QStringList& args); // before QGuiApplication
void note(const QString& line); // a line into the log, from the start on
int run(const QStringList& args); // after it; the exit code
int runLongCapture(const QStringList& args); // generated scroll fixture through real WGC (LongCapturePreview.cpp)

} // namespace ws::captureplugin::preview
