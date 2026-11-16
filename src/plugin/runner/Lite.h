#pragma once

// Small Win32 helpers for the programs of the plugin system that carry no
// Qt: the runner (WinShunPluginHost.exe) and the test plugin (plugin_fake).
// Header only.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

namespace ws::plugin::lite {

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept
        : h(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : h(std::exchange(other.h, nullptr))
    {
    }
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            h = std::exchange(other.h, nullptr);
        }
        return *this;
    }
    bool valid() const { return h != nullptr; }
    HANDLE get() const { return h; }
    HANDLE release() { return std::exchange(h, nullptr); }
    void reset(HANDLE handle = nullptr)
    {
        if (h)
            ::CloseHandle(h);
        h = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }
};

inline std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring wide(const std::string& s, UINT codePage = CP_UTF8)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Whether the bytes are well-formed UTF-8.
inline bool validUtf8(const std::string& s)
{
    return s.empty() || ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}

inline std::string format(const char* fmt, ...)
{
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}

inline std::wstring ownPath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

inline std::wstring environmentVariable(const wchar_t* name)
{
    wchar_t buffer[8192];
    const DWORD n = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? std::wstring(buffer, n) : std::wstring();
}

inline long long microsecondsSince(const LARGE_INTEGER& start)
{
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return (now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart;
}

inline LARGE_INTEGER tick()
{
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return t;
}

// Whole lines on a synchronous handle (a pipe end or the standard input):
// one line without its newline, or nullopt at the end.
class LineReader {
public:
    explicit LineReader(HANDLE handle)
        : m_handle(handle)
    {
    }
    // False at the end; `line` is what was read until then (may be partial).
    bool next(std::string& line)
    {
        for (;;) {
            if (const std::size_t nl = m_buffer.find('\n'); nl != std::string::npos) {
                line.assign(m_buffer, 0, nl);
                m_buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            if (m_eof) {
                line = std::move(m_buffer);
                m_buffer.clear();
                return false;
            }
            char chunk[16 * 1024];
            DWORD got = 0;
            if (!::ReadFile(m_handle, chunk, sizeof chunk, &got, nullptr) || got == 0) {
                m_eof = true;
                continue;
            }
            m_buffer.append(chunk, got);
            if (m_buffer.size() > kMaxLine) {
                m_eof = true;
                m_overlong = true;
            }
        }
    }
    bool overlong() const { return m_overlong; }
    static constexpr std::size_t kMaxLine = 16u << 20; // pipe::kMaxLineBytes

private:
    HANDLE m_handle;
    std::string m_buffer;
    bool m_eof = false;
    bool m_overlong = false;
};

inline bool writeAll(HANDLE handle, const std::string& text)
{
    const char* p = text.data();
    std::size_t n = text.size();
    while (n > 0) {
        DWORD moved = 0;
        if (!::WriteFile(handle, p, static_cast<DWORD>(n > (1u << 20) ? (1u << 20) : n), &moved, nullptr) || moved == 0)
            return false;
        p += moved;
        n -= moved;
    }
    return true;
}

} // namespace ws::plugin::lite
