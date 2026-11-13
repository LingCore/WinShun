#pragma once

#include <QString>

#include <windows.h>

#include <chrono>
#include <deque>
#include <optional>

// Taking the PrtScn key over from the Snipping Tool (docs/design-capture.md,
// 4.5). Windows 11 gives PrtScn to the Snipping Tool while
// HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled is not 0;
// at 0 a RegisterHotKey on it gets WM_HOTKEY. The value is read when the
// keyboard settings change, so every write is followed by WM_SETTINGCHANGE
// for "Control Panel\Keyboard": without it (stage 0, capture-probe prtscn)
// the hotkey is not delivered and PrtScn copies the whole screen to the
// clipboard instead, as before Windows 11.
namespace ws::printscreen {

// The key the value is in, under HKEY_CURRENT_USER.
inline constexpr wchar_t kKeyboardKey[] = L"Control Panel\\Keyboard";

// The value as it is: none when absent. `subkey` is for the tests, which keep
// a value of their own under another key; the real one is never touched there.
std::optional<DWORD> read(const wchar_t* subkey = kKeyboardKey);
// Writes the value (none: deletes it), then tells everyone (the real key only).
bool write(std::optional<DWORD> value, const wchar_t* subkey = kKeyboardKey);
void broadcast();

// The value as kept in WinShun.ini to be put back: "absent" or the number.
QString encode(std::optional<DWORD> value);
// Back from that; false when the text is neither.
bool decode(const QString& text, std::optional<DWORD>* value);

// Whether PrtScn is the Snipping Tool's with the value so: anything but 0.
inline bool snippingToolOwns(std::optional<DWORD> value)
{
    return !value || *value != 0;
}

// GitHub #322: the value is put back after it was taken (a late old process
// on an update, Windows, another program) and taken again. But a program that
// keeps doing it would be fought for ever: a 4th retake within a minute is refused.
class RetakeLimit {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMostRetakes = 3;
    static constexpr std::chrono::seconds kWindow {60};

    // Counts a retake at `now`; false (and not counted) when it is one too many.
    [[nodiscard]] bool allow(Clock::time_point now);
    void clear() noexcept { m_times.clear(); }

private:
    std::deque<Clock::time_point> m_times; // the retakes within the window, oldest first
};

} // namespace ws::printscreen
