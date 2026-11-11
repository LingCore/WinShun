#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ws::service {
class Server;
struct Connection;
bool canStartInputListener();
// The pipe client `pid` is WinShun.exe in this executable's folder
// (svcloc::isWinShunBeside): the protected clipboard display is for it alone.
bool requesterIsWinShun(DWORD pid);
// The pipe client `pid` is Win顺's own UI (svcloc::servesDialogNav: beside
// this executable, or the copy it was installed from). Only it is told of the
// dialog navigator (kCapabilityDialogNav) and given it; the key listener is
// for any client.
bool mayNavigateDialogs(const Server& server, DWORD pid);
bool canStartClipboardDisplay(DWORD pid);
void serveClipboardDisplay(Server& server, Connection& conn, std::span<const std::byte> payload);
void serveInputListener(Server& server, Connection& conn, std::span<const std::byte> payload);
// Navigates a higher-integrity file dialog to a folder for Win顺's UI (#104):
// launches the same uiAccess helper, which drives the dialog with window
// messages / UI Automation only. Replies DialogNavDone, or Error.
void serveDialogNav(Server& server, Connection& conn, std::span<const std::byte> payload);
// Presses the paste keys in a higher-integrity window for Win顺's UI, which
// has put what to paste on the clipboard and that window in front (#277):
// Windows drops the keys the UI sends there. The same uiAccess helper, for
// the one chord. Replies PasteDone, or Error.
void servePaste(Server& server, Connection& conn, std::span<const std::byte> payload);
// The same small, statically linked executable, in a user's session. No
// service dispatcher, volume access or pipe server is started in this mode.
// `keys`: the UI's plain hotkeys to hold back over administrator windows and
// tell of (keyproto::kHotkey, GitHub #243); none for any other client.
int runInputListener(DWORD clientPid, HWND target, const std::vector<std::uint32_t>& keys = {});
// The --navigate-dialog helper: validates the token, the window and the
// folder, then navigates. 0 on success, else a Win32 error.
int runDialogNav(HWND dialog, const std::wstring& folder);
// The --paste-keys helper: validates the token and the window, then presses
// Ctrl+V (Shift+Insert where the window takes that). 0 on success, else a
// Win32 error.
int runPaste(HWND window);
} // namespace ws::service
