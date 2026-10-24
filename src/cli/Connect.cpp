#include "Connect.h"

#include "Interrupt.h"
#include "Launch.h"
#include "Messages.h"
#include "Win.h"

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

} // namespace

Launcher systemLauncher(std::wstring instanceMutex)
{
    Launcher launcher;
    launcher.running = [mutex = std::move(instanceMutex)] { return launch::winShunRunning(mutex.c_str()); };
    if (std::wstring marker = environment(L"WS_TEST_START"); !marker.empty()) {
        launcher.start = [marker] {
            const Handle file(::CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            return static_cast<bool>(file);
        };
    } else {
        launcher.start = [] { return launch::startWinShun(); };
    }
    return launcher;
}

bool connectIfThere(pipe::LineClient& client, const std::wstring& pipe)
{
    return client.connect(pipe, 2000);
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
        // window); else it runs with the connection off, or is older.
        switch (waitForPipe(client, options.pipe, options.graceMs)) {
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
