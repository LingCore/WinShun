#include "MessageWindow.h"

#include <QStringList>

#include <shellapi.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr wchar_t kClassName[] = L"WinShun.MessageWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr ULONG_PTR kCopyDataTag = 0x5753484E; // "WSHN"
constexpr wchar_t kAppIconResource[] = L"IDI_ICON1"; // see app.rc

void copyTruncated(wchar_t* dest, std::size_t capacity, const QString& text)
{
    const auto n = std::min<std::size_t>(capacity - 1, static_cast<std::size_t>(text.size()));
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
    // second instance runs elevated too, so WM_COPYDATA stays closed to others.
    ::ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreated, MSGFLT_ALLOW, nullptr);

    const UINT dpi = ::GetDpiForSystem();
    m_icon = static_cast<HICON>(::LoadImageW(instance, kAppIconResource, IMAGE_ICON,
        ::GetSystemMetricsForDpi(SM_CXSMICON, dpi), ::GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
}

MessageWindow::~MessageWindow()
{
    removeTrayIcon();
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

void MessageWindow::showNotification(const QString& title, const QString& text)
{
    if (!m_trayVisible)
        return;
    NOTIFYICONDATAW nid {sizeof(NOTIFYICONDATAW)};
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    copyTruncated(nid.szInfoTitle, std::size(nid.szInfoTitle), title);
    copyTruncated(nid.szInfo, std::size(nid.szInfo), text);
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
    const HWND target = ::FindWindowW(kClassName, nullptr);
    if (!target)
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    ::AllowSetForegroundWindow(pid); // we were just launched by the user, so we may pass that on
    COPYDATASTRUCT data {};
    data.dwData = kCopyDataTag;
    data.cbData = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<void*>(static_cast<const void*>(command.utf16()));
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(
               target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result)
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

LRESULT MessageWindow::handle(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == kTrayMessage) {
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (m_callbacks.trayActivated)
                m_callbacks.trayActivated();
            break;
        case WM_CONTEXTMENU:
            if (m_callbacks.trayMenuRequested)
                m_callbacks.trayMenuRequested();
            break;
        case NIN_BALLOONUSERCLICK:
            if (m_callbacks.notificationClicked)
                m_callbacks.notificationClicked();
            break;
        default:
            break;
        }
        return 0;
    }
    if (msg == m_taskbarCreated && m_taskbarCreated != 0) {
        m_trayVisible = false;
        addTrayIcon();
        return 0;
    }
    switch (msg) {
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
    case WM_HOTKEY:
        if (m_callbacks.hotkeyPressed)
            m_callbacks.hotkeyPressed(static_cast<int>(wParam));
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
