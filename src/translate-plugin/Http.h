// The one HTTPS POST WinShunTranslate.exe makes, on WinHTTP (the system's
// proxy settings, the system's certificate store).
#pragma once

#include <windows.h>

#include <string>
#include <atomic>
#include <map>

namespace tr {

struct Response {
    DWORD status = 0;
    std::string body;
    DWORD error = 0; // WinHTTP's, when no usable answer came
    std::string host; // for the error text
};

// POSTs `body` as JSON to `url` with the Authorization header's value.
// `budgetMs` is the time for the WHOLE request (look-up, connect, TLS, send,
// answer): asynchronous I/O waits on one steady-clock deadline. On expiry,
// the calling thread cancels the request and waits for callback teardown;
// no thread closes a handle while another is calling a WinHTTP API on it.
// Timeout is reported as ERROR_WINHTTP_TIMEOUT.
Response post(const std::string& url, const std::string& body, const std::string& authorization, int budgetMs,
    const std::atomic_bool* canceled = nullptr, const std::map<std::string, std::string>& extraHeaders = {});

// A readable reason for a failed request.
std::string networkError(const Response& response, bool chinese);

} // namespace tr
