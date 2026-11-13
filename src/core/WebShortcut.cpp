#include "WebShortcut.h"

#include "Query.h"
#include "Wtf8.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

QString WebShortcut::iconRef() const
{
    return icon.isEmpty() ? homeUrl() : u"custom:"_s + icon;
}

QString WebShortcut::suggestUrl(const QString& words) const
{
    if (suggest.isEmpty() || !searches())
        return {};
    return QString(suggest).replace(u"%s"_s, QString::fromLatin1(QUrl::toPercentEncoding(words)));
}

WebShortcuts defaultWebShortcuts()
{
    // Each with its home page: what the keyword alone opens, and where the
    // site's icon comes from (the search's own host is often another one).
    // Keywords are whole words: one of two letters typed alone ("bd") would
    // put a site above the files looked for; the name finds it by pinyin
    // anyway ("bd" for 百度, "jd" for 京东). The old plugin's short aliases
    // are preserved by migration; there is now just one provider.
    WebShortcuts shortcuts {
        {u"baidu"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, u"https://www.baidu.com/"_s},
        {u"biying"_s, u"必应"_s, u"https://www.bing.com/search?q=%s"_s, u"https://www.bing.com/"_s},
        {u"bilibili"_s, u"哔哩哔哩"_s, u"https://search.bilibili.com/all?keyword=%s"_s, u"https://www.bilibili.com/"_s},
        {u"zhihu"_s, u"知乎"_s, u"https://www.zhihu.com/search?type=content&q=%s"_s, u"https://www.zhihu.com/"_s},
        {u"weibo"_s, u"微博"_s, u"https://s.weibo.com/weibo?q=%s"_s, u"https://weibo.com/"_s},
        {u"douyin"_s, u"抖音"_s, u"https://www.douyin.com/search/%s"_s, u"https://www.douyin.com/"_s},
        {u"xhs"_s, u"小红书"_s, u"https://www.xiaohongshu.com/search_result?keyword=%s"_s, u"https://www.xiaohongshu.com/"_s},
        {u"taobao"_s, u"淘宝"_s, u"https://s.taobao.com/search?q=%s"_s, u"https://www.taobao.com/"_s},
        {u"jingdong"_s, u"京东"_s, u"https://search.jd.com/Search?keyword=%s&enc=utf-8"_s, u"https://www.jd.com/"_s},
        {u"fanyi"_s, u"百度翻译"_s, u"https://fanyi.baidu.com/mtpe-individual/multimodal?query=%s"_s, u"https://fanyi.baidu.com/"_s},
        {u"winshun"_s, u"WinShun"_s, u"https://github.com/LingCore/WinShun"_s, {}},
        {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, u"https://github.com/"_s},
        {u"yt"_s, u"YouTube"_s, u"https://www.youtube.com/results?search_query=%s"_s, u"https://www.youtube.com/"_s},
        {u"wiki"_s, u"维基百科"_s, u"https://zh.wikipedia.org/w/index.php?search=%s"_s, u"https://zh.wikipedia.org/"_s},
    };
    for (WebShortcut& s : shortcuts)
        s.suggest = knownSuggestUrl(s.url);
    return shortcuts;
}

QString defaultWebSearchUrl(const WebShortcuts& shortcuts, const QString& words)
{
    const QString trimmed = words.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (const qsizetype first = defaultWebSearch(shortcuts); first >= 0)
        return shortcuts[first].searchUrl(trimmed);
    return u"https://www.bing.com/search?q="_s + QString::fromLatin1(QUrl::toPercentEncoding(trimmed));
}

qsizetype defaultWebSearch(const WebShortcuts& shortcuts) noexcept
{
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        if (shortcuts[i].searches())
            return i;
    }
    return -1;
}

WebShortcuts withNewDefaults(WebShortcuts shortcuts, const WebShortcuts& defaults)
{
    const WebShortcuts before = shortcuts;
    for (const WebShortcut& d : defaults) {
        const QString site = siteIconKey(d.homeUrl());
        const bool taken = std::any_of(before.begin(), before.end(), [&](const WebShortcut& s) {
            return s.keyword.compare(d.keyword, Qt::CaseInsensitive) == 0
                || (!site.isEmpty() && s.searches() == d.searches() && siteIconKey(s.homeUrl()) == site);
        });
        if (!taken)
            shortcuts.append(d);
    }
    return shortcuts;
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
            break; // a page takes no words; its keyword may still end them for another's search
        return TypedKeyword {i, words};
    }
    // The words first and the keyword after them ("how to cook eggs gg"), for
    // whoever thinks of the site last (GitHub #320).
    qsizetype start = text.size();
    while (start > 0 && !text[start - 1].isSpace())
        --start;
    const QStringView last = text.mid(start);
    const QStringView before = text.left(start).trimmed();
    if (before.isEmpty())
        return std::nullopt;
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        if (shortcuts[i].searches() && shortcuts[i].keyword.compare(last, Qt::CaseInsensitive) == 0)
            return TypedKeyword {i, before.toString(), true};
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

QString normalizeSuggestUrl(const QString& text)
{
    QString url = text.trimmed();
    url.replace(u"{query}"_s, u"%s"_s, Qt::CaseInsensitive);
    url = normalizeWebUrl(url);
    // Its own address, read by Win顺 with the words in it: http(s) only.
    if (url.isEmpty() || !url.contains(u"%s") || !isHttp(parsed(url)))
        return {};
    return url;
}

QString knownSuggestUrl(const QString& searchUrl)
{
    // Each answered a plain request (no cookies, Win顺's user agent) on
    // 2026-10-15. 知乎 (403), 京东 and 微博 (a visitor check's page), 小红书
    // (406) and 抖音 (nothing) did not: they have none.
    const QString host = parsed(searchUrl).host().toLower();
    if (host == u"www.baidu.com" || host == u"baidu.com")
        return u"https://suggestion.baidu.com/su?wd=%s&action=opensearch"_s; // OpenSearch, in GBK
    if (host == u"www.bing.com" || host == u"bing.com" || host == u"cn.bing.com")
        return u"https://api.bing.com/osjson.aspx?query=%s"_s;
    if (host == u"search.bilibili.com" || host == u"www.bilibili.com")
        return u"https://s.search.bilibili.com/main/suggest?term=%s"_s;
    if (host == u"s.taobao.com" || host == u"www.taobao.com")
        return u"https://suggest.taobao.com/sug?code=utf-8&q=%s"_s;
    if (host == u"www.google.com" || host == u"google.com")
        return u"https://suggestqueries.google.com/complete/search?client=firefox&q=%s"_s;
    return {};
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

QString defaultWebShortcutName(const WebShortcut& shortcut)
{
    const QString shown = displayWebUrl(shortcut.homeUrl());
    const QString site = shown.left(shown.indexOf(u'/'));
    return site.isEmpty() || site.contains(u':') ? shortcut.keyword : site;
}

KeywordClaim keywordClaim(const QString& keyword, const WebShortcuts& shortcuts, qsizetype except, const QString& captureKeyword,
    const std::function<QString(const QString& keyword)>& favoriteOwner)
{
    for (qsizetype i = 0; i < shortcuts.size(); ++i) {
        if (i != except && shortcuts[i].keyword.compare(keyword, Qt::CaseInsensitive) == 0)
            return {KeywordClaim::By::WebShortcut, shortcuts[i].name};
    }
    if (favoriteOwner) {
        if (const QString favorite = favoriteOwner(keyword); !favorite.isEmpty())
            return {KeywordClaim::By::Favorite, favorite};
    }
    if (!captureKeyword.isEmpty() && captureKeyword.compare(keyword, Qt::CaseInsensitive) == 0)
        return {KeywordClaim::By::Capture, captureKeyword};
    return {};
}

bool isValidCustomIconName(QStringView name) noexcept
{
    if (name.size() != 20 || !name.endsWith(u".png"))
        return false;
    return std::all_of(name.begin(), name.begin() + 16,
        [](QChar c) { return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'); });
}

namespace {

constexpr auto kFileFormat = "winshun-web-shortcuts";
constexpr int kFileVersion = 1;
constexpr qsizetype kMaxIconData = 1024 * 1024; // a site's icon is a few KB; a photo is no icon

} // namespace

QByteArray writeWebShortcutsFile(const WebShortcuts& shortcuts,
    const std::function<QByteArray(const QString& icon)>& iconPng)
{
    QJsonArray list;
    for (const WebShortcut& shortcut : shortcuts) {
        QJsonObject entry {
            {u"keyword"_s, shortcut.keyword},
            {u"name"_s, shortcut.name},
            {u"url"_s, shortcut.url},
        };
        if (!shortcut.home.isEmpty())
            entry.insert(u"home"_s, shortcut.home);
        entry.insert(u"suggest"_s, shortcut.suggest); // even empty: none, not "the one Win顺 knows"
        if (!shortcut.icon.isEmpty()) {
            if (const QByteArray png = iconPng(shortcut.icon); !png.isEmpty())
                entry.insert(u"icon"_s, QString::fromLatin1("data:image/png;base64," + png.toBase64()));
        }
        list.append(entry);
    }
    const QJsonObject file {
        {u"format"_s, QLatin1StringView(kFileFormat)},
        {u"version"_s, kFileVersion},
        {u"shortcuts"_s, list},
    };
    return QJsonDocument(file).toJson(QJsonDocument::Indented);
}

std::optional<WebShortcutFile> readWebShortcutsFile(QByteArrayView json, bool* newer)
{
    if (newer)
        *newer = false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toByteArray(), &parseError);
    const QJsonObject file = document.object();
    if (parseError.error != QJsonParseError::NoError || file.value(u"format"_s).toString() != QLatin1StringView(kFileFormat))
        return std::nullopt;
    if (file.value(u"version"_s).toInt() > kFileVersion) {
        if (newer)
            *newer = true;
        return std::nullopt;
    }
    WebShortcutFile result;
    for (const QJsonValue& value : file.value(u"shortcuts"_s).toArray()) {
        const QJsonObject entry = value.toObject();
        WebShortcutFileEntry read;
        WebShortcut& shortcut = read.shortcut;
        shortcut.keyword = entry.value(u"keyword"_s).toString().trimmed();
        shortcut.name = entry.value(u"name"_s).toString().trimmed();
        shortcut.url = normalizeWebUrl(entry.value(u"url"_s).toString());
        shortcut.home = normalizeWebUrl(entry.value(u"home"_s).toString());
        if (!isValidWebKeyword(shortcut.keyword) || shortcut.url.isEmpty()) {
            ++result.skipped;
            continue;
        }
        if (shortcut.name.isEmpty())
            shortcut.name = shortcut.keyword;
        shortcut.suggest = entry.contains(u"suggest"_s) ? normalizeSuggestUrl(entry.value(u"suggest"_s).toString())
                                                        : knownSuggestUrl(shortcut.url);
        // "data:image/png;base64,…": any picture's type, decoded (and checked) later.
        const QString icon = entry.value(u"icon"_s).toString();
        const qsizetype comma = icon.indexOf(u',');
        if (icon.startsWith(u"data:image/", Qt::CaseInsensitive) && comma > 0
            && icon.left(comma).endsWith(u";base64", Qt::CaseInsensitive) && icon.size() - comma < kMaxIconData * 4 / 3 + 4) {
            read.iconData = QByteArray::fromBase64(icon.mid(comma + 1).toLatin1());
        }
        // The later of two with one keyword: it replaces the earlier in its place.
        const auto same = std::find_if(result.entries.begin(), result.entries.end(), [&](const WebShortcutFileEntry& e) {
            return e.shortcut.keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0;
        });
        if (same != result.entries.end())
            *same = read;
        else
            result.entries.append(read);
    }
    return result;
}

WebMerge mergeWebShortcuts(WebShortcuts& into, const WebShortcuts& from)
{
    WebMerge merge;
    for (const WebShortcut& shortcut : from) {
        const auto same = std::find_if(into.begin(), into.end(), [&](const WebShortcut& s) {
            return s.keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0;
        });
        if (same == into.end()) {
            into.append(shortcut);
            ++merge.added;
        } else if (*same != shortcut) {
            *same = shortcut;
            ++merge.updated;
        } else {
            ++merge.unchanged;
        }
    }
    return merge;
}

QString siteIconKey(const QString& homeUrl)
{
    const QUrl url(homeUrl, QUrl::TolerantMode);
    if (!isHttp(url) || url.host().isEmpty())
        return {};
    return url.host().toLower();
}

SiteIconCandidates siteIconCandidates(QByteArrayView html, const QString& pageUrl)
{
    const QUrl page(pageUrl, QUrl::TolerantMode);
    struct Candidate {
        QString url;
        int rank; // 0 sized icon, 1 SVG icon, 2 other icon, 3 apple-touch-icon
        int size; // declared, else a guess for ordering
        qsizetype order;
    };
    std::vector<Candidate> found;
    static const QRegularExpression linkTag(u"<link\\b[^>]*>"_s, QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attribute(u"(?<![\\w-])(rel|href|sizes|type)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'>]+))"_s,
        QRegularExpression::CaseInsensitiveOption);
    const QString text = QString::fromUtf8(html);
    for (auto tags = linkTag.globalMatch(text); tags.hasNext();) {
        const QString tag = tags.next().captured();
        QString rel, href, sizes, type;
        for (auto attrs = attribute.globalMatch(tag); attrs.hasNext();) {
            const auto a = attrs.next();
            const QString value = a.captured(2) + a.captured(3) + a.captured(4); // whichever quoting it had
            const QString name = a.captured(1).toLower();
            (name == u"rel" ? rel : name == u"href" ? href : name == u"sizes" ? sizes : type) = value.trimmed();
        }
        const QStringList rels = rel.toLower().split(u' ', Qt::SkipEmptyParts);
        const bool touch = rels.contains(u"apple-touch-icon"_s) || rels.contains(u"apple-touch-icon-precomposed"_s);
        if (!touch && !rels.contains(u"icon"_s))
            continue; // mask-icon (one colour), fluid-icon, stylesheets
        href.replace(u"&amp;"_s, u"&"_s);
        const QUrl url = page.resolved(QUrl(href, QUrl::TolerantMode));
        if (href.isEmpty() || !isHttp(url))
            continue;
        const bool svg = type.contains(u"svg", Qt::CaseInsensitive) || url.path().endsWith(u".svg", Qt::CaseInsensitive);
        int size = 0; // "16x16 32x32": the largest
        for (const QString& s : sizes.split(u' ', Qt::SkipEmptyParts))
            size = std::max(size, s.section(u'x', 0, 0, QString::SectionCaseInsensitiveSeps).toInt());
        const int rank = touch ? 3 : svg ? 1 : size >= 64 ? 0 : 2;
        found.push_back({url.toString(), rank, touch && size == 0 ? 180 : size, static_cast<qsizetype>(found.size())});
    }
    // Larger first among the sized and the touch icons; the SVG and the
    // other icons as the page has them (their sizes, when given, are 16 or 32).
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        return (a.rank == 0 || a.rank == 3) && a.size != b.size ? a.size > b.size : a.order < b.order;
    });
    SiteIconCandidates out;
    for (const Candidate& c : found) {
        if (c.rank < 3 && !out.icons.contains(c.url))
            out.icons.append(c.url);
    }
    if (isHttp(page) && !page.host().isEmpty()) {
        const QString favicon = page.resolved(QUrl(u"/favicon.ico"_s)).toString();
        if (!out.icons.contains(favicon))
            out.icons.append(favicon);
    }
    for (const Candidate& c : found) {
        if (c.rank == 3 && !out.icons.contains(c.url) && !out.touchIcons.contains(c.url))
            out.touchIcons.append(c.url);
    }
    return out;
}

} // namespace ws
