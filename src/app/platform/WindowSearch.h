#pragma once

#include "SearchTypes.h"
#include <windows.h>

namespace ws::windowsearch {

// Read only, bounded to captioned task windows on this desktop. Own-process
// windows are excluded before GetWindowText (which otherwise sends messages).
SearchResults snapshot(DWORD excludedProcess = ::GetCurrentProcessId());
SearchResults matching(const SearchResults& windows, const QString& query, bool regex = false);
bool current(const SearchResult& window);
enum class Activation { Switched, Gone, Refused };
Activation activate(const SearchResult& window);

} // namespace ws::windowsearch
