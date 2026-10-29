#include "DialogJump.h"

#include "FileDialog.h"
#include "FileManagers.h"
#include "Integrity.h"
#include "PathText.h"

#include <QDebug>
#include <QString>

#include <algorithm>
#include <utility>

using namespace std::chrono_literals;

namespace ws {

namespace {

DialogJump* g_instance = nullptr; // for the WinEvent callbacks

// How long a window is awaited to become a file dialog (DialogJump::await),
// and the pauses between the looks at it when nothing happens in it: doubled
// each time, up to the longest; then, for a file dialog left behind, one a
// second. After its thread makes, shows or focuses a window it is looked at
// again a moment later, once for a burst.
constexpr auto kAwaitFor = 5s;
constexpr auto kFirstLook = 30ms;
constexpr auto kLongestLook = 800ms;
constexpr auto kBehindLook = 1s;
constexpr auto kAfterEvent = 15ms;

bool hasClass(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[32] {};
    return hwnd && ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0 && ::wcscmp(buffer, name) == 0;
}

// A window that may become a file dialog once all of its controls are there.
bool dialogLike(HWND hwnd)
{
    return hasClass(hwnd, L"#32770") || hasClass(hwnd, L"TSelectFolderForm") || hasClass(hwnd, L"TSelectDirDlg");
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
    m_recheck.setSingleShot(true);
    QObject::connect(&m_recheck, &QTimer::timeout, &m_recheck, [this] { recheck(); });
    // Out of context: delivered through this (the GUI) thread's message loop.
    m_foregroundHook = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &DialogJump::onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    follow(::GetForegroundWindow());
}

DialogJump::~DialogJump()
{
    stopAwaiting();
    if (m_foregroundHook)
        ::UnhookWinEvent(m_foregroundHook);
    if (m_locationHook)
        ::UnhookWinEvent(m_locationHook);
    if (m_dragHook)
        ::UnhookWinEvent(m_dragHook);
    g_instance = nullptr;
    setRegistered(false);
}

void CALLBACK DialogJump::onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (!g_instance)
        return;
    // What is in front now goes first: File Explorer, taken to another folder
    // by a program, says it came to the front while the dialog stays there.
    // (None in front, between two: the one named.)
    const HWND foreground = ::GetForegroundWindow();
    g_instance->follow(foreground ? foreground : hwnd);
    // But a dialog the event names may not be in front: opened by a program
    // that may not take the front (a console program's, from Windows
    // Terminal), it stays behind, and when it does come up no event may come
    // (docs/pitfalls.md). So it is awaited, unless one in front is.
    if (foreground && hwnd && hwnd != foreground && g_instance->m_pending != foreground)
        g_instance->await(hwnd, false);
}

void CALLBACK DialogJump::onPending(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD)
{
    if (!g_instance || !g_instance->m_pending)
        return;
    const HWND pending = g_instance->m_pending;
    QTimer& recheck = g_instance->m_recheck;
    if (event == EVENT_OBJECT_DESTROY) {
        if (hwnd == pending && object == OBJID_WINDOW)
            g_instance->stopAwaiting();
        return;
    }
    // In the dialog, a control made or shown (it may be the one still
    // missing) or the focus taken; not in the program's other windows on that
    // thread. Or the thread restacked its windows (named by the desktop): it
    // came up, which no foreground event says for a window opened behind
    // (docs/pitfalls.md), measured 3 ms after.
    const bool inDialog = hwnd == pending || ::IsChild(pending, hwnd);
    const bool look = (inDialog
                          && (event == EVENT_OBJECT_FOCUS
                              || ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) && object == OBJID_WINDOW)))
        || (event == EVENT_OBJECT_REORDER && hwnd == ::GetDesktopWindow());
    if (look && (!recheck.isActive() || recheck.remainingTimeAsDuration() > kAfterEvent))
        recheck.start(kAfterEvent);
}

void CALLBACK DialogJump::onLocation(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD)
{
    // The dialog's thread reordered the top-level windows (named by the
    // desktop): clicked into, the dialog came up, or with its program's
    // window, a click on the disabled owner of a modal one. No owner keeps
    // the bar above them (#95): it follows as for a move, unless it is still
    // right above the dialog (dialogRestacked).
    if (event == EVENT_OBJECT_REORDER && hwnd == ::GetDesktopWindow() && g_instance) {
        if (g_instance->m_callbacks.dialogRestacked)
            g_instance->m_callbacks.dialogRestacked();
        else if (g_instance->m_callbacks.dialogMoved)
            g_instance->m_callbacks.dialogMoved();
        return;
    }
    if (!g_instance || !hwnd || hwnd != g_instance->m_dialog)
        return;
    if (event == EVENT_SYSTEM_MOVESIZESTART || event == EVENT_SYSTEM_MOVESIZEEND) {
        if (g_instance->m_callbacks.dialogDragged)
            g_instance->m_callbacks.dialogDragged(event == EVENT_SYSTEM_MOVESIZESTART);
        return;
    }
    // The hook hears the whole thread: the caret, the cursor, every control.
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE)
        return;
    if (object == OBJID_WINDOW && g_instance->m_callbacks.dialogMoved)
        g_instance->m_callbacks.dialogMoved();
}

void DialogJump::follow(HWND foreground)
{
    m_probing = nullptr;
    std::erase_if(m_wentToManager, [](HWND dialog) { return !::IsWindow(dialog); });
    if (foreground && std::ranges::find(m_companions, foreground) != m_companions.end()) {
        // The bar, the clipboard: the dialog is still the one. Not back from
        // a file manager then, whatever is picked there: no going by itself after that.
        std::erase(m_wentToManager, m_lastDialog); // m_dialog: null since the file manager came up
        return;
    }
    // Left for a file manager, straight or by way of the taskbar, the task
    // switcher or the Start menu (each in front for a moment): what it shows
    // when it first comes up tells, on the way back, whether the user went
    // somewhere there.
    if (m_autoJump && m_lastDialog && ::IsWindow(m_lastDialog) && filemanager::kindOf(foreground).has_value()
        && std::ranges::find(m_wentToManager, m_lastDialog) == m_wentToManager.end()) {
        m_wentToManager.push_back(m_lastDialog);
        m_worker.post([this, dialog = m_lastDialog] { noteManager(dialog); });
    }
    const bool fromManager = std::erase(m_wentToManager, foreground) > 0;
    // Never attach to GUI-thread modal windows: following their drag on the
    // same thread caused linux.do #222. Our shell pickers now have their own
    // STA thread, so they can use the same navigation as other file dialogs.
    DWORD process = 0;
    const DWORD thread = foreground ? ::GetWindowThreadProcessId(foreground, &process) : 0;
    if (thread == ::GetCurrentThreadId() && process == ::GetCurrentProcessId()) {
        setDialog(nullptr, false);
        return;
    }
    // Elevated dialogs: the UI cannot send them navigation across UIPI, but
    // the index service's helper can (setDialog registers Ctrl+G and auto-jump
    // only when canFollow allows it); without the service, DialogBar copies
    // the path for manual paste.
    const bool fileDialog = filedialog::kind(foreground) != filedialog::Kind::None;
    if (fileDialog && foreground == m_pending)
        stopAwaiting(); // up, and an event said so this time
    setDialog(fileDialog ? foreground : nullptr, fromManager);
    if (fileDialog)
        return;
    // A dialog may come to the front before all of its controls are there:
    // some come seconds later (opened behind another window, on a slow drive).
    if (dialogLike(foreground)) {
        await(foreground, fromManager);
    } else if (filedialog::mayBeKingsoft(foreground) && !filedialog::probed(foreground)) {
        // WPS Office's own, asked through UI Automation: on the worker, as
        // that waits on the program.
        m_probing = foreground;
        m_probingFromManager = fromManager;
        m_worker.post([this, hwnd = foreground] {
            if (filedialog::probe(hwnd) == filedialog::Kind::None)
                return;
            QMetaObject::invokeMethod(&m_recheck, [this, hwnd] {
                if (hwnd == m_probing && ::GetForegroundWindow() == hwnd)
                    setDialog(hwnd, m_probingFromManager);
            }, Qt::QueuedConnection);
        });
    }
}

void DialogJump::await(HWND candidate, bool fromManager)
{
    DWORD process = 0;
    const DWORD thread = ::GetWindowThreadProcessId(candidate, &process);
    if (!thread || (thread == ::GetCurrentThreadId() && process == ::GetCurrentProcessId()) || isCompanion(candidate)
        || (!dialogLike(candidate) && filedialog::kind(candidate) == filedialog::Kind::None))
        return;
    const bool front = ::GetForegroundWindow() == candidate;
    if (candidate != m_pending) {
        // A file dialog left behind stays awaited over a window that is
        // neither in front nor a file dialog yet (a message box, say).
        if (m_pending && !front && ::IsWindow(m_pending) && filedialog::kind(candidate) == filedialog::Kind::None
            && filedialog::kind(m_pending) != filedialog::Kind::None)
            return;
        stopAwaiting();
        m_pending = candidate;
        m_pendingUntil = std::chrono::steady_clock::now() + kAwaitFor;
        m_pendingStep = kFirstLook;
        m_pendingFront = front;
        // Out of context, its thread only, and only while awaited (onPending).
        m_pendingHook = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_FOCUS, nullptr, &DialogJump::onPending,
            process, thread, WINEVENT_OUTOFCONTEXT);
    }
    m_pendingFromManager = m_pendingFromManager || fromManager;
    m_recheck.stop();
    recheck();
}

void DialogJump::recheck()
{
    const HWND dialog = m_pending;
    if (!dialog)
        return;
    if (!::IsWindow(dialog)) {
        stopAwaiting();
        return;
    }
    // Taken once in front with all a file dialog has; asked again each time,
    // as what the event named may since have become something else.
    const bool fileDialog = filedialog::kind(dialog) != filedialog::Kind::None;
    const bool front = ::GetForegroundWindow() == dialog;
    if (fileDialog && front) {
        const bool fromManager = std::erase(m_wentToManager, dialog) > 0 || m_pendingFromManager;
        stopAwaiting();
        setDialog(dialog, fromManager);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (front && !m_pendingFront) {
        // Up only now: the whole while again for its controls, from here.
        m_pendingUntil = now + kAwaitFor;
        m_pendingStep = kFirstLook;
    }
    m_pendingFront = front;
    if (now >= m_pendingUntil) {
        // Still no file dialog: none (a message box, say). A file dialog
        // behind is watched until it comes up or goes.
        if (!fileDialog)
            stopAwaiting();
        else
            m_recheck.start(kBehindLook);
        return;
    }
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(m_pendingUntil - now);
    m_recheck.start(std::min(m_pendingStep, left));
    m_pendingStep = std::min(m_pendingStep * 2, std::chrono::milliseconds(kLongestLook));
}

void DialogJump::stopAwaiting()
{
    m_recheck.stop();
    if (m_pendingHook) {
        ::UnhookWinEvent(m_pendingHook);
        m_pendingHook = nullptr;
    }
    m_pending = nullptr;
    m_pendingFromManager = false;
}

void DialogJump::setDialog(HWND dialog, bool fromManager)
{
    if (!dialog && !m_dialog)
        return;
    if (dialog != m_dialog) {
        for (HWINEVENTHOOK* hook : {&m_locationHook, &m_dragHook}) {
            if (*hook) {
                ::UnhookWinEvent(*hook);
                *hook = nullptr;
            }
        }
        m_dialog = dialog;
        if (dialog) {
            m_lastDialog = dialog;
            DWORD process = 0;
            const DWORD thread = ::GetWindowThreadProcessId(dialog, &process);
            // Shown, hidden, moved: a dialog comes to the front before it shows.
            m_locationHook = ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                &DialogJump::onLocation, process, thread, WINEVENT_OUTOFCONTEXT);
            // Dragged or sized by its frame (with the mouse, or Alt+Space then
            // the arrow keys): the bar keeps out of the way till it is let go.
            m_dragHook = ::SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
                &DialogJump::onLocation, process, thread, WINEVENT_OUTOFCONTEXT);
        }
    }
    setRegistered(m_hotkeyEnabled && canFollow(dialog));
    if (m_callbacks.dialogChanged)
        m_callbacks.dialogChanged(dialog);

    if (dialog && m_autoJump && canFollow(dialog)) {
        std::erase_if(m_seen, [](HWND seen) { return !::IsWindow(seen); });
        const bool first = std::ranges::find(m_seen, dialog) == m_seen.end();
        if (first)
            m_seen.push_back(dialog);
        if (fromManager || (first && m_atOpen))
            m_worker.post([this, dialog, first = first && !fromManager, atOpen = m_atOpen] {
                autoJump(dialog, first, atOpen);
            });
    }
}

void DialogJump::setHotkeyEnabled(bool on)
{
    m_hotkeyEnabled = on;
    setRegistered(on && canFollow(m_dialog));
}

bool DialogJump::canFollow(HWND dialog) const
{
    if (!dialog)
        return false;
    if (!integrity::higherThanOurs(dialog))
        return true;
    return m_callbacks.elevatedNavAvailable && m_callbacks.elevatedNavAvailable();
}

bool DialogJump::elevatedNav(HWND dialog) const
{
    return dialog && integrity::higherThanOurs(dialog) && m_callbacks.navigateElevated
        && m_callbacks.elevatedNavAvailable && m_callbacks.elevatedNavAvailable();
}

void DialogJump::refreshElevated()
{
    setRegistered(m_hotkeyEnabled && canFollow(m_dialog));
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
        qWarning() << "Ctrl+G is taken by another program: no jumping to the file manager's folder in file dialogs";
}

void DialogJump::noteManager(HWND dialog)
{
    std::erase_if(m_leftFor, [dialog](const auto& left) { return left.first == dialog || !::IsWindow(left.first); });
    const std::vector<filemanager::Folder> folders = filemanager::openFolders();
    m_leftFor.emplace_back(dialog, folders.empty() ? std::wstring() : folders.front().path);
}

void DialogJump::autoJump(HWND dialog, bool first, bool atOpen)
{
    const bool elevated = integrity::higherThanOurs(dialog);
    if (elevated && !elevatedNav(dialog))
        return;
    const std::vector<filemanager::Folder> folders = filemanager::openFolders();
    if (folders.empty())
        return;
    const std::wstring& folder = folders.front().path;
    std::erase_if(m_jumped, [](HWND jumped) { return !::IsWindow(jumped); });
    // Back from a file manager: only if it shows another folder than when the
    // user went there; but not at open, a dialog that has not gone yet goes
    // anyway (it opened where Windows left it).
    if (!first && (atOpen || std::ranges::find(m_jumped, dialog) != m_jumped.end())) {
        const auto left = std::ranges::find(m_leftFor, dialog, &decltype(m_leftFor)::value_type::first);
        if (left == m_leftFor.end() || sameFolder(left->second, folder))
            return;
        left->second = folder;
    }
    if (elevated) {
        // We cannot read the elevated dialog's current folder across UIPI; the
        // helper navigates it (a no-op if it is already there). No "go back"
        // origin, since we never read where it was.
        m_callbacks.navigateElevated(dialog, folder);
        if (std::ranges::find(m_jumped, dialog) == m_jumped.end())
            m_jumped.push_back(dialog);
        return;
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
        qWarning() << "The file dialog did not go to the file manager's folder by itself";
        return;
    }
    if (std::ranges::find(m_jumped, dialog) == m_jumped.end())
        m_jumped.push_back(dialog);
    if (m_callbacks.autoJumped)
        m_callbacks.autoJumped(dialog, from);
}

void DialogJump::jump()
{
    const HWND dialog = m_dialog;
    if (!dialog || !::IsWindow(dialog))
        return;
    const bool elevated = integrity::higherThanOurs(dialog);
    if (elevated && !elevatedNav(dialog))
        return;
    if (std::ranges::find(m_companions, ::GetForegroundWindow()) != m_companions.end())
        ::SetForegroundWindow(dialog); // ours is in front, so it may hand that on
    m_worker.post([this, dialog, elevated] {
        const std::vector<filemanager::Folder> folders = filemanager::openFolders();
        if (folders.empty())
            return;
        if (elevated) {
            m_callbacks.navigateElevated(dialog, folders.front().path); // the helper drives it
            return;
        }
        filedialog::waitForFront(dialog);
        if (!filedialog::goTo(dialog, folders.front().path))
            qWarning() << "Ctrl+G: the file dialog did not go to the folder";
    });
}

void DialogJump::go(HWND dialog, std::wstring path, bool isFile, bool open)
{
    if (!dialog || integrity::higherThanOurs(dialog))
        return;
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
