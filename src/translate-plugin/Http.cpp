#include "Http.h"

#include "winshun.h"

#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace tr {

namespace {

constexpr std::size_t kMaxBody = 8u << 20; // no translation is this big

HINTERNET session()
{
    // The system's proxy settings (WPAD and the user's), as a browser would.
    static const HINTERNET s = ::WinHttpOpen(L"WinShun-translate/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return s; // kept until the process ends
}

// The two handles of one request, closed exactly once: by the thread that is
// done with them, or by the watchdog when the time is up. Closing a handle
// that another thread waits on makes that wait fail at once, which is the
// point.
class Handles {
public:
    ~Handles() { close(); }
    // False when the watchdog already fired (nothing is kept then).
    bool adopt(HINTERNET connection, HINTERNET request)
    {
        const std::lock_guard lock(m_mutex);
        if (m_fired) {
            ::WinHttpCloseHandle(request);
            ::WinHttpCloseHandle(connection);
            return false;
        }
        m_connection = connection;
        m_request = request;
        return true;
    }
    HINTERNET request() const { return m_request; } // set before the calls that use it, never changed
    void close()
    {
        const std::lock_guard lock(m_mutex);
        closeLocked();
    }
    void fire()
    {
        const std::lock_guard lock(m_mutex);
        m_fired = true;
        closeLocked();
    }
    bool fired() const
    {
        const std::lock_guard lock(m_mutex);
        return m_fired;
    }

private:
    void closeLocked()
    {
        if (m_request)
            ::WinHttpCloseHandle(m_request);
        if (m_connection)
            ::WinHttpCloseHandle(m_connection);
        m_request = nullptr;
        m_connection = nullptr;
    }
    mutable std::mutex m_mutex;
    HINTERNET m_connection = nullptr;
    HINTERNET m_request = nullptr;
    bool m_fired = false;
};

// Runs fire() on `handles` when the time is up, unless stopped first.
class Watchdog {
public:
    Watchdog(Handles& handles, int budgetMs)
        : m_thread([this, &handles, budgetMs] {
              std::unique_lock lock(m_mutex);
              if (!m_cv.wait_for(lock, std::chrono::milliseconds(budgetMs), [this] { return m_stop; }))
                  handles.fire();
          })
    {
    }
    ~Watchdog()
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::thread m_thread; // last: it starts using the members above
};

} // namespace

Response post(const std::string& url, const std::string& body, const std::string& authorization, int budgetMs)
{
    Response r;
    const std::wstring wideUrl = winshun::wide(url);
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] {};
    wchar_t path[2048] {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        r.error = ERROR_WINHTTP_INVALID_URL;
        r.host = url;
        return r;
    }
    const std::wstring hostName(host, parts.dwHostNameLength);
    r.host = winshun::utf8(hostName);
    if (!session()) {
        r.error = ::GetLastError();
        return r;
    }

    Handles handles;
    Watchdog watchdog(handles, budgetMs);
    const auto failed = [&](DWORD error) {
        // The watchdog closing the handle shows up as "operation cancelled"
        // or "invalid handle"; the cause is the time.
        r.error = handles.fired() ? ERROR_WINHTTP_TIMEOUT : error;
        return r;
    };
    HINTERNET connection = ::WinHttpConnect(session(), hostName.c_str(), parts.nPort, 0);
    if (!connection)
        return failed(::GetLastError());
    const std::wstring objectPath(path, parts.dwUrlPathLength);
    HINTERNET request = ::WinHttpOpenRequest(connection, L"POST", objectPath.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        const DWORD error = ::GetLastError();
        ::WinHttpCloseHandle(connection);
        return failed(error);
    }
    if (!handles.adopt(connection, request))
        return failed(ERROR_WINHTTP_TIMEOUT);

    // Each phase also has its own limit, as a second line behind the watchdog.
    ::WinHttpSetTimeouts(request, budgetMs, budgetMs, budgetMs, budgetMs);
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!authorization.empty())
        headers += L"Authorization: " + winshun::wide(authorization) + L"\r\n";
    if (!::WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
        || !::WinHttpReceiveResponse(request, nullptr))
        return failed(::GetLastError());
    DWORD size = sizeof r.status;
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status,
        &size, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request, &available))
            return failed(::GetLastError());
        if (available == 0)
            break;
        const std::size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request, r.body.data() + at, available, &read))
            return failed(::GetLastError());
        r.body.resize(at + read);
        if (r.body.size() > kMaxBody)
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
