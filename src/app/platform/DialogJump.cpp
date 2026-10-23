#include "DialogJump.h"

#include "FileDialog.h"
#include "FileManagers.h"
#include "Integrity.h"
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
            setDialog(foreground, m_pendingFromManager);
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
    if (m_dragHook)
        ::UnhookWinEvent(m_dragHook);
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
    m_recheck.stop();
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
    // Win顺's own (shell::pickFolder and the other pickers): left alone. They
    // run their modal loop on this, the GUI, thread, through which these
    // out-of-context events also come: moving the bar after each step of a
    // drag (2+ ms each, nearly all in moving its window) held up the dialog's
    // own drag loop, which fell hundreds of pixels behind the pointer
    // (linux.do #222).
    DWORD process = 0;
    if (foreground && ::GetWindowThreadProcessId(foreground, &process) && process == ::GetCurrentProcessId()) {
        setDialog(nullptr, false);
        return;
    }
    // Elevated dialogs can still be searched: DialogBar copies the selected
    // path for manual paste. Never send navigation messages across UIPI.
    const bool fileDialog = filedialog::kind(foreground) != filedialog::Kind::None;
    setDialog(fileDialog ? foreground : nullptr, fromManager);
    if (fileDialog)
        return;
    // A dialog may come to the front before all of its controls are there.
    if (hasClass(foreground, L"#32770") || hasClass(foreground, L"TSelectFolderForm")
        || hasClass(foreground, L"TSelectDirDlg")) {
        m_pending = foreground;
        m_pendingFromManager = fromManager;
        m_retries = 5;
        m_recheck.start();
    } else if (filedialog::mayBeKingsoft(foreground) && !filedialog::probed(foreground)) {
        // WPS Office's own, asked through UI Automation: on the worker, as
        // that waits on the program.
        m_pending = foreground;
        m_pendingFromManager = fromManager;
        m_worker.post([this, hwnd = foreground] {
            if (filedialog::probe(hwnd) == filedialog::Kind::None)
                return;
            QMetaObject::invokeMethod(&m_recheck, [this, hwnd] {
                if (hwnd == m_pending && ::GetForegroundWindow() == hwnd)
                    setDialog(hwnd, m_pendingFromManager);
            }, Qt::QueuedConnection);
        });
    }
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
    setRegistered(m_hotkeyEnabled && dialog && !integrity::higherThanOurs(dialog));
    if (m_callbacks.dialogChanged)
        m_callbacks.dialogChanged(dialog);

    if (dialog && m_autoJump && !integrity::higherThanOurs(dialog)) {
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
    setRegistered(on && m_dialog && !integrity::higherThanOurs(m_dialog));
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
    if (integrity::higherThanOurs(dialog))
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
    if (!dialog || !::IsWindow(dialog) || integrity::higherThanOurs(dialog))
        return;
    if (std::ranges::find(m_companions, ::GetForegroundWindow()) != m_companions.end())
        ::SetForegroundWindow(dialog); // ours is in front, so it may hand that on
    m_worker.post([dialog] {
        const std::vector<filemanager::Folder> folders = filemanager::openFolders();
        filedialog::waitForFront(dialog);
        if (!folders.empty() && !filedialog::goTo(dialog, folders.front().path))
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
