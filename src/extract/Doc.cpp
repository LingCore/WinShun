// Word 97–2003 (.doc, and WPS's .wps saved in it), by [MS-DOC]: the text is
// in the WordDocument stream, in pieces listed by the piece table (the Clx
// in the table stream). A piece is either UTF-16 or "compressed": one byte
// per character in code page 1252. Chinese is always in UTF-16 pieces.

#include "Cfb.h"
#include "Formats.h"

#include <cstring>

namespace ws::extract {

namespace {

constexpr std::size_t kMaxStream = 512u << 20;

std::uint32_t u32At(const std::string& s, std::size_t at)
{
    std::uint32_t v = 0;
    if (at + 4 <= s.size())
        std::memcpy(&v, s.data() + at, 4);
    return v;
}

std::uint16_t u16At(const std::string& s, std::size_t at)
{
    std::uint16_t v = 0;
    if (at + 2 <= s.size())
        std::memcpy(&v, s.data() + at, 2);
    return v;
}

// Code page 1252's 0x80–0x9F (the rest of the byte range is Latin-1).
constexpr char16_t kCp1252High[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
    0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

// Word's special characters, and fields: { code \x14 result } where only the
// result shows (the code is like "HYPERLINK http://..." or "PAGE").
class WordChars {
public:
    explicit WordChars(Writer& out)
        : m_out(out)
    {
    }

    void put(char16_t c)
    {
        switch (c) {
        case 0x13: // field begin
            m_fields.push_back(true); // in its code
            return;
        case 0x14: // field separator: the result follows
            if (!m_fields.empty())
                m_fields.back() = false;
            return;
        case 0x15: // field end
            if (!m_fields.empty())
                m_fields.pop_back();
            return;
        default:
            break;
        }
        if (inCode())
            return;
        switch (c) {
        case 0x0D: // paragraph end
        case 0x0C: // page or section break
            m_out.newline();
            break;
        case 0x0B: // line break
            m_out.space();
            break;
        case 0x07: // end of a table cell, or of a row
            m_out.tab();
            break;
        case 0x09:
            m_out.tab();
            break;
        case 0x1E: // non-breaking hyphen
            m_out.put(char32_t {'-'});
            break;
        case 0x1F: // optional hyphen
        case 0x01: // picture
        case 0x02: // footnote mark
        case 0x03: // footnote separator
        case 0x04: // footnote continuation
        case 0x05: // comment mark
        case 0x08: // drawn object
            break;
        default:
            m_out.put(std::u16string_view(&c, 1));
            break;
        }
    }

private:
    bool inCode() const
    {
        for (const bool code : m_fields) {
            if (code)
                return true;
        }
        return false;
    }

    Writer& m_out;
    std::vector<bool> m_fields; // open fields, innermost last: still in the code?
};

} // namespace

Status extractDoc(Cfb& cfb, Writer& out)
{
    std::string word;
    if (!cfb.read(u"WordDocument", word, kMaxStream) || word.size() < 0x200)
        return Status::Broken;
    if (u16At(word, 0) != 0xA5EC)
        return Status::Broken;
    const std::uint16_t flags = u16At(word, 0x0A);
    if (flags & 0x0100)
        return Status::Encrypted; // fEncrypted
    // The FIB: a fixed base, then counted arrays of 16-bit and 32-bit values,
    // then fc/lcb pairs. Word 6/95 files have another layout.
    const std::uint16_t csw = u16At(word, 0x20);
    const std::size_t lwAt = 0x22 + std::size_t {csw} * 2;
    const std::uint16_t cslw = u16At(word, lwAt);
    const std::size_t fcLcbAt = lwAt + 2 + std::size_t {cslw} * 4;
    const std::uint16_t fcLcbCount = u16At(word, fcLcbAt);
    if (csw != 14 || cslw < 22 || fcLcbCount < 0x5D)
        return Status::Unsupported; // Word 95 or older
    const std::size_t blob = fcLcbAt + 2;
    const std::uint32_t fcClx = u32At(word, blob + 33 * 8);
    const std::uint32_t lcbClx = u32At(word, blob + 33 * 8 + 4);

    std::string table;
    if (!cfb.read((flags & 0x0200) ? u"1Table" : u"0Table", table, kMaxStream))
        return Status::Broken;
    if (std::uint64_t {fcClx} + lcbClx > table.size() || lcbClx == 0)
        return Status::Broken;

    // The Clx: property runs (skipped), then the piece table.
    std::size_t at = fcClx;
    const std::size_t end = std::size_t {fcClx} + lcbClx;
    std::size_t plcAt = 0;
    std::uint32_t plcBytes = 0;
    while (at < end) {
        const auto kind = static_cast<unsigned char>(table[at]);
        if (kind == 0x01) {
            at += 3 + u16At(table, at + 1);
        } else if (kind == 0x02) {
            plcBytes = u32At(table, at + 1);
            plcAt = at + 5;
            break;
        } else {
            return Status::Broken;
        }
    }
    if (plcAt == 0 || plcAt + plcBytes > end || plcBytes < 4 + 12)
        return Status::Broken;

    // PlcPcd: n + 1 character positions, then n piece descriptors of 8 bytes.
    const std::size_t pieces = (plcBytes - 4) / 12;
    WordChars text(out);
    for (std::size_t i = 0; i < pieces && !out.full(); ++i) {
        const std::uint32_t cpStart = u32At(table, plcAt + 4 * i);
        const std::uint32_t cpEnd = u32At(table, plcAt + 4 * (i + 1));
        if (cpEnd <= cpStart)
            continue;
        const std::size_t pcd = plcAt + 4 * (pieces + 1) + 8 * i;
        const std::uint32_t fc = u32At(table, pcd + 2);
        const bool compressed = (fc & 0x40000000) != 0;
        const std::uint64_t offset = compressed ? (fc & 0x3FFFFFFF) / 2 : (fc & 0x3FFFFFFF);
        const std::uint64_t count = cpEnd - cpStart;
        const std::uint64_t bytes = compressed ? count : count * 2;
        if (offset >= word.size())
            continue;
        const std::uint64_t available = std::min<std::uint64_t>(bytes, word.size() - offset);
        const char* p = word.data() + offset;
        if (compressed) {
            for (std::uint64_t k = 0; k < available; ++k) {
                const auto b = static_cast<unsigned char>(p[k]);
                text.put(b >= 0x80 && b < 0xA0 ? kCp1252High[b - 0x80] : static_cast<char16_t>(b));
            }
        } else {
            for (std::uint64_t k = 0; k + 1 < available; k += 2) {
                char16_t c;
                std::memcpy(&c, p + k, 2);
                text.put(c);
            }
        }
    }
    out.newline();
    return Status::Ok;
}

} // namespace ws::extract
