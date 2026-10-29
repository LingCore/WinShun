// The one HTTPS POST WinShunTranslate.exe makes, on WinHTTP (the system's
// proxy settings, the system's certificate store).
#pragma once

#include <windows.h>

#include <string>

namespace tr {

struct Response {
    DWORD status = 0;
    std::string body;
    DWORD error = 0; // WinHTTP's, when no usable answer came
    std::string host; // for the error text
};

// POSTs `body` as JSON to `url` with the Authorization header's value.
// `budgetMs` is the time for the WHOLE request (look-up, connect, TLS, send,
// answer): a watchdog closes the request handle when it is up, so the call
// returns by then however the pieces add up. That is then error
// ERROR_WINHTTP_TIMEOUT.
Response post(const std::string& url, const std::string& body, const std::string& authorization, int budgetMs);

// A readable reason for a failed request.
std::string networkError(const Response& response, bool chinese);

} // namespace tr
