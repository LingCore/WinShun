#include "DialogJump.h"

#include "FileDialog.h"
#include "PathText.h"

#include <QDebug>
#include <QString>

#include <algorithm>
#include <utility>

namespace ws {

namespace {

DialogJump* g_instance = nullptr; // for the WinEvent callbacks

bool hasClass(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[32] {};
    return hwnd && ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0 && ::wcscmp(buffer, name) == 0;
}

bool sameFolder(const std::wstring& a, const std::wstring& b)
{
    return pathtext::sameFolder(QString::fromStdWString(a), QString::fromStdWString(b));
}

} // namespace

DialogJump::DialogJump(Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
{
    g_instance = this;
    m_recheck.setInterval(100);
    QObject::connect(&m_recheck, &QTimer::timeout, &m_recheck, [this] {
        const HWND foreground = ::GetForegroundWindow();
        if (foreground == m_pending && filedialog::kind(foreground) != filedialog::Kind::None) {
            m_recheck.stop();
            setDialog(foreground, m_pendingFromExplorer);
        } else if (foreground != m_pending || --m_retries <= 0) {
            m_recheck.stop();
        }
    });
    // Out of context: delivered through this (the GUI) thread's message loop.
    m_foregroundHook = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &DialogJump::onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    follow(::GetForegroundWindow());
}

DialogJump::~DialogJump()
{
    if (m_foregroundHook)
        ::UnhookWinEvent(m_foregroundHook);
    if (m_locationHook)
        ::UnhookWinEvent(m_locationHook);
    g_instance = nullptr;
    setRegistered(false);
}

void CALLBACK DialogJump::onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    // What is in front now, rather than the window the event names: File
    // Explorer, taken to another folder by a program, says it came to the
    // front while the dialog stays there; and the event comes a moment late.
    // (None in front, between two: the one named.)
    if (!g_instance)
        return;
    const HWND foreground = ::GetForegroundWindow();
    g_instance->follow(foreground ? foreground : hwnd);
}

void CALLBACK DialogJump::onLocation(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD)
{
    // The hook hears the whole thread: the caret, the cursor, every control.
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE)
        return;
    if (g_instance && object == OBJID_WINDOW && hwnd && hwnd == g_instance->m_dialog
        && g_instance->m_callbacks.dialogMoved)
        g_instance->m_callbacks.dialogMoved();
}

void DialogJump::follow(HWND foreground)
{
    m_recheck.stop();
    if (foreground && foreground == m_companion) {
        // The bar: the dialog is still the one. Not back from Explorer then,
        // whatever is picked there: no going by itself after that.
        m_explorerInFront = false;
        return;
    }
    const bool explorer = hasClass(foreground, L"CabinetWClass");
    const bool fromExplorer = std::exchange(m_explorerInFront, explorer);
    // Left for Explorer: what it shows now tells, on the way back, whether
    // the user went somewhere there.
    if (explorer && m_dialog && m_autoJump)
        m_worker.post([this, dialog = m_dialog] { noteExplorer(dialog); });
    const bool fileDialog = filedialog::kind(foreground) != filedialog::Kind::None;
    setDialog(fileDialog ? foreground : nullptr, fromExplorer);
    // A dialog may come to the front before all of its controls are there.
    if (!fileDialog && hasClass(foreground, L"#32770")) {
        m_pending = foreground;
        m_pendingFromExplorer = fromExplorer;
        m_retries = 5;
        m_recheck.start();
    }
}

void DialogJump::setDialog(HWND dialog, bool fromExplorer)
{
    if (!dialog && !m_dialog)
        return;
    if (dialog != m_dialog) {
        if (m_locationHook) {
            ::UnhookWinEvent(m_locationHook);
            m_locationHook = nullptr;
        }
        m_dialog = dialog;
        if (dialog) {
            DWORD process = 0;
            const DWORD thread = ::GetWindowThreadProcessId(dialog, &process);
            // Shown, hidden, moved: a dialog comes to the front before it shows.
            m_locationHook = ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                &DialogJump::onLocation, process, thread, WINEVENT_OUTOFCONTEXT);
        }
    }
    setRegistered(m_hotkeyEnabled && dialog);
    if (m_callbacks.dialogChanged)
        m_callbacks.dialogChanged(dialog);

    if (dialog && m_autoJump) {
        std::erase_if(m_seen, [](HWND seen) { return !::IsWindow(seen); });
        const bool first = std::ranges::find(m_seen, dialog) == m_seen.end();
        if (first)
            m_seen.push_back(dialog);
        if (first || fromExplorer)
            m_worker.post([this, dialog, first] { autoJump(dialog, first); });
    }
}

void DialogJump::setHotkeyEnabled(bool on)
{
    m_hotkeyEnabled = on;
    setRegistered(on && m_dialog);
}

void DialogJump::setRegistered(bool on)
{
    if (on == m_registered)
        return;
    if (!on) {
        m_callbacks.setHotkey(false);
        m_registered = false;
        return;
    }
    m_registered = m_callbacks.setHotkey(true);
    if (!m_registered && !std::exchange(m_warned, true))
        qWarning() << "Ctrl+G is taken by another program: no jumping to Explorer's folder in file dialogs";
}

void DialogJump::noteExplorer(HWND dialog)
{
    std::erase_if(m_leftFor, [dialog](const auto& left) { return left.first == dialog || !::IsWindow(left.first); });
    const std::vector<std::wstring> folders = filedialog::explorerFolders();
    m_leftFor.emplace_back(dialog, folders.empty() ? std::wstring() : folders.front());
}

void DialogJump::autoJump(HWND dialog, bool first)
{
    const std::vector<std::wstring> folders = filedialog::explorerFolders();
    if (folders.empty())
        return;
    const std::wstring& folder = folders.front();
    if (!first) { // back from Explorer: only if it shows another folder than when the user went there
        const auto left = std::ranges::find(m_leftFor, dialog, &decltype(m_leftFor)::value_type::first);
        if (left == m_leftFor.end() || sameFolder(left->second, folder))
            return;
        left->second = folder;
    }
    filedialog::waitForFront(dialog);
    if (first) // where it opened: to go back to, and not to be gone to after us
        filedialog::waitForLocation(dialog);
    if (::GetForegroundWindow() != dialog)
        return; // the user has gone on
    const std::wstring from = filedialog::currentFolder(dialog);
    if (sameFolder(from, folder))
        return;
    if (!filedialog::goTo(dialog, folder)) {
        qWarning() << "The file dialog did not go to Explorer's folder by itself";
        return;
    }
    if (m_callbacks.autoJumped)
        m_callbacks.autoJumped(dialog, from);
}

void DialogJump::jump()
{
    const HWND dialog = m_dialog;
    if (!dialog || !::IsWindow(dialog))
        return;
    if (m_companion && ::GetForegroundWindow() == m_companion)
        ::SetForegroundWindow(dialog); // ours is in front, so it may hand that on
    m_worker.post([dialog] {
        const std::vector<std::wstring> folders = filedialog::explorerFolders();
        filedialog::waitForFront(dialog);
        if (!folders.empty() && !filedialog::goTo(dialog, folders.front()))
            qWarning() << "Ctrl+G: the file dialog did not go to the folder";
    });
}

void DialogJump::go(HWND dialog, std::wstring path, bool isFile, bool open)
{
    m_worker.post([dialog, path = std::move(path), isFile, open] {
        filedialog::waitForFront(dialog); // from the bar, it was just handed the front
        if (!isFile) {
            if (!filedialog::goTo(dialog, path))
                qWarning() << "The file dialog did not go to a folder found in it";
            return;
        }
        // The file's folder, then its name in the box: Enter opens it. Should
        // the dialog not go there, the whole path in the box does as well.
        const std::size_t slash = path.find_last_of(L'\\');
        bool named = false;
        if (slash == std::wstring::npos) {
            named = filedialog::setFileName(dialog, path);
        } else {
            const std::wstring folder = path.substr(0, slash == 2 ? 3 : slash); // "C:\" for a file at the root
            named = filedialog::goTo(dialog, folder) ? filedialog::setFileName(dialog, path.substr(slash + 1))
                                                     : filedialog::setFileName(dialog, path);
        }
        if (named && open)
            filedialog::accept(dialog);
    });
}

} // namespace ws
