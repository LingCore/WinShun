#pragma once

// Whether Win顺 runs, and starting it, for the programs that talk to it from
// outside: the relay WinShunMcp.exe (--start-winshun) and ws.exe. Plain
// Win32, no Qt: built into each of them (one has Qt's C runtime, the other
// the static one).

#include <functional>
#include <string>

namespace ws::launch {

// Win顺's single-instance mutex (src/app/main.cpp).
inline constexpr wchar_t kInstanceMutex[] = L"Local\\WinShun.Instance";

// Whether Win顺 is running at all (its single-instance mutex exists),
// whatever it serves: one that runs without the pipe has the AI connection
// turned off, or is a version without it. `mutex`: another name, for tests.
bool winShunRunning(const wchar_t* mutex = kInstanceMutex);

// Runs Win顺's autostart task. False when there is no such task, or it fails.
bool runAutostartTask();

// The WinShun.exe to start without the task: the one beside this program
// (they are installed together; ws.exe has a folder of its own, bin\, inside
// Win顺's), else the one the Run key's value starts (the autostart of an
// account that could not have the task, GitHub #52). Empty: none.
std::wstring winShunExe();

// Whether this process runs with administrator rights: elevated, or at a
// high integrity level (or SYSTEM's) without being so. What it starts
// directly gets its token, so runs that way too.
bool runsElevated();

// Starts `exe` with --background in its folder (ShellExecute).
bool startProgram(const std::wstring& exe);

// What startWinShun does, step by step: apart for tests.
struct StartSteps {
    std::function<bool()> runTask = runAutostartTask;
    std::function<bool()> elevated = runsElevated;
    std::function<std::wstring()> exe = winShunExe;
    std::function<bool(const std::wstring&)> startExe = startProgram;
};

// Starts Win顺 when it is not running: through its autostart task, which
// starts it without a UAC prompt (the taskbar button does the same); when
// there is none that can be run (an account whose task name was taken, a
// policy), the program itself with --background, as the stub does; but
// never from a process with administrator rights (runsElevated: a terminal
// or AI client run as administrator), as Win顺 would get them too: false
// then, and the caller says Win顺 is not running.
bool startWinShun(const StartSteps& steps);
bool startWinShun();

} // namespace ws::launch
