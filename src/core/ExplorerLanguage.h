#pragma once

#include <windows.h>

#include <string_view>

// The words Win顺 puts into Explorer itself (the right-click verbs of
// platform/ShellMenu.cpp and what the stub says when one runs, src/stub): they
// stand among Explorer's own, so they follow the language Windows is shown
// in, not the one Win顺 is set to (GitHub #190). Chinese Windows: Chinese;
// any other: English. Pure C++ and Win32, no Qt: the stub uses it too.
namespace ws::explorerlang {

// Whether `language` (a LANGID: GetUserDefaultUILanguage) is Chinese; our words
// and Win顺's "system" language are Chinese then, English for any other.
constexpr bool isChinese(LANGID language) noexcept
{
    return PRIMARYLANGID(language) == LANG_CHINESE;
}

constexpr bool isEnglish(LANGID language) noexcept
{
    return !isChinese(language);
}

// The language Windows itself is shown in, not the first of the preferred
// languages (not QLocale::uiLanguages(): that can put English first on a
// Chinese Windows).
inline bool windowsIsChinese() noexcept
{
    return isChinese(::GetUserDefaultUILanguage());
}

inline bool windowsIsEnglish() noexcept
{
    return !windowsIsChinese();
}

constexpr std::wstring_view searchHereTitle(bool english) noexcept
{
    return english ? L"Search here with WinShun" : L"用 Win顺 搜索此处";
}

constexpr std::wstring_view pasteFileTitle(bool english) noexcept
{
    return english ? L"Paste as file" : L"粘贴为文件";
}

// On a file or a folder: the user's own commands that ask for it (GitHub #61).
constexpr std::wstring_view commandsTitle(bool english) noexcept
{
    return english ? L"WinShun commands" : L"Win顺 命令";
}

// On a picture file, while the screenshot plugin is on (GitHub #128).
constexpr std::wstring_view pinPictureTitle(bool english) noexcept
{
    return english ? L"Pin to screen (WinShun)" : L"贴到屏幕（Win顺）";
}

// The entry in 发送到 for the LAN transfer plugin (GitHub #180); the file is this and ".lnk".
constexpr std::wstring_view lanSendTitle(bool english) noexcept
{
    return english ? L"LAN transfer (WinShun)" : L"局域网传输（Win顺）";
}

} // namespace ws::explorerlang
