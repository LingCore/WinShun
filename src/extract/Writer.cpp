#include "Writer.h"

namespace ws::extract {

using doctext::Place;
using doctext::PlaceKind;

bool isCjk(char32_t c) noexcept
{
    return (c >= 0x2E80 && c <= 0x2FDF) // radicals
        || (c >= 0x3000 && c <= 0x9FFF) // punctuation, kana, bopomofo, enclosed, ideographs
        || (c >= 0xAC00 && c <= 0xD7AF) // hangul syllables
        || (c >= 0xF900 && c <= 0xFAFF) // compatibility ideographs
        || (c >= 0xFE30 && c <= 0xFE4F) // compatibility forms
        || (c >= 0xFF00 && c <= 0xFFEF) // full-width forms: ，（）！
        || (c >= 0x20000 && c <= 0x323AF);
}

Writer::Writer(doctext::DocText& out, std::size_t maxBytes)
    : m_out(out)
    , m_max(maxBytes)
{
    m_lineStart = m_out.text.size();
}

void Writer::place(PlaceKind kind, std::uint32_t number, std::uint16_t sheet)
{
    if (full())
        return;
    newline();
    m_place = {m_line, number, sheet, kind};
    m_pendingPlace = true;
}

std::uint16_t Writer::addSheet(std::string_view utf8Name)
{
    if (m_out.sheets.size() >= doctext::kMaxSheets)
        return Place::kNoSheet;
    std::string name;
    for (const char c : utf8Name.substr(0, 255))
        name.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
    m_out.sheets.push_back(std::move(name));
    return static_cast<std::uint16_t>(m_out.sheets.size() - 1);
}

void Writer::put(char32_t c)
{
    if (full())
        return;
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\t') {
        tab();
        return;
    }
    if (c == ' ' || c < 0x20 || (c >= 0x7F && c < 0xA0) || c == 0xA0 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F
        || c == 0x205F) {
        space();
        return;
    }
    if (c == 0xAD || (c >= 0x200B && c <= 0x200F) || c == 0x2060 || c == 0xFEFF || c == 0xFFFE || c == 0xFFFF)
        return; // shows nothing
    if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF)
        c = 0xFFFD;
    append(c);
}

void Writer::put(std::u16string_view s)
{
    for (const char16_t u : s) {
        if (m_high) {
            const char16_t high = m_high;
            m_high = 0;
            if (u >= 0xDC00 && u <= 0xDFFF) {
                put(0x10000 + ((static_cast<char32_t>(high) - 0xD800) << 10) + (u - 0xDC00));
                continue;
            }
            put(char32_t {0xFFFD});
        }
        if (u >= 0xD800 && u <= 0xDBFF)
            m_high = u;
        else
            put(static_cast<char32_t>(u));
    }
}

void Writer::putUtf8(std::string_view s)
{
    const auto* b = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n && !full()) {
        const unsigned char c = b[i];
        if (c < 0x80) {
            put(static_cast<char32_t>(c));
            ++i;
            continue;
        }
        std::size_t len = 0;
        char32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        }
        bool ok = len > 0 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; ++k) {
            if ((b[i + k] & 0xC0) != 0x80)
                ok = false;
            else
                cp = (cp << 6) | (b[i + k] & 0x3F);
        }
        const char32_t minimum = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (!ok || cp < minimum || cp > 0x10FFFF) {
            put(char32_t {0xFFFD});
            ++i;
            continue;
        }
        put(cp);
        i += len;
    }
}

void Writer::newline()
{
    if (!lineEmpty())
        endLine();
}

void Writer::tab()
{
    if (full() || lineEmpty() || m_last == '\t')
        return;
    if (m_last == ' ')
        m_out.text.pop_back();
    append('\t');
}

void Writer::space()
{
    if (full() || lineEmpty() || m_last == ' ' || m_last == '\t')
        return;
    append(' ');
}

void Writer::finish()
{
    if (m_high) {
        m_high = 0;
        put(char32_t {0xFFFD});
    }
    if (!lineEmpty()) {
        // Room for the line end was kept by append().
        std::string& t = m_out.text;
        while (t.size() > m_lineStart && (t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
        if (t.size() > m_lineStart) {
            t.push_back('\n');
            ++m_line;
        }
        m_lineStart = t.size();
    }
    m_pendingPlace = false;
}

void Writer::append(char32_t c)
{
    char buf[4];
    std::size_t len = 0;
    if (c < 0x80) {
        buf[len++] = static_cast<char>(c);
    } else if (c < 0x800) {
        buf[len++] = static_cast<char>(0xC0 | (c >> 6));
        buf[len++] = static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        buf[len++] = static_cast<char>(0xE0 | (c >> 12));
        buf[len++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | (c & 0x3F));
    } else {
        buf[len++] = static_cast<char>(0xF0 | (c >> 18));
        buf[len++] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | (c & 0x3F));
    }
    std::string& t = m_out.text;
    if (t.size() + len + 1 > m_max) { // + 1: the line end
        m_out.truncated = true;
        finish();
        return;
    }
    if (m_pendingPlace && lineEmpty()) {
        m_pendingPlace = false;
        Place p = m_place;
        p.line = m_line;
        std::vector<Place>& places = m_out.places;
        const auto continues = [&](const Place& before) {
            if (before.kind != p.kind || before.sheet != p.sheet)
                return false;
            return p.kind == PlaceKind::Row ? before.number + (p.line - before.line) == p.number
                                            : before.number == p.number;
        };
        if (!places.empty() && places.back().line == p.line)
            places.back() = p;
        else if (places.empty() ? p.kind != PlaceKind::None : !continues(places.back()))
            places.push_back(p);
    }
    t.append(buf, len);
    m_last = c;
}

void Writer::endLine()
{
    std::string& t = m_out.text;
    while (t.size() > m_lineStart && (t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    if (t.size() > m_lineStart) {
        t.push_back('\n');
        ++m_line;
    }
    m_lineStart = t.size();
    m_last = 0;
}

} // namespace ws::extract
