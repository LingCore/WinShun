#pragma once

#include "GameGuard.h"

#include <QString>
#include <QStringView>

#include <windows.h>

namespace ws::foreground {

// The program file of the window, "notepad.exe"; empty when it cannot be told.
QString programOf(HWND hwnd);
// ... of the process, also of one that cannot be opened (GitHub #121).
QString programOfProcess(DWORD pid);

// Windows search and Start hosts across Windows 10 and 11. Both typing
// interception and foreground activation must recognize the same hosts.
bool isSearchHost(QStringView program);
bool isShellFlyout(QStringView program);

// About the window in front right now (see GameGuard). Nothing for Win顺's
// own windows and the desktop.
ForegroundFacts facts();

// What kind of window `front` is for a panel opened over `target` (see
// GameGuard's FrontTaker): none, Win顺's, the target's program, the desktop
// or the taskbar, another.
FrontTaker takerOf(HWND front, HWND target);

} // namespace ws::foreground
