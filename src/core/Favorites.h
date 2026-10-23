#pragma once

#include "SearchTypes.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace ws {

// Something its user keeps at hand: a folder, a file, an app or a place in
// Windows. Listed first in the launcher with nothing typed (in the dialog
// box's list after where its user is now: the folders open in file managers,
// the path copied), first among the matches when typed; its keyword alone
// ("gz") puts it on top, Enter opens it, Tab goes into a folder.
struct Favorite {
    enum class Kind : quint8 { Folder, File, App, Place, Command };
    Kind kind = Kind::Folder;
    // As SearchResult::path and History write it: a file's or folder's full
    // path ("D:\工作文档", "C:\"), an app's "shell:AppsFolder\<id>", a
    // place's "winshun-place:<key>".
    QString path;
    // What it is shown as: its own name when added, or what its user renamed
    // it to. Also what it is shown as once it is gone (an app uninstalled).
    QString name;
    QString keyword; // one word, matched ignoring case; empty: none
    QString command; // Command only: executable and arguments; {query} is one quoted argument
    QString workingDirectory; // Command only, optional

    bool isFileOrFolder() const noexcept { return kind == Kind::Folder || kind == Kind::File; }
    friend bool operator==(const Favorite&, const Favorite&) = default;
};

using FavoriteList = QList<Favorite>; // in the order shown

// The favorite a row stands for: a file's or folder's (a content match's:
// its file), an app's or a place's; none for a web shortcut's or a
// plugin's row.
std::optional<Favorite> favoriteOf(const SearchResult& row);

// Whether a scope lists a favorite of this kind: 全部 all, 文件 files and
// folders, 文件夹 folders, 内容 none.
bool scopeShows(Scope scope, Favorite::Kind kind) noexcept;

// The favorite whose keyword is all that was typed (trimmed, ignoring case):
// its index, or -1. "gz" and " GZ " find it; "gzx" and "gz 报告" do not.
qsizetype typedFavoriteKeyword(const FavoriteList& favorites, QStringView text);

// A command's keyword may be followed by words. Other favorites still match
// only the complete keyword. The words are passed as one Windows argument.
qsizetype typedCommandKeyword(const FavoriteList& favorites, QStringView text);
QString commandWords(const Favorite& favorite, QStringView text);
QString commandArguments(QStringView arguments, QStringView words);

// A keyword one can type and that is no path: not empty, no spaces or
// quotes (as a web shortcut's), no \ / or :.
bool isValidFavoriteKeyword(QStringView keyword) noexcept;

qsizetype indexOfFavorite(const FavoriteList& favorites, QStringView path); // ignoring case; -1: not there

// ---- the file ------------------------------------------------------------
// favorites.json in the data folder, also used for export. Version 1 for
// ordinary favorites; version 2 when commands are present, so older builds
// keep the file read-only instead of dropping commands when saving it.

QByteArray writeFavoritesFile(const FavoriteList& favorites);

struct FavoritesFile {
    FavoriteList favorites;
    int skipped = 0; // entries with no path, of no kind known, or there twice
};
// Null for a file that is not one of these (or not JSON), `newer` set when
// it is one a newer Win顺 wrote. A keyword that cannot be typed is dropped,
// one already taken by an earlier entry too.
std::optional<FavoritesFile> readFavoritesFile(QByteArrayView json, bool* newer = nullptr);

// The folders pinned in the dialog box's list before there were favorites
// (dialog-pins.txt, newest first): favorites in that order.
FavoriteList favoritesFromPins(const QStringList& pinnedFolders);

// Its user's favorites, kept in one file and saved on every change. Lives on
// the GUI thread; a search takes a copy of items().
class Favorites : public QObject {
    Q_OBJECT

public:
    // `filePath`: favorites.json. `pinsFile`: the dialog box's pins of old,
    // taken over once when there is no favorites.json yet.
    explicit Favorites(QString filePath, QString pinsFile = {}, QObject* parent = nullptr);

    // A file that cannot be read is kept aside as favorites.json.bad, not
    // overwritten; one a newer Win顺 wrote is read-only meanwhile.
    void load();

    const FavoriteList& items() const noexcept { return m_items; }
    qsizetype indexOf(QStringView path) const { return indexOfFavorite(m_items, path); }
    bool contains(QStringView path) const { return indexOf(path) >= 0; }

    bool add(Favorite favorite, qsizetype at = -1); // false: already there (or no path); at -1: at the end
    int addAll(const FavoriteList& favorites); // how many were not there yet
    // Brought in from a file (导入): the new ones added; one there already
    // given the keyword it lacks (if free) and the name its user gave it
    // there (if its own here). How many were added, and how many completed.
    struct Merged {
        int added = 0;
        int completed = 0;
    };
    Merged merge(const FavoriteList& favorites);
    bool remove(QStringView path);
    int removeAll(const QStringList& paths);
    // Its name, keyword or path ("重新选择"); false if the path is another
    // favorite's or nothing changed.
    bool update(qsizetype index, const Favorite& favorite);
    bool move(qsizetype from, qsizetype to);

signals:
    void changed();

private:
    bool insert(Favorite favorite, qsizetype at); // named, its keyword checked; not saved
    void save();

    QString m_filePath;
    QString m_pinsFile;
    FavoriteList m_items;
    bool m_readOnly = false; // a newer Win顺's file
};

} // namespace ws
