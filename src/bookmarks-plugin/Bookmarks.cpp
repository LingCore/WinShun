#include "Bookmarks.h"

#include <algorithm>
#include <set>

namespace bm {

namespace {

constexpr std::size_t kMaxRows = 50;
constexpr const char* kSeparator = " › ";
constexpr const char* kGlyph = "glyph:U+E734";

bool startsWith(const std::string& s, const char* prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// The code point at s[i] and its length; a byte that does not start a
// sequence counts as itself (the text comes from the environment, already UTF-8).
unsigned codePoint(const std::string& s, std::size_t i, std::size_t& len)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    len = 1;
    if (c < 0x80)
        return c;
    const std::size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n == 1 || i + n > s.size())
        return c;
    unsigned cp = c & (0xFFu >> (n + 1));
    for (std::size_t k = 1; k < n; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    len = n;
    return cp;
}

// What Python's str.split() treats as white space.
bool isSpace(unsigned cp)
{
    return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 || cp == 0x1680
        || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// `bytes` as UTF-8 with every bad sequence replaced by one U+FFFD per
// maximal invalid prefix (what Python's "replace" does).
std::string repairUtf8(const std::string& bytes)
{
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            ++i;
            continue;
        }
        std::size_t n = 0;
        unsigned char low = 0x80, high = 0xBF; // the second byte's range
        if (lead >= 0xC2 && lead <= 0xDF)
            n = 2;
        else if (lead >= 0xE0 && lead <= 0xEF) {
            n = 3;
            if (lead == 0xE0)
                low = 0xA0;
            else if (lead == 0xED)
                high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            n = 4;
            if (lead == 0xF0)
                low = 0x90;
            else if (lead == 0xF4)
                high = 0x8F;
        }
        std::size_t used = 1; // the bytes that fit so far
        if (n) {
            for (; used < n && i + used < bytes.size(); ++used) {
                const unsigned char c = static_cast<unsigned char>(bytes[i + used]);
                const unsigned char from = used == 1 ? low : 0x80;
                const unsigned char to = used == 1 ? high : 0xBF;
                if (c < from || c > to)
                    break;
            }
        }
        if (n && used == n) {
            out.append(bytes, i, n);
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            i += used;
        }
    }
    return out;
}

// Every url under `node`, with the folder it is in. The root's own name is
// part of the folder text.
void walk(const winshun::Json& node, const std::string& folder, const std::string& browser, std::vector<Entry>& out)
{
    if (node["type"].toString() == "url") {
        out.push_back({node["name"].toString(), node["url"].toString(), folder, browser});
        return;
    }
    const std::string& name = node["name"].toString();
    const std::string sub = !folder.empty() && !name.empty() ? folder + kSeparator + name : (name.empty() ? folder : name);
    for (const winshun::Json& child : node["children"].toArray()) {
        if (child.isObject())
            walk(child, sub, browser, out);
    }
}

std::string join(const std::vector<const std::string*>& parts)
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

std::string lower(const std::string& s)
{
    if (s.empty())
        return {};
    const std::wstring in = winshun::wide(s);
    std::wstring out(in.size(), L'\0');
    const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, in.data(), static_cast<int>(in.size()), out.data(),
                                  static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n <= 0)
        return s;
    out.resize(static_cast<std::size_t>(n));
    return winshun::utf8(out);
}

std::string unquote(const std::string& url)
{
    if (url.find('%') == std::string::npos)
        return url;
    std::string bytes;
    bytes.reserve(url.size());
    for (std::size_t i = 0; i < url.size(); ++i) {
        if (url[i] == '%' && i + 2 < url.size() && hexValue(url[i + 1]) >= 0 && hexValue(url[i + 2]) >= 0) {
            bytes += static_cast<char>(hexValue(url[i + 1]) * 16 + hexValue(url[i + 2]));
            i += 2;
        } else
            bytes += url[i];
    }
    return repairUtf8(bytes);
}

std::vector<std::string> words(const std::string& text)
{
    std::vector<std::string> out;
    std::string word;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t len = 1;
        const unsigned cp = codePoint(text, i, len);
        if (isSpace(cp)) {
            if (!word.empty())
                out.push_back(lower(word));
            word.clear();
        } else
            word.append(text, i, len);
        i += len;
    }
    if (!word.empty())
        out.push_back(lower(word));
    return out;
}

std::string browserOf(const std::string& path, bool chinese)
{
    const std::string low = lower(path);
    if (low.find("\\google\\chrome\\") != std::string::npos)
        return "Chrome";
    if (low.find("\\microsoft\\edge\\") != std::string::npos)
        return "Edge";
    for (const char* name : {"Brave", "Vivaldi", "Chromium", "Opera", "Yandex", "Arc"}) {
        if (low.find("\\" + lower(name)) != std::string::npos)
            return name;
    }
    return chinese ? "书签文件" : "Bookmarks file";
}

std::vector<Entry> parseFile(const std::string& content, const std::string& browser)
{
    std::vector<Entry> out;
    const std::size_t skip = startsWith(content, "\xEF\xBB\xBF") ? 3 : 0;
    const auto data = winshun::Json::parse(content.substr(skip));
    if (!data || !data->isObject())
        return out;
    for (const auto& [key, root] : (*data)["roots"].toObject()) {
        (void)key;
        if (root.isObject())
            walk(root, {}, browser, out);
    }
    return out;
}

std::string readFile(const std::string& path)
{
    const HANDLE file = ::CreateFileW(winshun::wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::string content;
    char chunk[64 * 1024];
    DWORD got = 0;
    while (::ReadFile(file, chunk, sizeof chunk, &got, nullptr) && got > 0)
        content.append(chunk, got);
    ::CloseHandle(file);
    return content;
}

std::vector<std::string> bookmarkFiles(const std::string& localAppData, const std::string& extraFile)
{
    std::vector<std::string> files;
    const auto isFile = [](const std::wstring& path) {
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!localAppData.empty()) {
        for (const char* browser : {"\\Google\\Chrome\\User Data", "\\Microsoft\\Edge\\User Data"}) {
            const std::wstring root = winshun::wide(localAppData + browser);
            WIN32_FIND_DATAW found;
            const HANDLE find = ::FindFirstFileW((root + L"\\*").c_str(), &found);
            if (find == INVALID_HANDLE_VALUE)
                continue;
            do {
                // Like a glob's "*": not the names that start with a dot.
                if (found.cFileName[0] == L'.')
                    continue;
                const std::wstring path = root + L"\\" + found.cFileName + L"\\Bookmarks";
                if (isFile(path))
                    files.push_back(winshun::utf8(path));
            } while (::FindNextFileW(find, &found));
            ::FindClose(find);
        }
    }
    if (!extraFile.empty() && isFile(winshun::wide(extraFile)))
        files.push_back(extraFile);
    return files;
}

winshun::Items search(const std::vector<Entry>& entries, const std::string& text, bool chinese)
{
    const std::vector<std::string> typed = words(text);
    winshun::Items rows;
    std::set<std::string> seen;
    for (const Entry& e : entries) {
        // Shown and matched decoded, opened as it is.
        const std::string shown = unquote(e.url);
        const std::string haystack = lower(e.title + "\n" + e.url + "\n" + shown + "\n" + e.folder);
        const auto missing = [&haystack](const std::string& w) { return haystack.find(w) == std::string::npos; };
        if (std::any_of(typed.begin(), typed.end(), missing))
            continue;
        if (!seen.insert(lower(e.url)).second)
            continue;
        // Title matches rank above address-only matches.
        const std::string title = lower(e.title);
        const bool inTitle = std::all_of(typed.begin(), typed.end(), [&title](const std::string& w) { return title.find(w) != std::string::npos; });
        const std::string low = lower(e.url);
        const bool web = startsWith(low, "https://") || startsWith(low, "http://");
        rows.push_back(winshun::Item("bm-" + std::to_string(rows.size()), e.title.empty() ? shown : e.title, join({&e.browser, &e.folder, &shown}))
                           // The site's own icon; Win顺 fetches and keeps it.
                           .withIcon(web ? "site:" + e.url : kGlyph)
                           .withScore(inTitle ? 80 : 60)
                           .withUrl(e.url)
                           .withCopy(e.url));
        rows.back().match = e.title + " " + shown;
        if (rows.size() >= kMaxRows)
            break;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const winshun::Item& a, const winshun::Item& b) { return a.score > b.score; });
    if (!rows.empty())
        return rows;
    if (entries.empty()) {
        rows.push_back(winshun::Item("none", chinese ? "没有找到书签文件" : "No bookmarks file found",
                                     chinese ? "装了 Chrome 或 Edge 就会自动读；其他浏览器的在设置里指定"
                                             : "Chrome's and Edge's are read on their own; name another browser's in the settings")
                           .withIcon(kGlyph)
                           .invalid());
    } else {
        rows.push_back(winshun::Item("none", chinese ? "没有匹配的书签" : "No matching bookmark", text)
                           .withIcon(kGlyph)
                           .invalid());
    }
    return rows;
}

} // namespace bm
