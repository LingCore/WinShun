#pragma once

namespace ws {

// The program used to be called 快搜 / QuickFind. On the first start under the
// new name this asks a running QuickFind to quit (it saves its index), then
// moves %APPDATA%\QuickFind and %LOCALAPPDATA%\QuickFind over to WinShun, so
// settings, the index and the history carry over, and moves autostart to the
// new logon task. Runs before anything reads the settings; a no-op afterwards.
void migrateFromQuickFind();

} // namespace ws
