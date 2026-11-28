#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration>

namespace ws {

class AppCatalog;
class SettingsEditor;

// The settings' 搜索范围 › 应用 tab (#505): the folders of portable programs,
// the programs found in them, and the names given to apps (Settings' [Apps];
// AppCatalog lists them).
class UserAppsSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the settings editor")
    Q_PROPERTY(QStringList folders READ folders NOTIFY itemsChanged FINAL)
    // {path (its launch path), name (as shown), ownName, named (the user's
    // name), where (its file, or what it is), icon, portable (one of the
    // folders' or a program file named), hidden, missing (named, not found)}:
    // the portable programs, then the other apps named, each by name.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged FINAL)

public:
    explicit UserAppsSettings(SettingsEditor* editor, QObject* parent = nullptr);

    void setCatalog(AppCatalog* apps); // the app's; outlives the settings window

    QStringList folders() const;
    QVariantList items() const { return m_items; }

    Q_INVOKABLE void addFolder();
    Q_INVOKABLE void removeFolder(int index);
    // `name` for the app of that launch path; its own name again when empty
    // or its own (cleanAppName).
    Q_INVOKABLE void setName(const QString& path, const QString& name);
    Q_INVOKABLE void setHidden(const QString& path, bool hidden);
    Q_INVOKABLE void forget(const QString& path); // a missing one's name, and hiding, gone from the settings

signals:
    void itemsChanged();

private:
    void refresh();

    SettingsEditor* m_editor;
    AppCatalog* m_apps = nullptr;
    QVariantList m_items;
    QStringList m_folders;
};

} // namespace ws
