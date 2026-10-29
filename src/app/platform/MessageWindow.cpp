#include "MessageWindow.h"

#include "MonoLogo.h"

#include <QDebug>
#include <QDeadlineTimer>
#include <QStringList>

#include <shellapi.h>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr wchar_t kClassName[] = L"WinShun.MessageWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kHeldTimer = 1; // a plugin's notification held back: time to look again (showHeld)
constexpr ULONG_PTR kCopyDataTag = 0x5753484E; // "WSHN"
// The window Explorer's "search here" reaches (WinShunSearch.exe, src/stub).
constexpr wchar_t kSearchHereClass[] = L"WinShun.SearchHere";
constexpr ULONG_PTR kSearchHereTag = 0x57535348; // "WSSH"
constexpr ULONG_PTR kFolderMenuTag = 0x5753464D; // "WSFM"

// A folder's full path as Explorer gives it: on a drive or a share, nothing
// that could be read as anything else.
bool isFolderPath(const QString& path)
{
    if (path.size() < 2 || path.size() > 32767
        || std::any_of(path.begin(), path.end(), [](QChar c) { return c.unicode() < 0x20 || c == u'"'; }))
        return false;
    const bool drive = path[0].isLetter() && path[0].unicode() < 128 && path[1] == u':'
        && (path.size() == 2 || path[2] == u'\\');
    const bool share = path.startsWith(u"\\\\") && path.size() > 2 && path[2] != u'\\' && path[2] != u'?'
        && path[2] != u'.';
    return drive || share;
}
constexpr wchar_t kAppIconResource[] = L"IDI_ICON1"; // see app.rc
// How long a second start waits for the running copy to take its command.
constexpr std::chrono::milliseconds kRunningInstanceWait {30000};

void copyTruncated(wchar_t* dest, std::size_t capacity, const QString& text)
{
    auto n = std::min<std::size_t>(capacity - 1, static_cast<std::size_t>(text.size()));
    if (n > 0 && n < static_cast<std::size_t>(text.size()) && text.at(static_cast<qsizetype>(n) - 1).isHighSurrogate())
        --n; // never half a pair
    std::memcpy(dest, text.utf16(), n * sizeof(wchar_t));
    dest[n] = 0;
}

} // namespace

MessageWindow::MessageWindow(Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = &MessageWindow::wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);

    // A real (hidden) top-level window rather than HWND_MESSAGE: message-only
    // windows miss the "TaskbarCreated" broadcast.
    m_hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, kClassName, L"WinShun", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    m_taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
    // Let Explorer (medium integrity) reach us even if we run elevated. A
    // second start of Win顺 too: it runs with the user's normal rights
    // (GitHub #21), while this copy may be elevated (the 管理员直读 mode), and
    // its WM_COPYDATA would be dropped by UIPI otherwise. What it carries is
    // one of toggle, quit, settings and query (App::handleCommand): no input
    // for anything, nothing run. Not the same window as "WinShun.SearchHere"
    // below, which has its own.
    ::ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreated, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(m_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    // Open to WinShunSearch.exe, which runs with the user's normal rights:
    // it takes a folder to search in, and nothing else.
    WNDCLASSEXW searchHere {sizeof(WNDCLASSEXW)};
    searchHere.lpfnWndProc = &MessageWindow::searchHereProc;
    searchHere.hInstance = instance;
    searchHere.lpszClassName = kSearchHereClass;
    ::RegisterClassExW(&searchHere);
    m_searchHere = ::CreateWindowExW(
        0, kSearchHereClass, L"WinShun", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    ::ChangeWindowMessageFilterEx(m_searchHere, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    m_powerNotify = ::RegisterSuspendResumeNotification(m_hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);

    loadIcon();
}

void MessageWindow::loadIcon()
{
    if (m_icon)
        ::DestroyIcon(m_icon);
    // The tray's icons are at the system's scaling (it changes only when the user signs in again).
    const UINT dpi = ::GetDpiForSystem();
    const int size = ::GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    m_icon = m_mono ? monologo::icon(size, monologo::taskbarInk())
                    : static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), kAppIconResource, IMAGE_ICON, size,
                          ::GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
}

void MessageWindow::setTrayIconMono(bool mono)
{
    if (m_mono == mono)
        return;
    m_mono = mono;
    loadIcon();
    if (!m_trayVisible)
        return;
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON;
    nid.hIcon = m_icon;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

MessageWindow::~MessageWindow()
{
    removeTrayIcon();
    if (m_powerNotify)
        ::UnregisterSuspendResumeNotification(m_powerNotify);
    if (m_searchHere)
        ::DestroyWindow(m_searchHere);
    if (m_hwnd)
        ::DestroyWindow(m_hwnd);
    if (m_icon)
        ::DestroyIcon(m_icon);
}

void MessageWindow::showTrayIcon(const QString& tooltip)
{
    m_tooltip = tooltip;
    addTrayIcon();
}

void MessageWindow::setTrayTooltip(const QString& tooltip)
{
    m_tooltip = tooltip;
    if (!m_trayVisible)
        return; // used when the icon is added
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    copyTruncated(nid.szTip, std::size(nid.szTip), m_tooltip);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool MessageWindow::addTrayIcon()
{
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = m_icon;
    copyTruncated(nid.szTip, std::size(nid.szTip), m_tooltip);
    m_trayVisible = ::Shell_NotifyIconW(NIM_ADD, &nid);
    if (m_trayVisible) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    return m_trayVisible;
}

void MessageWindow::removeTrayIcon()
{
    if (!m_trayVisible)
        return;
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    m_trayVisible = false;
}

void MessageWindow::showNotification(const QString& title, const QString& text, std::function<void()> onClick, const QString& owner)
{
    const std::uint64_t now = ::GetTickCount64();
    if (!owner.isEmpty()) {
        if (const std::uint64_t until = m_turns.ownUpUntil(now)) {
            // Win顺's own is up: this one waits (the newest of those waiting).
            qInfo() << "Notification: a plugin's waits for Win顺's own to go";
            m_held = Held {title, text, std::move(onClick), owner, now};
            holdUntil(until, now);
            return;
        }
        if (m_held) {
            qInfo() << "Notification: a newer plugin's goes first, the one waiting is dropped";
            dropHeld();
        }
    }
    m_onNotificationClick = std::move(onClick); // also when there is no tray icon to show it on
    if (!m_trayVisible)
        return;
    m_turns.shown(owner, now);
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    copyTruncated(nid.szInfoTitle, std::size(nid.szInfoTitle), title);
    // No text would take the balloon away instead (hideNotification): the title then.
    copyTruncated(nid.szInfo, std::size(nid.szInfo), text.trimmed().isEmpty() ? title : text);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Its end may never be told (a balloon replaced, quiet hours): the timer
// looks again when Win顺's own goes at the latest, or the held one is out of date.
void MessageWindow::holdUntil(std::uint64_t until, std::uint64_t now)
{
    if (!m_held)
        return;
    const std::uint64_t at = std::min(until, m_held->since + kHeldMostMs);
    ::SetTimer(m_hwnd, kHeldTimer, static_cast<UINT>(at > now ? at - now + 50 : 50), nullptr);
}

void MessageWindow::showHeld()
{
    if (!m_held)
        return;
    const std::uint64_t now = ::GetTickCount64();
    if (now - m_held->since > kHeldMostMs) {
        qInfo() << "Notification: a plugin's held back too long, dropped";
        dropHeld();
        return;
    }
    if (const std::uint64_t until = m_turns.ownUpUntil(now)) {
        holdUntil(until, now); // another of Win顺's own came meanwhile
        return;
    }
    ::KillTimer(m_hwnd, kHeldTimer);
    Held held = std::move(*m_held);
    m_held.reset();
    showNotification(held.title, held.text, std::move(held.onClick), held.owner);
}

void MessageWindow::dropHeld()
{
    m_held.reset();
    ::KillTimer(m_hwnd, kHeldTimer);
}

void MessageWindow::hideNotification()
{
    m_onNotificationClick = nullptr;
    m_turns.clear();
    dropHeld(); // a screenshot or a recording begins: not now, and later it is out of date
    if (!m_trayVisible)
        return;
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO; // with no text: the balloon (Windows' toast of it) goes
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool MessageWindow::parseHotkey(const QString& shortcut, UINT* modifiers, UINT* vk)
{
    *modifiers = MOD_NOREPEAT;
    *vk = 0;
    const QStringList parts = shortcut.split(u'+', Qt::SkipEmptyParts);
    for (QString part : parts) {
        part = part.trimmed();
        const QString key = part.toLower();
        if (key == u"ctrl" || key == u"control")
            *modifiers |= MOD_CONTROL;
        else if (key == u"alt")
            *modifiers |= MOD_ALT;
        else if (key == u"shift")
            *modifiers |= MOD_SHIFT;
        else if (key == u"win" || key == u"meta")
            *modifiers |= MOD_WIN;
        else if (key == u"space")
            *vk = VK_SPACE;
        else if (key == u"tab")
            *vk = VK_TAB;
        else if (key == u"enter" || key == u"return")
            *vk = VK_RETURN;
        else if (key == u"`" || key == u"backquote")
            *vk = VK_OEM_3;
        else if (key == u"printscreen" || key == u"print" || key == u"prtsc")
            *vk = VK_SNAPSHOT; // the screen capture's, once taken over from the Snipping Tool (printscreen::)
        else if (key.size() == 1 && key[0].isLetterOrNumber() && key[0].unicode() < 128)
            *vk = part.toUpper()[0].unicode();
        else if (key.startsWith(u'f') && key.size() <= 3) {
            bool ok = false;
            const int n = key.mid(1).toInt(&ok);
            if (!ok || n < 1 || n > 24)
                return false;
            *vk = VK_F1 + n - 1;
        } else {
            return false;
        }
    }
    return *vk != 0;
}

bool MessageWindow::registerHotkey(int id, const QString& shortcut)
{
    UINT modifiers = 0;
    UINT vk = 0;
    if (!parseHotkey(shortcut, &modifiers, &vk))
        return false;
    return ::RegisterHotKey(m_hwnd, id, modifiers, vk);
}

void MessageWindow::unregisterHotkey(int id)
{
    ::UnregisterHotKey(m_hwnd, id);
}

bool MessageWindow::sendToRunningInstance(const QString& command)
{
    // The running copy may still be starting: this window comes once the
    // launcher's is made, and its first start after an update is slow (the
    // antivirus reads every new file). Started from the installer's last page,
    // the command would be lost otherwise.
    const QDeadlineTimer deadline(kRunningInstanceWait);
    HWND target = ::FindWindowW(kClassName, nullptr);
    while (!target && !deadline.hasExpired()) {
        ::Sleep(100);
        target = ::FindWindowW(kClassName, nullptr);
    }
    if (!target)
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    ::AllowSetForegroundWindow(pid); // we were just launched by the user, so we may pass that on
    COPYDATASTRUCT data {};
    data.dwData = kCopyDataTag;
    data.cbData = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<void*>(static_cast<const void*>(command.utf16()));
    // Answered once it is done starting. Sent once: a send that timed out may
    // still arrive, and a toggle sent twice would undo itself.
    const auto timeout = static_cast<UINT>(std::max<qint64>(deadline.remainingTime(), 5000));
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_NORMAL, timeout, &result)
        != 0;
}

LRESULT CALLBACK MessageWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<MessageWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->m_hwnd == hwnd)
        return self->handle(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MessageWindow::searchHereProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (msg != WM_COPYDATA)
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    auto* self = reinterpret_cast<MessageWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
    if (!self || !data || (data->dwData != kSearchHereTag && data->dwData != kFolderMenuTag) || !data->lpData
        || data->cbData < sizeof(wchar_t) || data->cbData > 32768 * sizeof(wchar_t))
        return FALSE;
    QString folder = QString::fromWCharArray(
        static_cast<const wchar_t*>(data->lpData), static_cast<qsizetype>(data->cbData / sizeof(wchar_t)));
    while (folder.endsWith(QChar(0)))
        folder.chop(1);
    const auto& callback
        = data->dwData == kFolderMenuTag ? self->m_callbacks.folderMenu : self->m_callbacks.searchHere;
    if (!isFolderPath(folder) || !callback)
        return FALSE;
    callback(folder);
    return TRUE;
}

LRESULT MessageWindow::handle(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == kTrayMessage) {
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // Posted, it carries the click's time; sent from Explorer's
            // thread, it has none of its own (GetMessageTime is the last
            // posted message's), and is handled as it comes.
            if (m_callbacks.trayActivated)
                m_callbacks.trayActivated(
                    ::InSendMessage() ? ::GetTickCount() : static_cast<std::uint32_t>(::GetMessageTime()));
            break;
        case WM_CONTEXTMENU:
            if (m_callbacks.trayMenuRequested)
                m_callbacks.trayMenuRequested();
            break;
        case NIN_BALLOONUSERCLICK: {
            // Taken out first: the action may show a notification of its own.
            // Right after a plugin's replaced another owner's, which one was
            // clicked cannot be told: nothing runs rather than another's.
            // (Win顺's own newest: its action, which only ever does Win顺's
            // own harmless things, runs.)
            auto action = std::exchange(m_onNotificationClick, nullptr);
            const bool unclear = m_turns.clickUnclear(::GetTickCount64());
            m_turns.clear(); // clicked away
            if (unclear && action) {
                qInfo() << "Notification: clicked right after one of another owner's was replaced, nothing done";
            } else if (action) {
                qInfo() << "Notification: clicked, running its action";
                action();
            } else {
                qInfo() << "Notification: clicked, nothing to do";
            }
            showHeld();
            break;
        }
        case NIN_BALLOONSHOW:
            qInfo() << "Notification: shown";
            break;
        case NIN_BALLOONHIDE: // closed with its cross, or taken away
        case NIN_BALLOONTIMEOUT: // gone by itself: it is not kept in the Action Center, so no click can come later
            // A newer one may already have replaced this one's action: only
            // the newest's end clears it (one replaced may tell its end, right
            // then, or never: NotificationTurns).
            if (m_turns.ended(::GetTickCount64())) {
                if (m_onNotificationClick)
                    qInfo() << "Notification: gone, its click action dropped";
                m_onNotificationClick = nullptr;
            }
            showHeld();
            break;
        default:
            break;
        }
        return 0;
    }
    if (msg == m_taskbarCreated && m_taskbarCreated != 0) {
        m_trayVisible = false;
        m_turns.clear(); // the notifications went with the old taskbar
        m_onNotificationClick = nullptr;
        dropHeld();
        addTrayIcon();
        if (m_callbacks.shellRestarted)
            m_callbacks.shellRestarted();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wParam == kHeldTimer) {
            ::KillTimer(m_hwnd, kHeldTimer);
            showHeld(); // Win顺's own has gone by now, its end told or not
            return 0;
        }
        return ::DefWindowProcW(m_hwnd, msg, wParam, lParam);
    case WM_SETTINGCHANGE: // the taskbar light or dark, high contrast: the one-colour icon in its new ink
        if (m_mono) {
            m_mono = false;
            setTrayIconMono(true);
        }
        return ::DefWindowProcW(m_hwnd, msg, wParam, lParam);
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam && m_callbacks.sessionEnding)
            m_callbacks.sessionEnding(); // the process may be terminated right after this returns
        return 0;
    case WM_CLOSE:
        // The installer and uninstaller ask a running copy to go this way
        // (installer/WinShun.iss). Quit as with --quit, saving the index;
        // DefWindowProc would only destroy this window and leave the app running.
        if (m_callbacks.commandReceived)
            m_callbacks.commandReceived(u"quit"_s);
        return 0;
    case WM_DEVICECHANGE:
        return m_callbacks.deviceChange ? m_callbacks.deviceChange(wParam, lParam) : TRUE;
    case WM_POWERBROADCAST:
        if (m_callbacks.drawingMessage)
            m_callbacks.drawingMessage(msg, wParam);
        if (wParam == PBT_APMRESUMEAUTOMATIC && m_callbacks.resumed)
            m_callbacks.resumed(); // whether or not someone is at the keyboard
        return TRUE;
    case WM_FONTCHANGE:
        if (m_callbacks.drawingMessage)
            m_callbacks.drawingMessage(msg, wParam);
        return 0;
    case WM_HOTKEY: // posted when the keys were pressed
        if (m_callbacks.hotkeyPressed)
            m_callbacks.hotkeyPressed(static_cast<int>(wParam), static_cast<std::uint32_t>(::GetMessageTime()));
        return 0;
    case WM_COPYDATA: {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || data->dwData != kCopyDataTag || !data->lpData || data->cbData < sizeof(wchar_t))
            return FALSE;
        const auto chars = static_cast<qsizetype>(data->cbData / sizeof(wchar_t));
        QString command = QString::fromWCharArray(static_cast<const wchar_t*>(data->lpData), chars);
        while (command.endsWith(QChar(0)))
            command.chop(1);
        if (m_callbacks.commandReceived)
            m_callbacks.commandReceived(command);
        return TRUE;
    }
    default:
        return ::DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}

} // namespace ws
