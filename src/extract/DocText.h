#pragma once

// The text of a document (Word, Excel, PowerPoint, PDF...) as the extractor
// (WinShunExtract.exe) hands it over: UTF-8 lines, and where in the document
// each line is (page, slide, row of a sheet). The extractor writes it; the
// app keeps it with the content index and searches it, so a search never
// parses a document. Qt-free: the extractor does not load Qt.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ws::doctext {

enum class PlaceKind : std::uint8_t {
    None = 0, // nowhere in particular: a Word document without page breaks, its notes, headers
    Page = 1,
    Slide = 2,
    Row = 3, // of a sheet: the number counts up by one per line
};

// Where the lines from `line` on are, up to the next place.
struct Place {
    static constexpr std::uint16_t kNoSheet = 0xFFFF;

    std::uint32_t line = 1; // 1-based
    std::uint32_t number = 0; // page, slide or row (of `line`)
    std::uint16_t sheet = kNoSheet; // Row: index into DocText::sheets
    PlaceKind kind = PlaceKind::None;

    friend bool operator==(const Place&, const Place&) = default;
};

struct DocText {
    std::string text; // UTF-8, every line ends with '\n'
    std::vector<Place> places; // ascending by line
    std::vector<std::string> sheets; // names, UTF-8
    bool truncated = false; // stopped at the size limit

    friend bool operator==(const DocText&, const DocText&) = default;
};

struct Location {
    PlaceKind kind = PlaceKind::None;
    std::uint32_t number = 0;
    std::string sheet;
};

// Where line `line` (1-based) is.
inline Location locate(const DocText& doc, std::uint32_t line)
{
    const auto it = std::upper_bound(doc.places.begin(), doc.places.end(), line,
        [](std::uint32_t l, const Place& p) { return l < p.line; });
    if (it == doc.places.begin())
        return {};
    const Place& p = *(it - 1);
    Location out;
    out.kind = p.kind;
    out.number = p.kind == PlaceKind::Row ? p.number + (line - p.line) : p.number;
    if (p.sheet < doc.sheets.size())
        out.sheet = doc.sheets[p.sheet];
    return out;
}

// ---- serialized form ---------------------------------------------------------
//
//   u32 magic, u32 flags (1: truncated), u32 placeCount, u32 sheetCount, u32 textBytes
//   placeCount × (u32 line, u32 number, u16 sheet, u8 kind, u8 0)
//   sheetCount × (u16 bytes, UTF-8)
//   text

inline constexpr std::uint32_t kMagic = 0x31545844; // "DXT1"
inline constexpr std::size_t kMaxPlaces = std::size_t {1} << 22;
inline constexpr std::size_t kMaxSheets = 65535;

namespace detail {

inline void put32(std::string& out, std::uint32_t v)
{
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}

inline void put16(std::string& out, std::uint16_t v)
{
    char b[2];
    std::memcpy(b, &v, 2);
    out.append(b, 2);
}

class Reader {
public:
    explicit Reader(std::string_view data)
        : m_data(data)
    {
    }
    template <typename T> bool get(T& v)
    {
        if (m_data.size() - m_pos < sizeof(T))
            return false;
        std::memcpy(&v, m_data.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return true;
    }
    bool bytes(std::size_t n, std::string_view& out)
    {
        if (m_data.size() - m_pos < n)
            return false;
        out = m_data.substr(m_pos, n);
        m_pos += n;
        return true;
    }
    bool atEnd() const noexcept { return m_pos == m_data.size(); }

private:
    std::string_view m_data;
    std::size_t m_pos = 0;
};

} // namespace detail

inline std::string serialize(const DocText& doc)
{
    std::string out;
    out.reserve(20 + doc.places.size() * 12 + doc.text.size());
    detail::put32(out, kMagic);
    detail::put32(out, doc.truncated ? 1 : 0);
    detail::put32(out, static_cast<std::uint32_t>(doc.places.size()));
    detail::put32(out, static_cast<std::uint32_t>(doc.sheets.size()));
    detail::put32(out, static_cast<std::uint32_t>(doc.text.size()));
    for (const Place& p : doc.places) {
        detail::put32(out, p.line);
        detail::put32(out, p.number);
        detail::put16(out, p.sheet);
        out.push_back(static_cast<char>(p.kind));
        out.push_back('\0');
    }
    for (const std::string& s : doc.sheets) {
        const std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
        detail::put16(out, static_cast<std::uint16_t>(n));
        out.append(s.data(), n);
    }
    out.append(doc.text);
    return out;
}

// What serialize() wrote, checked throughout: it comes from another process
// (or a file) and is not trusted. The text itself may still be any bytes.
inline std::optional<DocText> deserialize(std::string_view data, std::size_t maxText)
{
    detail::Reader r(data);
    std::uint32_t magic = 0, flags = 0, placeCount = 0, sheetCount = 0, textBytes = 0;
    if (!r.get(magic) || magic != kMagic || !r.get(flags) || !r.get(placeCount) || !r.get(sheetCount)
        || !r.get(textBytes) || placeCount > kMaxPlaces || sheetCount > kMaxSheets || textBytes > maxText)
        return std::nullopt;
    DocText doc;
    doc.truncated = (flags & 1) != 0;
    doc.places.resize(placeCount);
    std::uint32_t lastLine = 0;
    for (Place& p : doc.places) {
        std::uint8_t kind = 0, pad = 0;
        if (!r.get(p.line) || !r.get(p.number) || !r.get(p.sheet) || !r.get(kind) || !r.get(pad))
            return std::nullopt;
        if (kind > static_cast<std::uint8_t>(PlaceKind::Row) || p.line == 0 || p.line < lastLine)
            return std::nullopt;
        p.kind = static_cast<PlaceKind>(kind);
        lastLine = p.line;
    }
    doc.sheets.resize(sheetCount);
    for (std::string& s : doc.sheets) {
        std::uint16_t n = 0;
        std::string_view bytes;
        if (!r.get(n) || !r.bytes(n, bytes))
            return std::nullopt;
        s.assign(bytes);
    }
    for (const Place& p : doc.places) {
        if (p.sheet != Place::kNoSheet && p.sheet >= sheetCount)
            return std::nullopt;
    }
    std::string_view text;
    if (!r.bytes(textBytes, text) || !r.atEnd())
        return std::nullopt;
    doc.text.assign(text);
    return doc;
}

} // namespace ws::doctext
