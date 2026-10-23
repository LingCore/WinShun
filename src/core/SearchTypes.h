#pragma once

#include "SwitchIntent.h"

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>

#include <cstdint>
#include <memory>
#include <utility>

namespace ws {

// 全部: apps, files, folders, then file contents. 文件: files, then folders
// (the other way round if asked, see KindOrder); 文件夹 (Folders), shown
// within 文件: folders alone. 内容: file contents.
enum class Scope : int { All = 0, Files = 1, Content = 2, Folders = 3 };

// Which of files and folders a name search lists first. Each kind keeps its
// own best matches, so the second is not crowded out by the first.
enum class KindOrder : std::uint8_t { FilesFirst, FoldersFirst };

// How files and folders rank, found by name or content: by how well they match (a tie goes to the one
// written last), or by when they were last written, newest or oldest first.
// Not known when (0): last either way.
enum class RankBy : std::uint8_t { Match, Newest, Oldest };

// What the launcher lists with nothing typed: what was opened last
// (History), or the files written last (the folders, in 文件夹).
enum class StartList : std::uint8_t { Opened, Modified };

// How files and folders are listed in the search panel: as ranked (RankBy),
// or by one of its columns, up or down. By name and by size, a tie goes as
// ranked; by path, a folder's files stay together (its path, then the
// name). A size or a time not known comes last either way. By type, the
// extension, then the name (#44: the launcher's own menu sorts by it; the
// panel has no column for it).
enum class SortKey : std::uint8_t { Rank, Name, Path, Size, Modified, Type };
struct SortOrder {
    SortKey key = SortKey::Rank;
    bool descending = false;
    friend bool operator==(const SortOrder&, const SortOrder&) = default;
};

// A kind of file 文件 can be narrowed to (图片: jpg, png…), as the strip
// under the search box offers them; its user's to name and change.
struct FileType {
    QString name; // empty for a built-in one not renamed: named in the language shown (by id)
    QList<QString> extensions; // without the dot, lower case
    QString id; // a built-in one's ("pictures"); empty for its user's own
    bool operator==(const FileType&) const = default;
};
using FileTypes = QList<FileType>;

// What kind of installed app a result is (see AppCatalog), or of place in
// Windows (SystemCatalog), or None for a file or folder.
enum class AppKind : quint8 {
    None,
    Desktop, // a classic program, listed through its Start-menu shortcut
    Store, // packaged, from Microsoft Store
    System, // packaged, part of Windows
    Package, // packaged, installed another way (MSIX)
    Plugin, // a plugin's answer (PluginRow): an app in the list's terms, never a place
    // Places: opened by a command, with no file of their own.
    Setting, // a page of Settings
    ControlPanel, // a Control Panel item or task
    Security, // a page of Windows Security
    Tool, // a system tool or folder (WinShun's own list)
    Web, // a web page or a search on it, by keyword or name (WebShortcut)
};

// What a plugin said about a row of its own (the plugin system, src/plugin):
// where it came from, what it shows, and what its actions are. Rows are
// copied about, and few are a plugin's, so a SearchResult only points at it.
struct PluginRow {
    QString pluginId;
    QString commandId;
    QString itemId; // the item's own, or made up from its place in the answer
    QString source; // the plugin's name, in the interface's language: the badge
    QString subtitle;
    QString icon; // as the item wrote it ("file:C:\…", "glyph:U+E8E5", "icon.png"), else the command's or plugin's
    QString autocomplete; // Tab puts this in the box
    QString copy; // Ctrl+C; empty: the title
    QString preview;
    // Done by Win顺 without asking the plugin (the default action).
    enum class OpenKind : quint8 { None, Path, Url };
    OpenKind openKind = OpenKind::None;
    QString openTarget;
    QString openArgs; // Path: a program's arguments
    QString openCwd;
    QString setQuery; // put this in the box
    QString searchQuery; // search for this instead (a zero-code `search`)
    QString searchScope; // all | files | folders | content
    struct Action {
        QString id;
        QString title; // in the interface's language
        QString key; // "Enter", "Ctrl+Enter", "Shift+Enter"
        friend bool operator==(const Action&, const Action&) = default;
    };
    QList<Action> actions; // the item's, else the command's; may be empty (one unnamed default)
    bool valid = true; // false: Enter does nothing (a line of information)
    int score = 50; // 0–100, as the plugin gave it (or 50)
    QJsonObject item; // the item as it came: goes back to the plugin with an action
    friend bool operator==(const PluginRow&, const PluginRow&) = default;
};

// Held by the rows that are a plugin's; compared by what it says, not by
// which object says it (a plugin answering again makes new ones).
struct PluginRowRef {
    std::shared_ptr<const PluginRow> row;
    const PluginRow* operator->() const noexcept { return row.get(); }
    const PluginRow& operator*() const noexcept { return *row; }
    explicit operator bool() const noexcept { return row != nullptr; }
    friend bool operator==(const PluginRowRef& a, const PluginRowRef& b)
    {
        return a.row == b.row || (a.row && b.row && *a.row == *b.row);
    }
};

struct SearchResult {
    QString name;
    QString path; // full path; for an app "shell:AppsFolder\<id>", which opens it
    bool isDir = false;
    bool recent = false;
    // 全部: named just what was typed (arrange's second run of files; an app
    // so named). With `recent`, what a plugin's rows never go above.
    bool exact = false;
    // Files and folders: when last written, in seconds since 1970 (UTC);
    // 0 when not known.
    std::uint32_t modified = 0;
    // A file's size in bytes (as the index has it: within a hundred-thousandth
    // past 16 MB, FileSize); -1 for a folder, or when not known.
    std::int64_t size = -1;
    bool offline = false; // on a network share that cannot be reached now (ShareIndexer::offline)
    // Found by how its name reads, not by what was typed (年度报告 for
    // 年度报高; homophoneQueries): listed under a heading that says so.
    bool approximate = false;
    // One of its user's favorites (Favorites): listed first, marked 收藏.
    // With `recent`, what a plugin's rows never go above.
    bool favorite = false;
    QString favoriteKeyword; // the favorite's keyword, shown beside it; empty: none
    bool missing = false; // a favorite no longer there (deleted, moved, uninstalled): shown dimmed

    // Installed apps and places only. `path` stands for them in History.
    AppKind app = AppKind::None;
    // 全部: how well an app, place or web shortcut matched (searchApps and
    // the like; a typed web keyword's row above them all), so that a
    // plugin's rows (0–100, the same scale) can take their place among them.
    int score = 0;
    bool elevatable = false; // can run as administrator
    // The program it starts, or a packaged app's install folder; may be
    // empty. A place's command ("ms-settings:display").
    QString target;
    QString workingDirectory; // a custom command's working directory; otherwise empty
    QString icon; // places: see PlaceInfo::icon; web: the home page, whose site's icon it shows

    // Places only: the system switch the row stands for (PlaceInfo::switchId,
    // "bluetooth"), else empty, and what is asked of it: Toggle for a plain
    // row, what "关蓝牙" or "音量 30" says for a typed one. Its state is not
    // here (a change of state would make the row a new one in the list): the
    // app asks Switches.
    QString switchId;
    SwitchIntent intent;
    // A switch listed above the recent items with nothing typed, because it
    // is on and must be seen to be ended (Request::pinnedPlaces). Not the
    // row Enter acts on when the window opens.
    bool statusRow = false;

    // Content search only.
    int line = 0; // 1-based: of a text file, or of the text read out of a document
    enum class Where : quint8 {
        Line, // of a text file
        Document, // a document's: no page known
        Page,
        Slide,
        Row, // of `sheet`
    };
    Where where = Where::Line;
    int placeNumber = 0; // page, slide or row
    QString sheet;
    QString snippet;
    QList<std::pair<qsizetype, qsizetype>> snippetMatches; // the terms in it: start, length
    int relevance = 0; // how well it matches, higher first (contentRelevance)

    // Web shortcuts only; `target` is the address to open.
    QString keyword; // "gh"
    QString words; // searched for; empty: the site itself
    // One of the site's search suggestions for the words typed after its
    // keyword (WebShortcut::suggest), listed under the search's own row.
    bool suggestion = false;

    // Plugin rows only (AppKind::Plugin); `path` is "winshun-plugin:…" (pluginPath).
    PluginRowRef plugin;
    // A screenshot of the history (the launcher's jt): its clipboard entry;
    // `target` its picture's file, `modified` when it was taken. A plugin
    // row otherwise (Win顺's own: Launcher::screenshotRows).
    qint64 clipId = 0;

    // An app or a place: something to start, not a file (no deleting it).
    bool isApp() const noexcept { return app != AppKind::None; }
    bool isPlace() const noexcept { return app >= AppKind::Setting; }
    bool isCommand() const noexcept { return path.startsWith(u"winshun-command:"); }
    bool isWeb() const noexcept { return app == AppKind::Web; }
    bool isPlugin() const noexcept { return app == AppKind::Plugin; }
    bool isPackagedApp() const noexcept
    {
        return app == AppKind::Store || app == AppKind::System || app == AppKind::Package;
    }
    // A desktop app's program (often a standalone exe) can be copied like a file.
    bool hasCopyableTarget() const noexcept { return app == AppKind::Desktop && !target.isEmpty(); }

    friend bool operator==(const SearchResult&, const SearchResult&) = default;
};

using SearchResults = QList<SearchResult>;

} // namespace ws

Q_DECLARE_METATYPE(ws::SearchResult)
