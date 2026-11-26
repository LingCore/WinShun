#include "DialogJump.h"

#include "FileDialog.h"
#include "BlenderFileDialog.h"
#include "DiagnosticLog.h"
#include "FileManagers.h"
#include "Foreground.h"
#include "Integrity.h"
#include "PathText.h"

#include <QDebug>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <chrono>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

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
    return hasClass(hwnd, L"#32770") || hasClass(hwnd, L"TSelectFolderForm") || hasClass(hwnd, L"TSelectDirDlg")
        || blenderdialog::mayBe(hwnd);
}

// A file dialog for the bar: one we know, or Tessoa's own (filedialog::
// isTessoaPicker), which we can only name and copy paths for.
bool fileWindow(HWND hwnd)
{
    return filedialog::kind(hwnd) != filedialog::Kind::None || filedialog::isTessoaPicker(hwnd);
}

bool sameFolder(const std::wstring& a, const std::wstring& b)
{
    return pathtext::sameFolder(QString::fromStdWString(a), QString::fromStdWString(b));
}

QString className(HWND hwnd)
{
    wchar_t buffer[64] {};
    return hwnd && ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0 ? QString::fromWCharArray(buffer) : QString();
}

// A move of a dialog, in the diagnostics: whose dialog and of which kind, how
// it was asked for, how long it took, and how the dialog was left (in front?
// the keyboard focus where? the mouse held by something?). No folder, no
// name. A report that a file needs several clicks after Ctrl+G could not be
// repeated here and told none of this; the next one's package does.
void recordMove(QLatin1StringView how, HWND dialog, bool went, std::chrono::steady_clock::time_point began)
{
    if (!diagnostics::enabled())
        return;
    using Kind = filedialog::Kind;
    const Kind kind = filedialog::kind(dialog);
    const QLatin1StringView kindName = kind == Kind::Open ? "open"_L1 : kind == Kind::Save ? "save"_L1
        : kind == Kind::Folder ? "folder"_L1 : kind == Kind::Legacy ? "legacy"_L1 : kind == Kind::Browse ? "browse"_L1 : "none"_L1;
    GUITHREADINFO info {sizeof(GUITHREADINFO)};
    const DWORD thread = ::GetWindowThreadProcessId(dialog, nullptr);
    const bool known = thread && ::GetGUIThreadInfo(thread, &info);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    diagnostics::record(u"dialog-move"_s,
        {{u"how"_s, how}, {u"program"_s, foreground::programOf(dialog)}, {u"kind"_s, kindName},
            {u"by_rule"_s, filedialog::byRule(dialog)}, {u"went"_s, went}, {u"ms"_s, static_cast<qint64>(took)},
            {u"in_front"_s, ::GetForegroundWindow() == dialog},
            {u"focus"_s, known ? className(info.hwndFocus) : u"?"_s},
            {u"capture"_s, known ? className(info.hwndCapture) : u"?"_s}});
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
    if (object == OBJID_WINDOW && event == EVENT_OBJECT_HIDE && !::IsWindowVisible(hwnd)) {
        // Reusable dialogs (e.g. Qt/WPS) hide rather than destroy on close:
        // forget them so the next open triggers auto-jump as a fresh dialog.
        std::erase(g_instance->m_seen, hwnd);
        std::erase(g_instance->m_wentToManager, hwnd);
        if (g_instance->m_lastDialog == hwnd)
            g_instance->m_lastDialog = nullptr;
    }
    if (object == OBJID_WINDOW && g_instance->m_callbacks.dialogMoved)
        g_instance->m_callbacks.dialogMoved();
}

void DialogJump::follow(HWND foreground)
{
    if (foreground && std::ranges::find(m_companions, foreground) != m_companions.end()) {
        // The bar, the clipboard: the dialog is still the one. Not back from
        // a file manager then, whatever is picked there: no going by itself after that.
        std::erase(m_wentToManager, m_lastDialog); // m_dialog: null since the file manager came up
        return;
    }
    const auto stale = [foreground](HWND dialog) {
        return dialog != foreground && (!::IsWindow(dialog) || !::IsWindowVisible(dialog));
    };
    if (m_probing && m_probing != foreground) {
        if (m_probingFromManager && !stale(m_probing)
            && std::ranges::find(m_wentToManager, m_probing) == m_wentToManager.end())
            m_wentToManager.push_back(m_probing);
        m_probing = nullptr;
        m_probingFromManager = false;
    }
    std::erase_if(m_wentToManager, stale);
    std::erase_if(m_seen, stale);
    if (m_lastDialog && stale(m_lastDialog))
        m_lastDialog = nullptr;
    // Left for a file manager, straight or by way of the taskbar, the task
    // switcher or the Start menu (each in front for a moment): on the way
    // back the dialog may go to the folder shown there.
    if (m_autoJump && m_lastDialog && ::IsWindow(m_lastDialog) && filemanager::kindOf(foreground).has_value()
        && std::ranges::find(m_wentToManager, m_lastDialog) == m_wentToManager.end())
        m_wentToManager.push_back(m_lastDialog);
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
    // the path for manual paste, as for Tessoa's own window.
    const bool fileDialog = fileWindow(foreground);
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
        probeKingsoft(foreground, fromManager, true);
    }
}

void DialogJump::probeKingsoft(HWND hwnd, bool fromManager, bool allowRetry)
{
    if (m_probing == hwnd) {
        m_probingFromManager = m_probingFromManager || fromManager;
        return;
    }
    m_probing = hwnd;
    m_probingFromManager = fromManager;
    m_worker.post([this, hwnd, allowRetry] {
        if (filedialog::probe(hwnd) == filedialog::Kind::None) {
            const bool undecided = allowRetry && filedialog::mayBeKingsoft(hwnd) && !filedialog::probed(hwnd);
            QMetaObject::invokeMethod(&m_recheck, [this, hwnd, undecided] {
                const bool back = (m_probing == hwnd && m_probingFromManager)
                    || std::erase(m_wentToManager, hwnd) > 0;
                if (m_probing == hwnd) {
                    m_probing = nullptr;
                    m_probingFromManager = false;
                }
                if (undecided && ::GetForegroundWindow() == hwnd && m_dialog != hwnd) {
                    QTimer::singleShot(500ms, &m_recheck, [this, hwnd, back] {
                        if (::GetForegroundWindow() == hwnd && m_dialog != hwnd
                            && filedialog::mayBeKingsoft(hwnd) && !filedialog::probed(hwnd))
                            probeKingsoft(hwnd, back, false);
                    });
                }
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(&m_recheck, [this, hwnd] {
            const bool back = (m_probing == hwnd && m_probingFromManager)
                || std::erase(m_wentToManager, hwnd) > 0;
            if (m_probing == hwnd) {
                m_probing = nullptr;
                m_probingFromManager = false;
            }
            if (::GetForegroundWindow() == hwnd && m_dialog != hwnd)
                setDialog(hwnd, back);
        }, Qt::QueuedConnection);
    });
}

void DialogJump::await(HWND candidate, bool fromManager)
{
    DWORD process = 0;
    const DWORD thread = ::GetWindowThreadProcessId(candidate, &process);
    if (!thread || (thread == ::GetCurrentThreadId() && process == ::GetCurrentProcessId()) || isCompanion(candidate)
        || (!dialogLike(candidate) && !fileWindow(candidate)))
        return;
    const bool front = ::GetForegroundWindow() == candidate;
    if (candidate != m_pending) {
        // A file dialog left behind stays awaited over a window that is
        // neither in front nor a file dialog yet (a message box, say).
        if (m_pending && !front && ::IsWindow(m_pending) && !fileWindow(candidate) && fileWindow(m_pending))
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
    const bool fileDialog = fileWindow(dialog);
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
        std::erase_if(m_seen, [dialog](HWND seen) { return seen != dialog && (!::IsWindow(seen) || !::IsWindowVisible(seen)); });
        const bool first = std::ranges::find(m_seen, dialog) == m_seen.end();
        if (first)
            m_seen.push_back(dialog);
        if (fromManager || (first && m_atOpen))
            m_worker.post([this, dialog, first = first && !fromManager] { autoJump(dialog, first); });
    }
}

void DialogJump::setHotkeyEnabled(bool on)
{
    m_hotkeyEnabled = on;
    setRegistered(on && canFollow(m_dialog));
}

bool DialogJump::canFollow(HWND dialog) const
{
    // Tessoa's own: nothing can take it anywhere, the service's helper neither.
    if (!dialog || filedialog::isTessoaPicker(dialog))
        return false;
    if (!integrity::higherThanOurs(dialog))
        return true;
    return m_callbacks.elevatedNavAvailable && m_callbacks.elevatedNavAvailable();
}

bool DialogJump::elevatedNav(HWND dialog) const
{
    return dialog && integrity::higherThanOurs(dialog) && !filedialog::isTessoaPicker(dialog)
        && m_callbacks.navigateElevated && m_callbacks.elevatedNavAvailable && m_callbacks.elevatedNavAvailable();
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

void DialogJump::autoJump(HWND dialog, bool first)
{
    const bool elevated = integrity::higherThanOurs(dialog);
    if (elevated && !elevatedNav(dialog))
        return;
    const std::vector<filemanager::Folder> folders = filemanager::openFolders();
    if (folders.empty())
        return;
    const std::wstring& folder = folders.front().path;
    std::erase_if(m_sentTo, [dialog](const auto& sent) {
        return sent.first != dialog && (!::IsWindow(sent.first) || !::IsWindowVisible(sent.first));
    });
    if (first)
        std::erase_if(m_sentTo, [dialog](const auto& sent) { return sent.first == dialog; });
    // Back from a file manager: to the folder in front there, each time,
    // whether the user went to it there or picked a window already showing
    // it (GitHub #334: the second folder picked was not gone to). Not to the
    // one the dialog was sent to last, though: the file manager only looked
    // at, the dialog stays where the user has taken it since.
    const auto sent = std::ranges::find(m_sentTo, dialog, &decltype(m_sentTo)::value_type::first);
    if (!first && sent != m_sentTo.end() && sameFolder(sent->second, folder))
        return;
    const auto noteSent = [&] {
        if (const auto it = std::ranges::find(m_sentTo, dialog, &decltype(m_sentTo)::value_type::first); it != m_sentTo.end())
            it->second = folder;
        else
            m_sentTo.emplace_back(dialog, folder);
    };
    if (elevated) {
        // We cannot read the elevated dialog's current folder across UIPI; the
        // helper navigates it (a no-op if it is already there). No "go back"
        // origin, since we never read where it was.
        m_callbacks.navigateElevated(dialog, folder);
        noteSent();
        return;
    }
    filedialog::waitForFront(dialog);
    if (first) // where it opened: to go back to, and not to be gone to after us
        filedialog::waitForLocation(dialog);
    if (::GetForegroundWindow() != dialog)
        return; // the user has gone on
    const std::wstring from = filedialog::currentFolder(dialog);
    if (sameFolder(from, folder)) {
        noteSent(); // there already: as good as sent
        return;
    }
    const auto began = std::chrono::steady_clock::now();
    const bool went = filedialog::goTo(dialog, folder);
    recordMove("auto"_L1, dialog, went, began);
    if (!went) {
        qWarning() << "The file dialog did not go to the file manager's folder by itself";
        return;
    }
    noteSent();
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
        const auto began = std::chrono::steady_clock::now();
        const bool went = filedialog::goTo(dialog, folders.front().path);
        recordMove("ctrl+g"_L1, dialog, went, began);
        if (!went)
            qWarning() << "Ctrl+G: the file dialog did not go to the folder";
    });
}

void DialogJump::go(HWND dialog, std::wstring path, bool isFile, bool open)
{
    if (!dialog || integrity::higherThanOurs(dialog))
        return;
    m_worker.post([dialog, path = std::move(path), isFile, open] {
        filedialog::waitForFront(dialog); // from the bar, it was just handed the front
        const auto began = std::chrono::steady_clock::now();
        if (!isFile) {
            const bool went = filedialog::goTo(dialog, path);
            recordMove("bar-folder"_L1, dialog, went, began);
            if (!went)
                qWarning() << "The file dialog did not go to a folder found in it";
            return;
        }
        const bool went = filedialog::goToFile(dialog, path, open);
        if (!open) // opened, the dialog is gone: nothing of it to tell
            recordMove("bar-file"_L1, dialog, went, began);
        if (!went)
            qWarning() << "The file dialog did not take a file found in it";
    });
}

} // namespace ws
