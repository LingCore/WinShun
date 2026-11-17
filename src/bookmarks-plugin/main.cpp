// WinShunBookmarks.exe: the winshun.bookmarks plugin ("浏览器书签"), a once
// C++ plugin on the single-header SDK (src/plugins-sdk/winshun.h): `bm words`
// lists the bookmarks of Chrome and Edge (every profile) and of one more
// Bookmarks file from the settings, whose title, folder or address has every
// word typed. Enter opens the address (a declarative `url`, no second
// process). It was a Python script; as an exe it needs no Python and starts
// in milliseconds. `bh words` lists the pages opened in them (History.cpp),
// once turned on. Firefox's bookmarks and history come from its places.sqlite
// (Firefox.cpp). No Qt, the C runtime linked in. The logic is in
// Bookmarks.cpp, which tests/tst_bookmarks.cpp builds in again.

#include "Firefox.h"

int main()
{
    const winshun::Query q = winshun::onceQuery();
    const winshun::Json settings = winshun::onceSettings();
    const bool chinese = winshun::env("WS_LOCALE") != "en";
    const std::string extraFile = winshun::trim(settings["extraFile"].toString());
    const std::vector<std::string> firefox = bm::placesFiles(winshun::env("APPDATA"), winshun::env("LOCALAPPDATA"));

    // `bh words`: the pages opened in those browsers (GitHub #160), once the
    // user has turned it on. Changing by the minute, it is kept half as long.
    if (q.command == "history") {
        winshun::Items rows = bm::historyOff(chinese);
        if (settings["history"].toBool()) {
            std::vector<std::string> files = bm::historyFiles(winshun::env("LOCALAPPDATA"), extraFile);
            files.insert(files.end(), firefox.begin(), firefox.end());
            if (bm::isPlaces(extraFile)) // the setting names a Firefox-like browser's places.sqlite
                files.push_back(extraFile);
            std::vector<bm::Visit> visits;
            for (const std::string& path : files) {
                std::vector<bm::Visit> found = bm::isPlaces(path) ? bm::readPlacesHistory(path, bm::browserOf(path, chinese))
                                                                  : bm::readHistory(path, bm::browserOf(path, chinese));
                visits.insert(visits.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
            }
            rows = bm::searchHistory(std::move(visits), q.text, chinese, bm::historyNow(), !files.empty());
        }
        const winshun::Json answer = winshun::Json::Object {{"items", winshun::itemsJson(rows)}, {"cache", 30}};
        return winshun::detail::writeLine(answer.dump()) ? 0 : 1;
    }

    std::vector<bm::Entry> entries;
    std::vector<std::string> files = bm::bookmarkFiles(winshun::env("LOCALAPPDATA"), extraFile);
    files.insert(files.end(), firefox.begin(), firefox.end());
    for (const std::string& path : files) {
        const std::vector<bm::Entry> found = bm::isPlaces(path) ? bm::readPlaces(path, bm::browserOf(path, chinese), chinese)
                                                                : bm::parseFile(bm::readFile(path), bm::browserOf(path, chinese));
        entries.insert(entries.end(), found.begin(), found.end());
    }

    // `cache`: the same words again within a minute are not run again. The
    // SDK's once() prints no cache, so the answer is built here.
    const winshun::Json answer = winshun::Json::Object {{"items", winshun::itemsJson(bm::search(entries, q.text, chinese))}, {"cache", 60}};
    return winshun::detail::writeLine(answer.dump()) ? 0 : 1;
}
