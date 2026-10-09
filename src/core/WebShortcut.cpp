#include "WebShortcut.h"

#include "Query.h"
#include "Wtf8.h"

#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// A name match ranks a little below an app or place matched as well: those
// are on this computer.
constexpr int kBelowApps = 5;

// %s is no valid escape: parsed with a word in its place.
QUrl parsed(const QString& url)
{
    return QUrl(QString(url).replace(u"%s"_s, u"x"_s), QUrl::TolerantMode);
}

bool isHttp(const QUrl& url)
{
    return url.scheme() == u"http" || url.scheme() == u"https";
}

} // namespace

QString WebShortcut::homeUrl() const
{
    if (!home.isEmpty())
        return home;
    if (!searches())
        return url;
    const QUrl search = parsed(url);
    if (isHttp(search) && !search.host().isEmpty()) {
        QUrl site;
        site.setScheme(search.scheme());
        site.setAuthority(search.authority());
        site.setPath(u"/"_s);
        return site.toString();
    }
    return QString(url).remove(u"%s"_s); // an app's link, with no words
}

QString WebShortcut::searchUrl(const QString& words) const
{
    return QString(url).replace(u"%s"_s, QString::fromLatin1(QUrl::toPercentEncoding(words)));
}

WebShortcuts defaultWebShortcuts()
{
    return {WebShortcut {u"winshun"_s, u"WinShun"_s, u"https://github.com/LingCore/WinShun"_s, {}}};
}

QString webPath(const QString& keyword, const QString& words)
{
    QString path = u"winshun-web:"_s + keyword;
    if (!words.isEmpty())
        path += u' ' + words;
    return path;
}

bool isWebPath(QStringView path) noexcept
{
    return path.startsWith(u"winshun-web:");
}

std::optional<TypedKeyword> typedKeyword(const WebShortcuts& shortcuts, QStringView text)
{
    text = text.trimmed();
    qsizetype end = 0;
    while (end < text.size() && !text[end].isSpace())
        ++end;
    const QStringView keyword = text.left(end);
    if (keyword.isEmpty())
        return std::nullopt;
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        const WebShortcut& shortcut = shortcuts[i];
        if (shortcut.keyword.compare(keyword, Qt::CaseInsensitive) != 0)
            continue;
        const QString words = text.mid(end).trimmed().toString();
        if (!words.isEmpty() && !shortcut.searches())
            return std::nullopt;
        return TypedKeyword {i, words};
    }
    return std::nullopt;
}

std::vector<WebHit> searchWebShortcuts(const WebShortcuts& shortcuts, const NameMatcher& matcher)
{
    std::vector<WebHit> hits;
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        // Above 0: a query of exclusions alone ("!draft") scores 0 on every name.
        if (const int score = matcher.matchName(wtf8::fromUtf16(wtf8::view(shortcuts[i].name))); score > 0)
            hits.push_back({i, score - kBelowApps});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const WebHit& a, const WebHit& b) { return a.score > b.score; });
    return hits;
}

QString normalizeWebUrl(const QString& text)
{
    QString url = text.trimmed();
    if (url.isEmpty())
        return {};
    if (!url.contains(u"%s") && url.contains(u"%25s"))
        url.replace(u"%25s"_s, u"%s"_s);
    // A scheme has two letters or more ("C:\..." is a path) and no port after
    // it ("example.com:8080/..."); "github.com/..." needs one.
    static const QRegularExpression scheme(u"^[A-Za-z][A-Za-z0-9+.-]+:(?!\\d)"_s);
    if (!scheme.match(url).hasMatch())
        url.prepend(u"https://"_s);
    const QUrl parsedUrl = parsed(url);
    if (!parsedUrl.isValid() || parsedUrl.scheme().size() < 2 || parsedUrl.scheme() == u"file")
        return {};
    if (isHttp(parsedUrl) && parsedUrl.host().isEmpty())
        return {};
    return url;
}

bool isValidWebKeyword(QStringView keyword) noexcept
{
    return !keyword.isEmpty()
        && std::none_of(keyword.begin(), keyword.end(), [](QChar c) { return c.isSpace() || c == u'"'; });
}

QString displayWebUrl(const QString& url)
{
    // A template's %s stays: decoding takes any two characters after a % for hex digits.
    QStringList parts = url.split(u"%s"_s);
    for (QString& part : parts)
        part = QUrl::fromPercentEncoding(part.toUtf8());
    QString shown = parts.join(u"%s"_s);
    for (const QStringView scheme : {QStringView(u"https://"), QStringView(u"http://")}) {
        if (shown.startsWith(scheme, Qt::CaseInsensitive)) {
            shown.remove(0, scheme.size());
            if (shown.startsWith(u"www.", Qt::CaseInsensitive))
                shown.remove(0, 4);
            const qsizetype slash = shown.indexOf(u'/');
            if (slash == shown.size() - 1)
                shown.chop(1); // "github.com/": the site itself
            break;
        }
    }
    return shown;
}

} // namespace ws
