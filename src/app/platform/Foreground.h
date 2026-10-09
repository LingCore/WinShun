#pragma once

#include "GameGuard.h"

#include <QString>

#include <windows.h>

namespace ws::foreground {

// The program file of the window, "notepad.exe"; empty when it cannot be told.
QString programOf(HWND hwnd);

// About the window in front right now (see GameGuard). Nothing for Win顺's
// own windows and the desktop.
ForegroundFacts facts();

} // namespace ws::foreground
