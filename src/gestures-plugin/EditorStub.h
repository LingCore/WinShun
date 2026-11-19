// `WinShunGestures.exe --editor`: the settings window's start. The exe has no
// Qt; this finds Win顺's folder and loads WinShunGesturesEditor.dll, where the
// window is (editor/). Kept out of the plugin's own path: main() asks first.
#pragma once

namespace gestures {

// Whether this start is the settings window's (--editor on the command line).
bool editorRequested();
// Loads the window's DLL and runs it; its exit code. Nothing but protocol
// lines may reach stdout (editor/EditorLink.h): messages go to stderr.
int runEditor();

} // namespace gestures
