#include "Http.h"

#include "winshun.h"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace tr {

namespace {

constexpr std::size_t kMaxBody = 8u << 20; // no translation is this big

HINTERNET session()
{
    // The system's proxy settings (WPAD and the user's), as a browser would.
    static const HINTERNET s = ::WinHttpOpen(L"WinShun-translate/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    return s; // kept until the process ends
}

// One request, one absolute deadline. Only the calling thread uses or closes
// the handles; WinHTTP's callbacks report completion and never start I/O.
// Buffers and callback state live until HANDLE_CLOSING, including on timeout.
class Request {
public:
    ~Request()
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
        auto& request = *reinterpret_cast<Request*>(context);
        const std::lock_guard lock(request.m_mutex);
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            request.m_closed = true;
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (info && size >= sizeof(WINHTTP_ASYNC_RESULT))
                request.m_error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            else
                request.m_error = ERROR_WINHTTP_INTERNAL_ERROR;
            break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            request.bytes = info && size >= sizeof(DWORD) ? *static_cast<DWORD*>(info) : 0;
            request.m_completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            request.bytes = size;
            request.m_completed = status;
            break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            request.m_completed = status;
            break;
        default:
            return;
        }
        request.m_cv.notify_all(); // under the lock: no state touched after unlocking
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
        if (!m_cv.wait_until(lock, deadline, [this, expected] { return m_error || m_completed == expected; }))
            return ERROR_WINHTTP_TIMEOUT;
        return m_error;
    }

    HINTERNET connection = nullptr;
    HINTERNET handle = nullptr;
    bool callbacks = false;
    DWORD bytes = 0;
    std::array<char, 64 * 1024> buffer;

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    DWORD m_completed = 0;
    DWORD m_error = 0;
    bool m_closed = false;
};

} // namespace

Response post(const std::string& url, const std::string& body, const std::string& authorization, int budgetMs)
{
    Response r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, budgetMs));
    const std::wstring wideUrl = winshun::wide(url);
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        r.error = ERROR_WINHTTP_INVALID_URL;
        r.host = url;
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
    if (budgetMs <= 0)
        return failed(ERROR_WINHTTP_TIMEOUT);
    Request request;
    request.connection = ::WinHttpConnect(session(), hostName.c_str(), parts.nPort, 0);
    if (!request.connection)
        return failed(::GetLastError());
    std::wstring objectPath(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (objectPath.empty())
        objectPath = L"/";
    if (parts.dwExtraInfoLength)
        objectPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    request.handle = ::WinHttpOpenRequest(request.connection, L"POST", objectPath.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.handle)
        return failed(::GetLastError());
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&request);
    if (!::WinHttpSetOption(request.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context))
        return failed(::GetLastError());
    if (::WinHttpSetStatusCallback(request.handle, &Request::status,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return failed(::GetLastError());
    request.callbacks = true;
    ::WinHttpSetTimeouts(request.handle, budgetMs, budgetMs, budgetMs, budgetMs);
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!authorization.empty())
        headers += L"Authorization: " + winshun::wide(authorization) + L"\r\n";
    DWORD error = request.wait(::WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(headers.size()),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), context),
        WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline);
    if (error)
        return failed(error);
    request.prepare();
    error = request.wait(::WinHttpReceiveResponse(request.handle, nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline);
    if (error)
        return failed(error);
    DWORD size = sizeof r.status;
    if (!::WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &r.status, &size, WINHTTP_NO_HEADER_INDEX))
        return failed(::GetLastError());
    for (;;) {
        request.prepare();
        error = request.wait(::WinHttpQueryDataAvailable(request.handle, nullptr), WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, deadline);
        if (error)
            return failed(error);
        if (request.bytes == 0)
            break;
        if (request.bytes > kMaxBody - r.body.size())
            return failed(ERROR_FILE_TOO_LARGE);
        const DWORD read = std::min(request.bytes, static_cast<DWORD>(request.buffer.size()));
        request.prepare();
        error = request.wait(::WinHttpReadData(request.handle, request.buffer.data(), read, nullptr),
            WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline);
        if (error)
            return failed(error);
        r.body.append(request.buffer.data(), request.bytes);
        if (request.bytes == 0)
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
        return chinese ? "找不到服务器 " + r.host + "：检查接口地址和网络" : "Server not found (" + r.host + "): check the endpoint and the network";
    case ERROR_WINHTTP_TIMEOUT:
        return chinese ? r.host + " 没有及时回答" : r.host + " did not answer in time";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return chinese ? "HTTPS 证书有问题" : "The HTTPS certificate is not valid";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return chinese ? "接口地址不对：" + r.host : "Not a valid endpoint: " + r.host;
    default:
        return (chinese ? "网络错误 " : "Network error ") + std::to_string(r.error);
    }
}

} // namespace tr
