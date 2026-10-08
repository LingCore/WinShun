#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <utility>

namespace ws::http {

struct Response {
    int status = 0; // HTTP status; 0 when no answer came
    QByteArray body;
    unsigned long error = 0; // Win32 / WinHTTP error code when status is 0
};

// A blocking GET through WinHTTP: the system's TLS and proxy settings (a
// proxy many users need to reach GitHub), and no TLS plugin for Qt Network
// to ship. Redirects are followed. Run it off the GUI thread.
Response get(const QString& url, const QList<std::pair<QString, QString>>& headers, int timeoutMs = 20000);

} // namespace ws::http
