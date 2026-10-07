#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <utility>

namespace qf::win32 {

// Move-only owner of a kernel HANDLE.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept
        : m_handle(h)
    {
    }
    ~UniqueHandle() { reset(); }

    UniqueHandle(UniqueHandle&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr))
    {
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept { return m_handle; }
    bool valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h = nullptr) noexcept
    {
        if (valid())
            ::CloseHandle(m_handle);
        m_handle = h;
    }

private:
    HANDLE m_handle = nullptr;
};

// Owner of a FindFirstFile search handle.
class UniqueFind {
public:
    explicit UniqueFind(HANDLE h) noexcept
        : m_handle(h)
    {
    }
    ~UniqueFind()
    {
        if (valid())
            ::FindClose(m_handle);
    }
    UniqueFind(const UniqueFind&) = delete;
    UniqueFind& operator=(const UniqueFind&) = delete;

    HANDLE get() const noexcept { return m_handle; }
    bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle;
};

// Owner of an open registry key.
class UniqueKey {
public:
    UniqueKey() noexcept = default;
    ~UniqueKey()
    {
        if (m_key)
            ::RegCloseKey(m_key);
    }
    UniqueKey(const UniqueKey&) = delete;
    UniqueKey& operator=(const UniqueKey&) = delete;

    HKEY get() const noexcept { return m_key; }
    HKEY* out() noexcept { return &m_key; }

private:
    HKEY m_key = nullptr;
};

// Ordinal, case-insensitive comparison: the same rule NTFS uses for names.
inline bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    if (a.empty())
        return true;
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
        == CSTR_EQUAL;
}

// "C:\dir" -> "\\?\C:\dir" so paths longer than MAX_PATH keep working.
std::wstring longPath(std::wstring_view path);

// Expands %VAR% references, e.g. "%WINDIR%\WinSxS".
std::wstring expandEnvironment(std::wstring_view text);

} // namespace qf::win32
