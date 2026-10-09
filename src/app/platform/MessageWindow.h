#pragma once

#include <QString>

#include <windows.h>

#include <functional>

namespace ws {

// A hidden native window that receives what Qt does not handle for us:
// tray icon events, RegisterHotKey hotkeys, commands from a second instance
// of the app (WM_COPYDATA), drives coming and going (WM_DEVICECHANGE) and
// Windows resuming. It also restores the tray icon when Explorer restarts.
class MessageWindow {
public:
    struct Callbacks {
        std::function<void()> trayActivated; // left click
        std::function<void()> trayMenuRequested; // right click
        std::function<void()> notificationClicked; // the last showNotification()
        std::function<void(int id)> hotkeyPressed;
        std::function<void(const QString&)> commandReceived;
        std::function<void()> sessionEnding; // Windows is logging off / shutting down
        std::function<LRESULT(WPARAM, LPARAM)> deviceChange; // WM_DEVICECHANGE (see VolumeNotifier)
        std::function<void()> shellRestarted; // Explorer started again (its taskbar is back)
        std::function<void()> resumed; // from sleep or hibernation
    };

    explicit MessageWindow(Callbacks callbacks);
    ~MessageWindow();

    MessageWindow(const MessageWindow&) = delete;
    MessageWindow& operator=(const MessageWindow&) = delete;

    HWND hwnd() const noexcept { return m_hwnd; }

    void showTrayIcon(const QString& tooltip);
    void setTrayTooltip(const QString& tooltip); // the language changed
    void showNotification(const QString& title, const QString& text);

    bool registerHotkey(int id, const QString& shortcut); // "Alt+Space", "Ctrl+Shift+F"
    void unregisterHotkey(int id);

    // Returns true when another instance received the command.
    static bool sendToRunningInstance(const QString& command);
    static bool parseHotkey(const QString& shortcut, UINT* modifiers, UINT* vk);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    bool addTrayIcon();
    void removeTrayIcon();

    Callbacks m_callbacks;
    HWND m_hwnd = nullptr;
    HICON m_icon = nullptr;
    HPOWERNOTIFY m_powerNotify = nullptr; // so that resuming is told on every kind of standby
    QString m_tooltip;
    bool m_trayVisible = false;
    UINT m_taskbarCreated = 0;
};

} // namespace ws
