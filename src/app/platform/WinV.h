#pragma once

// Taking Win+V over from Windows' clipboard history, and Win+S from its
// search.
//
// Explorer registers these keys for itself (RegisterHotKey), so nobody else
// can, unless its "DisabledHotkeys" value (HKCU\Software\Microsoft\Windows\
// CurrentVersion\Explorer\Advanced, one character per key) holds the letter:
// Explorer reads it when it starts and leaves every Win combination with that
// letter alone (S frees Win+S and Win+Shift+S, the screen capture; Win+Ctrl+S
// is not Explorer's). So taking one over takes an Explorer restart, or the
// next sign-in. No keyboard hook is needed (see KeyListener for why that
// matters).
namespace ws::winv {

// `key` (an upper-case letter) is in DisabledHotkeys: Explorer leaves its
// Win combinations alone once it (re)starts.
bool releasedByExplorer(wchar_t key);
// Adds `key` to DisabledHotkeys or takes it out again, keeping the other keys
// there. Returns false if the value could not be written.
bool setReleasedByExplorer(wchar_t key, bool released);

// Whether an Explorer runs the taskbar (one to restart).
bool canRestartExplorer();
// Ends the Explorer that runs the taskbar and starts it again with the
// user's rights, not ours (as administrator, everything started from the
// taskbar would be too). Windows starts it again by itself only when it had
// started that one at sign-in, so this does when Windows does not. Its folder
// windows close. Takes seconds: not for the GUI thread. Returns whether the
// taskbar is back.
bool restartExplorer();

} // namespace ws::winv
