#include "Power.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QTimer>

#include <windows.h>

#include <powrprof.h>
#include <reason.h>

#include <memory>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::power {

namespace {

// How long a sleep or hibernate request is given to fail before the tool
// answers that it is under way: a refusal comes back at once, a success only
// when the computer has woken again.
constexpr int kSuspendFailsWithinMs = 400;

Performer& testPerformer()
{
    static Performer performer;
    return performer;
}

mcp::ToolResult failure(const QString& text)
{
    return {{}, true, text};
}

// What Windows says of an error, in the user's language.
QString windowsError(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    QString text = length ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed() : QString();
    ::LocalFree(buffer);
    return text.isEmpty() ? u"错误 %1"_s.arg(error) : text;
}

// SeShutdownPrivilege is in the token of a user who may shut the computer down
// but off until it is asked for; the plugin's restricted token has none, which
// is why this is Win顺's to do. Empty when it is on.
QString enableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return windowsError(::GetLastError());
    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    QString error;
    if (!::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid))
        error = windowsError(::GetLastError());
    else if (!::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr))
        error = windowsError(::GetLastError());
    else if (::GetLastError() == ERROR_NOT_ALL_ASSIGNED) // it says yes when it set nothing: the token has none
        error = windowsError(ERROR_NOT_ALL_ASSIGNED);
    ::CloseHandle(token);
    return error;
}

QString performForReal(Action action)
{
    if (const QString error = enableShutdownPrivilege(); !error.isEmpty())
        return error;
    switch (action) {
    case Action::Shutdown:
    case Action::Restart: {
        // Neither SHUTDOWN_FORCE_OTHERS nor SHUTDOWN_FORCE_SELF: a program with
        // unsaved work gets to say so, and the shutdown waits for the user.
        const DWORD flags = action == Action::Restart ? SHUTDOWN_RESTART : SHUTDOWN_POWEROFF;
        const DWORD result = ::InitiateShutdownW(nullptr, nullptr, 0, flags,
            SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        return result == ERROR_SUCCESS ? QString() : windowsError(result);
    }
    case Action::Sleep:
    case Action::Hibernate: {
        const bool hibernate = action == Action::Hibernate;
        // Without it the call would put the computer to sleep instead.
        if (hibernate && !::IsPwrHibernateAllowed())
            return u"这台电脑没开休眠"_s;
        // Returns after the computer has woken up (true), or at once with an error.
        if (!::SetSuspendState(hibernate ? TRUE : FALSE, FALSE, FALSE))
            return windowsError(::GetLastError());
        return {};
    }
    }
    return {};
}

} // namespace

std::optional<Action> parseAction(const QString& name)
{
    if (name == u"shutdown")
        return Action::Shutdown;
    if (name == u"restart")
        return Action::Restart;
    if (name == u"sleep")
        return Action::Sleep;
    if (name == u"hibernate")
        return Action::Hibernate;
    return std::nullopt;
}

QString actionName(Action action)
{
    switch (action) {
    case Action::Shutdown: return u"shutdown"_s;
    case Action::Restart: return u"restart"_s;
    case Action::Sleep: return u"sleep"_s;
    case Action::Hibernate: return u"hibernate"_s;
    }
    return {};
}

void setPerformerForTest(Performer performer)
{
    testPerformer() = std::move(performer);
}

void run(const QString& pluginId, const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done)
{
    // One line a call, whatever it asks for (a plugin's own words, cut short).
    const QString asked = arguments.value(u"action"_s).toString();
    qInfo().noquote() << u"Power: %1 asks for \"%2\""_s.arg(pluginId, asked.simplified().left(40));
    const std::optional<Action> action = parseAction(asked);
    if (!action) {
        done(failure(u"action must be shutdown, restart, sleep or hibernate"_s));
        return;
    }
    const Performer perform = testPerformer() ? testPerformer() : Performer(performForReal);
    const auto answer = [](const std::function<void(mcp::ToolResult)>& to, const QString& error) {
        to(error.isEmpty() ? mcp::ToolResult {QJsonObject {{u"ok"_s, true}}} : failure(error));
    };
    if (*action == Action::Shutdown || *action == Action::Restart) {
        answer(done, perform(*action)); // Windows takes the request and returns
        return;
    }
    // Sleep and hibernate come back only when the computer has woken: on a
    // thread of its own, never this one. Its answer, if it is a refusal, and
    // otherwise the clock's, whichever comes first; the other finds it answered.
    struct Pending {
        std::function<void(mcp::ToolResult)> done;
        bool answered = false; // the calling thread's alone
    };
    const auto pending = std::make_shared<Pending>(Pending {std::move(done), false});
    const auto settle = [pending, answer](const QString& error) {
        if (std::exchange(pending->answered, true))
            return;
        answer(pending->done, error);
        pending->done = nullptr;
    };
    QTimer::singleShot(kSuspendFailsWithinMs, QCoreApplication::instance(), [settle] { settle({}); });
    std::thread([perform, action = *action, settle] {
        const QString error = perform(action);
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [settle, error] { settle(error); }, Qt::QueuedConnection);
    }).detach();
}

} // namespace ws::power
