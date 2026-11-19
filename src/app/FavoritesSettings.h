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
    // {path, name, keyword, kind ("folder", "file", "app", "place", "command"), icon,
    // shownPath, missing, modifiedText, sizeText}, in their order.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL) // what came of the last add, import or export
    // GitHub #397: the column they are sorted by ("name", "modified", "type",
    // "size"; "" none, by hand), which way, and whether the order from
    // before the first sort is kept to go back to (恢复默认).
    Q_PROPERTY(QString sort READ sort NOTIFY orderChanged FINAL)
    Q_PROPERTY(bool sortDescending READ sortDescending NOTIFY orderChanged FINAL)
    Q_PROPERTY(bool canRestoreOrder READ canRestoreOrder NOTIFY orderChanged FINAL)

public:
    explicit FavoritesSettings(SettingsEditor* editor, QObject* parent = nullptr);

    void setStore(Favorites* favorites); // the app's; outlives the settings window
    void setCatalogs(AppCatalog* apps, SystemCatalog* places); // names and icons of the apps and places

    QVariantList items() const { return m_items; }
    QString message() const { return m_message; }
    QString sort() const;
    bool sortDescending() const;
    bool canRestoreOrder() const { return m_store && m_store->hasManualOrder(); }

    // A column header clicked: sorted by it (names and types A to Z, dates
    // and sizes newest and largest first, as Explorer), the other way when
    // clicked again, and the third time back to the order of before. By
    // date or size once the disk has been asked, off the GUI thread.
    Q_INVOKABLE void sortBy(const QString& column);
    Q_INVOKABLE void restoreOrder();

    // The favorite's name and keyword: why they cannot be saved, or empty
    // once they are. An empty name: its own (the file's, the app's).
    Q_INVOKABLE QString save(int index, const QString& name, const QString& keyword);
    Q_INVOKABLE QString saveCommand(int index, const QString& name, const QString& keyword,
        const QString& command, const QString& directory, const QString& icon = {}, bool silent = false,
        bool administrator = false, bool searchFiles = false, bool fileMenu = false);
    Q_INVOKABLE QString pickCommand();
    Q_INVOKABLE QString commandIconUrl(const QString& command, const QString& directory, const QString& icon, bool dark) const;
    Q_INVOKABLE int pickCommandIcon(); // 0: cancelled; otherwise commandIconReady carries this request
    int loadCommandIcon(const QString& path); // same worker, after a native picker or in an isolated preview
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
    void orderChanged();
    void commandIconReady(int request, const QString& icon, const QString& error);

private:
    QString validateKeyword(int index, const QString& word) const;
    void refresh(); // m_items from the store as it is
    // Which are gone, and the others' dates and sizes: in the background,
    // then refresh() and the sort asked for meanwhile.
    void lookForMissing();
    void applyPendingSort();
    void setMessage(const QString& message);
    QString ownName(const Favorite& favorite) const; // the file's or folder's, the app's, the place's
    void addPicked(const QStringList& paths);

    SettingsEditor* m_editor;
    QPointer<Favorites> m_store;
    AppCatalog* m_apps = nullptr;
    SystemCatalog* m_places = nullptr;
    QVariantList m_items;
    QSet<QString> m_missing; // case-folded paths known not to be there
    FavoriteFactsMap m_facts; // the others' dates and sizes, as the disk last said
    int m_looks = 0; // the newest lookForMissing() wins
    // By date or size, asked for and waiting for the disk: shown in the header meanwhile.
    FavoriteSort m_pendingSort = FavoriteSort::None;
    bool m_pendingDescending = false;
    int m_iconRequest = 0;
    QString m_message;
};

} // namespace ws
