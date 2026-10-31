#pragma once

#include <QByteArray>
#include <QStringList>

#include <functional>

namespace ws::download {

// Mirrors in order, then the origin. Only public GitHub release/raw files
// receive automatic mirrors; local files, APIs and URLs with credentials or
// queries keep their original source. Explicit service mirrors keep their order.
QStringList sources(const QString& source, const QStringList& preferred = {});
bool tooSlow(qint64 received, qint64 elapsedMs);

struct Fetched {
    int status = 0;
    QByteArray body;
    unsigned long error = 0;
};
using Progress = std::function<bool(qint64 received)>;
using Fetcher = std::function<Fetched(const QString& url, qint64 maxBytes, const Progress& progress)>;
using Validator = std::function<bool(const QByteArray&)>;

// Blocking, with injected web transport. Rejected content and connection
// failures try the next source; cancellation never starts another request.
// A local file is read once and never sent to a mirror.
// If every source fails, the last response is retained for the caller's error
// message; rejected content has ERROR_CRC and must not be installed unchecked.
Fetched fetch(const QString& source, qint64 maxBytes, const Fetcher& web,
    const Progress& progress = {}, const Validator& accept = {});

} // namespace ws::download
