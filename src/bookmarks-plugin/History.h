// The browsing history half of WinShunBookmarks.exe (GitHub #160): finding
// the History databases of Chromium browsers, reading the pages visited, and
// picking those that have every word typed. Off until the user turns it on
// in the plugin's settings. Read with Windows' own SQLite (winsqlite3.dll,
// part of Windows 10 and later): nothing is added to the package. Plain C++
// on the plugin SDK, no Qt; tests/tst_bookmarks.cpp builds it in again.

#pragma once

#include "Bookmarks.h"

namespace bm {

// One page of the history: when it was last opened, in microseconds since
// 1601 (UTC), as the browser keeps it.
struct Visit {
    std::string title;
    std::string url;
    std::string browser;
    long long lastVisit = 0;
};

// The file as an SQLite URI that is read as it is: no lock asked for (the
// browser holds its own for as long as it runs), nothing written beside it.
std::string immutableUri(const std::string& path);

// Chrome's and Edge's History of every profile under `localAppData`, then
// the History beside `extraFile` (the settings' Bookmarks file of another
// browser) when there is one.
std::vector<std::string> historyFiles(const std::string& localAppData, const std::string& extraFile);

// The pages of a History database, the last opened first, at most `limit`.
// The browser has the file open and locked while it runs: it is read as it
// is, with no lock taken, and never written. Not a History database, or one
// caught half written: what could be read of it, maybe nothing.
std::vector<Visit> readHistory(const std::string& path, const std::string& browser, std::size_t limit = 50000);

// Now, as the history counts time.
long long historyNow();

// When a page was opened, as the row says it, in local time: "今天 14:05",
// "昨天 09:30", "11-12 18:40" within the year, else "2025-03-04".
std::string visitTime(long long lastVisit, long long now, bool chinese);

// The rows for the typed `text`: at most 50. Nothing typed: the pages last
// opened. Words are matched as in the bookmarks (pinyin for a Chinese
// title); a title that says them comes before an address that does, and
// within each the last opened first. The same address is listed once.
// When nothing is found, one invalid row says why (`files` none: no browser
// history here at all).
winshun::Items searchHistory(std::vector<Visit> visits, const std::string& text, bool chinese, long long now,
    bool files = true);

// The one row while the history is off: where to turn it on.
winshun::Items historyOff(bool chinese);

} // namespace bm
