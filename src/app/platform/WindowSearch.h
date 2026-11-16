#pragma once

#include "SearchTypes.h"
#include <windows.h>

namespace ws::windowsearch {

// Read only, bounded to captioned task windows on this desktop. Win顺's own
// (its settings window) only when asked: 应用 › 运行 lists them as the
// taskbar does (#341), a search does not put them first. Their captions are
// read without a message, so a busy thread of ours cannot hold the scan up.
enum class Own { Excluded, Included };
SearchResults snapshot(Own own = Own::Excluded);
SearchResults matching(const SearchResults& windows, const QString& query, bool regex = false);
bool current(const SearchResult& window);
enum class Activation { Switched, Gone, Refused };
Activation activate(const SearchResult& window);

} // namespace ws::windowsearch
