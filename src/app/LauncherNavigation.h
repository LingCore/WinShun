#pragma once

#include "SearchTypes.h"

namespace ws::navigation {

enum class TabAction { SwitchScope, Complete, BrowseFolder, None };

inline bool canBrowseFolder(const SearchResult& result) noexcept
{
    return result.isDir && !result.isApp() && !result.offline && !result.missing;
}

// The action and the footer use the same policy. Favorites only change
// Tab when explicitly enabled; keyword and web completion stay independent.
inline TabAction tabAction(
    Scope scope, bool keywordMode, bool enterFavoriteFolder, const SearchResult* row, bool hasCompletion) noexcept
{
    if (!keywordMode && scope == Scope::All && enterFavoriteFolder && row && row->favorite && canBrowseFolder(*row))
        return TabAction::BrowseFolder;
    if (hasCompletion)
        return TabAction::Complete;
    return keywordMode ? TabAction::None : TabAction::SwitchScope;
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
