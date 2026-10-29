#pragma once

#include <QString>

namespace ws::proxy {

// The address of a proxy typed in the settings (网络代理 › 手动, GitHub #84),
// checked and written as WinHTTP wants it. WinHTTP's named proxy is an HTTP
// one (CERN type); a SOCKS address it takes without a word and then goes
// direct, which would only look as if the proxy did nothing, so SOCKS (and
// anything else) is refused here, with the reason.
enum class Problem {
    None,
    Empty,
    NoPort, // "127.0.0.1": the port is needed
    BadAddress, // not a host name or an IP address
    BadPort, // not 1 to 65535
    Socks, // socks4://, socks5://: WinHTTP cannot
    OtherScheme, // https://, ftp://...
    Credentials, // user:password@host: WinHTTP's named proxy has no place for them
};

struct Address {
    Problem problem = Problem::None;
    QString server; // "127.0.0.1:7890", "[::1]:7890": what WinHttpOpen is given; empty with a problem

    bool ok() const noexcept { return problem == Problem::None; }
};

// "127.0.0.1:7890", "http://127.0.0.1:7890", "localhost:7890/", "[::1]:8080",
// with spaces round it; a host name is lower-cased.
Address parse(const QString& text);

} // namespace ws::proxy
