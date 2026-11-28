#include "UserAppsSettings.h"

#include "AppCatalog.h"
#include "FileIconProvider.h"
#include "SettingsEditor.h"
#include "platform/Shell.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QSet>
#include <QStyleHints>
#include <QWindow>

#include <windows.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

bool packaged(const AppInfo& app)
{
    return app.kind == AppKind::Store || app.kind == AppKind::System || app.kind == AppKind::Package;
}

QString iconOf(const AppInfo& app, bool dark)
{
    return app.portable ? FileIconProvider::iconUrl(app.target, false)
                        : FileIconProvider::appIconUrl(app.id, packaged(app), dark);
}

// Where an app is, as its row in the list says.
QString whereOf(const AppInfo& app)
{
    if (packaged(app))
        return UserAppsSettings::tr("Microsoft Store 或 Windows 自带的应用");
    if (app.target.isEmpty())
        return UserAppsSettings::tr("开始菜单里的应用");
    return QDir::toNativeSeparators(app.target);
}

} // namespace

UserAppsSettings::UserAppsSettings(SettingsEditor* editor, QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    connect(editor, &SettingsEditor::changed, this, &UserAppsSettings::refresh);
    refresh();
}

void UserAppsSettings::setCatalog(AppCatalog* apps)
{
    if (m_apps == apps)
        return;
    if (m_apps)
        disconnect(m_apps, nullptr, this, nullptr);
    m_apps = apps;
    if (apps)
        connect(apps, &AppCatalog::changed, this, &UserAppsSettings::refresh);
    refresh();
}

QStringList UserAppsSettings::folders() const
{
    return m_folders;
}

void UserAppsSettings::refresh()
{
    const Settings& settings = m_editor->m_settings;
    QStringList folders;
    for (const QString& folder : settings.portableFolders)
        folders.push_back(QDir::toNativeSeparators(folder));

    static const auto kNone = std::make_shared<const AppList>();
    const std::shared_ptr<const AppList> programs = m_apps ? m_apps->programs() : kNone;
    const std::shared_ptr<const AppList> apps = m_apps ? m_apps->apps() : kNone;
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;

    QHash<QString, QString> names;
    for (const AppName& named : settings.appNames)
        names.insert(named.path.toCaseFolded(), named.name);
    QSet<QString> hidden;
    for (const QString& path : settings.hiddenApps)
        hidden.insert(path.toCaseFolded());

    struct Row {
        int group; // the portable programs, the other apps named, those not found
        QVariantMap item;
    };
    std::vector<Row> rows;
    QSet<QString> listed;
    const auto add = [&](int group, const QString& path, const QString& own, const QString& where, const QString& icon,
                         bool portable, bool missing) {
        const QString key = path.toCaseFolded();
        const QString given = names.value(key);
        listed.insert(key);
        rows.push_back({group, {{u"path"_s, path}, {u"name"_s, given.isEmpty() ? own : given}, {u"ownName"_s, own},
            {u"named"_s, !given.isEmpty()}, {u"where"_s, where}, {u"icon"_s, icon}, {u"portable"_s, portable},
            {u"hidden"_s, hidden.contains(key)}, {u"missing"_s, missing}}});
    };

    for (const AppInfo& program : *programs) {
        const QString path = program.launchPath();
        // Not listed, nor hidden: the Start menu has it, and its app is the one named.
        if (!findApp(*apps, path) && !hidden.contains(path.toCaseFolded()))
            continue;
        add(0, path, program.name, QDir::toNativeSeparators(program.target), iconOf(program, dark), true, false);
    }
    for (const AppName& named : settings.appNames) {
        if (listed.contains(named.path.toCaseFolded()))
            continue;
        const AppInfo* app = findApp(*apps, named.path);
        if (!app && !isAppLaunchPath(named.path)) { // a program file named from its row that the Start menu has
            const auto it = std::ranges::find_if(*apps, [&](const AppInfo& a) {
                return a.kind == AppKind::Desktop && !a.hosted && a.target.compare(named.path, Qt::CaseInsensitive) == 0;
            });
            app = it == apps->end() ? nullptr : &*it;
        }
        if (app)
            add(1, named.path, app->ownName.isEmpty() ? app->name : app->ownName, whereOf(*app), iconOf(*app, dark),
                app->portable, false);
        else if (m_apps && m_apps->isLoaded())
            add(2, named.path, isAppLaunchPath(named.path) ? QString() : QFileInfo(named.path).completeBaseName(),
                isAppLaunchPath(named.path) ? tr("已卸载") : QDir::toNativeSeparators(named.path),
                FileIconProvider::iconUrl(isAppLaunchPath(named.path) ? u"app.exe"_s : named.path, false), false, true);
    }
    for (const QString& path : settings.hiddenApps) { // hidden, and gone since
        if (!listed.contains(path.toCaseFolded()) && m_apps && m_apps->isLoaded())
            add(2, path, QFileInfo(path).completeBaseName(), QDir::toNativeSeparators(path),
                FileIconProvider::iconUrl(path, false), true, true);
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        if (a.group != b.group)
            return a.group < b.group;
        return collator.compare(a.item.value(u"name"_s).toString(), b.item.value(u"name"_s).toString()) < 0;
    });
    QVariantList items;
    items.reserve(static_cast<qsizetype>(rows.size()));
    for (Row& row : rows)
        items.push_back(std::move(row.item));

    if (items == m_items && folders == m_folders)
        return; // the list's rows stay as they are (the settings changed elsewhere)
    m_items = std::move(items);
    m_folders = std::move(folders);
    emit itemsChanged();
}

void UserAppsSettings::addFolder()
{
    const HWND owner = m_editor->m_window ? reinterpret_cast<HWND>(m_editor->m_window->winId()) : nullptr;
    const QString folder = shell::pickFolder(owner, tr("选择放便携软件的文件夹"));
    if (folder.isEmpty() || m_folders.contains(QDir::toNativeSeparators(folder), Qt::CaseInsensitive))
        return;
    m_editor->m_settings.portableFolders.append(QDir::toNativeSeparators(folder));
    m_editor->commit();
}

void UserAppsSettings::removeFolder(int index)
{
    QStringList& folders = m_editor->m_settings.portableFolders;
    if (index < 0 || index >= folders.size())
        return;
    folders.removeAt(index);
    m_editor->commit();
}

void UserAppsSettings::setName(const QString& path, const QString& name)
{
    QString clean = cleanAppName(name);
    const auto row = std::ranges::find_if(m_items, [&](const QVariant& item) {
        return item.toMap().value(u"path"_s).toString().compare(path, Qt::CaseInsensitive) == 0;
    });
    if (row != m_items.end() && clean == row->toMap().value(u"ownName"_s).toString())
        clean.clear(); // its own name: none of the user's
    if (m_editor->m_settings.setAppName(path, clean))
        m_editor->commit();
}

void UserAppsSettings::setHidden(const QString& path, bool hide)
{
    QStringList& hidden = m_editor->m_settings.hiddenApps;
    const auto at = std::ranges::find_if(hidden, [&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; });
    if (hide == (at != hidden.end()))
        return;
    if (hide)
        hidden.append(path);
    else
        hidden.erase(at);
    m_editor->commit();
}

void UserAppsSettings::forget(const QString& path)
{
    Settings& settings = m_editor->m_settings;
    const bool named = settings.setAppName(path, QString());
    const qsizetype removed = settings.hiddenApps.removeIf([&](const QString& p) { return p.compare(path, Qt::CaseInsensitive) == 0; });
    if (named || removed > 0)
        m_editor->commit();
}

} // namespace ws
