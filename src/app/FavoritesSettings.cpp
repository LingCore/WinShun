#include "FavoritesSettings.h"
#include "PathText.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "SiteIconImage.h"
#include "SvgIcon.h"
#include "SettingsEditor.h"
#include "ShareLister.h"
#include "SystemCatalog.h"
#include "Unc.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSaveFile>
#include <QStyleHints>
#include <QThreadPool>
#include <QUrl>
#include <QUuid>

#include <windows.h>

#include <thread>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

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

QString lastName(const QString& path)
{
    const QString name = path.mid(path.lastIndexOf(u'\\') + 1);
    return name.isEmpty() ? path : name;
}

const AppInfo* findApp(const AppList& apps, const QString& path)
{
    const QString id = appIdOf(path);
    const auto it = std::find_if(apps.begin(), apps.end(), [&](const AppInfo& a) { return a.id == id; });
    return id.isEmpty() || it == apps.end() ? nullptr : &*it;
}

const PlaceInfo* findPlace(const PlaceList& places, const QString& path)
{
    const QString key = placeKeyOf(path);
    const auto it = std::find_if(places.begin(), places.end(), [&](const PlaceInfo& p) { return p.key == key; });
    return key.isEmpty() || it == places.end() ? nullptr : &*it;
}

enum class Where { Local, Network, Unknown };

// Where a path is: on this PC's disks, on a network (a drive letter mapped
// to a share, or a share), or nowhere to ask.
Where whereIs(const QString& path)
{
    if (path.startsWith(u"\\\\"))
        return Where::Network;
    if (path.size() < 3 || path[1] != u':' || path[2] != u'\\')
        return Where::Unknown;
    const wchar_t root[] {static_cast<wchar_t>(path[0].unicode()), L':', L'\\', L'\0'};
    const UINT type = ::GetDriveTypeW(root);
    return type == DRIVE_REMOTE ? Where::Network : type == DRIVE_UNKNOWN ? Where::Unknown : Where::Local;
}

// Whether the disk says a path is not there: not when its server does not
// answer, or it will not say (no access, no disk in the drive). As the
// launcher tells (SearchEngine's RecentOnDisk).
bool gone(const QString& path)
{
    if (::GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16())) != INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool dark()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

} // namespace

FavoritesSettings::FavoritesSettings(SettingsEditor* editor, QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &FavoritesSettings::refresh);
}

void FavoritesSettings::setStore(Favorites* favorites)
{
    if (m_store)
        disconnect(m_store, nullptr, this, nullptr);
    m_store = favorites;
    if (favorites) {
        connect(favorites, &Favorites::changed, this, [this] {
            refresh();
            lookForMissing();
        });
    }
    refresh();
    lookForMissing();
}

void FavoritesSettings::setCatalogs(AppCatalog* apps, SystemCatalog* places)
{
    m_apps = apps;
    m_places = places;
    if (apps)
        connect(apps, &AppCatalog::changed, this, &FavoritesSettings::refresh);
    if (places)
        connect(places, &SystemCatalog::changed, this, &FavoritesSettings::refresh);
    refresh();
}

QString FavoritesSettings::ownName(const Favorite& f) const
{
    if (f.kind == Favorite::Kind::App && m_apps) {
        if (const auto apps = m_apps->apps(); apps) {
            if (const AppInfo* app = findApp(*apps, f.path))
                return app->name;
        }
    }
    if (f.kind == Favorite::Kind::Place && m_places) {
        if (const auto places = m_places->places(); places) {
            if (const PlaceInfo* place = findPlace(*places, f.path))
                return place->name;
        }
    }
    return f.isFileOrFolder() ? lastName(f.path) : f.name;
}

void FavoritesSettings::refresh()
{
    QVariantList items;
    const std::shared_ptr<const AppList> apps = m_apps ? m_apps->apps() : nullptr;
    const std::shared_ptr<const PlaceList> places = m_places ? m_places->places() : nullptr;
    for (const Favorite& f : m_store ? m_store->items() : FavoriteList()) {
        QString icon;
        QString shownPath = f.path;
        bool missing = m_missing.contains(pathtext::pathKey(f.path));
        if (f.kind == Favorite::Kind::App) {
            const AppInfo* app = apps ? findApp(*apps, f.path) : nullptr;
            const bool packaged = app && (app->kind == AppKind::Store || app->kind == AppKind::System || app->kind == AppKind::Package);
            icon = FileIconProvider::appIconUrl(appIdOf(f.path), packaged, dark());
            shownPath = tr("应用");
            missing = apps && !apps->empty() && !app;
        } else if (f.kind == Favorite::Kind::Place) {
            const PlaceInfo* place = places ? findPlace(*places, f.path) : nullptr;
            if (place) {
                icon = place->icon.startsWith(u"app:") ? FileIconProvider::appIconUrl(place->icon.mid(4), true, dark())
                                                       : FileIconProvider::placeIconUrl(place->icon);
            }
            shownPath = tr("系统");
            missing = places && !places->empty() && !place;
        } else if (f.kind == Favorite::Kind::Command) {
            icon = FileIconProvider::commandIconUrl(f.command, f.workingDirectory, f.icon, dark());
            shownPath = f.command;
            missing = false;
        } else {
            icon = FileIconProvider::iconUrl(f.path, f.kind == Favorite::Kind::Folder);
        }
        items.append(QVariantMap {
            {u"path"_s, f.path},
            {u"name"_s, f.name},
            {u"ownName"_s, ownName(f)},
            {u"keyword"_s, f.keyword},
            {u"kind"_s, kindName(f.kind)},
            {u"icon"_s, icon},
            {u"shownPath"_s, shownPath},
            {u"missing"_s, missing},
            {u"command"_s, f.command},
            {u"workingDirectory"_s, f.workingDirectory},
            {u"commandIcon"_s, f.icon},
            {u"silent"_s, f.silent},
            {u"administrator"_s, f.runAsAdmin},
        });
    }
    if (items == m_items)
        return;
    m_items = std::move(items);
    emit itemsChanged();
}

void FavoritesSettings::lookForMissing()
{
    QStringList local;
    QStringList network;
    for (const Favorite& f : m_store ? m_store->items() : FavoriteList()) {
        if (!f.isFileOrFolder())
            continue;
        const Where where = whereIs(f.path);
        if (where == Where::Local)
            local.append(f.path);
        else if (where == Where::Network)
            network.append(f.path);
    }
    const int look = ++m_looks;
    // This PC's disks first, shown at once; then those on a network, which
    // may take long: until then they keep what was known of them.
    std::thread([self = QPointer(this), look, local, network] {
        const auto tell = [&](QSet<QString> missing, bool final) {
            QMetaObject::invokeMethod(qApp, [self, look, missing = std::move(missing), final, network]() mutable {
                if (!self || look != self->m_looks)
                    return;
                if (!final) {
                    for (const QString& path : network) {
                        if (self->m_missing.contains(pathtext::pathKey(path)))
                            missing.insert(pathtext::pathKey(path));
                    }
                }
                self->m_missing = std::move(missing);
                self->refresh();
            }, Qt::QueuedConnection);
        };
        QSet<QString> missing;
        for (const QString& path : local) {
            if (gone(path))
                missing.insert(pathtext::pathKey(path));
        }
        tell(missing, network.isEmpty());
        if (network.isEmpty())
            return;
        for (const QString& path : network) {
            // A server that does not answer would take Windows half a minute to say so.
            if (path.startsWith(u"\\\\") && !share::reachablePath(path.toStdWString()))
                continue;
            if (gone(path))
                missing.insert(pathtext::pathKey(path));
        }
        tell(missing, true);
    }).detach();
}

void FavoritesSettings::setMessage(const QString& message)
{
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

QString FavoritesSettings::keywordOwner(const QString& keyword) const
{
    if (!m_store)
        return {};
    const qsizetype i = typedFavoriteKeyword(m_store->items(), keyword);
    return i < 0 ? QString() : m_store->items()[i].name;
}

QString FavoritesSettings::validateKeyword(int index, const QString& word) const
{
    if (!word.isEmpty()) {
        if (!isValidFavoriteKeyword(word))
            return tr("关键词里不能有空格、引号、斜杠或冒号");
        for (qsizetype i = 0; i < m_store->items().size(); ++i) {
            const Favorite& other = m_store->items()[i];
            if (i != index && other.keyword.compare(word, Qt::CaseInsensitive) == 0)
                return tr("关键词“%1”已经给了“%2”").arg(other.keyword, other.name);
        }
        for (const WebShortcut& web : m_editor->m_settings.webShortcuts) {
            if (web.keyword.compare(word, Qt::CaseInsensitive) == 0)
                return tr("关键词“%1”已经给了网页搜索“%2”").arg(web.keyword, web.name);
        }
        if (const QString taken = m_editor->captureKeywordTaken(word); !taken.isEmpty())
            return taken;
    }
    return {};
}

QString FavoritesSettings::save(int index, const QString& name, const QString& keyword)
{
    if (!m_store || index < 0 || index >= m_store->items().size())
        return tr("这一项已经不在了");
    Favorite f = m_store->items()[index];
    const QString word = keyword.trimmed();
    if (const QString error = validateKeyword(index, word); !error.isEmpty())
        return error;
    f.keyword = word;
    f.name = name.trimmed().isEmpty() ? ownName(f) : name.trimmed();
    m_store->update(index, f);
    return {};
}

QString FavoritesSettings::saveCommand(int index, const QString& name, const QString& keyword,
    const QString& command, const QString& directory, const QString& icon, bool silent, bool administrator)
{
    if (!m_store || index < -1 || index >= m_store->items().size()
        || (index >= 0 && m_store->items()[index].kind != Favorite::Kind::Command))
        return tr("这一项已经不在了");
    const QString word = keyword.trimmed();
    if (name.trimmed().isEmpty() || word.isEmpty())
        return tr("请填写名称和关键词");
    if (const QString error = validateKeyword(index, word); !error.isEmpty())
        return error;
    const QString line = command.trimmed();
    if (line.isEmpty() || line.contains(QChar::Null) || line.contains(u'\n') || line.contains(u'\r')
        || (line.startsWith(u'"') && line.indexOf(u'"', 1) < 0))
        return tr("请填写有效的单行命令；含空格的程序路径请加双引号");
    const auto [program, arguments] = splitCommand(line);
    if (program.isEmpty() || program.contains(u"{query}"))
        return tr("{query} 只能放在程序后面的参数中");
    if (directory.contains(QChar::Null) || directory.contains(u'\n') || directory.contains(u'\r')
        || directory.contains(u'"'))
        return tr("工作文件夹请填写路径，不要加引号");
    if (!icon.isEmpty() && favoriteIconPng(icon).isEmpty())
        return tr("图标读不了，请重新选择或恢复自动提取");
    Favorite f = index >= 0 ? m_store->items()[index] : Favorite();
    f.kind = Favorite::Kind::Command;
    if (index < 0)
        f.path = u"winshun-command:"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
    f.name = name.trimmed();
    f.keyword = word;
    f.command = line;
    f.workingDirectory = QDir::toNativeSeparators(directory.trimmed());
    f.icon = icon;
    f.silent = silent;
    f.runAsAdmin = administrator;
    if (index < 0)
        m_store->add(f);
    else
        m_store->update(index, f);
    return {};
}

QString FavoritesSettings::pickCommand()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("选择程序"),
        {{tr("程序和脚本"), u"*.exe;*.com;*.bat;*.cmd;*.lnk"_s}, {tr("所有文件"), u"*.*"_s}});
    return path.isEmpty() ? QString() : u'"' + QDir::toNativeSeparators(path) + u'"';
}

QString FavoritesSettings::commandIconUrl(const QString& command, const QString& directory, const QString& icon, bool dark) const
{
    return FileIconProvider::commandIconUrl(command, directory, icon, dark);
}

int FavoritesSettings::pickCommandIcon()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("选择命令图标"),
        {{tr("图片或程序（PNG、ICO、SVG、EXE、快捷方式）"), u"*.png;*.ico;*.svg;*.exe;*.lnk"_s}});
    if (path.isEmpty())
        return 0;
    return loadCommandIcon(path);
}

int FavoritesSettings::loadCommandIcon(const QString& path)
{
    const int request = ++m_iconRequest;
    QThreadPool::globalInstance()->start([self = QPointer(this), path, request] {
        QImage image;
        QString error;
        const QString suffix = QFileInfo(path).suffix();
        if (suffix.compare(u"exe"_s, Qt::CaseInsensitive) == 0 || suffix.compare(u"lnk"_s, Qt::CaseInsensitive) == 0) {
            image = FileIconProvider::fileIcon(path, 128);
        } else {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                error = tr("读不了这个文件");
            } else if (file.size() > 1 << 20) {
                error = tr("图片太大了：请选择 1 MB 以内的 PNG、ICO 或 SVG");
            } else {
                const QByteArray bytes = file.read(1 << 20);
                svgicon::Problem problem = svgicon::Problem::None;
                image = svgicon::looksLikeSvg(bytes) ? svgicon::render(bytes, 128, &problem) : siteicon::decode(bytes);
                if (problem == svgicon::Problem::External)
                    error = tr("SVG 引用了外部文件或网址，请选择不含外部引用的图标");
            }
        }
        QString icon;
        if (!image.isNull()) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (siteicon::normalized(image).save(&buffer, "PNG"))
                icon = QString::fromLatin1("data:image/png;base64," + png.toBase64());
        }
        if (icon.isEmpty() && error.isEmpty())
            error = tr("无法读取图标，请选择有效的 PNG、ICO、SVG、程序或快捷方式");
        QMetaObject::invokeMethod(qApp, [self, request, icon, error] {
            if (self)
                emit self->commandIconReady(request, icon, error);
        }, Qt::QueuedConnection);
    });
    return request;
}

void FavoritesSettings::remove(int index)
{
    if (m_store && index >= 0 && index < m_store->items().size())
        m_store->remove(m_store->items()[index].path);
}

bool FavoritesSettings::move(int from, int to)
{
    return m_store && m_store->move(from, to);
}

void FavoritesSettings::addPicked(const QStringList& paths)
{
    if (!m_store || paths.isEmpty())
        return;
    FavoriteList favorites;
    for (const QString& picked : paths) {
        Favorite f;
        f.path = QDir::toNativeSeparators(QFileInfo(picked).absoluteFilePath());
        if (f.path.size() > 3 && f.path.endsWith(u'\\'))
            f.path.chop(1);
        f.kind = QFileInfo(f.path).isDir() ? Favorite::Kind::Folder : Favorite::Kind::File;
        f.name = lastName(f.path);
        favorites.append(f);
    }
    const int added = m_store->addAll(favorites);
    const int there = static_cast<int>(favorites.size()) - added;
    if (there == 0)
        setMessage(tr("已添加 %n 项", "", added));
    else if (added == 0)
        setMessage(there == 1 ? tr("它已经在收藏夹里了") : tr("这 %n 项都已在收藏夹里", "", there));
    else
        setMessage(tr("已添加 %n 项", "", added) + tr("，") + tr("%n 项本来就有", "", there));
}

void FavoritesSettings::addFolder()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    if (const QString path = shell::pickFolder(owner, tr("选择要收藏的文件夹")); !path.isEmpty())
        addPicked({path});
}

void FavoritesSettings::addFile()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    if (const QString path = shell::pickFile(owner, tr("选择要收藏的文件"), {{tr("所有文件"), u"*.*"_s}}); !path.isEmpty())
        addPicked({path});
}

void FavoritesSettings::addPaths(const QVariantList& urls)
{
    QStringList paths;
    for (const QVariant& v : urls) {
        const QString text = v.typeId() == QMetaType::QUrl ? v.toUrl().toLocalFile() : v.toString();
        const QString path = text.startsWith(u"file:", Qt::CaseInsensitive) ? QUrl(text).toLocalFile() : text;
        if (!path.isEmpty() && QFileInfo::exists(path))
            paths.append(path);
    }
    if (paths.isEmpty())
        setMessage(tr("只能收藏文件和文件夹"));
    addPicked(paths);
}

void FavoritesSettings::repick(int index)
{
    if (!m_store || index < 0 || index >= m_store->items().size())
        return;
    Favorite f = m_store->items()[index];
    if (!f.isFileOrFolder())
        return;
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString path = f.kind == Favorite::Kind::Folder
        ? shell::pickFolder(owner, tr("“%1”现在在哪里？").arg(f.name))
        : shell::pickFile(owner, tr("“%1”现在在哪里？").arg(f.name), {{tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return;
    const bool ownNamed = f.name == lastName(f.path); // not renamed: the new one's name then
    f.path = QDir::toNativeSeparators(path);
    if (f.path.size() > 3 && f.path.endsWith(u'\\'))
        f.path.chop(1);
    if (ownNamed)
        f.name = lastName(f.path);
    if (!m_store->update(index, f))
        setMessage(tr("“%1”已经在收藏夹里了").arg(lastName(f.path)));
}

void FavoritesSettings::open(int index)
{
    if (!m_store || index < 0 || index >= m_store->items().size())
        return;
    const Favorite& f = m_store->items()[index];
    switch (f.kind) {
    case Favorite::Kind::Command:
        shell::runCustom(f.command, f.workingDirectory, {}, {}, {f.silent, f.runAsAdmin});
        break;
    case Favorite::Kind::App:
        shell::launchApp(f.path);
        break;
    case Favorite::Kind::Place:
        if (m_places) {
            if (const auto places = m_places->places(); places) {
                if (const PlaceInfo* place = findPlace(*places, f.path))
                    shell::run(place->command);
            }
        }
        break;
    default:
        shell::open(f.path);
        break;
    }
}

int FavoritesSettings::indexOf(const QString& path) const
{
    return m_store ? static_cast<int>(m_store->indexOf(path)) : -1;
}

void FavoritesSettings::importFile()
{
    if (!m_store)
        return;
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString path = shell::pickFile(owner, tr("导入收藏夹"), {{tr("收藏夹（JSON）"), u"*.json"_s}, {tr("所有文件"), u"*.*"_s}});
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFile) {
        setMessage(tr("读不了这个文件"));
        return;
    }
    bool newer = false;
    const std::optional<FavoritesFile> read = readFavoritesFile(file.readAll(), &newer);
    if (!read) {
        setMessage(newer ? tr("这个文件是更新版本的 Win顺 导出的，请先更新 Win顺") : tr("这不是 Win顺 导出的收藏夹文件"));
        return;
    }
    // A keyword the web shortcuts have stays theirs.
    FavoriteList favorites = read->favorites;
    int keywordsTaken = 0;
    for (Favorite& f : favorites) {
        for (const WebShortcut& web : m_editor->m_settings.webShortcuts) {
            if (!f.keyword.isEmpty() && web.keyword.compare(f.keyword, Qt::CaseInsensitive) == 0) {
                f.keyword.clear();
                ++keywordsTaken;
            }
        }
    }
    const Favorites::Merged merged = m_store->merge(favorites);
    QStringList parts;
    parts.append(tr("新增 %n 项", "", merged.added));
    if (const int there = static_cast<int>(favorites.size()) - merged.added; there > 0)
        parts.append(tr("%n 项本来就有", "", there));
    if (merged.completed > 0)
        parts.append(tr("其中 %n 项补上了名称、关键词或图标", "", merged.completed));
    if (read->skipped > 0)
        parts.append(tr("%n 项用不了，跳过了", "", read->skipped));
    if (keywordsTaken > 0)
        parts.append(tr("%n 个关键词已经给了网页搜索，没有带上", "", keywordsTaken));
    if (read->badIcons > 0)
        parts.append(tr("%n 个图标读不了，已恢复自动提取", "", read->badIcons));
    setMessage(tr("已导入：") + parts.join(tr("，")));
}

void FavoritesSettings::exportFile()
{
    if (!m_store || m_store->items().isEmpty()) {
        setMessage(tr("还没有收藏可以导出"));
        return;
    }
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString path = shell::pickSaveFile(owner, tr("导出收藏夹"), tr("Win顺 收藏夹.json"), tr("收藏夹（JSON）"), u"json"_s);
    if (path.isEmpty())
        return;
    const QByteArray json = writeFavoritesFile(m_store->items());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
        setMessage(tr("写不了这个文件：%1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    setMessage(tr("已导出 %n 项到 %1", "", static_cast<int>(m_store->items().size())).arg(QDir::toNativeSeparators(path)));
}

} // namespace ws
