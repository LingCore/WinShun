#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ws::plugin::json {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(const std::string& text)
        : m_text(text)
    {
    }

    std::optional<Value> run(std::string* problem)
    {
        skipSpace();
        std::optional<Value> value = parseValue(0);
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

    std::optional<Value> parseValue(int depth)
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
            return Value(std::move(s));
        }
        case 't':
            if (consume("true"))
                return Value(true);
            break;
        case 'f':
            if (consume("false"))
                return Value(false);
            break;
        case 'n':
            if (consume("null"))
                return Value(nullptr);
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                return parseNumber();
        }
        fail("unexpected character");
        return std::nullopt;
    }

    std::optional<Value> parseNumber()
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
        return Value(d);
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
        ++m_at; // the opening quote
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

    std::optional<Value> parseArray(int depth)
    {
        ++m_at;
        Value::Array items;
        skipSpace();
        if (peek() == ']') {
            ++m_at;
            return Value(std::move(items));
        }
        for (;;) {
            skipSpace();
            std::optional<Value> item = parseValue(depth + 1);
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
                return Value(std::move(items));
            }
            fail("expected , or ]");
            return std::nullopt;
        }
    }

    std::optional<Value> parseObject(int depth)
    {
        ++m_at;
        Value::Object members;
        skipSpace();
        if (peek() == '}') {
            ++m_at;
            return Value(std::move(members));
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
            std::optional<Value> value = parseValue(depth + 1);
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
                return Value(std::move(members));
            }
            fail("expected , or }");
            return std::nullopt;
        }
    }
};

void writeValue(std::string& out, const Value& value)
{
    switch (value.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += value.toBool() ? "true" : "false"; break;
    case Value::Type::Number: {
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
    case Value::Type::String: out += quote(value.toString()); break;
    case Value::Type::Array: {
        out += '[';
        bool first = true;
        for (const Value& item : value.toArray()) {
            if (!first)
                out += ',';
            first = false;
            writeValue(out, item);
        }
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.toObject()) {
            if (!first)
                out += ',';
            first = false;
            out += quote(key);
            out += ':';
            writeValue(out, item);
        }
        out += '}';
        break;
    }
    }
}

// The length of the well-formed UTF-8 sequence starting at s[i], a byte of
// 0x80 or more, or 0 when there is none (Unicode, table 3-7).
std::size_t utf8Sequence(const std::string& s, std::size_t i)
{
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = at(i);
    std::size_t n = 0;
    unsigned char low = 0x80, high = 0xBF; // the range of the second byte
    if (lead >= 0xC2 && lead <= 0xDF) {
        n = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
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
    } else {
        return 0;
    }
    if (i + n > s.size() || at(i + 1) < low || at(i + 1) > high)
        return 0;
    for (std::size_t k = 2; k < n; ++k)
        if ((at(i + k) & 0xC0) != 0x80)
            return 0;
    return n;
}

} // namespace

std::optional<Value> parse(const std::string& text, std::string* problem)
{
    return Parser(text).run(problem);
}

std::string quote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
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
            } else if (static_cast<unsigned char>(c) < 0x80) {
                out += c;
            } else if (const std::size_t n = utf8Sequence(s, i)) {
                out.append(s, i, n);
                i += n - 1;
            } else {
                out += "\xEF\xBF\xBD"; // U+FFFD: a byte that is not UTF-8 never makes the line invalid
            }
        }
    }
    out += '"';
    return out;
}

std::string write(const Value& value)
{
    std::string out;
    writeValue(out, value);
    return out;
}

} // namespace ws::plugin::json
