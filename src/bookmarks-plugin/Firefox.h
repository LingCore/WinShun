// Firefox's half of WinShunBookmarks.exe: Firefox (and the browsers made
// from it, LibreWolf, Zen, Waterfox ...) keeps its bookmarks and its history
// in one SQLite database per profile, places.sqlite. Read with Windows' own
// SQLite like the Chromium history (History.cpp), read-only and never
// written; the WAL beside it is read too when it can be (Firefox.cpp says
// why). Plain C++ on the plugin SDK, no Qt;
// tests/tst_bookmarks.cpp builds it in again.

#pragma once

#include "History.h"

namespace bm {

// Whether the file is a places.sqlite (by its name): read as Firefox's, not
// as a Chromium Bookmarks file.
bool isPlaces(const std::string& path);

// places.sqlite of every Firefox profile: under `appData` (the installer's
// Firefox) and under the Microsoft Store's Firefox in `localAppData`.
std::vector<std::string> placesFiles(const std::string& appData, const std::string& localAppData);

// The bookmarks of a places.sqlite, each with the folders it sits in
// ("书签工具栏 › 工作"). Firefox's own top folders are named in the user's
// language; tags and saved searches (place: addresses) are left out.
// Not a places database: nothing.
std::vector<Entry> readPlaces(const std::string& path, const std::string& browser, bool chinese);

// The pages of a places.sqlite's history, the last opened first, at most
// `limit`, their times counted as Chromium counts them (microseconds since 1601).
std::vector<Visit> readPlacesHistory(const std::string& path, const std::string& browser, std::size_t limit = 50000);

} // namespace bm
