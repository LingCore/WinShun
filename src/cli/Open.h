#pragma once

// Opening a file for `ws open`, and making sure that the Win顺 whose answer is
// opened is Win顺 (docs/design-cli.md §3).

#include "PipeClient.h"
#include "Result.h"

#include <string>
#include <string_view>

namespace ws::cli {

// This process runs with an elevated token (as administrator).
bool processElevated();

// Opens `path` as a double click does (the verb Explorer would use). `asUser`:
// with the user's normal rights even when ws itself is elevated, handed to the
// desktop shell (Explorer) to start, so that a file a search found cannot run
// with more rights than the user double-clicking it would give it; refused
// (exit 7) when no Explorer can take it. Only a path the user named may be
// opened directly. Never a dialog: an error comes back as the failure.
// `reveal`: select it in its folder instead (nothing runs).
// WS_TEST_OPEN: what would be done, written to that file (a line "open ",
// "user-open " or "reveal " and the path); WS_TEST_ELEVATED: 1 pretends to be elevated, 0 not to be.
Result<void> openPath(const std::wstring& path, bool reveal, bool asUser);

// Whether `image` (the file a process runs) is Win顺's own program: WinShun.exe
// in the folder that holds ws's folder (ws.exe is in <install>\bin), or in
// `installed` (the folder the uninstaller names), any case.
bool isWinShunImage(std::wstring_view image, std::wstring_view wsExe, std::wstring_view installed);
// The folder Win顺 is installed in according to the uninstall entry of the installer (the user's, else the machine's); empty if none.
std::wstring installedFolder();
// The process behind the other end of the pipe is Win顺: any program can create
// a pipe of that name first (the name is the user's SID), and `open` acts on
// what it answers. Failure (exit 7) when it is another. WS_TEST_VERIFY: also
// checked with a test pipe.
Result<void> checkWinShunServes(const pipe::LineClient& client);

} // namespace ws::cli
