// WinShunBookmarks.exe: the winshun.bookmarks plugin ("浏览器书签"), a once
// C++ plugin on the single-header SDK (src/plugins-sdk/winshun.h): `bm words`
// lists the bookmarks of Chrome and Edge (every profile) and of one more
// Bookmarks file from the settings, whose title, folder or address has every
// word typed. Enter opens the address (a declarative `url`, no second
// process). It was a Python script; as an exe it needs no Python and starts
// in milliseconds. No Qt, the C runtime linked in. The logic is in
// Bookmarks.cpp, which tests/tst_bookmarks.cpp builds in again.

#include "Bookmarks.h"

int main()
{
    const winshun::Query q = winshun::onceQuery();
    const winshun::Json settings = winshun::onceSettings();
    const bool chinese = winshun::env("WS_LOCALE") != "en";

    std::vector<bm::Entry> entries;
    for (const std::string& path : bm::bookmarkFiles(winshun::env("LOCALAPPDATA"), winshun::trim(settings["extraFile"].toString()))) {
        const std::vector<bm::Entry> found = bm::parseFile(bm::readFile(path), bm::browserOf(path, chinese));
        entries.insert(entries.end(), found.begin(), found.end());
    }

    // `cache`: the same words again within a minute are not run again. The
    // SDK's once() prints no cache, so the answer is built here.
    const winshun::Json answer = winshun::Json::Object {{"items", winshun::itemsJson(bm::search(entries, q.text, chinese))}, {"cache", 60}};
    return winshun::detail::writeLine(answer.dump()) ? 0 : 1;
}
