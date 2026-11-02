#include "PluginHotkeys.h"
#include "HotkeyText.h"

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {
namespace {
constexpr wchar_t kClass[] = L"WinShun.PluginHotkeys";
}

PluginHotkeys::PluginHotkeys(Pressed pressed) : m_pressed(std::move(pressed))
{
    WNDCLASSW type {};
    type.lpfnWndProc = windowProc;
    type.hInstance = ::GetModuleHandleW(nullptr);
    type.lpszClassName = kClass;
    ::RegisterClassW(&type);
    m_window = ::CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, type.hInstance, this);
}

PluginHotkeys::~PluginHotkeys()
{
    if (m_window) {
        for (const auto& [id, key] : m_keys)
            ::UnregisterHotKey(m_window, id);
        ::DestroyWindow(m_window);
    }
}

bool PluginHotkeys::valid(const QStringList& keys)
{
    return keys.size() <= 32 && std::ranges::all_of(keys, [](const QString& key) {
        return key.size() <= 64 && (hotkeys::allowed(key) || hotkeys::normalized(key) == u"printscreen");
    });
}

QJsonObject PluginHotkeys::replace(const QString& owner, const QStringList& keys)
{
    QJsonObject errors;
    if (!valid(keys)) {
        errors.insert(u"invalid"_s, int(ERROR_INVALID_PARAMETER));
        return errors; // malformed requests do not release existing keys
    }
    QStringList wanted;
    for (const QString& key : keys)
        wanted.append(hotkeys::normalized(key));
    for (auto it = m_keys.begin(); it != m_keys.end();) {
        if (it->second.owner == owner && !wanted.contains(it->second.normalized)) {
            ::UnregisterHotKey(m_window, it->first);
            it = m_keys.erase(it);
        } else {
            ++it;
        }
    }
    QStringList seen;
    for (qsizetype i = 0; i < keys.size(); ++i) {
        if (seen.contains(wanted[i])) {
            errors.insert(keys[i], int(ERROR_HOTKEY_ALREADY_REGISTERED));
            continue;
        }
        seen.append(wanted[i]);
        const auto existing = std::ranges::find_if(m_keys, [&](const auto& entry) {
            return entry.second.owner == owner && entry.second.normalized == wanted[i];
        });
        if (existing != m_keys.end()) {
            existing->second.text = keys[i];
            continue;
        }
        UINT modifiers = 0, vk = 0;
        hotkeys::parse(wanted[i], &modifiers, &vk);
        // Do not immediately reuse retired IDs: WM_HOTKEY may already be queued.
        while (m_keys.contains(m_nextId))
            m_nextId = m_nextId == 0xBFFF ? 1 : m_nextId + 1;
        const int id = m_nextId;
        m_nextId = m_nextId == 0xBFFF ? 1 : m_nextId + 1;
        if (!m_window || !::RegisterHotKey(m_window, id, modifiers, vk)) {
            errors.insert(keys[i], int(m_window ? ::GetLastError() : ERROR_INVALID_WINDOW_HANDLE));
        } else {
            m_keys.emplace(id, Key {owner, keys[i], wanted[i]});
        }
    }
    return errors;
}

void PluginHotkeys::remove(const QString& owner)
{
    replace(owner, {});
}

LRESULT CALLBACK PluginHotkeys::windowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<PluginHotkeys*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && message == WM_HOTKEY) {
        const auto it = self->m_keys.find(static_cast<int>(w));
        if (it != self->m_keys.end() && self->m_pressed) {
            const Key key = it->second; // the callback may replace the registrations
            self->m_pressed(key.owner, key.text);
        }
        return 0;
    }
    return ::DefWindowProcW(window, message, w, l);
}
} // namespace ws
