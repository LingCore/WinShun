#pragma once

// Small Win32 pieces ws uses everywhere: an owned handle, the environment,
// paths, a whole file read.

#include <windows.h>

#include <string>
#include <string_view>
#include <utility>

namespace ws::cli {

// A kernel handle closed when it goes. Null and INVALID_HANDLE_VALUE are both
// "none".
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept
        : m_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr))
    {
    }
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }
    void reset(HANDLE handle = nullptr) noexcept
    {
        if (m_handle)
            ::CloseHandle(m_handle);
        m_handle = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

private:
    HANDLE m_handle = nullptr;
};

// An environment variable; empty when it is not set.
std::wstring environment(const wchar_t* name);
bool environmentSet(const wchar_t* name); // set, even to nothing

// This program's full path.
std::wstring modulePath();

// `path` made absolute against the current folder, as the shell would.
std::wstring absolutePath(std::wstring_view path);

bool pathExists(const std::wstring& path);
// What is at `path`, as far as ws itself can tell.
enum class PathKind { None, File, Folder };
PathKind pathKind(const std::wstring& path);

// A whole file, at most `limit` bytes; false when it cannot be read or is longer.
bool readFile(const std::wstring& path, std::string& bytes, std::size_t limit);

// The system's text for a Win32 error, one line, in Windows' language.
std::wstring systemMessage(DWORD code);

} // namespace ws::cli
