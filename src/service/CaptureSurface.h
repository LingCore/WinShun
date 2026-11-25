#pragma once

#include <windows.h>

#include <string>

namespace ws::service {

// Runs in the user's interactive session as a non-administrator High-IL
// UIAccess helper launched by WinShunService.exe (never from the plugin's
// user-writable directory). Creates Band-2 (ZBID_UIACCESS) DWM mirror windows
// over a full-screen Band-8 Store app for the ordinary-rights capture plugin's
// Band-1 overlays, and relays pointer, keyboard, cursor and IME events over
// the authenticated private pipe. Never calls SendInput or touches any window
// other than the validated Medium-IL plugin overlays.
int runCaptureSurface(DWORD pluginPid, HWND fullScreenTarget, const std::wstring& pipeName);

} // namespace ws::service
