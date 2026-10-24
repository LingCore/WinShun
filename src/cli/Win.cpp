#include "Win.h"

#include <algorithm>

namespace ws::cli {

std::wstring environment(const wchar_t* name)
{
    std::wstring value(256, L'\0');
    for (;;) {
        const DWORD n = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (n == 0)
            return {};
        if (n < value.size()) {
            value.resize(n);
            return value;
        }
        value.resize(n); // n counts the terminating null when the buffer was short
    }
}

bool environmentSet(const wchar_t* name)
{
    wchar_t probe[1];
    ::SetLastError(ERROR_SUCCESS);
    const DWORD n = ::GetEnvironmentVariableW(name, probe, 1);
    return n > 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

std::wstring modulePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0)
            return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring absolutePath(std::wstring_view path)
{
    const std::wstring given(path);
    const DWORD needed = ::GetFullPathNameW(given.c_str(), 0, nullptr, nullptr);
    if (needed == 0)
        return given;
    std::wstring full(needed, L'\0');
    const DWORD n = ::GetFullPathNameW(given.c_str(), needed, full.data(), nullptr);
    if (n == 0 || n >= needed)
        return given;
    full.resize(n);
    return full;
}

bool pathExists(const std::wstring& path)
{
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

PathKind pathKind(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return PathKind::None;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? PathKind::Folder : PathKind::File;
}

bool readFile(const std::wstring& path, std::string& bytes, std::size_t limit)
{
    const Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return false;
    LARGE_INTEGER size {};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit)
        return false;
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t at = 0;
    while (at < bytes.size()) {
        DWORD got = 0;
        if (!::ReadFile(file.get(), bytes.data() + at, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - at, 1u << 20)), &got, nullptr))
            return false;
        if (got == 0)
            break;
        at += got;
    }
    bytes.resize(at);
    return true;
}

std::wstring systemMessage(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = n > 0 && text ? std::wstring(text, n) : std::wstring();
    if (text)
        ::LocalFree(text);
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ' || message.back() == L'.'
               || message.back() == L'\x3002'))
        message.pop_back();
    return message.empty() ? L"error " + std::to_wstring(code) : message;
}

} // namespace ws::cli
