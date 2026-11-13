// The HTTP requests WinShunCloudBackup.exe makes (WebDAV's PROPFIND, MKCOL,
// PUT, GET, DELETE; GitHub's REST API), on WinHTTP: the system's proxy
// settings, the system's certificate store.
#pragma once

#include <windows.h>

#include <atomic>
#include <map>
#include <string>

namespace cb {

struct Request {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    // The time for the WHOLE request (look-up, connect, TLS, send, answer):
    // asynchronous I/O waits on one steady-clock deadline. On expiry, or once
    // `canceled` is set, the calling thread cancels the request and waits for
    // callback teardown; no thread closes a handle while another is calling a
    // WinHTTP API on it.
    int budgetMs = 30000;
    std::size_t maxBody = 4u << 20; // an answer longer than this fails (ERROR_FILE_TOO_LARGE)
    const std::atomic_bool* canceled = nullptr;
};

struct Response {
    DWORD status = 0;
    std::string body;
    DWORD error = 0; // WinHTTP's, when no usable answer came
    std::string host; // for the error text
};

Response send(const Request& request);

// A readable reason for a failed request.
std::string networkError(const Response& response, bool chinese);

} // namespace cb
