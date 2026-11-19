#include "CrashRecord.h"

#include "CodeAddress.h"

#include <QFile>
#include <QJsonArray>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>

using namespace Qt::StringLiterals;

namespace ws::crashrecord {

namespace {

constexpr ULONG kMaxFrames = 64;
constexpr DWORD kCppException = 0xE06D7363; // MSVC's throw: goes on to std::terminate, written there

std::array<wchar_t, 1024> g_file {};
std::array<char, 256> g_activity {}; // UTF-8; read by a crash on any thread, a torn copy at worst
std::atomic<bool> g_writing {false};
DWORD g_mainThread = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

void put(HANDLE file, const char* text, int length) noexcept
{
    if (length <= 0)
        return;
    DWORD written = 0;
    ::WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
}

// Lines of "key value": why, where, which thread, what it was doing, its calls.
void write(const char* reason, const EXCEPTION_RECORD* exception) noexcept
{
    if (!g_file[0] || g_writing.exchange(true))
        return; // a second crash while the first is written: the first one stands
    const HANDLE file = ::CreateFileW(g_file.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    std::array<char, 640> line {};
    std::array<char, 512> where {};
    const auto clamp = [&](int length) { return (std::min)(length, static_cast<int>(line.size()) - 1); };
    put(file, line.data(), clamp(std::snprintf(line.data(), line.size(), "reason %s\n", reason)));
    if (exception) {
        codeaddress::describe(reinterpret_cast<std::uintptr_t>(exception->ExceptionAddress), where.data(), where.size());
        put(file, line.data(),
            clamp(std::snprintf(line.data(), line.size(), "exception %08lx at %s\n",
                static_cast<unsigned long>(exception->ExceptionCode), where.data())));
    }
    const DWORD thread = ::GetCurrentThreadId();
    if (thread == g_mainThread)
        put(file, "thread main\n", 12);
    else
        put(file, line.data(), clamp(std::snprintf(line.data(), line.size(), "thread %lu\n", static_cast<unsigned long>(thread))));
    std::array<char, g_activity.size()> activity {};
    std::memcpy(activity.data(), g_activity.data(), activity.size() - 1);
    if (activity[0])
        put(file, line.data(), clamp(std::snprintf(line.data(), line.size(), "activity %s\n", activity.data())));
    std::array<void*, kMaxFrames> frames {};
    const USHORT count = ::RtlCaptureStackBackTrace(1, kMaxFrames, frames.data(), nullptr); // not this one
    for (USHORT i = 0; i < count; ++i) {
        codeaddress::describe(reinterpret_cast<std::uintptr_t>(frames[i]), where.data(), where.size());
        put(file, line.data(), clamp(std::snprintf(line.data(), line.size(), "frame %s\n", where.data())));
    }
    ::CloseHandle(file);
}

// The runtime ends the program once this returns, and Windows reports it as before.
void onAbort(int)
{
    write("abort", nullptr);
}

LONG WINAPI onException(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* exception = info ? info->ExceptionRecord : nullptr;
    // Not a stack that ran out: nothing more fits on it.
    if (exception && exception->ExceptionCode != kCppException && exception->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        write("exception", exception);
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install(const QString& file)
{
    if (file.isEmpty() || file.size() >= static_cast<qsizetype>(g_file.size()))
        return;
    g_file.fill(0);
    file.toWCharArray(g_file.data());
    g_mainThread = ::GetCurrentThreadId();
    std::array<char, 64> warm {}; // the look-ups' one-time set-up, here rather than in a crash
    codeaddress::describe(reinterpret_cast<std::uintptr_t>(&install), warm.data(), warm.size());
    std::signal(SIGABRT, onAbort); // the whole process's with the C runtime DLL (std::terminate)
    g_previous = ::SetUnhandledExceptionFilter(onException); // the runtime's own (C++ exceptions → terminate) after it
}

void setActivity(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    const std::size_t length = (std::min)(static_cast<std::size_t>(utf8.size()), g_activity.size() - 1);
    g_activity[0] = 0;
    std::memcpy(g_activity.data() + 1, utf8.constData() + 1, length > 0 ? length - 1 : 0);
    g_activity[length] = 0;
    if (length > 0)
        g_activity[0] = utf8[0]; // last: a crash meanwhile reads it as empty rather than half
}

void clearActivity()
{
    g_activity[0] = 0;
}

QJsonObject takeLeft(const QString& file)
{
    QFile record(file);
    if (!record.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = record.read(64 * 1024);
    record.close();
    record.remove();
    QJsonObject left;
    QJsonArray frames;
    for (const QByteArray& raw : bytes.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const qsizetype space = line.indexOf(u' ');
        if (space <= 0)
            continue;
        const QString key = line.left(space);
        const QString value = line.mid(space + 1);
        if (key == u"frame")
            frames.append(value);
        else if (key == u"reason" || key == u"exception" || key == u"thread" || key == u"activity")
            left.insert(key, value);
    }
    if (left.isEmpty())
        return {};
    left.insert(u"frames"_s, frames);
    return left;
}

} // namespace ws::crashrecord
