#include "Win32Util.h"

namespace ws::win32 {

std::wstring longPath(std::wstring_view path)
{
    if (path.starts_with(L"\\\\?\\"))
        return std::wstring(path);
    if (path.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    return L"\\\\?\\" + std::wstring(path);
}

std::wstring expandEnvironment(std::wstring_view text)
{
    const std::wstring input(text);
    DWORD needed = ::ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0)
        return input;
    std::wstring out(needed, L'\0');
    needed = ::ExpandEnvironmentStringsW(input.c_str(), out.data(), needed);
    if (needed == 0)
        return input;
    out.resize(needed - 1); // drop the terminator
    return out;
}

} // namespace ws::win32
