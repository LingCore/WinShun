#include "Http.h"

#include <QCoreApplication>

#include <windows.h>
#include <winhttp.h>

#include <memory>

using namespace Qt::StringLiterals;

namespace ws::http {

namespace {

struct InternetHandle {
    void operator()(HINTERNET h) const { ::WinHttpCloseHandle(h); }
};
using Handle = std::unique_ptr<void, InternetHandle>;

} // namespace

Response get(const QString& url, const QList<std::pair<QString, QString>>& headers, int timeoutMs)
{
    Response response;
    const auto fail = [&response] {
        response.error = ::GetLastError();
        return response;
    };

    const std::wstring wurl = url.toStdWString();
    URL_COMPONENTS parts {sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
        return fail();
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength)
        + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const std::wstring agent = (QCoreApplication::applicationName() + u'/' + QCoreApplication::applicationVersion())
                                   .toStdWString();
    // Automatic proxy: the system's settings, including PAC scripts (Windows 8.1+).
    const Handle session(::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return fail();
    ::WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    const Handle connection(::WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection)
        return fail();
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const Handle request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request)
        return fail();
    std::wstring headerText;
    for (const auto& [name, value] : headers)
        headerText += (name + u": "_qs + value + u"\r\n"_qs).toStdWString();
    if (!::WinHttpSendRequest(request.get(), headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
            static_cast<DWORD>(headerText.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !::WinHttpReceiveResponse(request.get(), nullptr))
        return fail();

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return fail();
    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.get(), &available))
            return fail();
        if (available == 0)
            break;
        const qsizetype at = response.body.size();
        response.body.resize(at + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request.get(), response.body.data() + at, available, &read))
            return fail();
        response.body.resize(at + read);
    }
    response.status = static_cast<int>(status);
    return response;
}

} // namespace ws::http
