#include "PrintScreen.h"

#include <string_view>

using namespace Qt::StringLiterals;

namespace ws::printscreen {

namespace {

constexpr wchar_t kValue[] = L"PrintScreenKeyForSnippingEnabled";

} // namespace

std::optional<DWORD> read(const wchar_t* subkey)
{
    DWORD value = 0;
    DWORD size = sizeof value;
    if (::RegGetValueW(HKEY_CURRENT_USER, subkey, kValue, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

bool write(std::optional<DWORD> value, const wchar_t* subkey)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS status = ERROR_SUCCESS;
    if (value) {
        const DWORD v = *value;
        status = ::RegSetValueExW(key, kValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
    } else {
        status = ::RegDeleteValueW(key, kValue);
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }
    ::RegCloseKey(key);
    if (std::wstring_view(subkey) == kKeyboardKey) // a key of the tests' is nobody else's business
        broadcast();
    return status == ERROR_SUCCESS;
}

void broadcast()
{
    // Explorer reads the value again on this; a hung window must not hold
    // us up, hence the timeout and SMTO_ABORTIFHUNG.
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kKeyboardKey),
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &result);
}

QString encode(std::optional<DWORD> value)
{
    return value ? QString::number(*value) : u"absent"_s;
}

bool decode(const QString& text, std::optional<DWORD>* value)
{
    if (text == u"absent") {
        *value = std::nullopt;
        return true;
    }
    bool ok = false;
    const uint v = text.toUInt(&ok);
    if (!ok)
        return false;
    *value = static_cast<DWORD>(v);
    return true;
}

bool RetakeLimit::allow(Clock::time_point now)
{
    while (!m_times.empty() && now - m_times.front() >= kWindow)
        m_times.pop_front();
    if (m_times.size() >= kMostRetakes)
        return false;
    m_times.push_back(now);
    return true;
}

} // namespace ws::printscreen
