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
class PluginHotkeys {
public:
    using Pressed = std::function<void(const QString& owner, const QString& key)>;
    explicit PluginHotkeys(Pressed pressed);
    ~PluginHotkeys();
    PluginHotkeys(const PluginHotkeys&) = delete;
    PluginHotkeys& operator=(const PluginHotkeys&) = delete;

    static bool valid(const QStringList& keys);
    // Replaces this owner's registrations, retaining unchanged keys. Errors
    // are Windows error codes keyed by the requested spelling. Empty releases.
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
