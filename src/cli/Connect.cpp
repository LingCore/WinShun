#include "Connect.h"

#include "Interrupt.h"
#include "Launch.h"
#include "Messages.h"
#include "Win.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>

namespace ws::cli {

namespace {

enum class Wait { Connected, TimedOut, Interrupted };

// Tries the pipe until it is there or `ms` have passed.
Wait waitForPipe(pipe::LineClient& client, const std::wstring& name, unsigned ms)
{
    const ULONGLONG deadline = ::GetTickCount64() + ms;
    for (;;) {
        if (client.connect(name, 1000))
            return Wait::Connected;
        if (::GetTickCount64() >= deadline)
            return Wait::TimedOut;
        if (!interrupt::sleep(50))
            return Wait::Interrupted;
    }
}

Failure interrupted()
{
    return {Exit::Interrupted, {}};
}

// Whether another ws holds the "starting" mutex: it is starting Win顺 now.
bool someoneIsStarting(const std::wstring& mutex)
{
    if (mutex.empty())
        return false;
    const Handle handle(::OpenMutexW(SYNCHRONIZE, FALSE, mutex.c_str()));
    return static_cast<bool>(handle);
}

// How long Win顺 runs already: its oldest process of this session (a second
// one, started to hand something over, ends within moments).
std::optional<unsigned long long> winShunAgeMs()
{
    const Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
        return std::nullopt;
    DWORD session = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session))
        return std::nullopt;
    FILETIME now {};
    ::GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    std::optional<unsigned long long> oldest;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof entry;
    for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more; more = ::Process32NextW(snapshot.get(), &entry)) {
        if (::_wcsicmp(entry.szExeFile, L"WinShun.exe") != 0)
            continue;
        DWORD other = 0;
        if (!::ProcessIdToSessionId(entry.th32ProcessID, &other) || other != session)
            continue;
        const Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        FILETIME created {}, exited {}, kernel {}, user {};
        if (!process || !::GetProcessTimes(process.get(), &created, &exited, &kernel, &user) || ticks(created) > ticks(now))
            continue;
        const unsigned long long age = (ticks(now) - ticks(created)) / 10'000; // 100 ns → ms
        oldest = oldest ? std::max(*oldest, age) : age;
    }
    return oldest;
}

// How long to wait for the pipe of a Win顺 that is running without one yet:
// the grace; the whole start-up while another ws is starting it; what is left
// of the first seconds of a process that is moments old (the pipe comes
// after its window, and at logon after a good deal else).
unsigned patience(const ConnectOptions& options, const Launcher& launcher)
{
    if (someoneIsStarting(options.startingMutex))
        return std::max(options.graceMs, options.startWaitMs);
    if (launcher.runningFor) {
        if (const std::optional<unsigned long long> age = launcher.runningFor(); age && *age < options.freshMs)
            return std::max(options.graceMs, static_cast<unsigned>(options.freshMs - *age));
    }
    return options.graceMs;
}

// A Win顺 that runs and has no pipe (yet): waits as long as patience() says.
Wait waitForRunning(pipe::LineClient& client, const ConnectOptions& options, const Launcher& launcher)
{
    const unsigned wait = patience(options, launcher);
    // Said when the wait is more than a blink: it is a start-up being waited for.
    if (wait > options.graceMs && options.note)
        options.note(tr(msg::kStartingWait));
    return waitForPipe(client, options.pipe, wait);
}

} // namespace

Launcher systemLauncher(std::wstring instanceMutex)
{
    Launcher launcher;
    launcher.running = [mutex = std::move(instanceMutex)] { return launch::winShunRunning(mutex.c_str()); };
    launcher.runningFor = [] { return winShunAgeMs(); };
    if (std::wstring marker = environment(L"WS_TEST_START"); !marker.empty()) {
        launcher.start = [marker] {
            const Handle file(::CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            return static_cast<bool>(file);
        };
        // A test's Win顺 is no process of ours to look up.
        launcher.runningFor = nullptr;
    } else {
        launcher.start = [] { return launch::startWinShun(); };
    }
    return launcher;
}

bool connectIfThere(pipe::LineClient& client, const std::wstring& pipe)
{
    return client.connect(pipe, 2000);
}

Result<bool> connectIfThere(pipe::LineClient& client, const ConnectOptions& options, const Launcher& launcher)
{
    DWORD error = 0;
    if (client.connect(options.pipe, 2000, &error))
        return true;
    // Not running, or the switch off: nothing to wait for. Running with the
    // switch on and no pipe: it may be on its way up.
    if (error != ERROR_FILE_NOT_FOUND || !options.switchOn || !launcher.running())
        return false;
    switch (waitForRunning(client, options, launcher)) {
    case Wait::Connected:
        return true;
    case Wait::Interrupted:
        return interrupted();
    case Wait::TimedOut:
        break;
    }
    return false;
}

Result<void> connectToWinShun(pipe::LineClient& client, const ConnectOptions& options, const Launcher& launcher)
{
    DWORD error = 0;
    if (client.connect(options.pipe, 2000, &error))
        return {};
    if (error != ERROR_FILE_NOT_FOUND) // busy for two seconds, no access…
        return Failure {Exit::Failed, fmt(msg::kPipeError, systemMessage(error))};
    if (!options.switchOn)
        return Failure {Exit::Off, std::wstring(tr(msg::kSwitchOff))};
    if (launcher.running()) {
        // Win顺 may be on its way up (the pipe comes a moment after its
        // window; another ws or the logon autostart has just started it);
        // else it runs with the connection off, or is older.
        switch (waitForRunning(client, options, launcher)) {
        case Wait::Connected:
            return {};
        case Wait::Interrupted:
            return interrupted();
        case Wait::TimedOut:
            break;
        }
        return Failure {Exit::Off, std::wstring(tr(msg::kRunningNoPipe))};
    }
    if (!options.mayStart)
        return Failure {Exit::NotRunning, std::wstring(tr(msg::kNotRunningNoStart))};

    // One ws starts Win顺; others started meanwhile wait for its pipe.
    Handle starting(::CreateMutexW(nullptr, FALSE, options.startingMutex.empty() ? nullptr : options.startingMutex.c_str()));
    const bool someoneElse = starting && ::GetLastError() == ERROR_ALREADY_EXISTS;
    if (options.note)
        options.note(tr(msg::kStarting));
    if (!someoneElse && !launcher.start())
        return Failure {Exit::NotRunning, std::wstring(tr(msg::kCannotStart))};
    switch (waitForPipe(client, options.pipe, options.startWaitMs)) {
    case Wait::Connected:
        return {};
    case Wait::Interrupted:
        return interrupted();
    case Wait::TimedOut:
        break;
    }
    // Started, and the settings file said on: its own may differ.
    return Failure {Exit::NotRunning, fmt(msg::kStartTimedOut, options.startWaitMs / 1000)};
}

} // namespace ws::cli
