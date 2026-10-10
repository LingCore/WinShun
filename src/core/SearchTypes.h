#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

#include <cstdint>

namespace ws {

// 全部: apps, files, folders, then file contents. 文件: files, then folders
// (the other way round if asked, see KindOrder); 文件夹 (Folders), shown
// within 文件: folders alone. 内容: file contents.
enum class Scope : int { All = 0, Files = 1, Content = 2, Folders = 3 };

// Which of files and folders a name search lists first. Each kind keeps its
// own best matches, so the second is not crowded out by the first.
enum class KindOrder : std::uint8_t { FilesFirst, FoldersFirst };

// How name matches rank: by how well they match (a tie goes to the one
// written last), or by when they were last written, newest first.
enum class RankBy : std::uint8_t { Match, Modified };

// What kind of installed app a result is (see AppCatalog), or of place in
// Windows (SystemCatalog), or None for a file or folder.
enum class AppKind : quint8 {
    None,
    Desktop, // a classic program, listed through its Start-menu shortcut
    Store, // packaged, from Microsoft Store
    System, // packaged, part of Windows
    Package, // packaged, installed another way (MSIX)
    // Places: opened by a command, with no file of their own.
    Setting, // a page of Settings
    ControlPanel, // a Control Panel item or task
    Security, // a page of Windows Security
    Tool, // a system tool or folder (WinShun's own list)
    Web, // a web page or a search on it, by keyword or name (WebShortcut)
};

struct SearchResult {
    QString name;
    QString path; // full path; for an app "shell:AppsFolder\<id>", which opens it
    bool isDir = false;
    bool recent = false;
    // Files and folders: when last written, in seconds since 1970 (UTC);
    // 0 when not known.
    std::uint32_t modified = 0;

    // Installed apps and places only. `path` stands for them in History.
    AppKind app = AppKind::None;
    bool elevatable = false; // can run as administrator
    // The program it starts, or a packaged app's install folder; may be
    // empty. A place's command ("ms-settings:display").
    QString target;
    QString icon; // places only: see PlaceInfo::icon

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
    int snippetMatchStart = -1;
    int snippetMatchLength = 0;

    // Web shortcuts only; `target` is the address to open.
    QString keyword; // "gh"
    QString words; // searched for; empty: the site itself

    // An app or a place: something to start, not a file (no deleting it).
    bool isApp() const noexcept { return app != AppKind::None; }
    bool isPlace() const noexcept { return app >= AppKind::Setting; }
    bool isWeb() const noexcept { return app == AppKind::Web; }
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
