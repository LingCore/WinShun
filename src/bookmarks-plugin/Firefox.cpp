#include "Firefox.h"

#include <winsqlite/winsqlite3.h>

#include <cstring>
#include <map>

namespace bm {

namespace {

// Firefox counts from 1970, Chromium (and Visit) from 1601.
constexpr long long kFrom1601To1970Micros = 11644473600ll * 1'000'000;

std::string column(sqlite3_stmt* rows, int index)
{
    const unsigned char* const text = sqlite3_column_text(rows, index);
    return text ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(rows, index)))
                : std::string();
}

// Runs `read` on the database, read-only. Firefox writes into places.sqlite-wal
// first and folds it into the file only now and then (2 MB of it right after
// a first start), so the file as it is lacks what was added lately: it is
// opened the normal way first, the WAL read too (Firefox takes no exclusive
// lock; a reader does not hold up its writes). When that cannot be done (the
// file locked, no -shm beside a WAL to share), as it is, like Chromium's.
// `read` says whether its query went through; it starts over clean each time.
template<typename Read>
void readPlacesDb(const std::string& path, Read read)
{
    std::string normal = immutableUri(path);
    normal.replace(normal.rfind("?immutable=1"), std::string::npos, "?mode=ro");
    for (const std::string& uri : {normal, immutableUri(path)}) {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 200);
            if (read(db)) {
                sqlite3_close(db);
                return;
            }
        }
        sqlite3_close(db);
    }
}

// Firefox's own top folders, by their fixed ids: the names the database keeps
// ("toolbar") are not what the browser shows.
const char* rootName(const std::string& guid, bool chinese)
{
    if (guid == "menu________")
        return chinese ? "书签菜单" : "Bookmarks Menu";
    if (guid == "toolbar_____")
        return chinese ? "书签工具栏" : "Bookmarks Toolbar";
    if (guid == "unfiled_____")
        return chinese ? "其他书签" : "Other Bookmarks";
    if (guid == "mobile______")
        return chinese ? "移动设备书签" : "Mobile Bookmarks";
    return nullptr;
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

void addProfiles(const std::wstring& root, std::vector<std::string>& files)
{
    WIN32_FIND_DATAW found;
    const HANDLE find = ::FindFirstFileW((root + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (found.cFileName[0] == L'.' || !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const std::wstring path = root + L"\\" + found.cFileName + L"\\places.sqlite";
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(winshun::utf8(path));
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
}

} // namespace

bool isPlaces(const std::string& path)
{
    const std::size_t slash = path.find_last_of("\\/");
    return lower(slash == std::string::npos ? path : path.substr(slash + 1)) == "places.sqlite";
}

std::vector<std::string> placesFiles(const std::string& appData, const std::string& localAppData)
{
    std::vector<std::string> files;
    if (!appData.empty())
        addProfiles(winshun::wide(appData + "\\Mozilla\\Firefox\\Profiles"), files);
    if (!localAppData.empty()) {
        // The Store's Firefox keeps its profiles in its package's folder.
        const std::wstring packages = winshun::wide(localAppData + "\\Packages");
        WIN32_FIND_DATAW found;
        const HANDLE find = ::FindFirstFileW((packages + L"\\Mozilla.Firefox_*").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    addProfiles(packages + L"\\" + found.cFileName + L"\\LocalCache\\Roaming\\Mozilla\\Firefox\\Profiles", files);
            } while (::FindNextFileW(find, &found));
            ::FindClose(find);
        }
    }
    return files;
}

std::vector<Entry> readPlaces(const std::string& path, const std::string& browser, bool chinese)
{
    std::vector<Entry> out;
    struct Node {
        long long id;
        bool folder;
        std::string title;
        std::string guid;
        std::string url;
    };
    std::vector<Node> nodes;
    std::map<long long, std::vector<std::size_t>> children; // parent id: its nodes, in their order
    std::map<long long, bool> isFolder;
    readPlacesDb(path, [&](sqlite3* db) {
        nodes.clear();
        children.clear();
        isFolder.clear();
        sqlite3_stmt* rows = nullptr;
        // type 1: a bookmark, 2: a folder (3: a separator).
        if (sqlite3_prepare_v2(db,
                "SELECT b.id, b.parent, b.type, b.title, b.guid, p.url, p.title FROM moz_bookmarks b "
                "LEFT JOIN moz_places p ON b.fk = p.id WHERE b.type IN (1, 2) ORDER BY b.parent, b.position",
                -1, &rows, nullptr)
            != SQLITE_OK) {
            sqlite3_finalize(rows);
            return false;
        }
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(rows)) == SQLITE_ROW) {
            const long long id = sqlite3_column_int64(rows, 0);
            const bool folder = sqlite3_column_int(rows, 2) == 2;
            std::string title = column(rows, 3);
            if (title.empty() && !folder)
                title = column(rows, 6);
            isFolder[id] = folder;
            children[sqlite3_column_int64(rows, 1)].push_back(nodes.size());
            nodes.push_back({id, folder, std::move(title), column(rows, 4), column(rows, 5)});
        }
        sqlite3_finalize(rows);
        return step == SQLITE_DONE;
    });

    // In the order the browser shows them: down the tree from the top, the
    // folders it went through as "书签工具栏 › 工作". Not into the tags
    // folder: what is under it are tags, not bookmarks of their own.
    const auto walk = [&](const auto& self, long long parent, const std::string& path, int depth) -> void {
        const auto list = children.find(parent);
        if (list == children.end() || depth > 64)
            return;
        for (const std::size_t index : list->second) {
            const Node& node = nodes[index];
            if (node.folder) {
                if (node.guid == "tags________")
                    continue;
                if (node.guid == "root________") {
                    self(self, node.id, path, depth + 1);
                    continue;
                }
                const char* const root = rootName(node.guid, chinese);
                const std::string name = root ? std::string(root) : node.title;
                self(self, node.id, path.empty() ? name : path + " › " + name, depth + 1);
            } else if (!node.url.empty() && !startsWith(lower(node.url), "place:")) {
                out.push_back({node.title, node.url, path, browser});
            }
        }
    };
    // The top: whatever hangs under no folder (the root folder itself).
    for (const auto& [parent, list] : children) {
        (void)list;
        if (!isFolder.count(parent))
            walk(walk, parent, {}, 0);
    }
    return out;
}

std::vector<Visit> readPlacesHistory(const std::string& path, const std::string& browser, std::size_t limit)
{
    std::vector<Visit> visits;
    readPlacesDb(path, [&](sqlite3* db) {
        visits.clear();
        sqlite3_stmt* rows = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT url, title, last_visit_date FROM moz_places WHERE hidden = 0 AND last_visit_date > 0 "
                "ORDER BY last_visit_date DESC LIMIT ?",
                -1, &rows, nullptr)
            != SQLITE_OK) {
            sqlite3_finalize(rows);
            return false;
        }
        sqlite3_bind_int64(rows, 1, static_cast<sqlite3_int64>(limit));
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(rows)) == SQLITE_ROW) {
            Visit visit {column(rows, 1), column(rows, 0), browser, sqlite3_column_int64(rows, 2) + kFrom1601To1970Micros};
            if (!visit.url.empty() && !startsWith(lower(visit.url), "place:"))
                visits.push_back(std::move(visit));
        }
        sqlite3_finalize(rows);
        return step == SQLITE_DONE;
    });
    return visits;
}

} // namespace bm
