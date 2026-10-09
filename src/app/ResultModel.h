#pragma once

#include "SearchTypes.h"

#include <QAbstractListModel>
#include <QColor>
#include <QSet>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

namespace ws {

// The visible result list. Holds at most a few hundred rows; HTML for match
// highlighting is generated lazily for the rows QML actually draws.
//
// Never reset: new results are written over the rows already there, so the
// view keeps its delegates (no rebuilt rows, lost hover or reloaded icons)
// and an unchanged list signals nothing at all.
//
// Several rows can be selected (Ctrl / Shift with the mouse or arrows). The
// selection is kept by path, so it stays with its files when a refresh moves
// rows; files that leave the list leave it too.
class ResultModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Launcher")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor NOTIFY highlightColorChanged FINAL)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        NameHtmlRole,
        FolderRole,
        PathRole,
        IsDirRole,
        IconRole,
        SnippetHtmlRole,
        LineRole,
        LocationRole, // of a content match: "第 3 行", "第 2 页", "Sheet1 第 12 行"
        RecentRole,
        IsAppRole, // an app or a place in Windows
        PlaceRole, // a place: a page of Settings, a Control Panel task, a system tool, a web page
        WebRole, // a web shortcut's site, or a search on it
        PackagedAppRole,
        ElevatableRole, // run as administrator
        RevealableRole, // has a file or folder to open the location of
        CopyableRole, // has a file to copy for pasting in Explorer
        SelectedRole,
    };

    explicit ResultModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const noexcept { return static_cast<int>(m_items.size()); }
    const SearchResult* at(int row) const;
    int indexOf(const QString& path) const; // -1 if no row has that path
    Q_INVOKABLE QString pathAt(int row) const; // empty if out of range

    void assign(SearchResults items, QStringList highlights); // changes only the rows that differ
    void update(int row, const SearchResult& item);
    void append(const SearchResults& items);
    bool remove(const QString& path); // false if no row has that path
    int removeAll(const QSet<QString>& paths); // number of rows removed
    void clear();
    void retranslate(); // the language changed: apps' second line, web searches' title, places of content matches
    static QString location(const SearchResult& r); // LocationRole

    QColor highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QColor& color);

    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    const QSet<QString>& selectedPaths() const { return m_selected; }
    Q_INVOKABLE bool isSelected(int row) const;
    void setSelection(QSet<QString> paths); // of rows in the list; others are left out
    SearchResults selection() const; // in list order, each file once

signals:
    void countChanged();
    void highlightColorChanged();
    void selectionChanged();

private:
    void keepSelectionInList(); // after rows went away
    SearchResults m_items;
    QStringList m_highlights;
    QColor m_highlightColor {0x00, 0x5F, 0xB8};
    QSet<QString> m_selected; // paths
};

} // namespace ws
