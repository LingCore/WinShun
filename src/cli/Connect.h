#pragma once

// Reaching Win顺 (docs/design-cli.md §7): its pipe when it serves one; when
// not, why not, and starting it when that is what the user wants.
//
//   pipe there                       → connected
//   switch off (settings file)       → Off (4), Win顺 not started
//   running, no pipe                 → wait for it (see patience()), then Off
//                                      (4); the task never run: that would
//                                      only pop the launcher up
//   not running, --no-start          → NotRunning (3)
//   not running                      → start it (task, else the exe), wait
//                                      for the pipe, else NotRunning (3)
//
// The pipe is tried first: when it is there the switch is on, whatever the
// settings file says (another profile, a portable copy elsewhere).

#include "PipeClient.h"
#include "Result.h"

#include <functional>
#include <optional>
#include <string>

namespace ws::cli {

// How Win顺 is found and started; the tests' fakes start nothing.
struct Launcher {
    std::function<bool()> running; // its single-instance mutex exists
    std::function<bool()> start; // the autostart task, else WinShun.exe --background
    // How long ago Win顺's process started, in ms; nothing when that cannot be
    // told (or the field is not set): the pipe comes after its window, so a
    // process that is moments old is probably on its way to it.
    std::function<std::optional<unsigned long long>()> runningFor;
};

// The real one: launch::winShunRunning(mutex), launch::startWinShun(); with
// WS_TEST_START set, start() writes that file instead (the tests).
Launcher systemLauncher(std::wstring instanceMutex);

struct ConnectOptions {
    std::wstring pipe; // its name, without the \\.\pipe prefix
    bool switchOn = false; // [Mcp] Enabled
    bool mayStart = true; // not --no-start
    unsigned startWaitMs = 12'000; // for the pipe after starting, or while another ws is starting it
    unsigned graceMs = 2'000; // running, switch on, no pipe yet
    unsigned freshMs = 8'000; // a Win顺 process younger than this is still on its way to its pipe
    std::wstring startingMutex; // held while starting: a second ws waits instead of starting again
    std::function<void(std::wstring_view)> note; // "starting…", to stderr
};

Result<void> connectToWinShun(pipe::LineClient& client, const ConnectOptions& options, const Launcher& launcher);

// Only the pipe, and the waiting a Win顺 on its way up deserves (another ws is
// starting it, or its process is moments old): never starting it. For status.
// False: not there (why: the caller asks the launcher and the settings).
Result<bool> connectIfThere(pipe::LineClient& client, const ConnectOptions& options, const Launcher& launcher);

// Only the pipe, now: no starting, no waiting (--version, ws alone).
bool connectIfThere(pipe::LineClient& client, const std::wstring& pipe);

} // namespace ws::cli
