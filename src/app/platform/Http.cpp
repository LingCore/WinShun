#include "Http.h"

#include <QCoreApplication>

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::http {

namespace {

struct InternetHandle {
    void operator()(HINTERNET h) const { ::WinHttpCloseHandle(h); }
};
using Handle = std::unique_ptr<void, InternetHandle>;

// The proxy setting. WinHTTP takes it when a session is opened and nowhere
// later (the option is refused on a request: ERROR_WINHTTP_INCORRECT_HANDLE_STATE),
// so a session is of one choice: each Own session is opened with the current
// one, and the shared session, which lives for the whole run and may be in
// use by other threads at this very moment (a handle in use is never closed),
// is opened again after a change, the old one left to its calls and its idle
// connections, which WinHTTP lets go of itself. One small handle for each
// change of the setting.
struct ProxyChoice {
    Proxy mode = Proxy::System;
    std::wstring server;
    unsigned generation = 0; // up by one for each setProxy()
};
std::mutex g_proxyLock;
ProxyChoice g_proxy;

ProxyChoice currentProxy()
{
    const std::lock_guard lock(g_proxyLock);
    return g_proxy;
}

HINTERNET openSession(const ProxyChoice& choice)
{
    const std::wstring agent = (QCoreApplication::applicationName() + u'/' + QCoreApplication::applicationVersion())
                                   .toStdWString();
    switch (choice.mode) {
    case Proxy::None:
        return ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    case Proxy::Named:
        // "<local>": the names of the machine's own network (no dot in them) go direct, as Windows' own do.
        return ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NAMED_PROXY, choice.server.c_str(), L"<local>", 0);
    case Proxy::System:
        break;
    }
    // Automatic proxy: the system's settings, including PAC scripts (Windows 8.1+).
    return ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
}

// Never closed: a call may still be using it while Win顺 ends. WinHTTP's
// handles may be used from several threads at once.
HINTERNET sharedSession()
{
    static std::mutex lock;
    static HINTERNET session = nullptr;
    static unsigned generation = ~0u; // of the setting `session` was opened for
    const std::lock_guard guard(lock);
    const ProxyChoice choice = currentProxy();
    if (!session || generation != choice.generation) {
        const HINTERNET opened = openSession(choice);
        if (opened) // the name lookup's timeout is the session's alone (WinHTTP's own: none); the rest are per request
            ::WinHttpSetTimeouts(opened, 2000, 2000, 2000, 2000);
        session = opened;
        generation = choice.generation;
    }
    return session;
}

// get() and download(): with a `sink` the body goes there, and is read
// only for an answer of 2xx.
Response perform(const QString& url, const QList<std::pair<QString, QString>>& headers, int timeoutMs,
    qint64 maxBytes, Session sessionKind, const Progress& progress, const Sink& sink)
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

    Handle own;
    HINTERNET session = nullptr;
    if (sessionKind == Session::Shared) {
        session = sharedSession();
    } else {
        own.reset(openSession(currentProxy()));
        session = own.get();
        if (session) // the name lookup's timeout is the session's alone
            ::WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    }
    if (!session)
        return fail();
    const Handle connection(::WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (!connection)
        return fail();
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const Handle request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request)
        return fail();
    ::WinHttpSetTimeouts(request.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    std::wstring headerText;
    for (const auto& [name, value] : headers)
        headerText += (name + u": "_s + value + u"\r\n"_s).toStdWString();
    if (!::WinHttpSendRequest(request.get(), headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
            static_cast<DWORD>(headerText.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !::WinHttpReceiveResponse(request.get(), nullptr))
        return fail();

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return fail();
    DWORD urlBytes = 0;
    ::WinHttpQueryOption(request.get(), WINHTTP_OPTION_URL, nullptr, &urlBytes);
    if (urlBytes > 0) {
        std::wstring finalUrl(urlBytes / sizeof(wchar_t), L'\0');
        if (::WinHttpQueryOption(request.get(), WINHTTP_OPTION_URL, finalUrl.data(), &urlBytes))
            response.finalUrl = QString::fromWCharArray(finalUrl.c_str());
    }
    if (response.finalUrl.isEmpty())
        response.finalUrl = url;
    DWORD typeBytes = 0;
    ::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &typeBytes,
        WINHTTP_NO_HEADER_INDEX);
    if (typeBytes > 0) {
        std::wstring type(typeBytes / sizeof(wchar_t), L'\0');
        if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, type.data(),
                &typeBytes, WINHTTP_NO_HEADER_INDEX))
            response.contentType = QString::fromWCharArray(type.c_str());
    }
    wchar_t lengthText[32] = {};
    DWORD lengthBytes = sizeof(lengthText);
    if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
            &lengthBytes, WINHTTP_NO_HEADER_INDEX)) {
        bool ok = false;
        const qint64 length = QString::fromWCharArray(lengthText).trimmed().toLongLong(&ok);
        if (ok && length >= 0)
            response.contentLength = length;
    }
    if (sink) {
        response.status = static_cast<int>(status);
        if (status < 200 || status >= 300)
            return response;
        if (maxBytes > 0 && response.contentLength > maxBytes) {
            response.error = ERROR_FILE_TOO_LARGE;
            return response;
        }
        std::vector<char> buffer(256 * 1024);
        qint64 received = 0;
        for (;;) {
            DWORD read = 0;
            if (!::WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                return fail();
            if (read == 0)
                break;
            received += read;
            if (maxBytes > 0 && received > maxBytes) {
                response.error = ERROR_FILE_TOO_LARGE;
                return response;
            }
            if (!sink(buffer.data(), read)) {
                response.error = ERROR_CANCELLED;
                return response;
            }
        }
        return response;
    }
    while (maxBytes <= 0 || response.body.size() < maxBytes) {
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
        if (progress && !progress(response.body.size())) {
            response.body.clear();
            response.error = ERROR_CANCELLED;
            return response;
        }
    }
    response.status = static_cast<int>(status);
    return response;
}

} // namespace

Response get(const QString& url, const QList<std::pair<QString, QString>>& headers, int timeoutMs, qsizetype maxBytes,
    Session sessionKind, const Progress& progress)
{
    return perform(url, headers, timeoutMs, maxBytes, sessionKind, progress, {});
}

Response download(const QString& url, const QList<std::pair<QString, QString>>& headers, int timeoutMs,
    qint64 maxBytes, const Sink& sink)
{
    return perform(url, headers, timeoutMs, maxBytes, Session::Own, {}, sink);
}

QString proxyServer()
{
    const ProxyChoice choice = currentProxy();
    return choice.mode == Proxy::Named ? QString::fromStdWString(choice.server) : QString();
}

void setProxy(Proxy mode, const QString& server)
{
    const std::lock_guard lock(g_proxyLock);
    g_proxy.mode = mode == Proxy::Named && server.isEmpty() ? Proxy::System : mode;
    g_proxy.server = server.toStdWString();
    ++g_proxy.generation;
}

} // namespace ws::http
