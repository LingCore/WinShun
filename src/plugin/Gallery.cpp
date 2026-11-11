#include "Gallery.h"

#include "Catalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QUrl>

#include <windows.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::plugin::gallery {

namespace {

bool isWeb(const QString& address)
{
    return address.startsWith(u"https://"_s, Qt::CaseInsensitive) || address.startsWith(u"http://"_s, Qt::CaseInsensitive);
}

QString localPath(const QString& source)
{
    return source.startsWith(u"file:"_s, Qt::CaseInsensitive) ? QUrl(source).toLocalFile() : source;
}

// A string, or {zh, en}.
Text textOf(const QJsonValue& value)
{
    if (value.isString())
        return Text {value.toString(), value.toString()};
    const QJsonObject o = value.toObject();
    return Text {o.value(u"zh"_s).toString(), o.value(u"en"_s).toString()};
}

QStringList stringsOf(const QJsonValue& value)
{
    QStringList out;
    for (const QJsonValue v : value.toArray()) {
        if (v.isString() && !v.toString().isEmpty())
            out.push_back(v.toString());
    }
    return out;
}

// A package's address as the list has it, made whole: one beside a list
// read from a file may be a file too (or relative to it); one from a list
// fetched from the web must be https.
QString packageAddress(const QString& given, const QString& source)
{
    if (given.isEmpty())
        return {};
    if (isWeb(source)) {
        const QString address = QUrl(source).resolved(QUrl(given)).toString();
        return address.startsWith(u"https://"_s, Qt::CaseInsensitive) ? address : QString();
    }
    if (isWeb(given))
        return given;
    const QString path = localPath(given);
    if (QDir::isAbsolutePath(path))
        return QDir::toNativeSeparators(path);
    return QDir::toNativeSeparators(QFileInfo(localPath(source)).absoluteDir().filePath(path));
}

bool isSha256(const QString& hex)
{
    if (hex.size() != 64)
        return false;
    for (const QChar c : hex) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    }
    return true;
}

std::optional<Program> programOf(const QJsonValue& runtime)
{
    const QString name = runtime.toObject().value(u"program"_s).toString();
    if (name.isEmpty() || runtime.toObject().value(u"mode"_s).toString() == u"zero-code")
        return std::nullopt;
    if (name == u"python")
        return Program::Python;
    if (name == u"pwsh")
        return Program::Pwsh;
    if (name == u"powershell")
        return Program::PowerShell;
    if (name == u"node")
        return Program::Node;
    return Program::Exe;
}

} // namespace

const Item* Index::find(QStringView id) const
{
    for (const Item& item : items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

namespace {

bool asciiName(QStringView s, QStringView alsoAllowed)
{
    return !s.isEmpty() && s.size() <= 100 && std::ranges::all_of(s, [&](QChar c) {
        return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || alsoAllowed.contains(c);
    });
}

// "owner/name" of a GitHub repository: `path` is just that (`whole`), or
// starts with it (a page in it); empty when it is none.
QString githubRepository(QStringView path, bool whole)
{
    const QList<QStringView> parts = path.split(u'/', Qt::SkipEmptyParts);
    if (parts.size() < 2 || (whole && (parts.size() != 2 || path.startsWith(u'/'))))
        return {};
    QStringView name = parts[1];
    if (name.endsWith(u".git", Qt::CaseInsensitive))
        name.chop(4);
    if (!asciiName(parts[0], u"-") || !asciiName(name, u"-_.") || name == u"." || name == u"..")
        return {};
    return parts[0].toString() + u'/' + name.toString();
}

} // namespace

QString repositoryAddress(const QString& repo)
{
    const QString listed = githubRepository(repo, true);
    return listed.isEmpty() || listed.compare(u"LingCore/WinShun-plugins"_s, Qt::CaseInsensitive) == 0 ? QString() : u"https://github.com/"_s + listed;
}

bool fitsFilter(const QString& words, const QString& filter)
{
    const QStringList typed = filter.toLower().split(u' ', Qt::SkipEmptyParts);
    return std::ranges::all_of(typed, [&](const QString& word) { return words.contains(word); });
}

QString issuesAddress(const QString& repo, const QString& homepage)
{
    const auto issues = [](const QString& ownerName) { return u"https://github.com/"_s + ownerName + u"/issues"_s; };
    if (const QString own = repositoryAddress(repo); !own.isEmpty())
        return own + u"/issues"_s;
    const QString listed = githubRepository(repo, true);
    const QString page = homepage.trimmed();
    const QUrl url(page, QUrl::StrictMode);
    if (url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && (url.scheme() == u"https" || url.scheme() == u"http")) {
        const QString host = url.host().toLower();
        if (host == u"github.com" || host == u"www.github.com") {
            if (const QString onGithub = githubRepository(url.path(), false); !onGithub.isEmpty())
                return issues(onGithub);
        }
        return page;
    }
    return listed.isEmpty() ? QString() : issues(listed);
}

std::optional<Index> parse(const QByteArray& json, const QString& source, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return std::nullopt;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(u"不是 JSON：%1"_s.arg(parseError.errorString()));
    const QJsonObject root = document.object();
    if (root.value(u"schema"_s).toInt() != 1)
        return fail(u"不认识的 schema：%1"_s.arg(root.value(u"schema"_s).toVariant().toString()));
    if (!root.value(u"plugins"_s).isArray())
        return fail(u"没有 plugins 数组"_s);
    Index index;
    index.generated = root.value(u"generated"_s).toString();
    for (const QJsonValue value : root.value(u"plugins"_s).toArray()) {
        const QJsonObject o = value.toObject();
        Item item;
        item.id = o.value(u"id"_s).toString();
        item.version = o.value(u"version"_s).toString();
        item.url = packageAddress(o.value(u"url"_s).toString(), source);
        item.sha256 = o.value(u"sha256"_s).toString().toLower();
        item.size = static_cast<qint64>(o.value(u"size"_s).toDouble());
        if (!validPluginId(item.id) || item.version.isEmpty() || item.url.isEmpty() || !isSha256(item.sha256) || item.size <= 0
            || index.find(item.id)) {
            ++index.skipped;
            continue;
        }
        item.name = textOf(o.value(u"name"_s));
        if (item.name.isEmpty())
            item.name = Text {item.id, item.id};
        item.description = textOf(o.value(u"description"_s));
        item.author = o.value(u"author"_s).toString();
        item.homepage = o.value(u"homepage"_s).toString();
        item.repo = o.value(u"repo"_s).toString();
        item.keywords = stringsOf(o.value(u"keywords"_s));
        // Names; a manifest's {"files:read": [paths]} by its name.
        for (const QJsonValue p : o.value(u"permissions"_s).toArray()) {
            const QString name = p.isObject() && !p.toObject().isEmpty() ? p.toObject().constBegin().key() : p.toString();
            if (!name.isEmpty() && !item.permissions.contains(name))
                item.permissions.push_back(name);
        }
        item.program = programOf(o.value(u"runtime"_s));
        const QJsonValue review = o.value(u"review"_s);
        if (review == u"official"_s)
            item.review = Review::Official;
        else if (review == u"reviewed"_s)
            item.review = Review::Reviewed;
        else if (review == u"unreviewed-native"_s)
            item.review = Review::UnreviewedNative;
        else if (!review.isUndefined()) {
            ++index.skipped;
            continue;
        }
        item.minWinShun = o.value(u"minWinShun"_s).toString();
        item.bundled = o.value(u"bundled"_s).toBool();
        index.items.push_back(std::move(item));
    }
    return index;
}

Local localOf(const Catalog& catalog, const QString& bundledDir, const QString& id)
{
    Local local;
    if (const Entry* e = catalog.find(id); e && e->source == Entry::Source::User) {
        local.userCopy = true;
        if (e->manifest)
            local.userVersion = e->manifest->version;
    }
    local.bundled = !bundledDir.isEmpty() && QFileInfo::exists(QDir(bundledDir).filePath(id + u"/plugin.json"_s));
    return local;
}

State stateOf(const Item& item, const Local& local, QStringView appVersion)
{
    if (local.userCopy && !local.userVersion.isEmpty() && compareVersions(local.userVersion, item.version) >= 0)
        return State::Installed;
    const bool newerApp = !item.minWinShun.isEmpty() && compareVersions(item.minWinShun, appVersion) > 0;
    if (local.userCopy)
        return newerApp ? State::NeedsNewerApp : State::Update;
    if (local.bundled)
        return State::Bundled;
    return newerApp ? State::NeedsNewerApp : State::Install;
}

Problem check(const Item& item, const QByteArray& package)
{
    if (item.size > kMaxPackageBytes)
        return Problem::TooBig;
    if (package.size() != item.size)
        return Problem::SizeMismatch;
    const QByteArray hash = QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex();
    return QLatin1StringView(hash) == item.sha256 ? Problem::None : Problem::HashMismatch;
}

QString saveTemporary(const Item& item, const QByteArray& package, QString* error)
{
    // The ".wsplugin" ending: the install takes the file for an archive by it.
    QTemporaryFile file(QDir::temp().filePath(u"WinShun-%1-XXXXXX.wsplugin"_s.arg(item.id)));
    file.setAutoRemove(false);
    if (!file.open() || file.write(package) != package.size() || !file.flush()) {
        if (error)
            *error = file.errorString();
        file.remove();
        return {};
    }
    const QString path = QDir::toNativeSeparators(file.fileName());
    file.close();
    return path;
}

} // namespace ws::plugin::gallery
