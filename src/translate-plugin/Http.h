// The one HTTPS POST WinShunTranslate.exe makes, on WinHTTP (the system's
// proxy settings, the system's certificate store).
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace tr {

struct Response {
    DWORD status = 0;
    std::string body;
    DWORD error = 0; // WinHTTP's, when no usable answer came
    std::string host; // for the error text
};

// The body's bytes as they come (a streamed answer), on the calling thread.
using OnData = std::function<void(std::string_view chunk)>;

// POSTs `body` as JSON to `url` with the Authorization header's value.
// `budgetMs` is the time for the WHOLE request (look-up, connect, TLS, send,
// answer): asynchronous I/O waits on one steady-clock deadline. On expiry,
// the calling thread cancels the request and waits for callback teardown;
// no thread closes a handle while another is calling a WinHTTP API on it.
// Timeout is reported as ERROR_WINHTTP_TIMEOUT. An address on this
// computer (127.0.0.1, localhost) is reached directly, never through the
// proxy. `onData` sees each piece of a 2xx answer's body as it comes; the
// whole body is in the Response too.
Response post(const std::string& url, const std::string& body, const std::string& authorization, int budgetMs,
    const std::atomic_bool* canceled = nullptr, const std::map<std::string, std::string>& extraHeaders = {},
    const OnData& onData = {});

// A readable reason for a failed request.
std::string networkError(const Response& response, bool chinese);

// No connection came about (nothing listens there, or it was dropped before
// an answer).
bool notConnected(const Response& response);

} // namespace tr
