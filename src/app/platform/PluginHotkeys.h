#pragma once

#include <QJsonObject>
#include <QStringList>
#include <windows.h>

#include <functional>
#include <map>

namespace ws {

// Register on the host's integrity level: unmodified function keys registered
// by an ordinary plugin do not fire over an elevated foreground application.
// Only a notification crosses back to the plugin; it keeps its normal rights.
// With Win顺 itself at normal rights, the service's UIAccess listener holds
// such keys back from an administrator's window for it (plainkeys, #243).
class PluginHotkeys {
public:
    using Pressed = std::function<void(const QString& owner, const QString& key)>;
    explicit PluginHotkeys(Pressed pressed);
    ~PluginHotkeys();
    PluginHotkeys(const PluginHotkeys&) = delete;
    PluginHotkeys& operator=(const PluginHotkeys&) = delete;

    // A well-formed request: at most 32 keys, none longer than 64 characters.
    static bool valid(const QStringList& keys);
    // A key the shortcut recorder accepts, or PrintScreen.
    static bool allowed(const QString& key);
    // Replaces this owner's registrations, retaining unchanged keys. Errors
    // are Windows error codes keyed by the requested spelling: a key not
    // allowed is ERROR_INVALID_PARAMETER, the rest are still registered; a
    // key repeated in another spelling is ERROR_HOTKEY_ALREADY_REGISTERED.
    // A malformed request (see valid) changes nothing. Empty releases.
    QJsonObject replace(const QString& owner, const QStringList& keys);
    void remove(const QString& owner);

private:
    struct Key { QString owner, text, normalized; };
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l);
    HWND m_window = nullptr;
    Pressed m_pressed;
    std::map<int, Key> m_keys;
    int m_nextId = 1;
};

} // namespace ws
