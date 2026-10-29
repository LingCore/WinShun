#include "ExternalPreview.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <windows.h>
#include <sddl.h>

#include <string>

using namespace Qt::StringLiterals;

namespace ws::extpreview {

namespace {

// ---- QuickLook ---------------------------------------------------------------------

std::wstring quickLookPipe()
{
    static const std::wstring name = [] {
        std::wstring pipe = L"\\\\.\\pipe\\QuickLook.App.Pipe.";
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return std::wstring();
        alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] {};
        DWORD length = 0;
        wchar_t* sid = nullptr;
        if (::GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &length)
            && ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) {
            pipe += sid;
            ::LocalFree(sid);
        } else {
            pipe.clear();
        }
        ::CloseHandle(token);
        return pipe;
    }();
    return name;
}

bool quickLookRunning()
{
    const HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, L"QuickLook.App.Mutex");
    if (!mutex)
        return false;
    ::CloseHandle(mutex);
    return !quickLookPipe().empty();
}

// One message a connection, as QuickLook's own second instance sends it.
// Between two connections the pipe is busy for a moment: wait a little.
bool sendQuickLook(QStringView message, const QString& path)
{
    const std::wstring pipe = quickLookPipe();
    if (pipe.empty())
        return false;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3 && handle == INVALID_HANDLE_VALUE; ++attempt) {
        handle = ::CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (::GetLastError() != ERROR_PIPE_BUSY || !::WaitNamedPipeW(pipe.c_str(), 100))
                break;
        }
    }
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const QByteArray line = (u"QuickLook.App.PipeMessages."_s + message + u'|' + path + u"|\r\n"_s).toUtf8();
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr)
        && written == static_cast<DWORD>(line.size());
    ::CloseHandle(handle);
    return ok;
}

bool isQuickLookWindow(HWND window)
{
    wchar_t name[64] {};
    // WPF: "HwndWrapper[QuickLook.exe;;<guid>]"
    return ::GetClassNameW(window, name, 64) > 0 && std::wstring_view(name).starts_with(L"HwndWrapper[QuickLook.exe;");
}

bool quickLookVisible()
{
    bool found = false;
    ::EnumWindows([](HWND window, LPARAM found) -> BOOL {
        if (!::IsWindowVisible(window) || !isQuickLookWindow(window))
            return TRUE;
        wchar_t title[16] {};
        ::GetWindowTextW(window, title, 16);
        if (std::wstring_view(title) != L"QuickLook")
            return TRUE; // its tray icon's window, and the like
        *reinterpret_cast<bool*>(found) = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// ---- Seer --------------------------------------------------------------------------

constexpr ULONG_PTR kSeerInvoke = 5000; // SEER_INVOKE_W32
constexpr ULONG_PTR kSeerIsVisible = 5004; // SEER_IS_VISIBLE
constexpr ULONG_PTR kSeerHide = 5005; // SEER_HIDE
constexpr ULONG_PTR kSeerVersion = 5007; // answers its version, 4.7.0 as 40700
constexpr int kSeerInterval = 200; // ms between two SEER_INVOKE_* (ipc.h)

DWORD g_passedOverSeer = 0; // its process (passOver); GUI thread only

DWORD processOf(HWND window)
{
    DWORD process = 0;
    if (window)
        ::GetWindowThreadProcessId(window, &process);
    return process;
}

HWND seerWindow()
{
    return ::FindWindowW(L"SeerWindowClass", nullptr);
}

// A visible window of the process, of some size: its preview, whatever the
// version calls it.
bool seerShowsWindow()
{
    struct Search {
        DWORD process;
        bool found = false;
    } search {processOf(seerWindow())};
    if (!search.process)
        return false;
    ::EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(param);
        RECT r {};
        if (processOf(window) != s.process || !::IsWindowVisible(window) || !::GetWindowRect(window, &r)
            || r.right - r.left < 50 || r.bottom - r.top < 50)
            return TRUE;
        s.found = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// A message to it, answered within a moment or not at all (Seer busy, or
// elevated while Win顺 is not: UIPI drops it). `answer`: what it returned.
bool sendSeer(ULONG_PTR command, const QString& text, LRESULT* answer = nullptr)
{
    const HWND seer = seerWindow();
    if (!seer)
        return false;
    COPYDATASTRUCT data {};
    data.dwData = command;
    if (!text.isEmpty()) {
        data.cbData = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)); // with the terminating NUL
        data.lpData = const_cast<void*>(static_cast<const void*>(text.utf16()));
    }
    DWORD_PTR result = 0;
    if (!::SendMessageTimeoutW(seer, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result))
        return false;
    if (answer)
        *answer = static_cast<LRESULT>(result);
    return true;
}

// Its version, asked once a process (again when it was busy); 0 when it
// does not tell.
LRESULT seerVersion()
{
    static DWORD askedOf = 0;
    static LRESULT version = 0;
    const DWORD process = processOf(seerWindow());
    if (process && process != askedOf) {
        LRESULT answer = 0;
        if (sendSeer(kSeerVersion, QString(), &answer)) {
            askedOf = process;
            version = answer;
        }
    }
    return process && process == askedOf ? version : 0;
}

// Invocations no closer together than Seer takes them: the latest path waits.
struct SeerPacer {
    QElapsedTimer last;
    QString pending;
    QTimer* timer = nullptr;

    bool invoke(const QString& path)
    {
        if (!timer) {
            timer = new QTimer(QCoreApplication::instance());
            timer->setSingleShot(true);
            QObject::connect(timer, &QTimer::timeout, [this] {
                if (!pending.isEmpty()) {
                    const QString path = std::exchange(pending, QString());
                    sendSeer(kSeerInvoke, path);
                    last.start();
                }
            });
        }
        if (last.isValid() && last.elapsed() < kSeerInterval) {
            pending = path;
            timer->start(static_cast<int>(kSeerInterval - last.elapsed()));
            return seerWindow() != nullptr;
        }
        pending.clear();
        timer->stop();
        const bool ok = sendSeer(kSeerInvoke, path);
        last.start();
        return ok;
    }
    void cancel()
    {
        pending.clear();
        if (timer)
            timer->stop();
    }
};

SeerPacer& seerPacer()
{
    static SeerPacer pacer;
    return pacer;
}

} // namespace

std::optional<Engine> running()
{
    if (quickLookRunning())
        return Engine::QuickLook;
    if (const HWND seer = seerWindow(); seer && processOf(seer) != g_passedOverSeer)
        return Engine::Seer;
    return std::nullopt;
}

bool knownToShow(Engine engine)
{
    switch (engine) {
    case Engine::QuickLook: return true;
    case Engine::Seer: return seerVersion() > 0;
    }
    return false;
}

bool answered(Engine engine)
{
    switch (engine) {
    case Engine::QuickLook: return quickLookVisible();
    case Engine::Seer: return visible(Engine::Seer) || seerShowsWindow();
    }
    return false;
}

void passOver(Engine engine)
{
    if (engine == Engine::Seer)
        g_passedOverSeer = processOf(seerWindow());
}

bool show(Engine engine, const QString& path)
{
    const QString native = QString(path).replace(u'/', u'\\');
    switch (engine) {
    case Engine::QuickLook: return sendQuickLook(u"Invoke", native);
    case Engine::Seer: return seerPacer().invoke(native);
    }
    return false;
}

bool switchTo(Engine engine, const QString& path)
{
    const QString native = QString(path).replace(u'/', u'\\');
    switch (engine) {
    case Engine::QuickLook: return sendQuickLook(u"Switch", native); // only while its viewer is up
    case Engine::Seer: return seerPacer().invoke(native);
    }
    return false;
}

void close(Engine engine)
{
    switch (engine) {
    case Engine::QuickLook:
        sendQuickLook(u"Close", QString());
        break;
    case Engine::Seer:
        seerPacer().cancel();
        sendSeer(kSeerHide, QString());
        break;
    }
}

bool visible(Engine engine)
{
    switch (engine) {
    case Engine::QuickLook: return quickLookVisible();
    case Engine::Seer: {
        LRESULT answer = 0;
        return sendSeer(kSeerIsVisible, QString(), &answer) && answer == 1;
    }
    }
    return false;
}

bool owns(Engine engine, HWND window)
{
    if (!window)
        return false;
    const HWND root = ::GetAncestor(window, GA_ROOTOWNER);
    switch (engine) {
    case Engine::QuickLook: return isQuickLookWindow(window) || (root && isQuickLookWindow(root));
    case Engine::Seer: {
        const HWND seer = seerWindow();
        if (!seer)
            return false;
        DWORD seerProcess = 0;
        DWORD process = 0;
        ::GetWindowThreadProcessId(seer, &seerProcess);
        ::GetWindowThreadProcessId(window, &process);
        return process != 0 && process == seerProcess;
    }
    }
    return false;
}

} // namespace ws::extpreview
