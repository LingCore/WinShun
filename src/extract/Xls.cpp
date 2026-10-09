// Excel 97–2003 (BIFF8) and Excel 5.0/95 (BIFF5), and WPS's .et saved in
// them, by [MS-XLS]: a stream of records, the workbook's first (sheet names,
// shared strings), then each sheet's (its cells). Strings in BIFF8 are UTF-16
// or "compressed" (Latin-1, the high bytes left out), so there is no code
// page to guess; BIFF5's are in the workbook's code page.

#include "Cfb.h"
#include "Formats.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace ws::extract {

namespace {

using doctext::PlaceKind;

constexpr std::size_t kMaxStream = 512u << 20;
constexpr std::size_t kMaxStrings = 16u << 20;
constexpr std::size_t kMaxCells = 8u << 20; // per sheet

enum Record : std::uint16_t {
    kFormula = 0x0006,
    kEof = 0x000A,
    kFilePass = 0x002F,
    kContinue = 0x003C,
    kCodePage = 0x0042,
    kBoundSheet = 0x0085,
    kMulRk = 0x00BD,
    kRString = 0x00D6,
    kSst = 0x00FC,
    kLabelSst = 0x00FD,
    kNumber = 0x0203,
    kLabel = 0x0204,
    kString = 0x0207,
    kRk = 0x027E,
    kBof = 0x0809,
    kBofBiff5 = 0x0409, // BIFF4's, seen in some writers' BIFF5
};

std::uint16_t u16(std::string_view s, std::size_t at)
{
    std::uint16_t v = 0;
    if (at + 2 <= s.size())
        std::memcpy(&v, s.data() + at, 2);
    return v;
}

std::uint32_t u32(std::string_view s, std::size_t at)
{
    std::uint32_t v = 0;
    if (at + 4 <= s.size())
        std::memcpy(&v, s.data() + at, 4);
    return v;
}

std::u16string number(double v)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return {buf, r.ptr};
}

double rkValue(std::uint32_t rk)
{
    double v;
    if (rk & 2) {
        v = static_cast<double>(static_cast<std::int32_t>(rk) >> 2);
    } else {
        const std::uint64_t bits = std::uint64_t {rk & 0xFFFFFFFCu} << 32;
        std::memcpy(&v, &bits, 8);
    }
    return (rk & 1) ? v / 100 : v;
}

// A record's data and the CONTINUE records after it, read as one: a string
// cut by a CONTINUE goes on after a new flags byte (its characters may switch
// between one and two bytes there).
class Pieces {
public:
    explicit Pieces(std::vector<std::string_view> pieces)
        : m_pieces(std::move(pieces))
    {
    }

    bool byte(std::uint8_t& v)
    {
        if (!settle())
            return false;
        v = static_cast<std::uint8_t>(m_pieces[m_piece][m_pos++]);
        return true;
    }
    bool u16(std::uint16_t& v)
    {
        std::uint8_t a = 0, b = 0;
        if (!byte(a) || !byte(b))
            return false;
        v = static_cast<std::uint16_t>(a | (b << 8));
        return true;
    }
    bool u32(std::uint32_t& v)
    {
        std::uint16_t a = 0, b = 0;
        if (!u16(a) || !u16(b))
            return false;
        v = a | (std::uint32_t {b} << 16);
        return true;
    }
    bool skip(std::uint64_t n)
    {
        while (n > 0) {
            if (!settle())
                return false;
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(n, m_pieces[m_piece].size() - m_pos));
            m_pos += take;
            n -= take;
        }
        return true;
    }
    bool chars(std::size_t count, bool wide, std::u16string& out)
    {
        while (count > 0) {
            if (m_pos >= m_pieces[m_piece].size()) {
                if (m_piece + 1 >= m_pieces.size())
                    return false;
                ++m_piece; // characters go on after a flags byte
                m_pos = 0;
                std::uint8_t flags = 0;
                if (!byte(flags))
                    return false;
                wide = (flags & 1) != 0;
                continue;
            }
            const std::string_view p = m_pieces[m_piece];
            const std::size_t unit = wide ? 2 : 1;
            const std::size_t take = std::min(count, (p.size() - m_pos) / unit);
            if (take == 0) {
                m_pos = p.size(); // half a character: broken
                continue;
            }
            for (std::size_t k = 0; k < take; ++k) {
                const auto lo = static_cast<unsigned char>(p[m_pos + k * unit]);
                const auto hi = wide ? static_cast<unsigned char>(p[m_pos + k * unit + 1]) : 0;
                out.push_back(static_cast<char16_t>(lo | (hi << 8)));
            }
            m_pos += take * unit;
            count -= take;
        }
        return true;
    }

private:
    bool settle()
    {
        while (m_piece < m_pieces.size() && m_pos >= m_pieces[m_piece].size()) {
            ++m_piece;
            m_pos = 0;
        }
        return m_piece < m_pieces.size();
    }

    std::vector<std::string_view> m_pieces;
    std::size_t m_piece = 0;
    std::size_t m_pos = 0;
};

struct Cell {
    std::uint32_t row;
    std::uint16_t col;
    std::u16string text;
};

class Workbook {
public:
    Workbook(std::string_view stream, Writer& out, unsigned codePage)
        : m_stream(stream)
        , m_out(out)
        , m_codePage(codePage)
    {
    }

    Status read()
    {
        std::size_t pos = 0;
        int depth = 0;
        bool globals = true;
        bool inSheet = false; // a worksheet's own records (not a chart's in it)
        std::uint16_t sheet = doctext::Place::kNoSheet;
        std::size_t pendingFormula = SIZE_MAX; // the cell whose string a STRING record holds
        while (pos + 4 <= m_stream.size() && !m_out.full()) {
            const std::size_t at = pos;
            const std::uint16_t type = u16(m_stream, pos);
            const std::uint16_t length = u16(m_stream, pos + 2);
            if (pos + 4 + length > m_stream.size())
                break;
            const std::string_view data = m_stream.substr(pos + 4, length);
            pos += 4 + length;

            if (type == kBof || type == kBofBiff5) {
                ++depth;
                if (depth == 1 && !globals) {
                    // A sheet begins: which one is it, and is it a worksheet?
                    const std::uint16_t kind = u16(data, 2);
                    const auto it = std::find_if(m_sheets.begin(), m_sheets.end(),
                        [&](const SheetInfo& s) { return s.offset == at; });
                    inSheet = kind == 0x0010 && (it == m_sheets.end() || it->worksheet);
                    sheet = m_out.addSheet(it != m_sheets.end() ? it->name : std::string());
                    m_cells.clear();
                } else if (depth == 1) {
                    m_biff8 = u16(data, 0) == 0x0600;
                }
                continue;
            }
            if (type == kEof) {
                if (depth == 1) {
                    if (inSheet)
                        flushSheet(sheet);
                    inSheet = false;
                    globals = false;
                }
                depth = std::max(0, depth - 1);
                continue;
            }
            if (depth == 1 && globals) {
                switch (type) {
                case kFilePass:
                    return Status::Encrypted;
                case kCodePage:
                    if (const std::uint16_t cp = u16(data, 0); cp != 1200)
                        m_codePage = cp == 0x8001 ? 1252 : cp == 0x8000 ? 10000 : cp;
                    break;
                case kBoundSheet:
                    readBoundSheet(data);
                    break;
                case kSst:
                    readSst(data, pos);
                    break;
                default:
                    break;
                }
                continue;
            }
            if (depth != 1 || !inSheet || m_cells.size() >= kMaxCells)
                continue;
            const std::uint32_t row = u16(data, 0);
            const std::uint16_t col = u16(data, 2);
            switch (type) {
            case kLabelSst:
                if (const std::uint32_t i = u32(data, 6); i < m_strings.size())
                    add(row, col, m_strings[i]);
                break;
            case kLabel:
            case kRString:
                add(row, col, label(data.substr(std::min<std::size_t>(6, data.size()))));
                break;
            case kNumber:
                if (data.size() >= 14) {
                    double v;
                    std::memcpy(&v, data.data() + 6, 8);
                    add(row, col, number(v));
                }
                break;
            case kRk:
                if (data.size() >= 10)
                    add(row, col, number(rkValue(u32(data, 6))));
                break;
            case kMulRk: {
                const std::size_t n = data.size() >= 6 ? (data.size() - 6) / 6 : 0;
                for (std::size_t k = 0; k < n; ++k)
                    add(row, static_cast<std::uint16_t>(col + k), number(rkValue(u32(data, 4 + 6 * k + 2))));
                break;
            }
            case kFormula:
                if (data.size() >= 14) {
                    if (u16(data, 12) != 0xFFFF) {
                        double v;
                        std::memcpy(&v, data.data() + 6, 8);
                        add(row, col, number(v));
                    } else if (data[6] == 0) { // a string, in the STRING record that follows
                        m_cells.push_back({row, col, {}});
                        pendingFormula = m_cells.size() - 1;
                    }
                }
                break;
            case kString:
                if (pendingFormula < m_cells.size())
                    m_cells[pendingFormula].text = label(data);
                pendingFormula = SIZE_MAX;
                break;
            default:
                break;
            }
        }
        if (inSheet)
            flushSheet(sheet);
        return Status::Ok;
    }

private:
    struct SheetInfo {
        std::size_t offset;
        std::string name;
        bool worksheet;
    };

    // A string of a cell or a sheet name: BIFF8's XLUnicodeString (16-bit
    // length, flags, characters) or BIFF5's (16-bit length, bytes).
    std::u16string label(std::string_view data) const
    {
        const std::uint16_t count = u16(data, 0);
        if (!m_biff8)
            return decodeCodePage(data.substr(std::min<std::size_t>(2, data.size()), count), m_codePage);
        std::u16string out;
        Pieces p({data.substr(std::min<std::size_t>(3, data.size()))});
        p.chars(count, data.size() > 2 && (data[2] & 1), out);
        return out;
    }

    void readBoundSheet(std::string_view data)
    {
        SheetInfo s;
        s.offset = u32(data, 0);
        s.worksheet = static_cast<unsigned char>(data.size() > 5 ? data[5] : 0) == 0; // dt: 0 worksheet, 2 chart, 6 module
        const std::uint8_t count = data.size() > 6 ? static_cast<std::uint8_t>(data[6]) : 0;
        std::u16string name;
        if (m_biff8) {
            Pieces p({data.substr(std::min<std::size_t>(8, data.size()))});
            p.chars(count, data.size() > 7 && (data[7] & 1), name);
        } else {
            name = decodeCodePage(data.substr(std::min<std::size_t>(7, data.size()), count), m_codePage);
        }
        doctext::DocText scratch;
        Writer w(scratch, 1024);
        w.put(name);
        w.finish();
        s.name = scratch.text.empty() ? std::string() : scratch.text.substr(0, scratch.text.size() - 1);
        m_sheets.push_back(std::move(s));
    }

    // The shared strings: SST and the CONTINUE records after it.
    void readSst(std::string_view data, std::size_t next)
    {
        std::vector<std::string_view> pieces {data};
        while (next + 4 <= m_stream.size() && u16(m_stream, next) == kContinue) {
            const std::uint16_t length = u16(m_stream, next + 2);
            if (next + 4 + length > m_stream.size())
                break;
            pieces.push_back(m_stream.substr(next + 4, length));
            next += 4 + length;
        }
        Pieces p(std::move(pieces));
        std::uint32_t total = 0, unique = 0;
        if (!p.u32(total) || !p.u32(unique))
            return;
        std::size_t bytes = 0;
        for (std::uint32_t i = 0; i < unique && m_strings.size() < kMaxStrings; ++i) {
            std::uint16_t count = 0;
            std::uint8_t flags = 0;
            if (!p.u16(count) || !p.byte(flags))
                return;
            std::uint16_t runs = 0;
            std::uint32_t extra = 0;
            if ((flags & 0x08) && !p.u16(runs))
                return;
            if ((flags & 0x04) && !p.u32(extra))
                return;
            std::u16string s;
            if (!p.chars(count, flags & 1, s))
                return;
            bytes += s.size() * 2;
            m_strings.push_back(bytes < (256u << 20) ? std::move(s) : std::u16string());
            if (!p.skip(std::uint64_t {runs} * 4) || !p.skip(extra))
                return;
        }
    }

    void add(std::uint32_t row, std::uint16_t col, std::u16string text)
    {
        if (!text.empty())
            m_cells.push_back({row, col, std::move(text)});
    }

    void flushSheet(std::uint16_t sheet)
    {
        std::stable_sort(m_cells.begin(), m_cells.end(),
            [](const Cell& a, const Cell& b) { return a.row != b.row ? a.row < b.row : a.col < b.col; });
        std::uint32_t row = UINT32_MAX;
        for (Cell& c : m_cells) {
            if (c.text.empty())
                continue;
            for (char16_t& u : c.text) {
                if (u == '\n' || u == '\r' || u == '\t')
                    u = ' ';
            }
            if (c.row != row) {
                row = c.row;
                m_out.place(PlaceKind::Row, row + 1, sheet);
            } else {
                m_out.tab();
            }
            m_out.put(c.text);
        }
        m_out.newline();
        m_cells.clear();
    }

    std::string_view m_stream;
    Writer& m_out;
    unsigned m_codePage;
    bool m_biff8 = true;
    std::vector<SheetInfo> m_sheets;
    std::vector<std::u16string> m_strings;
    std::vector<Cell> m_cells;
};

} // namespace

Status extractXls(Cfb& cfb, Writer& out, unsigned codePage)
{
    std::string stream;
    if (!cfb.read(u"Workbook", stream, kMaxStream) && !cfb.read(u"Book", stream, kMaxStream))
        return Status::Broken;
    return Workbook(stream, out, codePage).read();
}

} // namespace ws::extract
