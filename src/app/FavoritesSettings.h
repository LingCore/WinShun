#pragma once

#include "Favorites.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace ws {

class AppCatalog;
class SettingsEditor;
class SystemCatalog;

// Settings → 收藏夹: the favorites (Favorites, the app's) as a list to
// rename, give keywords, put in order, remove, and to add to (a folder or a
// file picked, or dropped on the page), import and export. Whether each is
// still there is asked off the GUI thread (a USB stick, a disk asleep); one
// on a network is not asked at all.
class FavoritesSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the settings editor")
    // {path, name, keyword, kind ("folder", "file", "app", "place"), icon,
    // shownPath, missing}, in their order.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL) // what came of the last add, import or export

public:
    explicit FavoritesSettings(SettingsEditor* editor, QObject* parent = nullptr);

    void setStore(Favorites* favorites); // the app's; outlives the settings window
    void setCatalogs(AppCatalog* apps, SystemCatalog* places); // names and icons of the apps and places

    QVariantList items() const { return m_items; }
    QString message() const { return m_message; }

    // The favorite's name and keyword: why they cannot be saved, or empty
    // once they are. An empty name: its own (the file's, the app's).
    Q_INVOKABLE QString save(int index, const QString& name, const QString& keyword);
    Q_INVOKABLE QString saveCommand(int index, const QString& name, const QString& keyword,
        const QString& command, const QString& directory);
    Q_INVOKABLE QString pickCommand();
    Q_INVOKABLE void remove(int index);
    Q_INVOKABLE bool move(int from, int to);
    Q_INVOKABLE void addFolder(); // asks for it
    Q_INVOKABLE void addFile();
    Q_INVOKABLE void addPaths(const QVariantList& urls); // dropped on the page: "file:///D:/x" or paths
    Q_INVOKABLE void repick(int index); // one that is gone: where it is now
    Q_INVOKABLE void open(int index);
    Q_INVOKABLE int indexOf(const QString& path) const;
    Q_INVOKABLE void importFile();
    Q_INVOKABLE void exportFile();
    Q_INVOKABLE void clearMessage() { setMessage({}); }

    // The favorite whose keyword this is (its name), or empty: for the web
    // shortcuts, which may not take it.
    QString keywordOwner(const QString& keyword) const;

signals:
    void itemsChanged();
    void messageChanged();

private:
    QString validateKeyword(int index, const QString& word) const;
    void refresh(); // m_items from the store as it is
    void lookForMissing(); // in the background, then refresh()
    void setMessage(const QString& message);
    QString ownName(const Favorite& favorite) const; // the file's or folder's, the app's, the place's
    void addPicked(const QStringList& paths);

    SettingsEditor* m_editor;
    QPointer<Favorites> m_store;
    AppCatalog* m_apps = nullptr;
    SystemCatalog* m_places = nullptr;
    QVariantList m_items;
    QSet<QString> m_missing; // case-folded paths known not to be there
    int m_looks = 0; // the newest lookForMissing() wins
    QString m_message;
};

} // namespace ws
