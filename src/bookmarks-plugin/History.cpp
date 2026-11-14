#include "History.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace bm {

namespace {

constexpr std::size_t kMaxRows = 50;
constexpr const char* kGlyph = "glyph:U+E81C"; // History
constexpr long long kMicrosPerDay = 24ll * 60 * 60 * 1'000'000;

// The file as an SQLite URI that is read as it is: no lock asked for (the
// browser holds its own for as long as it runs), nothing written beside it.
std::string immutableUri(const std::string& path)
{
    std::string uri = "file:";
    if (path.size() > 1 && path[1] == ':')
        uri += '/';
    for (const char c : path) {
        switch (c) {
        case '\\': uri += '/'; break;
        case '%': uri += "%25"; break;
        case '?': uri += "%3f"; break;
        case '#': uri += "%23"; break;
        default: uri += c;
        }
    }
    return uri + "?immutable=1";
}

// The local date and time of a history time; false if Windows cannot say.
bool localTime(long long micros, SYSTEMTIME& out)
{
    ULARGE_INTEGER ticks;
    ticks.QuadPart = static_cast<ULONGLONG>(micros) * 10;
    const FILETIME ft {ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc;
    return micros > 0 && ::FileTimeToSystemTime(&ft, &utc) && ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &out);
}

// Days since 1601 of a local date: to tell today from yesterday.
long long dayNumber(const SYSTEMTIME& local)
{
    SYSTEMTIME day = local;
    day.wHour = day.wMinute = day.wSecond = day.wMilliseconds = 0;
    FILETIME ft;
    if (!::SystemTimeToFileTime(&day, &ft))
        return 0;
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(ticks.QuadPart / 10) / kMicrosPerDay;
}

std::string join(std::initializer_list<const std::string*> parts)
{
    std::string out;
    for (const std::string* part : parts) {
        if (part->empty())
            continue;
        if (!out.empty())
            out += " · ";
        out += *part;
    }
    return out;
}

} // namespace

std::vector<std::string> historyFiles(const std::string& localAppData, const std::string& extraFile)
{
    std::vector<std::string> files = profileFiles(localAppData, {L"\\History"});
    const std::size_t slash = extraFile.find_last_of("\\/");
    if (slash != std::string::npos) {
        const std::string beside = extraFile.substr(0, slash + 1) + "History";
        const DWORD attrs = ::GetFileAttributesW(winshun::wide(beside).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(beside);
    }
    return files;
}

std::vector<Visit> readHistory(const std::string& path, const std::string& browser, std::size_t limit)
{
    std::vector<Visit> visits;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(immutableUri(path).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return visits;
    }
    sqlite3_stmt* rows = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT url, title, last_visit_time FROM urls WHERE hidden = 0 AND last_visit_time > 0 "
            "ORDER BY last_visit_time DESC LIMIT ?",
            -1, &rows, nullptr)
        == SQLITE_OK) {
        sqlite3_bind_int64(rows, 1, static_cast<sqlite3_int64>(limit));
        const auto column = [rows](int index) {
            const unsigned char* const text = sqlite3_column_text(rows, index);
            return text ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(rows, index)))
                        : std::string();
        };
        // A page caught half written ends the reading: what came before it is good.
        while (sqlite3_step(rows) == SQLITE_ROW) {
            Visit visit {column(1), column(0), browser, sqlite3_column_int64(rows, 2)};
            if (!visit.url.empty())
                visits.push_back(std::move(visit));
        }
    }
    sqlite3_finalize(rows);
    sqlite3_close(db);
    return visits;
}

long long historyNow()
{
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(ticks.QuadPart / 10);
}

std::string visitTime(long long lastVisit, long long now, bool chinese)
{
    SYSTEMTIME then, today;
    if (!localTime(lastVisit, then) || !localTime(now, today))
        return {};
    char text[32];
    const long long days = dayNumber(today) - dayNumber(then);
    if (days == 0)
        std::snprintf(text, sizeof text, "%s %02d:%02d", chinese ? "今天" : "Today", then.wHour, then.wMinute);
    else if (days == 1)
        std::snprintf(text, sizeof text, "%s %02d:%02d", chinese ? "昨天" : "Yesterday", then.wHour, then.wMinute);
    else if (then.wYear == today.wYear)
        std::snprintf(text, sizeof text, "%02d-%02d %02d:%02d", then.wMonth, then.wDay, then.wHour, then.wMinute);
    else
        std::snprintf(text, sizeof text, "%04d-%02d-%02d", then.wYear, then.wMonth, then.wDay);
    return text;
}

winshun::Items searchHistory(std::vector<Visit> visits, const std::string& text, bool chinese, long long now, bool files)
{
    // Several browsers and profiles as one history: the last opened first.
    std::stable_sort(visits.begin(), visits.end(), [](const Visit& a, const Visit& b) { return a.lastVisit > b.lastVisit; });
    const std::vector<std::string> typed = words(text);
    std::vector<ws::pinyin::Matcher> pinyin;
    pinyin.reserve(typed.size());
    for (const std::string& word : typed)
        pinyin.emplace_back(word);

    winshun::Items rows;
    std::set<std::string> seen;
    for (const Visit& v : visits) {
        // Shown and matched decoded, opened as it is.
        const std::string shown = unquote(v.url);
        const std::string title = lower(v.title);
        const std::string address = lower(v.url + "\n" + shown);
        const bool han = ws::pinyin::hasHan(title);
        bool keep = true;
        bool inTitle = true; // every word is in the title, said or spelled
        bool literal = true; // ... and said
        for (std::size_t i = 0; i < typed.size() && keep; ++i) {
            if (title.find(typed[i]) != std::string::npos)
                continue;
            literal = false;
            if (han && pinyin[i].valid() && pinyin[i].findUtf8(title))
                continue;
            inTitle = false;
            keep = address.find(typed[i]) != std::string::npos;
        }
        if (!keep || !seen.insert(lower(v.url)).second)
            continue;
        const std::string low = lower(v.url);
        const bool web = low.compare(0, 8, "https://") == 0 || low.compare(0, 7, "http://") == 0;
        const std::string when = visitTime(v.lastVisit, now, chinese);
        rows.push_back(winshun::Item("bh-" + std::to_string(rows.size()), v.title.empty() ? shown : v.title, join({&v.browser, &when, &shown}))
                           .withIcon(web ? "site:" + v.url : kGlyph)
                           .withScore(typed.empty() ? 70 : inTitle ? (literal ? 80 : 74) : 62)
                           .withUrl(v.url)
                           .withCopy(v.url));
        rows.back().match = v.title + " " + shown;
        // Nothing typed, or every row as good as the next can be: the first 50 are the 50.
        if (typed.empty() && rows.size() == kMaxRows)
            break;
    }
    // Ranked over all that matched, then cut: the last opened first within a rank.
    std::stable_sort(rows.begin(), rows.end(), [](const winshun::Item& a, const winshun::Item& b) { return a.score > b.score; });
    if (rows.size() > kMaxRows)
        rows.resize(kMaxRows);
    if (!rows.empty())
        return rows;
    if (!files) {
        rows.push_back(winshun::Item("none", chinese ? "没有找到浏览历史" : "No browsing history found",
                                     chinese ? "装了 Chrome 或 Edge 就会自动读；其他浏览器的，在设置里指定它的书签文件"
                                             : "Chrome's and Edge's are read on their own; for another browser, name its Bookmarks file in the settings")
                           .withIcon(kGlyph)
                           .invalid());
    } else {
        rows.push_back(winshun::Item("none", chinese ? "没有匹配的浏览历史" : "No matching page in the history", text)
                           .withIcon(kGlyph)
                           .invalid());
    }
    return rows;
}

winshun::Items historyOff(bool chinese)
{
    winshun::Items rows;
    rows.push_back(winshun::Item("off", chinese ? "搜索浏览历史还没有打开" : "Searching the browsing history is off",
                                 chinese ? "在 设置 → 插件 → 浏览器书签 的设置里打开“搜索浏览历史”"
                                         : "Turn on \"Search the browsing history\" in Settings → Plugins → Browser bookmarks")
                       .withIcon(kGlyph)
                       .invalid());
    return rows;
}

} // namespace bm
