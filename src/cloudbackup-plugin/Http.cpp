#include "Http.h"

#include "winshun.h"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace cb {

namespace {

HINTERNET session()
{
    // The system's proxy settings (WPAD and the user's), as a browser would.
    static const HINTERNET s = ::WinHttpOpen(L"WinShun-cloud-backup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    return s; // kept until the process ends
}

// One request, one absolute deadline. Only the calling thread uses or closes
// the handles; WinHTTP's callbacks report completion and never start I/O.
// Buffers and callback state live until HANDLE_CLOSING, including on timeout.
class Call {
public:
    explicit Call(const std::atomic_bool* canceled) : m_canceled(canceled) {}

    ~Call()
    {
        if (handle) {
            ::WinHttpCloseHandle(handle); // cancels pending asynchronous I/O
            if (callbacks) {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return m_closed; });
            }
        }
        if (connection)
            ::WinHttpCloseHandle(connection);
    }

    static void CALLBACK status(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size)
    {
        if (!context)
            return;
        auto& call = *reinterpret_cast<Call*>(context);
        const std::lock_guard lock(call.m_mutex);
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            call.m_closed = true;
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (info && size >= sizeof(WINHTTP_ASYNC_RESULT))
                call.m_error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            else
                call.m_error = ERROR_WINHTTP_INTERNAL_ERROR;
            break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            call.bytes = info && size >= sizeof(DWORD) ? *static_cast<DWORD*>(info) : 0;
            call.m_completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            call.bytes = size;
            call.m_completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            call.m_completed = status;
            break;
        default:
            return;
        }
        call.m_cv.notify_all(); // under the lock: no state touched after unlocking
    }

    void prepare()
    {
        const std::lock_guard lock(m_mutex);
        m_completed = 0;
        bytes = 0;
    }

    DWORD wait(BOOL started, DWORD expected, std::chrono::steady_clock::time_point deadline)
    {
        if (!started) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING)
                return error;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return ERROR_WINHTTP_TIMEOUT;
        std::unique_lock lock(m_mutex);
        while (!m_error && m_completed != expected) {
            if (m_canceled && m_canceled->load())
                return ERROR_CANCELLED;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return ERROR_WINHTTP_TIMEOUT;
            const auto wake = m_canceled ? std::min(deadline, now + std::chrono::milliseconds(25)) : deadline;
            m_cv.wait_until(lock, wake);
        }
        return m_error;
    }

    HINTERNET connection = nullptr;
    HINTERNET handle = nullptr;
    bool callbacks = false;
    DWORD bytes = 0;
    std::array<char, 64 * 1024> buffer;

private:
    const std::atomic_bool* m_canceled = nullptr;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    DWORD m_completed = 0;
    DWORD m_error = 0;
    bool m_closed = false;
};

} // namespace

Response send(const Request& q)
{
    Response r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, q.budgetMs));
    const std::wstring wideUrl = winshun::wide(q.url);
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        r.error = ERROR_WINHTTP_INVALID_URL;
        r.host = q.url;
        return r;
    }
    const std::wstring hostName(parts.lpszHostName, parts.dwHostNameLength);
    r.host = winshun::utf8(hostName);
    if (!session()) {
        r.error = ::GetLastError();
        return r;
    }

    const auto failed = [&](DWORD error) {
        r.error = error;
        return r;
    };
    if (q.budgetMs <= 0)
        return failed(ERROR_WINHTTP_TIMEOUT);
    if (q.canceled && q.canceled->load())
        return failed(ERROR_CANCELLED);
    if (q.body.size() > 0x7fffffffu)
        return failed(ERROR_FILE_TOO_LARGE);
    Call call(q.canceled);
    call.connection = ::WinHttpConnect(session(), hostName.c_str(), parts.nPort, 0);
    if (!call.connection)
        return failed(::GetLastError());
    std::wstring objectPath(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (objectPath.empty())
        objectPath = L"/";
    if (parts.dwExtraInfoLength)
        objectPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    call.handle = ::WinHttpOpenRequest(call.connection, winshun::wide(q.method).c_str(), objectPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!call.handle)
        return failed(::GetLastError());
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&call);
    if (!::WinHttpSetOption(call.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context))
        return failed(::GetLastError());
    if (::WinHttpSetStatusCallback(call.handle, &Call::status,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return failed(::GetLastError());
    call.callbacks = true;
    ::WinHttpSetTimeouts(call.handle, q.budgetMs, q.budgetMs, q.budgetMs, q.budgetMs);
    std::wstring headers;
    for (const auto& [name, value] : q.headers) {
        if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
            return failed(ERROR_INVALID_PARAMETER);
        headers += winshun::wide(name) + L": " + winshun::wide(value) + L"\r\n";
    }
    const DWORD length = static_cast<DWORD>(q.body.size());
    DWORD error = call.wait(::WinHttpSendRequest(call.handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        static_cast<DWORD>(headers.size()), length ? const_cast<char*>(q.body.data()) : WINHTTP_NO_REQUEST_DATA, length, length, context),
        WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline);
    if (error)
        return failed(error);
    call.prepare();
    error = call.wait(::WinHttpReceiveResponse(call.handle, nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline);
    if (error)
        return failed(error);
    DWORD size = sizeof r.status;
    if (!::WinHttpQueryHeaders(call.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &r.status, &size, WINHTTP_NO_HEADER_INDEX))
        return failed(::GetLastError());
    for (;;) {
        call.prepare();
        error = call.wait(::WinHttpQueryDataAvailable(call.handle, nullptr), WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, deadline);
        if (error)
            return failed(error);
        if (call.bytes == 0)
            break;
        if (r.body.size() > q.maxBody || call.bytes > q.maxBody - r.body.size())
            return failed(ERROR_FILE_TOO_LARGE);
        const DWORD read = std::min(call.bytes, static_cast<DWORD>(call.buffer.size()));
        call.prepare();
        error = call.wait(::WinHttpReadData(call.handle, call.buffer.data(), read, nullptr),
            WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline);
        if (error)
            return failed(error);
        r.body.append(call.buffer.data(), call.bytes);
        if (call.bytes == 0)
            break;
    }
    return r;
}

std::string networkError(const Response& r, bool chinese)
{
    switch (r.error) {
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return chinese ? "连不上 " + r.host + "：检查网络或代理" : "Cannot reach " + r.host + ": check the network or proxy";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return chinese ? "找不到服务器 " + r.host + "：检查地址和网络" : "Server not found (" + r.host + "): check the address and the network";
    case ERROR_WINHTTP_TIMEOUT:
        return chinese ? r.host + " 没有及时回答" : r.host + " did not answer in time";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return chinese ? "HTTPS 证书有问题" : "The HTTPS certificate is not valid";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return chinese ? "地址不对：" + r.host : "Not a valid address: " + r.host;
    case ERROR_FILE_TOO_LARGE:
        return chinese ? "文件太大" : "The file is too large";
    case ERROR_CANCELLED:
        return chinese ? "已取消" : "Canceled";
    default:
        return (chinese ? "网络错误 " : "Network error ") + std::to_string(r.error);
    }
}

} // namespace cb
