#include "UiStall.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include <tlhelp32.h>

namespace ws {

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"WinShun.UiStallWatch";
constexpr UINT kAsk = WM_APP;
constexpr auto kAway = 60s; // nobody at the PC: not asked
constexpr std::size_t kStackBytes = 256 * 1024; // of the top of the stack: some hundred calls
constexpr int kMaxFrames = 48;
constexpr int kOtherFrames = 20; // of each other thread: what it waits in, and who asked
constexpr int kMostOthers = 32; // different stacks written down
constexpr qsizetype kPartBytes = 5000; // of the other threads' calls in one entry
constexpr DWORD kNearExport = 0x1000; // further past a name than this: not that function

// Milliseconds the PC was awake since it started: a wait it slept through is short by this clock.
qint64 awakeMs()
{
    ULONGLONG time = 0;
    ::QueryUnbiasedInterruptTime(&time);
    return static_cast<qint64>(time / 10'000);
}

qint64 milliseconds(const FILETIME& kernel, const FILETIME& user)
{
    const auto ticks = [](const FILETIME& time) { return (static_cast<quint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return static_cast<qint64>((ticks(kernel) + ticks(user)) / 10'000);
}

bool away()
{
    LASTINPUTINFO last {sizeof last, 0};
    return ::GetLastInputInfo(&last) && ::GetTickCount() - last.dwTime >= static_cast<DWORD>(std::chrono::milliseconds(kAway).count());
}

// The exported function of `module` that starts closest before `rva`, and how
// far in. Windows' own modules have no symbols to hand for whichever build a
// PC has, but what a thread waits in is nearly always an exported call
// (NtUserMessageCall, NtWaitForSingleObject, NtGdiDdDDIPresent). Plain data
// in and out, structured exceptions around it: the module may go meanwhile.
bool exportBefore(HMODULE module, DWORD rva, char* name, std::size_t room, DWORD* into) noexcept
{
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return false;
        const IMAGE_DATA_DIRECTORY& where = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!where.VirtualAddress || where.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            return false;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + where.VirtualAddress);
        const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        DWORD best = 0;
        const char* found = nullptr;
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            if (ordinals[i] >= exports->NumberOfFunctions)
                continue;
            const DWORD start = functions[ordinals[i]];
            // In the export table itself: a forwarder's text, no code. Of two
            // names for one address the first stays (Nt… before Zw…).
            if (start > rva || (found && start <= best)
                || (start >= where.VirtualAddress && start - where.VirtualAddress < where.Size))
                continue;
            best = start;
            found = reinterpret_cast<const char*>(base + names[i]);
        }
        if (!found || rva - best > kNearExport)
            return false;
        std::size_t length = 0;
        while (length + 1 < room && found[length]) {
            name[length] = found[length];
            ++length;
        }
        name[length] = 0;
        *into = rva - best;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Windows' folder with the separator after it; empty when it cannot be told.
const QString& windowsFolder()
{
    static const QString folder = [] {
        std::array<wchar_t, MAX_PATH> path {};
        const UINT length = ::GetSystemWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
        if (length == 0 || length >= path.size())
            return QString();
        return QString::fromWCharArray(path.data(), static_cast<qsizetype>(length)) + QLatin1Char('\\');
    }();
    return folder;
}

// "Qt6Core.dll+1a2b3c": the module an address is in and how far into it;
// the address alone when it is in none (code made at run time). In one of
// Windows' own modules the exported function comes after it:
// "win32u.dll+1234 NtUserMessageCall+14".
QString named(quintptr address)
{
    HMODULE module = nullptr;
    std::array<wchar_t, MAX_PATH> path {};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)
        || !module || !::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())))
        return QStringLiteral("0x") + QString::number(address, 16);
    const QString file = QString::fromWCharArray(path.data());
    const quintptr offset = address - reinterpret_cast<quintptr>(module);
    QString text = file.mid(file.lastIndexOf(QLatin1Char('\\')) + 1) + QLatin1Char('+') + QString::number(offset, 16);
    std::array<char, 96> function {};
    DWORD into = 0;
    if (!windowsFolder().isEmpty() && file.startsWith(windowsFolder(), Qt::CaseInsensitive) && offset <= MAXDWORD
        && exportBefore(module, static_cast<DWORD>(offset), function.data(), function.size(), &into))
        text += QLatin1Char(' ') + QLatin1StringView(function.data()) + QLatin1Char('+') + QString::number(into, 16);
    return text;
}

#if defined(_M_X64)

struct Frames {
    std::array<quintptr, kMaxFrames> at {};
    int count = 0;
};

// Up the calls of a thread from its registers and a copy of the top of its
// stack (`bytes` from `from`, where its stack pointer was). The thread runs
// on meanwhile: every address into its stack is read in the copy instead. A
// stack that makes no sense ends the walk (structured exceptions: nothing in
// here may need unwinding).
void walk(CONTEXT context, const std::byte* copy, std::size_t bytes, Frames& frames) noexcept
{
    const DWORD64 from = context.Rsp;
    const DWORD64 delta = reinterpret_cast<DWORD64>(copy) - from;
    const auto inStack = [&](DWORD64 address) { return address >= from && address - from < bytes; };
    const auto inCopy = [&](DWORD64 address) {
        return address >= reinterpret_cast<DWORD64>(copy) && address - reinterpret_cast<DWORD64>(copy) + sizeof(DWORD64) <= bytes;
    };
    // Registers a function may keep its frame in, or have saved for its caller.
    const auto moved = [&] {
        for (DWORD64* value : {&context.Rbx, &context.Rbp, &context.Rsi, &context.Rdi, &context.R12, &context.R13,
                 &context.R14, &context.R15}) {
            if (inStack(*value))
                *value += delta;
        }
    };
    context.Rsp += delta;
    moved();
    __try {
        while (frames.count < kMaxFrames && context.Rip && inCopy(context.Rsp)) {
            frames.at[static_cast<std::size_t>(frames.count++)] = context.Rip;
            DWORD64 base = 0;
            if (const PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(context.Rip, &base, nullptr)) {
                void* handler = nullptr;
                DWORD64 frame = 0;
                ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler, &frame, nullptr);
            } else { // a leaf: it has only its return address on the stack
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
            }
            if (inStack(context.Rsp)) // set from a frame register read off the stack
                context.Rsp += delta;
            moved();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

#endif

} // namespace

UiStallWatch::UiStallWatch(Sink sink, Options options)
    : m_sink(std::move(sink))
    , m_options(options)
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW type {sizeof type};
    type.lpfnWndProc = &UiStallWatch::windowProc;
    type.hInstance = instance;
    type.lpszClassName = kWindowClass;
    ::RegisterClassExW(&type); // again for a second watch: there already
    m_window = ::CreateWindowExW(0, kWindowClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    m_answered = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &m_watched,
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    ULONG_PTR low = 0, high = 0;
    ::GetCurrentThreadStackLimits(&low, &high);
    m_stackTop = high;
    if (!m_window || !m_answered || !m_stop || !m_watched)
        return; // not watched: nothing else depends on it
    ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_thread = std::jthread([this] { run(); });
}

UiStallWatch::~UiStallWatch()
{
    if (m_stop)
        ::SetEvent(m_stop);
    if (m_thread.joinable())
        m_thread.join();
    if (m_window)
        ::DestroyWindow(m_window);
    for (const HANDLE handle : {m_watched, m_answered, m_stop}) {
        if (handle)
            ::CloseHandle(handle);
    }
}

LRESULT CALLBACK UiStallWatch::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kAsk) {
        if (const auto* watch = reinterpret_cast<const UiStallWatch*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            ::SetEvent(watch->m_answered);
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

UiStallWatch::Waited UiStallWatch::wait(std::chrono::milliseconds limit) const
{
    const std::array handles {m_stop, m_answered};
    switch (::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, static_cast<DWORD>(limit.count()))) {
    case WAIT_OBJECT_0 + 1:
        return Waited::Answered;
    case WAIT_TIMEOUT:
        return Waited::Late;
    default:
        return Waited::Stopped;
    }
}

UiStallWatch::Spent UiStallWatch::spent() const
{
    Spent spent;
    FILETIME created, exited, kernel, user;
    if (::GetThreadTimes(m_watched, &created, &exited, &kernel, &user))
        spent.threadMs = milliseconds(kernel, user);
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        spent.processMs = milliseconds(kernel, user);
    return spent;
}

void UiStallWatch::run()
{
    int stalls = 0;
    for (;;) {
        if (::WaitForSingleObject(m_stop, static_cast<DWORD>(m_options.every.count())) != WAIT_TIMEOUT)
            return;
        if (m_options.onlyWhileUsed && away())
            continue;
        const Spent before = spent();
        const qint64 asked = awakeMs();
        ::ResetEvent(m_answered);
        if (!::PostMessageW(m_window, kAsk, 0, 0))
            continue;
        Waited waited = wait(m_options.late);
        if (waited == Waited::Late && awakeMs() - asked < m_options.late.count() * 9 / 10)
            waited = wait(m_options.late); // slept through: asked for the same time awake
        if (waited == Waited::Stopped)
            return;
        if (waited == Waited::Answered)
            continue;
        const int stall = ++stalls;
        int samples = 0;
        while (waited == Waited::Late) {
            if (stall <= m_options.stalls && samples < m_options.samples) {
                const Spent now = spent();
                const qint64 ms = awakeMs() - asked;
                QJsonObject details = look();
                // Before anything is written: as close to the same moment as can be.
                const QJsonArray others = m_options.others && samples == 0 ? lookAtOthers() : QJsonArray();
                details.insert(QStringLiteral("stall"), stall);
                details.insert(QStringLiteral("sample"), ++samples);
                details.insert(QStringLiteral("ms"), ms);
                details.insert(QStringLiteral("ui_cpu_ms"), now.threadMs - before.threadMs);
                details.insert(QStringLiteral("process_cpu_ms"), now.processMs - before.processMs);
                details.insert(QStringLiteral("busy"), (now.threadMs - before.threadMs) * 2 >= ms);
                m_sink(QStringLiteral("ui-stall"), std::move(details));
                // In parts: the journal takes 8 KB an entry, and a program with a
                // graphics driver in it has some dozens of threads.
                std::vector<QJsonArray> parts;
                qsizetype size = 0;
                for (const QJsonValue thread : others) {
                    const qsizetype bytes = QJsonDocument(thread.toObject()).toJson(QJsonDocument::Compact).size();
                    if (parts.empty() || size + bytes > kPartBytes) {
                        parts.emplace_back();
                        size = 0;
                    }
                    parts.back().append(thread);
                    size += bytes;
                }
                for (std::size_t i = 0; i < parts.size(); ++i) {
                    m_sink(QStringLiteral("ui-stall-threads"), {{QStringLiteral("stall"), stall},
                        {QStringLiteral("part"), static_cast<int>(i + 1)}, {QStringLiteral("of"), static_cast<int>(parts.size())},
                        {QStringLiteral("threads"), parts[i]}});
                }
            }
            waited = wait(m_options.again);
        }
        if (waited == Waited::Stopped)
            return;
        if (stall <= m_options.stalls) {
            m_sink(QStringLiteral("ui-stall-end"), {{QStringLiteral("stall"), stall},
                {QStringLiteral("ms"), awakeMs() - asked}, {QStringLiteral("samples"), samples}});
        }
    }
}

QJsonObject UiStallWatch::look()
{
    QJsonObject details;
#if defined(_M_X64)
    m_copy.resize(kStackBytes); // before the thread is stopped: it may hold the heap's lock then
    alignas(16) CONTEXT context {};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    std::size_t bytes = 0;
    if (::SuspendThread(m_watched) == static_cast<DWORD>(-1))
        return details;
    if (::GetThreadContext(m_watched, &context) && context.Rsp < m_stackTop) {
        bytes = std::min<std::size_t>(kStackBytes, m_stackTop - context.Rsp);
        std::memcpy(m_copy.data(), reinterpret_cast<const void*>(context.Rsp), bytes);
    }
    ::ResumeThread(m_watched);
    if (bytes == 0)
        return details;
    Frames frames;
    walk(context, m_copy.data(), bytes, frames);
    QJsonArray names;
    for (int i = 0; i < frames.count; ++i)
        names.append(named(frames.at[static_cast<std::size_t>(i)]));
    details.insert(QStringLiteral("frames"), names);
#endif
    return details;
}

// The program's other threads, read the same way. The interface's thread
// waiting says only that it waits (for the thread that draws, for a lock):
// what holds it up is in another's calls. Threads in the same calls (a pool
// at rest) are written once, with how many they are.
QJsonArray UiStallWatch::lookAtOthers()
{
    QJsonArray threads;
#if defined(_M_X64)
    struct Other {
        HANDLE thread = nullptr;
        Frames frames;
    };
    std::vector<Other> all;
    const DWORD process = ::GetCurrentProcessId();
    const DWORD self = ::GetCurrentThreadId();
    const DWORD watched = ::GetThreadId(m_watched);
    if (const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry {sizeof entry};
        for (BOOL more = ::Thread32First(snapshot, &entry); more; more = ::Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self || entry.th32ThreadID == watched)
                continue;
            if (const HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                    FALSE, entry.th32ThreadID))
                all.push_back({thread, {}});
        }
        ::CloseHandle(snapshot);
    }
    for (Other& other : all) {
        alignas(16) CONTEXT context {};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        std::size_t bytes = 0;
        // Stopped, it may hold the heap's lock: only calls into Windows until it runs again.
        if (::SuspendThread(other.thread) == static_cast<DWORD>(-1))
            continue;
        MEMORY_BASIC_INFORMATION region {};
        if (::GetThreadContext(other.thread, &context)
            && ::VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &region, sizeof region)
            && region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            // The committed part of a stack is one region, up to the stack's base.
            const DWORD64 end = reinterpret_cast<DWORD64>(region.BaseAddress) + region.RegionSize;
            bytes = std::min<std::size_t>(kStackBytes, end - context.Rsp);
            std::memcpy(m_copy.data(), reinterpret_cast<const void*>(context.Rsp), bytes);
        }
        ::ResumeThread(other.thread);
        if (bytes)
            walk(context, m_copy.data(), bytes, other.frames);
    }
    using Described = HRESULT(WINAPI*)(HANDLE, PWSTR*); // GetThreadDescription: Windows 10 1607 on
    const auto described = reinterpret_cast<Described>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    struct Seen {
        QString calls;
        QJsonArray frames;
        QStringList names;
        int count = 0;
    };
    std::vector<Seen> seen;
    for (const Other& other : all) {
        QString name;
        if (PWSTR text = nullptr; described && SUCCEEDED(described(other.thread, &text)) && text) {
            name = QString::fromWCharArray(text);
            ::LocalFree(text);
        }
        ::CloseHandle(other.thread);
        if (other.frames.count == 0)
            continue;
        QJsonArray frames;
        QString calls;
        for (int i = 0; i < std::min<int>(other.frames.count, kOtherFrames); ++i) {
            const QString call = named(other.frames.at[static_cast<std::size_t>(i)]);
            frames.append(call);
            calls += call + QLatin1Char('\n');
        }
        auto same = std::ranges::find(seen, calls, &Seen::calls);
        if (same == seen.end())
            same = seen.insert(seen.end(), Seen {calls, frames, {}, 0});
        ++same->count;
        if (!name.isEmpty() && !same->names.contains(name))
            same->names.append(name);
    }
    for (const Seen& one : seen) {
        if (threads.size() >= kMostOthers)
            break;
        QJsonObject thread {{QStringLiteral("frames"), one.frames}};
        if (one.count > 1)
            thread.insert(QStringLiteral("count"), one.count);
        if (!one.names.isEmpty())
            thread.insert(QStringLiteral("name"), one.names.join(QLatin1Char(',')));
        threads.append(thread);
    }
#endif
    return threads;
}

} // namespace ws
