#pragma once

// The previewers other programs offer, as Alt+P on a result hands them the
// file (FilePreview, 预览方式 自动): QuickLook (QL-Win) and Seer, each only
// while it runs in the background, as its users keep it. Both show a file
// without taking the focus, so the launcher keeps the keyboard; neither
// hears the keys typed there, so Win顺 tells them to close (Esc, Alt+P again,
// the launcher hidden) and which file comes next (the arrows).
//
//   QuickLook: one line a connection on its named pipe
//     \\.\pipe\QuickLook.App.Pipe.<the user's SID>, "QuickLook.App.PipeMessages.<Invoke|Switch|Close>|<path>|"
//     (QuickLook/PipeServerManager.cs; QuickLook.exe "<path>" sends the same).
//     Running: its mutex "QuickLook.App.Mutex". Its viewer: a visible
//     top-level window titled "QuickLook" of class "HwndWrapper[QuickLook.exe;…".
//   Seer: WM_COPYDATA to the window of class "SeerWindowClass" (Seer-sdk,
//     seer/ipc.h): 5000 shows a path (at least 200 ms apart), 5004 asks
//     whether it is visible, 5005 hides it, 5007 tells its version (4.x).
//     Older ones (2.8) have the window and show nothing (GitHub #131): see
//     knownToShow, passOver.
//   PowerToys Peek: left out. It takes a path on its command line only, as a
//     new process each time that takes the focus (the launcher would go),
//     and cannot be given the next file.

#include <QString>

#include <optional>

using HWND = struct HWND__*;

namespace ws::extpreview {

enum class Engine { QuickLook, Seer };

// The one running now, QuickLook first; none when neither is (or the one
// running was passed over).
std::optional<Engine> running();
// Shows what it is asked to, as far as can be told beforehand: QuickLook,
// and a Seer that tells its version (4.x; 2.8 does not, and shows nothing).
// The others are checked a moment later (answered).
bool knownToShow(Engine engine);
// Asked to show a file a moment ago: something of it is on the screen now
// (Seer: told so, or a window of its own came up).
bool answered(Engine engine);
// It took the file and showed nothing: left out from now on, while the
// same process runs (started again, a newer one, it is asked again).
void passOver(Engine engine);
// Shows the file (a folder too); false when it could not be told.
bool show(Engine engine, const QString& path);
// While its preview is up: the next file instead (Seer: no sooner than
// 200 ms after the last, the latest path wins; see FilePreview).
bool switchTo(Engine engine, const QString& path);
void close(Engine engine);
// Its preview is on the screen now.
bool visible(Engine engine);
// `window` is one of its windows (the preview may take the focus for a moment).
bool owns(Engine engine, HWND window);

} // namespace ws::extpreview
