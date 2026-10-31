#include "Download.h"

#include "GitHubMirrors.h"

#include <QElapsedTimer>
#include <QFile>
#include <QUrl>

#include <windows.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::download {
namespace {
bool isWeb(const QUrl& url) { return url.scheme() == u"https" || url.scheme() == u"http"; }

bool publicGitHubFile(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != u"https" || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment() || url.port(-1) != -1)
        return false;
    const QStringList path = url.path().split(u'/', Qt::SkipEmptyParts);
    return (url.host() == u"github.com" && path.size() >= 6 && path[2] == u"releases" && path[3] == u"download")
        || (url.host() == u"raw.githubusercontent.com" && path.size() >= 4);
}

Fetched readFile(const QString& source, qint64 maxBytes, const Progress& progress)
{
    const QUrl url(source);
    QFile file(url.isLocalFile() ? url.toLocalFile() : source);
    if (!file.open(QIODevice::ReadOnly))
        return {0, {}, static_cast<unsigned long>(file.exists() ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND)};
    Fetched result;
    constexpr qint64 pieceSize = 64 * 1024;
    while (maxBytes <= 0 || result.body.size() < maxBytes) {
        const qint64 wanted = maxBytes > 0 ? std::min(pieceSize, maxBytes - result.body.size()) : pieceSize;
        const QByteArray piece = file.read(wanted);
        if (piece.isEmpty()) {
            if (file.error() != QFileDevice::NoError)
                return {0, {}, ERROR_READ_FAULT};
            break;
        }
        result.body += piece;
        if (progress && !progress(result.body.size()))
            return {0, {}, ERROR_CANCELLED};
    }
    result.status = 200;
    return result;
}
}

QStringList sources(const QString& source, const QStringList& preferred)
{
    QStringList result = preferred;
    if (preferred.isEmpty() && publicGitHubFile(QUrl(source, QUrl::StrictMode))) {
        for (const auto prefix : kGitHubMirrors)
            result.append(QString::fromLatin1(prefix.data(), static_cast<qsizetype>(prefix.size())) + source);
    }
    result.removeAll(source);
    result.append(source);
    result.removeDuplicates();
    return result;
}

bool tooSlow(qint64 received, qint64 elapsedMs)
{
    return elapsedMs > 30000 && static_cast<double>(received) * 1000 / elapsedMs < 16 * 1024;
}

Fetched fetch(const QString& source, qint64 maxBytes, const Fetcher& web, const Progress& progress, const Validator& accept)
{
    if (progress && !progress(0))
        return {0, {}, ERROR_CANCELLED};
    if (!isWeb(QUrl(source))) {
        Fetched local = readFile(source, maxBytes, progress);
        if (local.status == 200 && accept && !accept(local.body))
            local.error = ERROR_CRC;
        return local;
    }
    if (!web)
        return {0, {}, ERROR_NOT_SUPPORTED};
    const QStringList urls = sources(source);
    Fetched result;
    for (qsizetype i = 0; i < urls.size(); ++i) {
        if (progress && !progress(0))
            return {0, {}, ERROR_CANCELLED};
        bool cancelled = false;
        bool slow = false;
        QElapsedTimer timer;
        timer.start();
        result = web(urls[i], maxBytes, [&](qint64 received) {
            if (progress && !progress(received)) {
                cancelled = true;
                return false;
            }
            slow = i + 1 < urls.size() && tooSlow(received, timer.elapsed());
            return !slow;
        });
        if (cancelled || (!slow && result.error == ERROR_CANCELLED))
            return {0, {}, ERROR_CANCELLED};
        if (slow)
            continue;
        if (result.status == 200 && result.error == 0) {
            if (!accept || accept(result.body))
                return result;
            result.error = ERROR_CRC;
        }
    }
    return result;
}

} // namespace ws::download
