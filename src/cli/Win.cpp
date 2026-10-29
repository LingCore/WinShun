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

std::wstring extendedPath(const std::wstring& path)
{
    // Only a path that long needs it (and the others keep every rule of the
    // ordinary form, such as the trailing dots and spaces Windows drops).
    if (path.size() < 240 || path.starts_with(L"\\\\?\\"))
        return path;
    if (path.starts_with(L"\\\\")) // \\server\share\x
        return L"\\\\?\\UNC\\" + path.substr(2);
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\')
        return L"\\\\?\\" + path;
    return path;
}

namespace {

// `path` with 8.3 short names (PROGRA~1, ADMINI~1) in their long form, as the
// index knows them. Only a path with a '~' can have one, and only a path
// that exists can be asked: the call reaches out to the disk, a share included.
std::wstring withLongNames(std::wstring path)
{
    if (path.find(L'~') == std::wstring::npos)
        return path;
    const std::wstring asked = extendedPath(path);
    std::wstring longForm(asked.size() + 512, L'\0');
    DWORD n = ::GetLongPathNameW(asked.c_str(), longForm.data(), static_cast<DWORD>(longForm.size()));
    if (n >= longForm.size()) { // longer than guessed: n is the size needed
        longForm.resize(n);
        n = ::GetLongPathNameW(asked.c_str(), longForm.data(), static_cast<DWORD>(longForm.size()));
    }
    if (n == 0 || n >= longForm.size())
        return path;
    longForm.resize(n);
    // The prefix is ours when the path did not have it.
    if (asked.size() != path.size()) {
        if (longForm.starts_with(L"\\\\?\\UNC\\"))
            longForm = L"\\" + longForm.substr(7);
        else if (longForm.starts_with(L"\\\\?\\"))
            longForm.erase(0, 4);
    }
    return longForm;
}

} // namespace

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
    return withLongNames(std::move(full));
}

bool pathExists(const std::wstring& path)
{
    return ::GetFileAttributesW(extendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

PathKind pathKind(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(extendedPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return PathKind::None;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? PathKind::Folder : PathKind::File;
}

bool readFile(const std::wstring& path, std::string& bytes, std::size_t limit)
{
    const Handle file(::CreateFileW(extendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
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
