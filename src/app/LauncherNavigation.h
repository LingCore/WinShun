#pragma once

#include "PathText.h"
#include "SearchTypes.h"

#include <optional>

namespace ws::navigation {

enum class TabAction { SwitchScope, Complete, BrowseFolder, None };

inline bool canBrowseFolder(const SearchResult& result) noexcept
{
    return result.isDir && !result.isApp() && !result.offline && !result.missing;
}

// The action and the footer use the same policy. Favorites only change
// Tab when explicitly enabled; keyword and web completion stay independent.
// GitHub #361: `stepsIn`, a folder the user chose or one that completes the
// path typed (Launcher::stepsInto), is stepped into, ahead of the hint row
// of a keyword elsewhere in the list; a folder the list only put under the
// keyboard by itself keeps Tab for the scopes (GitHub #81). Not in 内容.
inline TabAction tabAction(Scope scope, bool keywordMode, bool enterFavoriteFolder, const SearchResult* row,
    bool hasCompletion, bool stepsIn = false) noexcept
{
    const bool folder = !keywordMode && row && canBrowseFolder(*row);
    if (folder && scope == Scope::All && enterFavoriteFolder && row->favorite)
        return TabAction::BrowseFolder;
    if (folder && stepsIn && scope != Scope::Content)
        return TabAction::BrowseFolder;
    if (hasCompletion)
        return TabAction::Complete;
    return keywordMode ? TabAction::None : TabAction::SwitchScope;
}

// The words that list what `folder` holds: its path and a backslash
// ("D:\Work" -> "D:\Work\"; "D:\" stays).
inline QString browseQuery(const QString& folder)
{
    return folder.endsWith(u'\\') ? folder : folder + u'\\';
}

// Whether `path` is `folder` or lies inside it, case and a trailing
// backslash aside ("D:\Work" holds "D:\Work\合同", not "D:\Workshop").
inline bool within(const QString& folder, const QString& path)
{
    const QString f = pathtext::pathFromText(folder);
    const QString p = pathtext::pathFromText(path);
    if (f.isEmpty() || p.isEmpty() || !p.startsWith(f, Qt::CaseInsensitive))
        return false;
    return p.size() == f.size() || f.endsWith(u'\\') || p[f.size()] == u'\\';
}

// GitHub #361: whether `folder` completes the path being typed (`typed`,
// pathtext::splitTyped of the box) as a shell or Explorer's address bar
// completes it: a folder right in the folder typed, while a part of a name
// is typed after it ("D:\Wo" on D:\Work). Not with nothing after the last
// backslash (the folder's own listing), nor with a space right after it
// ("D:\Work\ 报告": words to look for in that folder).
inline bool completesPath(const pathtext::TypedPath& typed, const QString& folder)
{
    return !typed.folder.isEmpty() && !typed.name.isEmpty() && !typed.name.front().isSpace()
        && pathtext::isDirectChild(typed.folder, folder);
}

// GitHub #361: where Ctrl+Backspace takes a path typed or stepped into
// (`typed`, pathtext::splitTyped of the box), a level at a time: with a name
// being typed after the folder, the folder alone ("D:\Work\合" ->
// "D:\Work\"), as DialogBar::up(); else the folder above, where the one
// left is to be chosen again ("D:\Work\合同\" -> "D:\Work\", row
// "D:\Work\合同"). None for what is no path, or a drive's or a share's root
// (nothing above it: the box edits as ever).
struct Up {
    QString query;
    QString row; // empty: none to choose
};
inline std::optional<Up> upFrom(const pathtext::TypedPath& typed)
{
    if (typed.folder.isEmpty())
        return std::nullopt;
    if (!typed.name.isEmpty())
        return Up {browseQuery(typed.folder), {}};
    const QString& folder = typed.folder; // "D:\Work", "D:\", "\\server\share\dir"
    const qsizetype slash = folder.endsWith(u'\\') ? -1 : folder.lastIndexOf(u'\\');
    const QString parent = slash < 0 ? QString() : folder.left(slash == 2 ? 3 : slash);
    if (parent.isEmpty() || pathtext::splitTyped(browseQuery(parent)).folder.isEmpty())
        return std::nullopt; // "\\server" is no folder
    return Up {browseQuery(parent), folder};
}

// A keyword's list changed by itself (a rerun, a refresh: Launcher::
// followCurrentRow), from the rows' paths `before` to `after`: where the
// keyboard on `currentRow` (showing `currentPath`) goes. Stay: its row is
// still there. Move: it follows its row, the first row's too (GitHub #113: a
// request coming in on top never takes an Enter meant for the row below).
// Wait: it stays put and Enter waits a moment, when its row went, when what
// stands there now stood in for one that went a moment ago (`standIn`), or
// on the first row right after the list opened from the plugin's
// notification (`openedForIt`: what it was about comes in there).
struct RowFollow {
    enum class Kind { Stay, Move, Wait };
    Kind kind = Kind::Stay;
    int row = -1; // Move: where its row is now
};

inline RowFollow followRow(
    const QStringList& before, const QStringList& after, int currentRow, const QString& currentPath, bool standIn, bool openedForIt)
{
    if (before == after || currentPath.isEmpty() || currentRow < 0)
        return {};
    const int at = static_cast<int>(after.indexOf(currentPath));
    if (at == currentRow)
        return {};
    if (at < 0 || standIn || (currentRow == 0 && openedForIt))
        return {RowFollow::Kind::Wait, -1};
    return {RowFollow::Kind::Move, at};
}

} // namespace ws::navigation
