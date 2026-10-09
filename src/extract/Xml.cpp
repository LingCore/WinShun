#include "Xml.h"

namespace ws::extract {

namespace {

constexpr std::size_t kChunk = 64 * 1024;
constexpr std::size_t kMaxTag = 1 << 20; // a tag longer than this is taken as broken
constexpr std::size_t kTextPiece = 64 * 1024; // character data comes in pieces of about this size

std::string_view localName(std::string_view qualified)
{
    const std::size_t colon = qualified.rfind(':');
    return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void appendUtf8(std::string& out, char32_t c)
{
    if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF || c == 0)
        c = 0xFFFD;
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// Where a piece of `s` may end without cutting an entity or a UTF-8 sequence in two.
std::size_t safeCut(std::string_view s, std::size_t keep)
{
    std::size_t n = s.size() > keep ? s.size() - keep : 0;
    const std::size_t amp = s.substr(0, n).rfind('&');
    if (amp != std::string_view::npos && n - amp < 16 && s.substr(amp, n - amp).find(';') == std::string_view::npos)
        n = amp;
    std::size_t k = n;
    while (k > 0 && n - k < 3 && (static_cast<unsigned char>(s[k - 1]) & 0xC0) == 0x80)
        --k;
    if (k > 0) {
        const auto lead = static_cast<unsigned char>(s[k - 1]);
        const std::size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (len > 1 && k - 1 + len > n)
            n = k - 1;
    }
    return n == 0 ? s.size() : n;
}

} // namespace

void decodeXmlText(std::string_view raw, std::string& out)
{
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::size_t amp = raw.find('&', i);
        if (amp == std::string_view::npos) {
            out.append(raw.substr(i));
            return;
        }
        out.append(raw.substr(i, amp - i));
        const std::size_t semi = raw.find(';', amp);
        if (semi == std::string_view::npos || semi - amp > 12) {
            out.push_back('&');
            i = amp + 1;
            continue;
        }
        const std::string_view name = raw.substr(amp + 1, semi - amp - 1);
        bool known = true;
        if (name == "lt") {
            out.push_back('<');
        } else if (name == "gt") {
            out.push_back('>');
        } else if (name == "amp") {
            out.push_back('&');
        } else if (name == "quot") {
            out.push_back('"');
        } else if (name == "apos") {
            out.push_back('\'');
        } else if (name.size() >= 2 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            char32_t c = 0;
            bool digits = false;
            for (std::size_t k = hex ? 2 : 1; k < name.size() && known; ++k) {
                const char d = name[k];
                unsigned v = 0;
                if (d >= '0' && d <= '9')
                    v = static_cast<unsigned>(d - '0');
                else if (hex && d >= 'a' && d <= 'f')
                    v = static_cast<unsigned>(d - 'a' + 10);
                else if (hex && d >= 'A' && d <= 'F')
                    v = static_cast<unsigned>(d - 'A' + 10);
                else
                    known = false;
                c = c * (hex ? 16 : 10) + v;
                digits = true;
                if (c > 0x10FFFF)
                    c = 0x110000; // stays out of range
            }
            known = known && digits;
            if (known)
                appendUtf8(out, c);
        } else {
            known = false;
        }
        if (known) {
            i = semi + 1;
        } else {
            out.push_back('&');
            i = amp + 1;
        }
    }
}

XmlReader::XmlReader(Fill fill)
    : m_fill(std::move(fill))
{
}

bool XmlReader::more()
{
    if (m_eof)
        return false;
    if (m_pos > 0 && (m_pos > kChunk || m_pos * 2 >= m_buf.size())) {
        m_buf.erase(0, m_pos);
        m_pos = 0;
    }
    const std::size_t old = m_buf.size();
    m_buf.resize(old + kChunk);
    const std::size_t got = m_fill(m_buf.data() + old, kChunk);
    m_buf.resize(old + std::min(got, kChunk));
    if (got == 0) {
        m_eof = true;
        return false;
    }
    return true;
}

bool XmlReader::skipPast(std::string_view what)
{
    for (;;) {
        const std::size_t at = std::string_view(m_buf).substr(m_pos).find(what);
        if (at != std::string_view::npos) {
            m_pos += at + what.size();
            return true;
        }
        if (available() >= what.size())
            m_pos = m_buf.size() - (what.size() - 1);
        if (!more()) {
            m_pos = m_buf.size();
            return false;
        }
    }
}

bool XmlReader::skipDeclaration()
{
    m_pos += 2; // "<!"
    int depth = 0;
    char quote = 0;
    for (;;) {
        if (available() == 0 && !more())
            return false;
        const char c = m_buf[m_pos++];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            depth = depth > 0 ? depth - 1 : 0;
        } else if (c == '>' && depth == 0) {
            return true;
        }
    }
}

XmlReader::Token XmlReader::next()
{
    if (m_failed)
        return Token::Done;
    if (m_pendingEnd) {
        m_pendingEnd = false;
        m_attributes.clear();
        return Token::End;
    }
    if (m_inCdata)
        return readCdata();
    for (;;) {
        if (available() == 0 && !more())
            return Token::Done;
        if (m_buf[m_pos] != '<')
            return readText();
        while (available() < 9 && more()) {
        }
        const std::string_view rest(m_buf.data() + m_pos, available());
        if (rest.starts_with("<?")) {
            m_pos += 2;
            if (!skipPast("?>"))
                return Token::Done;
            continue;
        }
        if (rest.starts_with("<!--")) {
            m_pos += 4;
            if (!skipPast("-->"))
                return Token::Done;
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            m_pos += 9;
            m_inCdata = true;
            return readCdata();
        }
        if (rest.starts_with("<!")) {
            if (!skipDeclaration())
                return Token::Done;
            continue;
        }
        return readTag();
    }
}

XmlReader::Token XmlReader::readText()
{
    m_text.clear();
    for (;;) {
        const std::string_view rest(m_buf.data() + m_pos, available());
        const std::size_t lt = rest.find('<');
        if (lt != std::string_view::npos) {
            decodeXmlText(rest.substr(0, lt), m_text);
            m_pos += lt;
            return Token::Text;
        }
        if (m_eof) {
            decodeXmlText(rest, m_text);
            m_pos = m_buf.size();
            return Token::Text;
        }
        if (rest.size() >= kTextPiece) {
            const std::size_t cut = safeCut(rest, 0);
            decodeXmlText(rest.substr(0, cut), m_text);
            m_pos += cut;
            return Token::Text;
        }
        more();
    }
}

XmlReader::Token XmlReader::readCdata()
{
    m_text.clear();
    for (;;) {
        const std::string_view rest(m_buf.data() + m_pos, available());
        const std::size_t end = rest.find("]]>");
        if (end != std::string_view::npos) {
            m_text.assign(rest.substr(0, end));
            m_pos += end + 3;
            m_inCdata = false;
            return Token::Text;
        }
        if (m_eof) {
            m_text.assign(rest);
            m_pos = m_buf.size();
            m_inCdata = false;
            return Token::Text;
        }
        if (rest.size() >= kTextPiece) {
            const std::size_t cut = safeCut(rest, 2); // "]]" may be the start of the end
            m_text.assign(rest.substr(0, cut));
            m_pos += cut;
            return Token::Text;
        }
        more();
    }
}

XmlReader::Token XmlReader::readTag()
{
    // m_buf[m_pos] is '<'. Find the closing '>', which may be inside quotes.
    std::size_t i = 1;
    char quote = 0;
    for (;;) {
        if (i >= available()) {
            if (available() > kMaxTag || !more()) {
                m_failed = true;
                return Token::Done;
            }
            continue;
        }
        const char c = m_buf[m_pos + i];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            break;
        }
        ++i;
    }
    m_tag.assign(m_buf, m_pos + 1, i - 1);
    m_pos += i + 1;
    m_attributes.clear();

    std::string_view tag(m_tag);
    if (tag.starts_with('/')) {
        tag.remove_prefix(1);
        while (!tag.empty() && isSpace(tag.back()))
            tag.remove_suffix(1);
        m_name.assign(localName(tag));
        return Token::End;
    }
    const bool selfClosing = tag.ends_with('/');
    if (selfClosing)
        tag.remove_suffix(1);
    std::size_t p = 0;
    while (p < tag.size() && !isSpace(tag[p]))
        ++p;
    m_name.assign(localName(tag.substr(0, p)));
    for (;;) {
        while (p < tag.size() && isSpace(tag[p]))
            ++p;
        if (p >= tag.size())
            break;
        const std::size_t nameStart = p;
        while (p < tag.size() && tag[p] != '=' && !isSpace(tag[p]))
            ++p;
        const std::string_view attr = tag.substr(nameStart, p - nameStart);
        while (p < tag.size() && isSpace(tag[p]))
            ++p;
        if (p >= tag.size() || tag[p] != '=')
            continue; // a name without a value: not XML, skipped
        ++p;
        while (p < tag.size() && isSpace(tag[p]))
            ++p;
        if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\''))
            break;
        const char q = tag[p++];
        const std::size_t valueStart = p;
        while (p < tag.size() && tag[p] != q)
            ++p;
        m_attributes.emplace_back(attr, tag.substr(valueStart, p - valueStart));
        if (p < tag.size())
            ++p;
    }
    m_pendingEnd = selfClosing;
    return Token::Start;
}

std::string XmlReader::attribute(std::string_view name) const
{
    std::string out;
    const std::pair<std::string_view, std::string_view>* found = nullptr;
    for (const auto& a : m_attributes) {
        if (a.first == name) {
            found = &a;
            break;
        }
        if (!found && localName(a.first) == name)
            found = &a;
    }
    if (found)
        decodeXmlText(found->second, out);
    return out;
}

std::string XmlReader::prefixedAttribute(std::string_view name) const
{
    std::string out;
    for (const auto& [qualified, value] : m_attributes) {
        if (qualified.size() > name.size() && qualified.find(':') != std::string_view::npos
            && localName(qualified) == name) {
            decodeXmlText(value, out);
            break;
        }
    }
    return out;
}

} // namespace ws::extract
