#pragma once

#include <windows.h>
#include <cstddef>
#include <span>
#include <string>

namespace ws::service {
class Server;
struct Connection;
bool canStartInputListener();
// The pipe client `pid` is Win顺's own UI: WinShun.exe in this executable's
// folder (svcloc::isWinShunBeside). Only it is told of the dialog navigator
// (kCapabilityDialogNav) and given it; the key listener is for any client.
bool requesterIsWinShun(DWORD pid);
void serveInputListener(Server& server, Connection& conn, std::span<const std::byte> payload);
// Navigates a higher-integrity file dialog to a folder for Win顺's UI (#104):
// launches the same uiAccess helper, which drives the dialog with window
// messages / UI Automation only. Replies DialogNavDone, or Error.
void serveDialogNav(Server& server, Connection& conn, std::span<const std::byte> payload);
// The same small, statically linked executable, in a user's session. No
// service dispatcher, volume access or pipe server is started in this mode.
int runInputListener(DWORD clientPid, HWND target);
// The --navigate-dialog helper: validates the token, the window and the
// folder, then navigates. 0 on success, else a Win32 error.
int runDialogNav(HWND dialog, const std::wstring& folder);
} // namespace ws::service
