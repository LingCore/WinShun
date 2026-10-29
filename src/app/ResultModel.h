#pragma once

#include "SearchTypes.h"

#include <QAbstractListModel>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

namespace ws {

class Switches;

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
    // Typed in a folder's window (GitHub #110): the first row that is not in
    // that folder (ElsewhereRole), where the list's caption goes; -1: none.
    Q_PROPERTY(int elsewhereRow READ elsewhereRow NOTIFY elsewhereRowChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        NameHtmlRole,
        FolderRole,
        PathRole,
        IsDirRole,
        IconRole,
        // A picture's, video's, PDF's thumbnail (ThumbnailProvider), shown over
        // the icon once loaded; "" for none, or when they are turned off.
        ThumbnailRole,
        GalleryThumbnailRole, // gallery previews also work when list thumbnails are off
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
        ModifiedRole, // when a file or folder was last written, briefly: "昨天 14:32"; empty when not known
        ModifiedFullRole, // the same in full, for its tip
        ModifiedDateRole, // the panel's column: "2026-10-14 14:32"; empty when not known
        SizeRole, // a file's size, briefly: "1.2 MB"; empty for a folder, an app, or when not known
        CreatedDateRole,
        TypeNameRole,
        FolderNameRole,
        OfflineRole, // on a network share that cannot be reached now
        PluginRole, // a plugin's row (SearchResult::plugin)
        SourceRole, // ... the plugin's name, for its badge; empty otherwise
        FavoriteRole, // one of its user's favorites
        FavoriteKeywordRole, // ... its keyword; empty: none
        MissingRole, // a favorite no longer there
        // Where the terms the name lacks are in its folder (found there, "弯管机" in
        // F:\模型\3轴弯管机模型): start, length, start, length...; none for a content match.
        FolderMatchesRole,
        NameMatchesRole, // the same for the terms in a file's or folder's name; none for an app
        SuggestionRole, // a web search on one of the site's suggestions (SearchResult::suggestion)
        // A system switch's row (SearchResult::switchId), as Switches has it
        // now; typed defaults ("", -1, false) for any other row.
        SwitchIdRole,
        SwitchOnRole, // -1 not known, 0 off, 1 on, 2 mixed
        SwitchLevelRole, // the volume, 0-100; -1 for the others
        SwitchBusyRole, // being written
        SwitchNoteRole, // the second line: a device, why it is not ready
        SwitchErrorRole, // ... in red: why the last write failed
        SwitchReadyRole, // draws its switch (ready, or not read yet); else the row is an ordinary place
        StatusRowRole, // listed above the recent items because it is on (SearchResult::statusRow)
        ElsewhereRole, // typed in a folder's window: not in that folder (SearchResult::elsewhere)
        // A plugin's row that has its own text to copy (`copy`), or whose Enter
        // copies its title: it shows the copy button (Ctrl+C copies any row's).
        PluginCopiesRole,
        // A plugin's row: its second line while the keyboard is on another
        // row (PluginRow::subtitleAway); empty: the folder line as ever.
        FolderAwayRole,
    };

    explicit ResultModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const noexcept { return static_cast<int>(m_items.size()); }
    int elsewhereRow() const noexcept { return m_elsewhereRow; }
    const SearchResult* at(int row) const;
    Q_INVOKABLE int indexOf(const QString& path) const; // -1 if no row has that path
    Q_INVOKABLE QString pathAt(int row) const; // empty if out of range
    // The row the highlight starts on when rows are new: the first, but not
    // a status row (keep awake, listed above the recent items), which Enter
    // must not act on at once. 0 when there is no other.
    Q_INVOKABLE int defaultRow() const;
    // Per row (the first 64), what Enter copies when the row's title is the
    // payload: a plugin's row that opens, searches and sets nothing and says
    // what to copy (a translation, a sum). Empty for every other row.
    Q_INVOKABLE QStringList copyTexts() const;
    // `text` with `matches` (NameMatchesRole, FolderMatchesRole) highlighted,
    // shortened in the middle to `width` in `font` as Text.ElideMiddle would,
    // but around them: they stay in sight, with the start and the end of the
    // text. Without matches, in its middle: a name keeps its extension and
    // what is just before it (elide::shortenedHtml).
    Q_INVOKABLE static QString elidedHtml(
        const QString& text, const QVariantList& matches, qreal width, const QFont& font, const QColor& color);

    void assign(SearchResults items, QStringList highlights); // changes only the rows that differ
    void update(int row, const SearchResult& item);
    void append(const SearchResults& items);
    void insert(int row, const SearchResult& item);
    bool remove(const QString& path); // false if no row has that path
    void removeRange(int row, int rows); // `rows` rows from `row` (fewer at the end)
    int removeAll(const QSet<QString>& paths); // number of rows removed
    void clear();
    void retranslate(); // the language changed: apps' second line, web searches' title, places of content matches
    static QString location(const SearchResult& r); // LocationRole
    // ModifiedRole, ModifiedFullRole: in the default locale, against `now` (local time).
    static QString modifiedText(std::uint32_t modified, const QDateTime& now, bool withTime = false);
    void setShowModifiedTime(bool on);
    static QString modifiedFullText(std::uint32_t modified);

    QColor highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QColor& color);
    void setThumbnails(bool on); // ThumbnailRole: Settings::thumbnails
    // The rows of system switches take their state from it, and follow it.
    void setSwitches(Switches* switches);
    // A row's title or footer words for what it asks of its switch ("关闭蓝牙",
    // "音量设为 30%"): `title` for the row (a plain row is just its name), else
    // for the footer's Enter ("切换到浅色", "音量设为 30"). `intent` is what
    // Enter will do (Switches::resolve) for the footer, the row's own for the title.
    static QString switchWords(const SearchResult& row, const SwitchIntent& intent, bool title);
    // The rows of switch `id` are drawn again: its state, or the minutes in its second line, are different.
    void switchChanged(const QString& id);

    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    const QSet<QString>& selectedPaths() const { return m_selected; }
    Q_INVOKABLE bool isSelected(int row) const;
    void setSelection(QSet<QString> paths); // of rows in the list; others are left out
    SearchResults selection() const; // in list order, each file once

signals:
    void switchNudged(const QString& id); // asked for what it already is: its row's switch lights up
    void countChanged();
    void highlightColorChanged();
    void selectionChanged();
    void elsewhereRowChanged();

private:
    QString switchTitle(const SearchResult& r) const; // a switch row's title when it is not its name, else empty
    void keepSelectionInList(); // after rows went away
    void findElsewhere(); // after rows changed: elsewhereRow
    int m_elsewhereRow = -1;
    SearchResults m_items;
    QStringList m_highlights;
    QColor m_highlightColor {0x00, 0x5F, 0xB8};
    bool m_thumbnails = true;
    bool m_showModifiedTime = false;
    QSet<QString> m_selected; // paths
    QPointer<Switches> m_switches;
};

} // namespace ws
