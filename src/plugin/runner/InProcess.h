#pragma once

// The plugins that come with Win顺 and run inside the plugin host instead of
// a process each (fewer rows in Task Manager): winshun.tools (no Qt, built
// in) and winshun.switches (its Qt DLL, loaded once). Win顺 says which with
// the start message's "inProcess", for the copies installed with it. Each speaks the plugin protocol over pipes as it would over stdin
// and stdout, on a thread of its own; the host cannot end a thread, so a
// plugin that will not stop ends the host (Win顺 starts it again).

#include "Lite.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace ws::plugin::inprocess {

// One run: set when it has returned (its ends closed), and what it returned.
struct Run {
    lite::Handle done {::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<DWORD> code {STILL_ACTIVE};
};

bool known(const std::string& kind);

// Starts `kind` over `in` (it reads), `out` and `err` (it writes), which it
// closes when it returns. `exe`: the program the manifest names, beside the
// plugin's DLL; `appDir`: Win顺's folder (WS_APP_DIR), with Qt. Null, with a
// reason, when it cannot; the ends are closed then too.
std::shared_ptr<Run> start(const std::string& kind, const std::wstring& exe, const std::wstring& appDir, HANDLE in, HANDLE out,
    HANDLE err, std::string* problem);

} // namespace ws::plugin::inprocess
