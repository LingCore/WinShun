#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

namespace ws {

// 全部: apps, files, folders, then file contents. 文件: files, then folders.
// 内容: file contents.
enum class Scope : int { All = 0, Files = 1, Content = 2 };

// What kind of installed app a result is (see AppCatalog), or None for a
// file or folder.
enum class AppKind : quint8 {
    None,
    Desktop, // a classic program, listed through its Start-menu shortcut
    Store, // packaged, from Microsoft Store
    System, // packaged, part of Windows
    Package, // packaged, installed another way (MSIX)
};

struct SearchResult {
    QString name;
    QString path; // full path; for an app "shell:AppsFolder\<id>", which opens it
    bool isDir = false;
    bool recent = false;

    // Installed apps only.
    AppKind app = AppKind::None;
    bool elevatable = false; // can run as administrator
    QString target; // the program it starts, or a packaged app's install folder; may be empty

    // Content search only.
    int line = 0; // 1-based
    QString snippet;
    int snippetMatchStart = -1;
    int snippetMatchLength = 0;

    bool isApp() const noexcept { return app != AppKind::None; }
    bool isPackagedApp() const noexcept { return app != AppKind::None && app != AppKind::Desktop; }
    // A desktop app's program (often a standalone exe) can be copied like a file.
    bool hasCopyableTarget() const noexcept { return app == AppKind::Desktop && !target.isEmpty(); }

    friend bool operator==(const SearchResult&, const SearchResult&) = default;
};

using SearchResults = QList<SearchResult>;

} // namespace ws

Q_DECLARE_METATYPE(ws::SearchResult)
