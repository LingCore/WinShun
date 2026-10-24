#pragma once

// Reaching Win顺 (docs/design-cli.md §7): its pipe when it serves one; when
// not, why not, and starting it when that is what the user wants.
//
//   pipe there                       → connected
//   switch off (settings file)       → Off (4), Win顺 not started
//   running, no pipe                 → Off (4) after a short grace (it may be
//                                      starting); the task never run: that
//                                      would only pop the launcher up
//   not running, --no-start          → NotRunning (3)
//   not running                      → start it (task, else the exe), wait
//                                      for the pipe, else NotRunning (3)
//
// The pipe is tried first: when it is there the switch is on, whatever the
// settings file says (another profile, a portable copy elsewhere).

#include "PipeClient.h"
#include "Result.h"

#include <functional>
#include <string>

namespace ws::cli {

// How Win顺 is found and started; the tests' fakes start nothing.
struct Launcher {
    std::function<bool()> running; // its single-instance mutex exists
    std::function<bool()> start; // the autostart task, else WinShun.exe --background
};

// The real one: launch::winShunRunning(mutex), launch::startWinShun(); with
// WS_TEST_START set, start() writes that file instead (the tests).
Launcher systemLauncher(std::wstring instanceMutex);

struct ConnectOptions {
    std::wstring pipe; // its name, without the \\.\pipe prefix
    bool switchOn = false; // [Mcp] Enabled
    bool mayStart = true; // not --no-start
    unsigned startWaitMs = 12'000; // for the pipe after starting
    unsigned graceMs = 2'000; // running, switch on, no pipe yet
    std::wstring startingMutex; // held while starting: a second ws waits instead of starting again
    std::function<void(std::wstring_view)> note; // "starting…", to stderr
};

Result<void> connectToWinShun(pipe::LineClient& client, const ConnectOptions& options, const Launcher& launcher);

// Only the pipe: no starting, no waiting (status, --version).
bool connectIfThere(pipe::LineClient& client, const std::wstring& pipe);

} // namespace ws::cli
