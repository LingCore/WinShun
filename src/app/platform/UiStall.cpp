#include "UiStall.h"

#include <QJsonArray>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace ws {

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"WinShun.UiStallWatch";
constexpr UINT kAsk = WM_APP;
constexpr auto kAway = 60s; // nobody at the PC: not asked
constexpr std::size_t kStackBytes = 256 * 1024; // of the top of the stack: some hundred calls
constexpr int kMaxFrames = 48;

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

// "Qt6Core.dll+1a2b3c": the module an address is in and how far into it;
// the address alone when it is in none (code made at run time).
QString named(quintptr address)
{
    HMODULE module = nullptr;
    std::array<wchar_t, MAX_PATH> path {};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)
        || !module || !::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())))
        return QStringLiteral("0x") + QString::number(address, 16);
    const QString file = QString::fromWCharArray(path.data());
    return file.mid(file.lastIndexOf(QLatin1Char('\\')) + 1) + QLatin1Char('+')
        + QString::number(address - reinterpret_cast<quintptr>(module), 16);
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
                details.insert(QStringLiteral("stall"), stall);
                details.insert(QStringLiteral("sample"), ++samples);
                details.insert(QStringLiteral("ms"), ms);
                details.insert(QStringLiteral("ui_cpu_ms"), now.threadMs - before.threadMs);
                details.insert(QStringLiteral("process_cpu_ms"), now.processMs - before.processMs);
                details.insert(QStringLiteral("busy"), (now.threadMs - before.threadMs) * 2 >= ms);
                m_sink(QStringLiteral("ui-stall"), std::move(details));
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

} // namespace ws
