// Documents that are text files under a document's name: HTML or XML tables
// that web systems export as ".xls", Excel 2003's XML spreadsheets, CSV
// saved as ".xls", and the like.

#include "Formats.h"

#include <algorithm>
#include <cstring>

namespace ws::extract {

namespace {

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool startsWithFolded(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower(s[i]) != prefix[i])
            return false;
    }
    return true;
}

bool isUtf8(std::string_view b)
{
    std::size_t i = 0;
    while (i < b.size()) {
        const auto c = static_cast<unsigned char>(b[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const std::size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (len == 0)
            return false;
        if (i + len > b.size())
            return true; // cut off at the end of what was read
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(b[i + k]) & 0xC0) != 0x80)
                return false;
        }
        i += len;
    }
    return true;
}

// The code page an HTML or XML file names: <meta charset="gb2312">, <?xml encoding="GBK"?>.
unsigned declaredCodePage(std::string_view head)
{
    std::string folded(head.substr(0, 4096));
    for (char& c : folded)
        c = lower(c);
    for (const std::string_view key : {"charset=", "encoding="}) {
        std::size_t at = folded.find(key);
        if (at == std::string::npos)
            continue;
        at += key.size();
        while (at < folded.size() && (folded[at] == '"' || folded[at] == '\'' || folded[at] == ' '))
            ++at;
        const std::string_view name = std::string_view(folded).substr(at, 12);
        if (name.starts_with("utf-8") || name.starts_with("utf8"))
            return 65001;
        if (name.starts_with("gb2312") || name.starts_with("gbk") || name.starts_with("gb18030")
            || name.starts_with("x-gbk"))
            return name.starts_with("gb18030") ? 54936 : 936;
        if (name.starts_with("big5"))
            return 950;
        if (name.starts_with("shift_jis") || name.starts_with("sjis"))
            return 932;
        if (name.starts_with("euc-kr"))
            return 949;
        if (name.starts_with("windows-1252") || name.starts_with("iso-8859-1"))
            return 1252;
    }
    return 0;
}

// The file as UTF-16: a byte order mark, else what it declares, else UTF-8
// if it is valid, else the legacy code page.
std::u16string decodeText(std::string_view bytes, unsigned codePage, bool markup)
{
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 2 && ((at(0) == 0xFF && at(1) == 0xFE) || (at(0) == 0xFE && at(1) == 0xFF))) {
        const bool big = at(0) == 0xFE;
        std::u16string out;
        out.reserve(bytes.size() / 2);
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
            out.push_back(static_cast<char16_t>(big ? (at(i) << 8) | at(i + 1) : (at(i + 1) << 8) | at(i)));
        return out;
    }
    if (bytes.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF)
        return decodeCodePage(bytes.substr(3), 65001);
    unsigned cp = markup ? declaredCodePage(bytes) : 0;
    if (cp == 0)
        cp = isUtf8(bytes.substr(0, 1u << 20)) ? 65001 : codePage;
    return decodeCodePage(bytes, cp);
}

bool isSpace(char16_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// &nbsp;, &lt; and the like, and character references.
char32_t entity(std::u16string_view name)
{
    if (name.size() >= 2 && name[0] == '#') {
        const bool hex = name[1] == 'x' || name[1] == 'X';
        char32_t v = 0;
        for (std::size_t i = hex ? 2 : 1; i < name.size(); ++i) {
            const char16_t d = name[i];
            unsigned digit = 0;
            if (d >= '0' && d <= '9')
                digit = d - '0';
            else if (hex && d >= 'a' && d <= 'f')
                digit = d - 'a' + 10;
            else if (hex && d >= 'A' && d <= 'F')
                digit = d - 'A' + 10;
            else
                return 0;
            v = v * (hex ? 16 : 10) + digit;
            if (v > 0x10FFFF)
                return 0;
        }
        return v;
    }
    static constexpr std::pair<std::u16string_view, char32_t> kNamed[] = {{u"nbsp", ' '}, {u"lt", '<'}, {u"gt", '>'},
        {u"amp", '&'}, {u"quot", '"'}, {u"apos", '\''}, {u"yen", 0xA5}, {u"copy", 0xA9}, {u"reg", 0xAE},
        {u"middot", 0xB7}, {u"times", 0xD7}, {u"divide", 0xF7}, {u"ldquo", 0x201C}, {u"rdquo", 0x201D},
        {u"lsquo", 0x2018}, {u"rsquo", 0x2019}, {u"mdash", 0x2014}, {u"ndash", 0x2013}, {u"hellip", 0x2026},
        {u"emsp", ' '}, {u"ensp", ' '}};
    for (const auto& [n, c] : kNamed) {
        if (n == name)
            return c;
    }
    return 0;
}

} // namespace

std::u16string decodeCodePage(std::string_view bytes, unsigned codePage)
{
    std::u16string out;
    std::size_t done = 0;
    while (done < bytes.size()) {
        // In pieces that end on a character for double-byte code pages.
        std::size_t n = std::min<std::size_t>(bytes.size() - done, 1u << 24);
        if (done + n < bytes.size() && codePage != 65001) {
            std::size_t k = 0;
            while (k < n)
                k += ::IsDBCSLeadByteEx(codePage, static_cast<BYTE>(bytes[done + k])) ? 2 : 1;
            n = k > n ? n - 1 : n;
        } else if (done + n < bytes.size()) {
            while (n > 0 && (static_cast<unsigned char>(bytes[done + n]) & 0xC0) == 0x80)
                --n;
        }
        if (n == 0)
            break;
        const int wide = ::MultiByteToWideChar(codePage, 0, bytes.data() + done, static_cast<int>(n), nullptr, 0);
        if (wide > 0) {
            const std::size_t old = out.size();
            out.resize(old + static_cast<std::size_t>(wide));
            ::MultiByteToWideChar(codePage, 0, bytes.data() + done, static_cast<int>(n),
                reinterpret_cast<wchar_t*>(out.data() + old), wide);
        }
        done += n;
    }
    return out;
}

Status extractPlain(std::string_view bytes, Writer& out, unsigned codePage)
{
    const std::u16string text = decodeText(bytes, codePage, false);
    std::size_t start = 0;
    while (start < text.size() && !out.full()) {
        std::size_t end = text.find(u'\n', start);
        if (end == std::u16string::npos)
            end = text.size();
        out.put(std::u16string_view(text).substr(start, end - start));
        out.newline();
        start = end + 1;
    }
    return Status::Ok;
}

Status extractMarkup(std::string_view bytes, Writer& out, unsigned codePage)
{
    const std::u16string text = decodeText(bytes, codePage, true);
    const std::u16string_view s(text);
    std::size_t i = 0;
    while (i < s.size() && !out.full()) {
        const char16_t c = s[i];
        if (c == '&') {
            const std::size_t semi = s.find(u';', i);
            if (semi != std::u16string_view::npos && semi - i <= 10) {
                if (const char32_t e = entity(s.substr(i + 1, semi - i - 1))) {
                    out.put(e);
                    i = semi + 1;
                    continue;
                }
            }
            out.put(char32_t {'&'});
            ++i;
            continue;
        }
        if (c != '<') {
            if (isSpace(c))
                out.space(); // markup's line ends are not the text's
            else
                out.put(std::u16string_view(&s[i], 1));
            ++i;
            continue;
        }
        if (s.substr(i).starts_with(u"<!--")) {
            const std::size_t end = s.find(u"-->", i + 4);
            i = end == std::u16string_view::npos ? s.size() : end + 3;
            continue;
        }
        // A tag: its name decides what it means for the text.
        std::size_t end = i + 1;
        char16_t quote = 0;
        while (end < s.size() && (quote || s[end] != '>')) {
            if (quote && s[end] == quote)
                quote = 0;
            else if (!quote && (s[end] == '"' || s[end] == '\''))
                quote = s[end];
            ++end;
        }
        std::string name;
        std::size_t k = i + 1;
        const bool closing = k < s.size() && s[k] == '/';
        if (closing)
            ++k;
        for (; k < end && !isSpace(s[k]) && s[k] != '/' && name.size() < 16; ++k)
            name.push_back(s[k] < 0x80 ? lower(static_cast<char>(s[k])) : '?');
        if (const std::size_t colon = name.find(':'); colon != std::string::npos)
            name.erase(0, colon + 1); // ss:Row
        const bool selfClosing = end < s.size() && end > i + 1 && s[end - 1] == '/';
        i = end < s.size() ? end + 1 : s.size();
        if ((name == "script" || name == "style") && !closing && !selfClosing) {
            const std::u16string close = name == "script" ? u"</script" : u"</style";
            std::size_t at = i;
            for (;;) {
                at = s.find(u"</", at);
                if (at == std::u16string_view::npos) {
                    i = s.size();
                    break;
                }
                std::string tail;
                for (std::size_t m = at; m < s.size() && m < at + close.size(); ++m)
                    tail.push_back(s[m] < 0x80 ? lower(static_cast<char>(s[m])) : '?');
                if (std::equal(tail.begin(), tail.end(), close.begin(), close.end())) {
                    const std::size_t gt = s.find(u'>', at);
                    i = gt == std::u16string_view::npos ? s.size() : gt + 1;
                    break;
                }
                at += 2;
            }
            continue;
        }
        if (name == "td" || name == "th" || name == "cell")
            out.tab();
        else if (name == "br" || name == "p" || name == "div" || name == "tr" || name == "li" || name == "table"
            || name == "row" || name == "worksheet" || name == "title"
            || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6'))
            out.newline();
    }
    out.newline();
    return Status::Ok;
}

bool looksLikeMarkup(std::string_view head)
{
    std::size_t i = 0;
    if (head.starts_with("\xEF\xBB\xBF"))
        i = 3;
    while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n'))
        ++i;
    const std::string_view rest = head.substr(i);
    return rest.starts_with('<')
        && (startsWithFolded(rest, "<html") || startsWithFolded(rest, "<!doctype") || startsWithFolded(rest, "<?xml")
            || startsWithFolded(rest, "<table") || startsWithFolded(rest, "<meta") || startsWithFolded(rest, "<head")
            || startsWithFolded(rest, "<body") || startsWithFolded(rest, "<!--") || startsWithFolded(rest, "<style"));
}

} // namespace ws::extract
