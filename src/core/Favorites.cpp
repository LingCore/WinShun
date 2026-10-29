#include "Favorites.h"

#include "AppCatalog.h"
#include "History.h"
#include "PathText.h"
#include "SystemCatalog.h"
#include "WebShortcut.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr auto kFileFormat = "winshun-favorites";
constexpr int kFileVersion = 4;
constexpr qint64 kMaxFile = 16 << 20;

QString kindName(Favorite::Kind kind)
{
    switch (kind) {
    case Favorite::Kind::Folder:
        return u"folder"_s;
    case Favorite::Kind::File:
        return u"file"_s;
    case Favorite::Kind::App:
        return u"app"_s;
    case Favorite::Kind::Place:
        return u"place"_s;
    case Favorite::Kind::Command:
        return u"command"_s;
    }
    return {};
}

std::optional<Favorite::Kind> kindNamed(const QString& name)
{
    if (name == u"folder")
        return Favorite::Kind::Folder;
    if (name == u"file")
        return Favorite::Kind::File;
    if (name == u"app")
        return Favorite::Kind::App;
    if (name == u"place")
        return Favorite::Kind::Place;
    if (name == u"command")
        return Favorite::Kind::Command;
    return std::nullopt;
}

// Whether a path suits a kind: an app's and a place's are theirs alone.
bool fits(Favorite::Kind kind, const QString& path)
{
    switch (kind) {
    case Favorite::Kind::App:
        return isAppLaunchPath(path);
    case Favorite::Kind::Place:
        return isPlacePath(path);
    case Favorite::Kind::Command:
        return path.startsWith(u"winshun-command:") && path.size() > 16;
    default:
        return !isAppLaunchPath(path) && !isPlacePath(path) && !isWebPath(path) && !path.startsWith(u"winshun-");
    }
}

QString lastName(const QString& path)
{
    QString name = path.mid(path.lastIndexOf(u'\\') + 1);
    return name.isEmpty() ? path : name; // "C:\"
}

} // namespace

std::optional<Favorite> favoriteOf(const SearchResult& row)
{
    if (row.path.isEmpty() || row.isWeb() || row.isPlugin())
        return std::nullopt;
    Favorite f;
    f.path = row.path;
    f.name = row.name;
    if (row.isCommand()) {
        f.kind = Favorite::Kind::Command;
        f.command = row.target;
        f.workingDirectory = row.workingDirectory;
        f.icon = row.icon;
        f.silent = row.silent;
        f.runAsAdmin = row.runAsAdmin;
    } else if (row.isPlace())
        f.kind = Favorite::Kind::Place;
    else if (row.isApp())
        f.kind = Favorite::Kind::App;
    else if (row.isDir)
        f.kind = Favorite::Kind::Folder;
    else
        f.kind = Favorite::Kind::File;
    if (!fits(f.kind, f.path))
        return std::nullopt;
    if (f.isFileOrFolder()) {
        f.path = QDir::toNativeSeparators(f.path);
        f.name = lastName(f.path); // a content row's name may be anything
    }
    return f;
}

bool scopeShows(Scope scope, Favorite::Kind kind) noexcept
{
    switch (scope) {
    case Scope::All:
        return true;
    case Scope::Files:
        return kind == Favorite::Kind::File || kind == Favorite::Kind::Folder;
    case Scope::Folders:
        return kind == Favorite::Kind::Folder;
    case Scope::Content:
        return false;
    }
    return false;
}

qsizetype typedFavoriteKeyword(const FavoriteList& favorites, QStringView text)
{
    const QStringView word = text.trimmed();
    if (word.isEmpty())
        return -1;
    for (qsizetype i = 0; i < favorites.size(); ++i) {
        if (!favorites[i].keyword.isEmpty() && word.compare(favorites[i].keyword, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

bool isValidFavoriteKeyword(QStringView keyword) noexcept
{
    return isValidWebKeyword(keyword) && !keyword.contains(u'\\') && !keyword.contains(u'/') && !keyword.contains(u':');
}

qsizetype typedCommandKeyword(const FavoriteList& favorites, QStringView text)
{
    const QStringView typed = text.trimmed();
    for (qsizetype i = 0; i < favorites.size(); ++i) {
        const Favorite& f = favorites[i];
        const qsizetype n = f.keyword.size();
        if (f.kind == Favorite::Kind::Command && n > 0 && typed.size() >= n
            && typed.first(n).compare(f.keyword, Qt::CaseInsensitive) == 0
            && (typed.size() == n || typed[n].isSpace()))
            return i;
    }
    return -1;
}

QString commandWords(const Favorite& favorite, QStringView text)
{
    const QStringView typed = text.trimmed();
    const qsizetype n = favorite.keyword.size();
    return n > 0 && typed.size() > n && typed[n].isSpace()
            && typed.first(n).compare(favorite.keyword, Qt::CaseInsensitive) == 0
        ? typed.sliced(n).trimmed().toString() : QString();
}

QString commandArguments(QStringView arguments, QStringView words)
{
    // CommandLineToArgvW / CRT quoting: preserve spaces, quotes and trailing
    // backslashes. Substitute once so {query} in the user's input stays text.
    const QString quoted = pathtext::commandArgument(words.toString());
    QString result;
    for (qsizetype i = 0; i < arguments.size();) {
        const QStringView rest = arguments.sliced(i);
        const qsizetype token = rest.startsWith(u"\"{query}\"") ? 9 : rest.startsWith(u"{query}") ? 7 : 0;
        if (token) {
            result += quoted;
            i += token;
        } else {
            result += arguments[i++];
        }
    }
    return result;
}

qsizetype indexOfFavorite(const FavoriteList& favorites, QStringView path)
{
    for (qsizetype i = 0; i < favorites.size(); ++i) {
        if (pathtext::samePath(path, favorites[i].path))
            return i;
    }
    return -1;
}

QByteArray favoriteIconPng(QStringView icon)
{
    constexpr QStringView prefix = u"data:image/png;base64,";
    constexpr qsizetype maxBytes = 128 * 1024;
    if (!icon.startsWith(prefix) || icon.size() > prefix.size() + (maxBytes + 2) / 3 * 4)
        return {};
    const QByteArray png = QByteArray::fromBase64(icon.sliced(prefix.size()).toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (png.size() < 33 || png.size() > maxBytes || !png.startsWith(QByteArrayView("\x89PNG\r\n\x1a\n", 8))
        || qFromBigEndian<quint32>(png.constData() + 8) != 13 || png.sliced(12, 4) != "IHDR")
        return {};
    const quint32 width = qFromBigEndian<quint32>(png.constData() + 16);
    const quint32 height = qFromBigEndian<quint32>(png.constData() + 20);
    return width > 0 && height > 0 && width <= 128 && height <= 128 ? png : QByteArray();
}

QByteArray writeFavoritesFile(const FavoriteList& favorites)
{
    QJsonArray list;
    int version = 1;
    for (const Favorite& f : favorites) {
        QJsonObject entry {
            {u"kind"_s, kindName(f.kind)},
            {u"path"_s, f.path},
            {u"name"_s, f.name},
        };
        if (!f.keyword.isEmpty())
            entry.insert(u"keyword"_s, f.keyword);
        if (f.kind == Favorite::Kind::Command) {
            version = std::max(version, 2);
            entry.insert(u"command"_s, f.command);
            entry.insert(u"workingDirectory"_s, f.workingDirectory);
            if (f.silent || f.runAsAdmin) { // version 4: an older copy would drop them on its next save
                version = kFileVersion;
                if (f.silent)
                    entry.insert(u"silent"_s, true);
                if (f.runAsAdmin)
                    entry.insert(u"admin"_s, true);
            }
            if (!f.icon.isEmpty()) {
                version = std::max(version, 3);
                entry.insert(u"icon"_s, f.icon);
            }
        }
        list.append(entry);
    }
    const QJsonObject file {
        {u"format"_s, QLatin1StringView(kFileFormat)},
        {u"version"_s, version},
        {u"favorites"_s, list},
    };
    return QJsonDocument(file).toJson(QJsonDocument::Indented);
}

std::optional<FavoritesFile> readFavoritesFile(QByteArrayView json, bool* newer)
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
    FavoritesFile result;
    for (const QJsonValue& value : file.value(u"favorites"_s).toArray()) {
        const QJsonObject entry = value.toObject();
        const std::optional<Favorite::Kind> kind = kindNamed(entry.value(u"kind"_s).toString());
        Favorite f;
        f.path = entry.value(u"path"_s).toString().trimmed();
        if (!kind || f.path.isEmpty()) {
            ++result.skipped;
            continue;
        }
        f.kind = *kind;
        if (f.kind == Favorite::Kind::Command) {
            f.command = entry.value(u"command"_s).toString().trimmed();
            f.workingDirectory = entry.value(u"workingDirectory"_s).toString().trimmed();
            f.icon = entry.value(u"icon"_s).toString();
            f.silent = entry.value(u"silent"_s).toBool();
            f.runAsAdmin = entry.value(u"admin"_s).toBool();
            if (!f.icon.isEmpty() && favoriteIconPng(f.icon).isEmpty()) {
                f.icon.clear();
                ++result.badIcons;
            }
            if (f.command.isEmpty() || f.command.contains(QChar::Null) || f.command.contains(u'\n')
                || f.command.contains(u'\r') || f.workingDirectory.contains(QChar::Null)) {
                ++result.skipped;
                continue;
            }
        }
        if (f.isFileOrFolder()) {
            f.path = QDir::toNativeSeparators(f.path);
            if (f.path.size() > 3 && f.path.endsWith(u'\\'))
                f.path.chop(1);
        }
        if (!fits(f.kind, f.path) || indexOfFavorite(result.favorites, f.path) >= 0) {
            ++result.skipped;
            continue;
        }
        f.name = entry.value(u"name"_s).toString().trimmed();
        if (f.name.isEmpty())
            f.name = f.isFileOrFolder() ? lastName(f.path) : f.path;
        const QString keyword = entry.value(u"keyword"_s).toString().trimmed();
        if (isValidFavoriteKeyword(keyword) && typedFavoriteKeyword(result.favorites, keyword) < 0)
            f.keyword = keyword;
        result.favorites.append(f);
    }
    return result;
}

FavoriteList favoritesFromPins(const QStringList& pinnedFolders)
{
    FavoriteList favorites;
    for (const QString& folder : pinnedFolders) {
        Favorite f;
        f.kind = Favorite::Kind::Folder;
        f.path = QDir::toNativeSeparators(folder.trimmed());
        if (f.path.isEmpty() || !fits(f.kind, f.path) || indexOfFavorite(favorites, f.path) >= 0)
            continue;
        f.name = lastName(f.path);
        favorites.append(f);
    }
    return favorites;
}

Favorites::Favorites(QString filePath, QString pinsFile, QObject* parent)
    : QObject(parent)
    , m_filePath(std::move(filePath))
    , m_pinsFile(std::move(pinsFile))
{
}

void Favorites::load()
{
    m_items.clear();
    m_readOnly = false;
    QFile file(m_filePath);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.read(kMaxFile);
        file.close();
        bool newer = false;
        if (const std::optional<FavoritesFile> read = readFavoritesFile(bytes, &newer)) {
            m_items = read->favorites;
        } else if (newer) {
            m_readOnly = true; // a newer Win顺's: kept as it is, for when that one runs again
            qWarning("WinShun: the favorites were saved by a newer WinShun, not changed meanwhile");
        } else {
            // Kept aside, not overwritten by the next change: its user may want it back.
            const QString bad = m_filePath + u".bad"_s;
            QFile::remove(bad);
            if (!QFile::rename(m_filePath, bad))
                m_readOnly = true;
            qWarning("WinShun: the favorites could not be read, kept as %ls", qUtf16Printable(bad));
        }
    } else if (!QFileInfo::exists(m_filePath) && !m_pinsFile.isEmpty() && QFileInfo::exists(m_pinsFile)) {
        // Once: the dialog box's pinned folders, newest first as it listed them.
        History pins(m_pinsFile, 1000);
        pins.load();
        m_items = favoritesFromPins(pins.items());
        save();
        if (!QFile::rename(m_pinsFile, m_pinsFile + u".migrated"_s))
            qWarning("WinShun: %ls taken into the favorites but not renamed", qUtf16Printable(m_pinsFile));
    }
    emit changed();
}

bool Favorites::insert(Favorite favorite, qsizetype at)
{
    if (favorite.path.isEmpty() || contains(favorite.path))
        return false;
    if (favorite.name.isEmpty())
        favorite.name = favorite.isFileOrFolder() ? lastName(favorite.path) : favorite.path;
    if (!favorite.keyword.isEmpty() && (!isValidFavoriteKeyword(favorite.keyword) || typedFavoriteKeyword(m_items, favorite.keyword) >= 0))
        favorite.keyword.clear();
    m_items.insert(at < 0 || at > m_items.size() ? m_items.size() : at, favorite);
    return true;
}

bool Favorites::add(Favorite favorite, qsizetype at)
{
    if (!insert(std::move(favorite), at))
        return false;
    save();
    emit changed();
    return true;
}

int Favorites::addAll(const FavoriteList& favorites)
{
    int added = 0;
    for (const Favorite& f : favorites) {
        if (insert(f, -1))
            ++added;
    }
    if (added > 0) {
        save();
        emit changed();
    }
    return added;
}

Favorites::Merged Favorites::merge(const FavoriteList& favorites)
{
    Merged merged;
    for (const Favorite& f : favorites) {
        const qsizetype i = indexOf(f.path);
        if (i < 0) {
            if (insert(f, -1))
                ++merged.added;
            continue;
        }
        Favorite& there = m_items[i];
        bool completed = false;
        if (there.keyword.isEmpty() && isValidFavoriteKeyword(f.keyword) && typedFavoriteKeyword(m_items, f.keyword) < 0) {
            there.keyword = f.keyword;
            completed = true;
        }
        if (there.isFileOrFolder() && there.name == lastName(there.path) && !f.name.isEmpty() && f.name != there.name) {
            there.name = f.name;
            completed = true;
        }
        if (there.kind == Favorite::Kind::Command && f.kind == there.kind && there.icon.isEmpty()
            && !favoriteIconPng(f.icon).isEmpty()) {
            there.icon = f.icon;
            completed = true;
        }
        if (completed)
            ++merged.completed;
    }
    if (merged.added > 0 || merged.completed > 0) {
        save();
        emit changed();
    }
    return merged;
}

bool Favorites::remove(QStringView path)
{
    const qsizetype i = indexOf(path);
    if (i < 0)
        return false;
    m_items.removeAt(i);
    save();
    emit changed();
    return true;
}

int Favorites::removeAll(const QStringList& paths)
{
    const qsizetype removed = m_items.removeIf([&](const Favorite& f) {
        return std::ranges::any_of(paths, [&](const QString& path) { return pathtext::samePath(path, f.path); });
    });
    if (removed > 0) {
        save();
        emit changed();
    }
    return static_cast<int>(removed);
}

bool Favorites::update(qsizetype index, const Favorite& favorite)
{
    if (index < 0 || index >= m_items.size() || favorite.path.isEmpty() || m_items[index] == favorite)
        return false;
    const qsizetype there = indexOf(favorite.path);
    if (there >= 0 && there != index)
        return false;
    m_items[index] = favorite;
    save();
    emit changed();
    return true;
}

bool Favorites::move(qsizetype from, qsizetype to)
{
    if (from < 0 || from >= m_items.size() || to < 0 || to >= m_items.size() || from == to)
        return false;
    m_items.move(from, to);
    save();
    emit changed();
    return true;
}

void Favorites::save()
{
    if (m_readOnly)
        return;
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(writeFavoritesFile(m_items)) < 0 || !file.commit())
        qWarning("WinShun: the favorites could not be saved to %ls", qUtf16Printable(m_filePath));
}

} // namespace ws
