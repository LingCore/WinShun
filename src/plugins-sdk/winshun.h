// winshun.h — the Win顺 (WinShun) plugin SDK for C++, in one header.
//
// No dependencies beyond Win32 and the C++17 standard library; no Qt. Copy
// this file next to your main.cpp. A resident plugin derives from
// winshun::Plugin, answers query() (and action() if Enter should do more
// than copy), and calls serve():
//
//     struct Hello : winshun::Plugin {
//         winshun::Items query(const winshun::Query& q) override {
//             return {winshun::Item("hi", "你好，" + q.text).withCopy(q.text)};
//         }
//     };
//     int main() { return Hello().serve(); }
//
// A once plugin (runtime.mode = "once") reads the query from the
// environment and prints its items: winshun::once(items).
//
// The protocol (docs/plugins.md): JSON-RPC 2.0, one message per line, UTF-8,
// on stdin and stdout. Nothing but JSON-RPC may be written to stdout; write
// diagnostics to stderr, which goes to plugins.log.

#pragma once

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace winshun {

// ---- JSON ---------------------------------------------------------------------

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : m_type(Type::Bool), m_bool(b) {}
    Json(double d) : m_type(Type::Number), m_number(d) {}
    Json(int i) : Json(static_cast<double>(i)) {}
    Json(long long i) : Json(static_cast<double>(i)) {}
    Json(unsigned long u) : Json(static_cast<double>(u)) {}
    Json(unsigned long long u) : Json(static_cast<double>(u)) {}
    Json(std::string s) : m_type(Type::String), m_string(std::move(s)) {}
    Json(const char* s) : Json(std::string(s)) {}
    Json(Array a) : m_type(Type::Array), m_array(std::make_shared<Array>(std::move(a))) {}
    Json(Object o) : m_type(Type::Object), m_object(std::make_shared<Object>(std::move(o))) {}

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool fallback = false) const { return isBool() ? m_bool : fallback; }
    double toNumber(double fallback = 0) const { return isNumber() ? m_number : fallback; }
    long long toInt(long long fallback = 0) const { return isNumber() ? static_cast<long long>(m_number) : fallback; }
    const std::string& toString() const
    {
        static const std::string empty;
        return isString() ? m_string : empty;
    }
    const Array& toArray() const
    {
        static const Array empty;
        return isArray() ? *m_array : empty;
    }
    const Object& toObject() const
    {
        static const Object empty;
        return isObject() ? *m_object : empty;
    }
    // A member of an object, or null.
    const Json& operator[](const char* key) const
    {
        static const Json null;
        if (!isObject())
            return null;
        const auto it = m_object->find(key);
        return it == m_object->end() ? null : it->second;
    }
    const Json& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    bool has(const char* key) const { return isObject() && m_object->count(key) > 0; }
    // A member of an object, made when missing (the object is shared with
    // copies of this value: set members before copying).
    Json& set(const std::string& key, Json value)
    {
        if (!isObject()) {
            m_type = Type::Object;
            m_object = std::make_shared<Object>();
        }
        (*m_object)[key] = std::move(value);
        return *this;
    }
    void push(Json value)
    {
        if (!isArray()) {
            m_type = Type::Array;
            m_array = std::make_shared<Array>();
        }
        m_array->push_back(std::move(value));
    }

    // The JSON text of this value (compact; non-ASCII kept as UTF-8).
    std::string dump() const
    {
        std::string out;
        write(out, *this);
        return out;
    }
    // One JSON text; nullopt when it is not one (`problem` says where).
    static std::optional<Json> parse(const std::string& text, std::string* problem = nullptr);
    static std::string quote(const std::string& s);

private:
    static void write(std::string& out, const Json& value);
    class Parser;

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::shared_ptr<Array> m_array;
    std::shared_ptr<Object> m_object;
};

class Json::Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    std::optional<Json> run(std::string* problem)
    {
        skipSpace();
        std::optional<Json> value = parseValue(0);
        if (value) {
            skipSpace();
            if (m_at != m_text.size())
                fail("trailing characters");
        }
        if (!m_problem.empty()) {
            if (problem)
                *problem = m_problem;
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 64;
    const std::string& m_text;
    std::size_t m_at = 0;
    std::string m_problem;

    void fail(const char* why)
    {
        if (m_problem.empty())
            m_problem = std::string(why) + " at " + std::to_string(m_at);
    }
    bool more() const { return m_at < m_text.size(); }
    char peek() const { return more() ? m_text[m_at] : '\0'; }
    void skipSpace()
    {
        while (more() && (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
            ++m_at;
    }
    bool consume(const char* word)
    {
        const std::size_t n = std::strlen(word);
        if (m_text.compare(m_at, n, word) != 0)
            return false;
        m_at += n;
        return true;
    }
    std::optional<Json> parseValue(int depth)
    {
        if (depth > kMaxDepth) {
            fail("nested too deep");
            return std::nullopt;
        }
        switch (peek()) {
        case '{': return parseObject(depth);
        case '[': return parseArray(depth);
        case '"': {
            std::string s;
            if (!parseString(s))
                return std::nullopt;
            return Json(std::move(s));
        }
        case 't':
            if (consume("true"))
                return Json(true);
            break;
        case 'f':
            if (consume("false"))
                return Json(false);
            break;
        case 'n':
            if (consume("null"))
                return Json(nullptr);
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                return parseNumber();
        }
        fail("unexpected character");
        return std::nullopt;
    }
    std::optional<Json> parseNumber()
    {
        const std::size_t start = m_at;
        if (peek() == '-')
            ++m_at;
        while (more() && ((m_text[m_at] >= '0' && m_text[m_at] <= '9') || m_text[m_at] == '.' || m_text[m_at] == 'e'
                             || m_text[m_at] == 'E' || m_text[m_at] == '+' || m_text[m_at] == '-'))
            ++m_at;
        const std::string digits = m_text.substr(start, m_at - start);
        char* end = nullptr;
        const double d = std::strtod(digits.c_str(), &end);
        if (end == digits.c_str() || *end != '\0' || !std::isfinite(d)) {
            fail("bad number");
            return std::nullopt;
        }
        return Json(d);
    }
    static void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool parseHex4(unsigned& value)
    {
        if (m_at + 4 > m_text.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_at++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool parseString(std::string& out)
    {
        ++m_at;
        for (;;) {
            if (!more()) {
                fail("unterminated string");
                return false;
            }
            const char c = m_text[m_at++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (!more()) {
                fail("unterminated escape");
                return false;
            }
            const char e = m_text[m_at++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!parseHex4(cp)) {
                    fail("bad \\u escape");
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned low = 0;
                    if (m_text.compare(m_at, 2, "\\u") == 0) {
                        m_at += 2;
                        if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            fail("bad surrogate pair");
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else
                        cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    cp = 0xFFFD;
                appendUtf8(out, cp);
                break;
            }
            default:
                fail("bad escape");
                return false;
            }
        }
    }
    std::optional<Json> parseArray(int depth)
    {
        ++m_at;
        Array items;
        skipSpace();
        if (peek() == ']') {
            ++m_at;
            return Json(std::move(items));
        }
        for (;;) {
            skipSpace();
            std::optional<Json> item = parseValue(depth + 1);
            if (!item)
                return std::nullopt;
            items.push_back(std::move(*item));
            skipSpace();
            if (peek() == ',') {
                ++m_at;
                continue;
            }
            if (peek() == ']') {
                ++m_at;
                return Json(std::move(items));
            }
            fail("expected , or ]");
            return std::nullopt;
        }
    }
    std::optional<Json> parseObject(int depth)
    {
        ++m_at;
        Object members;
        skipSpace();
        if (peek() == '}') {
            ++m_at;
            return Json(std::move(members));
        }
        for (;;) {
            skipSpace();
            if (peek() != '"') {
                fail("expected a key");
                return std::nullopt;
            }
            std::string key;
            if (!parseString(key))
                return std::nullopt;
            skipSpace();
            if (peek() != ':') {
                fail("expected :");
                return std::nullopt;
            }
            ++m_at;
            skipSpace();
            std::optional<Json> value = parseValue(depth + 1);
            if (!value)
                return std::nullopt;
            members[std::move(key)] = std::move(*value);
            skipSpace();
            if (peek() == ',') {
                ++m_at;
                continue;
            }
            if (peek() == '}') {
                ++m_at;
                return Json(std::move(members));
            }
            fail("expected , or }");
            return std::nullopt;
        }
    }
};

inline std::optional<Json> Json::parse(const std::string& text, std::string* problem)
{
    return Parser(text).run(problem);
}

inline std::string Json::quote(const std::string& s)
{
    // The length of the well-formed UTF-8 sequence at s[i], or 0. A line that
    // is not UTF-8 is dropped whole by the host, so a byte that does not fit
    // (text cut in the middle of a character, a peer's raw bytes) is written
    // as U+FFFD instead.
    const auto sequence = [&s](std::size_t i) -> std::size_t {
        const auto at = [&s](std::size_t k) { return static_cast<unsigned char>(s[k]); };
        const unsigned char lead = at(i);
        std::size_t n = 0;
        unsigned char low = 0x80, high = 0xBF; // the second byte's range (Unicode table 3-7)
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
        } else
            return 0;
        if (i + n > s.size() || at(i + 1) < low || at(i + 1) > high)
            return 0;
        for (std::size_t k = 2; k < n; ++k) {
            if (at(i + k) < 0x80 || at(i + k) > 0xBF)
                return 0;
        }
        return n;
    };
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (static_cast<unsigned char>(c) >= 0x80) {
            const std::size_t n = sequence(i);
            if (n) {
                out.append(s, i, n);
                i += n - 1;
            } else
                out += "\xEF\xBF\xBD";
            continue;
        }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else
                out += c;
        }
    }
    out += '"';
    return out;
}

inline void Json::write(std::string& out, const Json& value)
{
    switch (value.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += value.toBool() ? "true" : "false"; break;
    case Type::Number: {
        const double d = value.toNumber();
        if (d == std::floor(d) && std::fabs(d) < 1e15)
            out += std::to_string(static_cast<long long>(d));
        else {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%.17g", d);
            out += buffer;
        }
        break;
    }
    case Type::String: out += quote(value.toString()); break;
    case Type::Array: {
        out += '[';
        bool first = true;
        for (const Json& item : value.toArray()) {
            if (!first)
                out += ',';
            first = false;
            write(out, item);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.toObject()) {
            if (!first)
                out += ',';
            first = false;
            out += quote(key);
            out += ':';
            write(out, item);
        }
        out += '}';
        break;
    }
    }
}

// ---- text helpers --------------------------------------------------------------

inline std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// An environment variable, UTF-8; empty when unset.
inline std::string env(const char* name)
{
    wchar_t buffer[32768];
    const DWORD n = ::GetEnvironmentVariableW(wide(name).c_str(), buffer, static_cast<DWORD>(sizeof buffer / sizeof buffer[0]));
    return n > 0 && n < sizeof buffer / sizeof buffer[0] ? utf8(std::wstring(buffer, n)) : std::string();
}

inline std::string trim(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

// A line to stderr (plugins.log; the developer mode's log pane).
inline void debug(const std::string& text)
{
    const std::string line = text + "\n";
    DWORD written = 0;
    ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

// ---- what a plugin answers with ------------------------------------------------

struct Action {
    std::string id;
    std::string title;
    std::string key; // "Enter", "Ctrl+Enter", "Shift+Enter"
};

// One result row (docs/plugins.md, "item"). Setters return *this so a row
// can be written in one expression.
struct Item {
    std::string id;
    std::string title;
    std::string subtitle;
    std::string subtitleAway; // the second line while the keyboard is on another row; empty: `subtitle`
    std::string icon; // "file:C:\…", "ext:pdf", "glyph:U+E8E5", "color:#ff8800", "icon.png", "data:image/png;base64,…", "site:https://…"
    int score = -1; // 0–100; -1: not given
    std::string match;
    std::string autocomplete;
    std::string copy;
    std::string preview;
    Json open; // {"path": …} or {"url": …}
    std::vector<Action> actions;
    bool valid = true;
    Json data;
    Json modified; // ISO 8601 with time zone or Unix seconds; optional
    Json created;
    long long size = -1; // bytes; -1 means unknown

    Item() = default;
    Item(std::string itemId, std::string itemTitle, std::string itemSubtitle = {})
        : id(std::move(itemId)), title(std::move(itemTitle)), subtitle(std::move(itemSubtitle))
    {
    }
    Item& withIcon(std::string s) { icon = std::move(s); return *this; }
    Item& withSubtitleAway(std::string s) { subtitleAway = std::move(s); return *this; }
    Item& withScore(int s) { score = s; return *this; }
    Item& withCopy(std::string s) { copy = std::move(s); return *this; }
    Item& withAutocomplete(std::string s) { autocomplete = std::move(s); return *this; }
    Item& withPreview(std::string s) { preview = std::move(s); return *this; }
    Item& withPath(std::string path) { open = Json::Object {{"path", std::move(path)}}; return *this; }
    Item& withUrl(std::string url) { open = Json::Object {{"url", std::move(url)}}; return *this; }
    Item& withData(Json d) { data = std::move(d); return *this; }
    Item& withModified(Json value) { modified = std::move(value); return *this; }
    Item& withCreated(Json value) { created = std::move(value); return *this; }
    Item& withSize(long long bytes) { size = bytes; return *this; }
    Item& withActions(std::vector<Action> a) { actions = std::move(a); return *this; }
    Item& invalid() { valid = false; return *this; }

    Json toJson() const
    {
        Json::Object o {{"id", id}, {"title", title}};
        if (!subtitle.empty())
            o["subtitle"] = subtitle;
        if (!subtitleAway.empty())
            o["subtitleAway"] = subtitleAway;
        if (!modified.isNull()) o["modified"] = modified;
        if (!created.isNull()) o["created"] = created;
        if (size >= 0) o["size"] = size;
        if (!icon.empty())
            o["icon"] = icon;
        if (score >= 0)
            o["score"] = score;
        if (!match.empty())
            o["match"] = match;
        if (!autocomplete.empty())
            o["autocomplete"] = autocomplete;
        if (!copy.empty())
            o["copy"] = copy;
        if (!preview.empty())
            o["preview"] = Json::Object {{"text", preview}};
        if (open.isObject())
            o["open"] = open;
        if (!actions.empty()) {
            Json::Array a;
            for (const Action& action : actions)
                a.push_back(Json::Object {{"id", action.id}, {"title", action.title}, {"key", action.key}});
            o["actions"] = Json(std::move(a));
        }
        if (!valid)
            o["valid"] = false;
        if (!data.isNull())
            o["data"] = data;
        return Json(std::move(o));
    }
};
using Items = std::vector<Item>;

inline Json itemsJson(const Items& items)
{
    Json::Array a;
    for (const Item& item : items)
        a.push_back(item.toJson());
    return Json(std::move(a));
}

// What an action asks Win顺 to do, in order (docs/plugins.md, "effects").
namespace effect {
inline Json hide() { return Json::Object {{"type", "hide"}}; }
inline Json keep() { return Json::Object {{"type", "keep"}}; }
inline Json notify(std::string text) { return Json::Object {{"type", "notify"}, {"text", std::move(text)}}; }
inline Json copy(std::string text) { return Json::Object {{"type", "copy"}, {"text", std::move(text)}}; }
inline Json setQuery(std::string text) { return Json::Object {{"type", "setQuery"}, {"text", std::move(text)}}; }
// openPath / openUrl (and an item's open): Win顺 carries these out. Without the
// "processes" permission it opens only a folder or an ordinary (non-program,
// non-script) file, and only http/https/mailto URLs, with no args/cwd; anything
// else is refused with a footer notice. Declare "processes" to open a program
// or a non-web link.
inline Json openPath(std::string path) { return Json::Object {{"type", "open"}, {"path", std::move(path)}}; }
inline Json openUrl(std::string url) { return Json::Object {{"type", "open"}, {"url", std::move(url)}}; }
inline Json replace(const Items& items) { return Json::Object {{"type", "replace"}, {"items", itemsJson(items)}}; }
} // namespace effect
using Effects = std::vector<Json>;

// A query as Win顺 sends it.
struct Query {
    long long queryId = 0;
    std::string command; // the command's id
    std::string trigger; // keyword | regex | push
    std::string keyword; // the one typed, in keyword mode
    std::string text; // after the keyword; the whole input otherwise
    std::string raw; // the whole input
    std::vector<std::string> match; // a regex trigger's groups
    int limit = 200;
    Json filters = Json::Object {}; // search-panel bounds; absent fields mean no restriction
};

// An action as Win顺 sends it.
struct ActionRequest {
    std::string command;
    std::string action; // the action's id ("default" when the command declares none)
    std::string text; // the words in the box
    Json item; // the item as the plugin gave it
    // A `files` trigger chosen on Win顺's result menu: trigger "files",
    // action "files", the chosen files and folders (full paths), no item.
    std::string trigger;
    std::vector<std::string> files;
};

// ---- the lines ------------------------------------------------------------------

namespace detail {

class LineReader {
public:
    explicit LineReader(HANDLE handle) : m_handle(handle) {}
    bool next(std::string& line)
    {
        for (;;) {
            if (const std::size_t nl = m_buffer.find('\n'); nl != std::string::npos) {
                line.assign(m_buffer, 0, nl);
                m_buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            if (m_eof) {
                line = std::move(m_buffer);
                m_buffer.clear();
                return false;
            }
            char chunk[16 * 1024];
            DWORD got = 0;
            if (!::ReadFile(m_handle, chunk, sizeof chunk, &got, nullptr) || got == 0) {
                m_eof = true;
                continue;
            }
            m_buffer.append(chunk, got);
        }
    }

private:
    HANDLE m_handle;
    std::string m_buffer;
    bool m_eof = false;
};

inline bool writeLine(const std::string& text)
{
    const std::string line = text + "\n";
    const char* p = line.data();
    std::size_t n = line.size();
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    while (n > 0) {
        DWORD moved = 0;
        if (!::WriteFile(out, p, static_cast<DWORD>(n), &moved, nullptr) || moved == 0)
            return false;
        p += moved;
        n -= moved;
    }
    return true;
}

} // namespace detail

// ---- a once plugin --------------------------------------------------------------

// The query a once plugin was started for (from the WS_* environment).
inline Query onceQuery()
{
    Query q;
    q.command = env("WS_COMMAND");
    q.trigger = env("WS_TRIGGER");
    q.keyword = env("WS_KEYWORD");
    q.text = env("WS_QUERY");
    q.raw = env("WS_TEXT");
    if (const auto filters = Json::parse(env("WS_FILTERS")); filters && filters->isObject())
        q.filters = *filters;
    if (const auto match = Json::parse(env("WS_MATCH"))) {
        for (const Json& group : match->toArray())
            q.match.push_back(group.toString());
    }
    return q;
}
inline Json onceSettings()
{
    const std::string text = env("WS_SETTINGS");
    const auto parsed = text.empty() ? std::nullopt : Json::parse(text);
    return parsed ? *parsed : Json(Json::Object {});
}
// Prints the items (the whole answer of a once plugin) and returns 0.
inline int once(const Items& items)
{
    return detail::writeLine(Json(Json::Object {{"items", itemsJson(items)}}).dump()) ? 0 : 1;
}
// For an action (WS_ACTION set): the item it is on, and how to answer.
inline bool onceIsAction() { return !env("WS_ACTION").empty(); }
inline ActionRequest onceAction()
{
    ActionRequest a;
    a.command = env("WS_COMMAND");
    a.action = env("WS_ACTION");
    a.text = env("WS_QUERY");
    a.trigger = env("WS_TRIGGER");
    if (const auto item = Json::parse(env("WS_ITEM")))
        a.item = *item;
    if (const auto files = Json::parse(env("WS_FILES"))) {
        for (const Json& f : files->toArray())
            a.files.push_back(f.toString());
    }
    return a;
}
inline int onceEffects(const Effects& effects)
{
    Json::Array a;
    for (const Json& e : effects)
        a.push_back(e);
    return detail::writeLine(Json(Json::Object {{"effects", Json(std::move(a))}}).dump()) ? 0 : 1;
}

// ---- a resident plugin -----------------------------------------------------------

class Plugin {
public:
    virtual ~Plugin() = default;

    // The protocol loop: reads requests until stdin closes or shutdown
    // comes. The process's exit code.
    int serve()
    {
        std::string line;
        for (;;) {
            if (!m_queued.empty()) {
                line = std::move(m_queued.front());
                m_queued.pop_front();
            } else if (!m_reader.next(line))
                return 0;
            if (handle(line))
                return 0;
        }
    }

    // What the plugin was told at initialize.
    const Json& settings() const { return m_settings; }
    const std::string& locale() const { return m_locale; } // zh | en
    const std::string& theme() const { return m_theme; } // light | dark
    const std::string& pluginDir() const { return m_pluginDir; }
    const std::string& dataDir() const { return m_dataDir; }
    bool chinese() const { return m_locale != "en"; }

    // Notifications to Win顺, at any time and from any thread (the lines go
    // out one at a time).
    void log(const std::string& level, const std::string& text)
    {
        notify("winshun/log", Json::Object {{"level", level}, {"text", text}});
    }
    void notifyUser(const std::string& text) { notify("winshun/notify", Json::Object {{"text", text}}); }
    // A system notification, for when the launcher is not open (the manifest
    // asks for "notifications"; without it the text goes to the footer).
    // `click`: what a click on it does, as an action's effects would
    // (effect::setQuery("kw ") opens the launcher on those words). At most
    // one every two seconds per plugin: a newer one waiting replaces the one
    // before it. Leave out what should not stay in the Action Center.
    void toast(const std::string& title, const std::string& text, const Effects& click = {})
    {
        Json::Array a;
        for (const Json& e : click)
            a.push_back(e);
        notify("winshun/notify", Json::Object {{"title", title}, {"text", text}, {"toast", true}, {"click", Json(std::move(a))}});
    }
    void progress(long long queryId, const std::string& text)
    {
        notify("winshun/progress", Json::Object {{"queryId", queryId}, {"text", text}});
    }
    // Rows shown before the final answer (keyword mode shows them at once).
    void partial(long long queryId, const Items& items, bool replace = false)
    {
        notify("query/partial", Json::Object {{"queryId", queryId}, {"items", itemsJson(items)}, {"replace", replace}});
    }
    // What the launcher shows changed by itself (a request came, a transfer
    // ended): when the last answer asked for `rerun` and the launcher still
    // shows it, Win顺 asks again now instead of when `rerun` is up. Call it
    // after the state the next query reads is written. Otherwise nothing.
    void refresh() { notify("winshun/refresh", Json::Object {}); }
    // One of Win顺's tools (the same names and arguments as its MCP tools:
    // search_files, search_content, recent_files, list_folder, get_file_info,
    // open_item, show_in_folder, clipboard_history, clipboard_get,
    // index_status), as the manifest's permissions allow. The result, or an
    // object with "error" {code, message}. Lines meanwhile are kept for after.
    // Only from the thread in serve() (it reads the answer itself).
    Json tool(const std::string& name, Json arguments)
    {
        const long long id = m_nextId++;
        send(Json::Object {{"id", id}, {"method", "winshun/tool"}, {"params", Json::Object {{"name", name}, {"arguments", std::move(arguments)}}}});
        std::string line;
        while (m_reader.next(line)) {
            const auto message = Json::parse(line);
            if (message && message->isObject() && !message->has("method") && (*message)["id"].isNumber() && (*message)["id"].toInt() == id)
                return message->has("error") ? Json(Json::Object {{"error", (*message)["error"]}}) : (*message)["result"];
            m_queued.push_back(line);
        }
        return Json(Json::Object {{"error", Json::Object {{"code", -32000}, {"message", "Win顺 went away"}}}});
    }

protected:
    // The plugin's answers. query() must be answered; the rest have defaults.
    virtual Items query(const Query& q) = 0;
    // Enter (or another action) on an item: by default the item's `copy`
    // (else its title) is copied and the window closes.
    virtual Effects action(const ActionRequest& a)
    {
        const std::string text = a.item["copy"].isString() ? a.item["copy"].toString() : a.item["title"].toString();
        return {effect::copy(text), effect::notify(chinese() ? "已复制" : "Copied"), effect::hide()};
    }
    virtual void settingsChanged(const Json& settings) { (void)settings; }
    // Once, right after initialize: settings(), dataDir() and the rest are
    // known. A plugin that works without being asked (start "eager": a
    // server, a watcher) starts that here, not at its first query.
    virtual void initialized() {}
    // Right before the answer to shutdown: the host ends the process as soon
    // as it has that answer, so what must not be left behind (half-written
    // files, a peer waiting) is put right here. Keep it short: the host waits
    // about 2 seconds for the answer.
    virtual void shuttingDown() {}
    virtual void cancel(long long queryId) { (void)queryId; }
    virtual std::string name() const { return "plugin"; }
    // A query's whole answer, when `rerun` (seconds) or `cache` (seconds)
    // are wanted too: override queryResult instead of query.
    virtual Json queryResult(const Query& q) { return Json::Object {{"items", itemsJson(query(q))}}; }

private:
    void send(const Json& message)
    {
        Json m = message;
        m.set("jsonrpc", "2.0");
        const std::string line = m.dump();
        const std::lock_guard lock(m_sendMutex); // a whole line at a time, whichever thread sends
        detail::writeLine(line);
    }
    void notify(const std::string& method, Json params) { send(Json::Object {{"method", method}, {"params", std::move(params)}}); }
    void respond(const Json& id, Json result) { send(Json::Object {{"id", id}, {"result", std::move(result)}}); }
    void fail(const Json& id, int code, const std::string& text)
    {
        send(Json::Object {{"id", id}, {"error", Json::Object {{"code", code}, {"message", text}}}});
    }

    // One line; true when the loop is to end. What the plugin throws (no
    // memory, a bug) answers that request with an error, once, and the loop
    // goes on: a process that ends is restarted, and after three it is off.
    bool handle(const std::string& line)
    {
        if (trim(line).empty())
            return false;
        const auto message = Json::parse(line);
        if (!message || !message->isObject()) {
            debug("winshun.h: not JSON: " + line.substr(0, 200));
            return false;
        }
        const Json& m = *message;
        const std::string method = m["method"].toString();
        const Json id = m["id"];
        bool answered = false;
        try {
            return dispatch(method, id, m["params"], answered);
        } catch (const std::exception& e) {
            debug("winshun.h: " + method + " threw: " + e.what());
        } catch (...) {
            debug("winshun.h: " + method + " threw");
        }
        try {
            if (!answered && !id.isNull()) {
                if (method == "shutdown")
                    respond(id, Json::Object {}); // ended all the same
                else
                    fail(id, -32603, "Internal error");
            }
        } catch (...) {
            // Not even that: Win顺's wait for the answer runs out.
        }
        return method == "shutdown";
    }

    // `answered`: set once `id` has its answer (initialize answers before
    // initialized(): an exception there is not answered twice).
    bool dispatch(const std::string& method, const Json& id, const Json& params, bool& answered)
    {
        const auto answer = [&](Json result) {
            respond(id, std::move(result));
            answered = true;
        };
        if (method == "initialize") {
            m_settings = params["settings"];
            m_locale = params["locale"].toString();
            m_theme = params["theme"].toString();
            m_pluginDir = params["pluginDir"].toString();
            m_dataDir = params["dataDir"].toString();
            answer(Json::Object {{"protocolVersion", "1"}, {"name", name()}});
            initialized();
        } else if (method == "query") {
            Query q;
            q.queryId = params["queryId"].toInt();
            q.command = params["command"].toString();
            q.trigger = params["trigger"].toString();
            q.keyword = params["keyword"].toString();
            q.text = params["text"].toString();
            q.raw = params["raw"].toString();
            if (params["filters"].isObject()) q.filters = params["filters"];
            q.limit = static_cast<int>(params["limit"].toInt(200));
            for (const Json& group : params["match"].toArray())
                q.match.push_back(group.toString());
            answer(queryResult(q));
        } else if (method == "action") {
            ActionRequest a;
            a.command = params["command"].toString();
            a.action = params["action"].toString();
            a.text = params["text"].toString();
            a.item = params["item"];
            a.trigger = params["trigger"].toString();
            for (const Json& f : params["files"].toArray())
                a.files.push_back(f.toString());
            Json::Array effects;
            for (const Json& e : action(a))
                effects.push_back(e);
            answer(Json::Object {{"effects", Json(std::move(effects))}});
        } else if (method == "cancel") {
            cancel(params["queryId"].toInt());
        } else if (method == "settingsChanged") {
            m_settings = params["settings"];
            settingsChanged(m_settings);
        } else if (method == "shutdown") {
            shuttingDown();
            answer(Json::Object {});
            return true;
        } else if (!id.isNull()) {
            fail(id, -32601, "Method not found: " + method);
            answered = true;
        }
        return false;
    }

    detail::LineReader m_reader {::GetStdHandle(STD_INPUT_HANDLE)};
    std::mutex m_sendMutex;
    std::deque<std::string> m_queued; // lines that came while a tool call waited
    long long m_nextId = 1;
    Json m_settings;
    std::string m_locale = "zh";
    std::string m_theme = "light";
    std::string m_pluginDir;
    std::string m_dataDir;
};

} // namespace winshun
