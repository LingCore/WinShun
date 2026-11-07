#include "PlainHotkeys.h"

#include "KeyListenerProtocol.h"

#include <algorithm>
#include <utility>

namespace ws::plainkeys {
namespace {

struct Held {
    HWND window = nullptr;
    int id = 0;
    std::uint32_t vk = 0;
};

std::vector<Held>& held()
{
    static std::vector<Held> keys;
    return keys;
}

std::function<void()>& changedCallback()
{
    static std::function<void()> changed;
    return changed;
}

// Runs `edit` and tells the listener if the relayed keys changed.
template <typename Edit>
void editing(Edit&& edit)
{
    const std::vector<std::uint32_t> before = relayed();
    std::forward<Edit>(edit)(held());
    if (relayed() != before) {
        const std::function<void()> changed = changedCallback(); // it may set another
        if (changed)
            changed();
    }
}

} // namespace

bool registerHotkey(HWND window, int id, UINT modifiers, UINT vk)
{
    if (!::RegisterHotKey(window, id, modifiers, vk))
        return false;
    if ((modifiers & ~MOD_NOREPEAT) == 0 && keyproto::relayable(vk)) {
        const DWORD error = ::GetLastError();
        editing([&](std::vector<Held>& keys) { keys.push_back({window, id, vk}); });
        ::SetLastError(error);
    }
    return true;
}

void unregisterHotkey(HWND window, int id)
{
    ::UnregisterHotKey(window, id);
    editing([&](std::vector<Held>& keys) {
        std::erase_if(keys, [&](const Held& key) { return key.window == window && key.id == id; });
    });
}

void unregisterAll(HWND window)
{
    editing([&](std::vector<Held>& keys) {
        std::erase_if(keys, [&](const Held& key) { return key.window == window; });
    });
}

std::vector<std::uint32_t> relayed()
{
    std::vector<std::uint32_t> keys;
    for (const Held& key : held())
        keys.push_back(key.vk);
    std::ranges::sort(keys);
    keys.erase(std::ranges::unique(keys).begin(), keys.end());
    if (keys.size() > keyproto::kMaxRelayedKeys)
        keys.resize(keyproto::kMaxRelayedKeys); // more cannot be: there are 25 such keys
    return keys;
}

bool deliver(std::uint32_t vk)
{
    // Windows lets one window hold a key: the last noted is the one holding it.
    const auto& keys = held();
    const auto it = std::ranges::find_if(keys.rbegin(), keys.rend(), [vk](const Held& key) { return key.vk == vk; });
    return it != keys.rend() && ::PostMessageW(it->window, WM_HOTKEY, static_cast<WPARAM>(it->id), MAKELPARAM(0, vk));
}

void setChanged(std::function<void()> changed)
{
    changedCallback() = std::move(changed);
}

} // namespace ws::plainkeys
