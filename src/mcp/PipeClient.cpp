#include "PipeClient.h"

#include <sddl.h>

#include <algorithm>
#include <vector>

namespace ws::pipe {

std::wstring userSid()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    wchar_t* text = nullptr;
    const bool ok = size != 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)
        && ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    ::CloseHandle(token);
    if (!ok)
        return {};
    std::wstring sid = text;
    ::LocalFree(text);
    return sid;
}

std::wstring defaultName()
{
    return L"WinShun.Mcp." + userSid();
}

std::wstring fullPath(std::wstring_view name)
{
    return L"\\\\.\\pipe\\" + std::wstring(name);
}

LineClient::LineClient()
    : m_readEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_writeEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

LineClient::~LineClient()
{
    close();
    ::CloseHandle(m_readEvent);
    ::CloseHandle(m_writeEvent);
}

bool LineClient::connect(std::wstring_view name, DWORD timeoutMs, DWORD* error)
{
    close();
    const std::wstring path = fullPath(name);
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        // Overlapped: a read waiting on one thread must not hold up a write
        // on another (synchronous operations on one handle take turns).
        // SECURITY_IMPERSONATION: the server may act as us on this computer
        // (it checks what we may see with our token), not beyond it.
        const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
            ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
            m_handle = handle;
            m_buffer.clear();
            m_scanned = 0;
            return true;
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_PIPE_BUSY) {
            if (error)
                *error = code;
            return false;
        }
        // Every instance is taken: wait for one to free up.
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline || !::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(deadline - now))) {
            if (error)
                *error = ERROR_PIPE_BUSY;
            return false;
        }
    }
}

bool LineClient::connected() const noexcept
{
    return m_handle != INVALID_HANDLE_VALUE;
}

void LineClient::close()
{
    if (m_handle == INVALID_HANDLE_VALUE)
        return;
    const HANDLE handle = m_handle;
    m_handle = INVALID_HANDLE_VALUE;
    ::CancelIoEx(handle, nullptr); // a read waiting on another thread returns
    ::CloseHandle(handle);
}

void LineClient::interrupt() noexcept
{
    if (const HANDLE handle = m_handle; handle != INVALID_HANDLE_VALUE)
        ::CancelIoEx(handle, nullptr);
}

bool LineClient::write(std::string_view line)
{
    const std::lock_guard lock(m_writeMutex);
    const HANDLE handle = m_handle;
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const char* p = line.data();
    std::size_t n = line.size();
    while (n > 0) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = m_writeEvent;
        ::ResetEvent(m_writeEvent);
        DWORD put = 0;
        if (!::WriteFile(handle, p, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 20)), &put, &overlapped)) {
            if (::GetLastError() != ERROR_IO_PENDING || !::GetOverlappedResult(handle, &overlapped, &put, TRUE))
                return false;
        }
        if (put == 0)
            return false;
        p += put;
        n -= put;
    }
    return true;
}

bool LineClient::readLine(std::string& line)
{
    for (;;) {
        // Only what came since the last look is searched: a long line (a
        // large result) arrives in many reads.
        if (const std::size_t at = m_buffer.find('\n', m_scanned); at != std::string::npos) {
            line.assign(m_buffer, 0, at);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_buffer.erase(0, at + 1);
            m_scanned = 0;
            return true;
        }
        m_scanned = m_buffer.size();
        const HANDLE handle = m_handle;
        if (handle == INVALID_HANDLE_VALUE || m_buffer.size() > kMaxLineBytes)
            return false;
        char chunk[64 * 1024];
        OVERLAPPED overlapped {};
        overlapped.hEvent = m_readEvent;
        ::ResetEvent(m_readEvent);
        DWORD got = 0;
        if (!::ReadFile(handle, chunk, sizeof chunk, &got, &overlapped)) {
            if (::GetLastError() != ERROR_IO_PENDING || !::GetOverlappedResult(handle, &overlapped, &got, TRUE))
                return false;
        }
        if (got == 0)
            return false;
        m_buffer.append(chunk, got);
    }
}

} // namespace ws::pipe
