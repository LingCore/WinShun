// Office Open XML: Word (docx), Excel (xlsx) and PowerPoint (pptx), with
// their macro-enabled and template kinds, and WPS files saved in them.

#include "Formats.h"
#include "Xml.h"
#include "Zip.h"

#include <algorithm>
#include <charconv>

namespace ws::extract {

namespace {

using doctext::PlaceKind;
using Token = XmlReader::Token;

// Text in a Word body beyond this many bytes, without a page break in it,
// is more than a page: the document was not laid out by Word (generated, or
// saved by another program), so its pages are not known.
constexpr std::size_t kOnePageBytes = 5000;
constexpr std::size_t kMaxRelationships = 100000;
constexpr std::size_t kMaxSharedStringBytes = 64u << 20;

struct Relationship {
    std::string id;
    std::string type;
    std::string target; // part path, resolved
};

XmlReader::Fill fillFrom(Zip::Entry& entry)
{
    return [&entry](char* buffer, std::size_t capacity) { return entry.read(buffer, capacity); };
}

std::string relationshipsPathOf(std::string_view part)
{
    const std::size_t slash = part.rfind('/');
    const std::string_view folder = slash == std::string_view::npos ? std::string_view() : part.substr(0, slash + 1);
    const std::string_view file = slash == std::string_view::npos ? part : part.substr(slash + 1);
    std::string out(folder);
    out += "_rels/";
    out += file;
    out += ".rels";
    return out;
}

// "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"
// (or the Strict namespace) is of kind "officeDocument".
bool isKind(std::string_view type, std::string_view kind)
{
    return type.size() > kind.size() && type.ends_with(kind) && type[type.size() - kind.size() - 1] == '/';
}

// The relationships of a part ("" for the package's own), targets resolved.
std::vector<Relationship> readRelationships(Zip& zip, std::string_view part)
{
    std::vector<Relationship> out;
    const auto index = zip.find(relationshipsPathOf(part));
    if (!index)
        return out;
    const auto entry = zip.open(*index);
    if (!entry)
        return out;
    XmlReader xml(fillFrom(*entry));
    for (Token t = xml.next(); t != Token::Done && out.size() < kMaxRelationships; t = xml.next()) {
        if (t != Token::Start || xml.name() != "Relationship" || xml.attribute("TargetMode") == "External")
            continue;
        out.push_back({xml.attribute("Id"), xml.attribute("Type"), resolvePartPath(part, xml.attribute("Target"))});
    }
    return out;
}

const Relationship* byId(const std::vector<Relationship>& rels, std::string_view id)
{
    for (const Relationship& r : rels) {
        if (r.id == id)
            return &r;
    }
    return nullptr;
}

// The local name of a part's first element: what kind of part it is.
std::string rootElementOf(Zip& zip, std::size_t index)
{
    const auto entry = zip.open(index);
    if (!entry)
        return {};
    XmlReader xml(fillFrom(*entry));
    for (Token t = xml.next(); t != Token::Done; t = xml.next()) {
        if (t == Token::Start)
            return std::string(xml.name());
    }
    return {};
}

template <typename F> bool readPart(Zip& zip, std::string_view part, F&& f)
{
    const auto index = zip.find(part);
    if (!index)
        return false;
    const auto entry = zip.open(*index);
    if (!entry)
        return false;
    XmlReader xml(fillFrom(*entry));
    f(xml);
    return true;
}

// Cell text on one line: line breaks in a cell (Alt+Enter) and tabs become spaces.
void putCell(Writer& out, std::string_view text)
{
    std::string flat(text);
    for (char& c : flat) {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }
    out.putUtf8(flat);
}

// ---- Word and DrawingML text ---------------------------------------------------

// The text of WordprocessingML (Word) and DrawingML (slides, text boxes):
// paragraphs, the text of runs, tabs and breaks, tables row by row (cells
// apart by tabs). With `pages`, Word's page breaks advance the page: the
// ones it laid out when last saving (lastRenderedPageBreak) and the hard
// ones. A paragraph counts as on the page it starts on, so that it stays one
// line and a phrase across the break is still found.
class FlowText {
public:
    FlowText(Writer& out, bool pages)
        : m_out(out)
        , m_pages(pages)
        , m_bytesAtBreak(out.bytes())
    {
    }

    void read(XmlReader& xml)
    {
        std::vector<std::string> stack; // local names of the open elements
        int inText = 0; // inside <t>: run text (w:t, a:t, m:t)
        int skip = 0; // inside an element left out with all it holds
        int tables = 0;
        for (Token t = xml.next(); t != Token::Done && !m_out.full(); t = xml.next()) {
            if (t == Token::Start) {
                const std::string_view name = xml.name();
                const std::string parent = stack.empty() ? std::string() : stack.back();
                stack.emplace_back(name);
                if (skip > 0 || name == "Fallback") { // mc:Fallback repeats mc:Choice for older readers
                    ++skip;
                } else if (name == "t") {
                    ++inText;
                } else if (name == "tbl") {
                    ++tables;
                } else if ((name == "tab" || name == "ptab") && parent == "r") {
                    m_out.tab(); // a tab in the text; <w:tab> elsewhere is a tab stop
                } else if (name == "br") {
                    if (m_pages && xml.attribute("type") == "page")
                        pageBreak();
                    else
                        m_out.put(char32_t {' '}); // a line break within the paragraph
                } else if (name == "cr") {
                    m_out.put(char32_t {' '});
                } else if (name == "lastRenderedPageBreak") {
                    if (m_pages)
                        pageBreak();
                } else if (name == "noBreakHyphen") {
                    m_out.put(char32_t {'-'});
                }
            } else if (t == Token::End) {
                const std::string_view name = xml.name();
                if (!stack.empty())
                    stack.pop_back();
                if (skip > 0) {
                    --skip;
                } else if (name == "t") {
                    inText = std::max(0, inText - 1);
                } else if (name == "p") {
                    if (tables > 0) {
                        m_out.space();
                    } else {
                        m_out.newline();
                        lineEnded();
                    }
                } else if (name == "tc") {
                    m_out.tab();
                } else if (name == "tr") {
                    m_out.newline();
                    lineEnded();
                } else if (name == "tbl") {
                    tables = std::max(0, tables - 1);
                    m_out.newline();
                    lineEnded();
                }
            } else if (inText > 0 && skip == 0) {
                m_out.putUtf8(xml.text());
            }
        }
    }

    bool sawPageBreak() const noexcept { return m_breaks > 0; }

private:
    void pageBreak()
    {
        if (m_out.bytes() == m_bytesAtBreak)
            return; // nothing since the last one: the same break twice (hard, then as laid out)
        m_bytesAtBreak = m_out.bytes();
        ++m_page;
        ++m_breaks;
        if (m_out.lineEmpty())
            m_out.place(PlaceKind::Page, m_page);
        else
            m_pendingPage = true;
    }

    void lineEnded()
    {
        if (m_pendingPage) {
            m_pendingPage = false;
            m_out.place(PlaceKind::Page, m_page);
        }
    }

    Writer& m_out;
    bool m_pages;
    std::uint32_t m_page = 1;
    int m_breaks = 0;
    std::size_t m_bytesAtBreak;
    bool m_pendingPage = false;
};

Status readWord(Zip& zip, const std::string& main, Writer& out, doctext::DocText& doc)
{
    const std::size_t placesBefore = doc.places.size();
    const std::size_t bytesBefore = out.bytes();
    out.place(PlaceKind::Page, 1);
    FlowText body(out, true);
    if (!readPart(zip, main, [&](XmlReader& xml) { body.read(xml); }))
        return Status::Broken;
    out.newline();
    if (!body.sawPageBreak() && out.bytes() - bytesBefore > kOnePageBytes) {
        doc.places.erase(std::remove_if(doc.places.begin() + static_cast<std::ptrdiff_t>(placesBefore),
                             doc.places.end(), [](const doctext::Place& p) { return p.kind == PlaceKind::Page; }),
            doc.places.end());
    }

    // Then what is outside the body: notes, comments, headers and footers.
    out.place(PlaceKind::None, 0);
    const std::vector<Relationship> rels = readRelationships(zip, main);
    for (const std::string_view kind : {"footnotes", "endnotes", "comments", "header", "footer"}) {
        for (const Relationship& r : rels) {
            if (!isKind(r.type, kind) || out.full())
                continue;
            FlowText part(out, false);
            readPart(zip, r.target, [&](XmlReader& xml) { part.read(xml); });
            out.newline();
        }
    }
    return Status::Ok;
}

// ---- PowerPoint ----------------------------------------------------------------

Status readPresentation(Zip& zip, const std::string& main, Writer& out)
{
    const std::vector<Relationship> rels = readRelationships(zip, main);
    std::vector<std::string> slides; // in show order
    if (!readPart(zip, main, [&](XmlReader& xml) {
            for (Token t = xml.next(); t != Token::Done; t = xml.next()) {
                if (t == Token::Start && xml.name() == "sldId") {
                    if (const Relationship* r = byId(rels, xml.prefixedAttribute("id")))
                        slides.push_back(r->target);
                }
            }
        }))
        return Status::Broken;
    std::uint32_t number = 0;
    for (const std::string& slide : slides) {
        if (out.full())
            break;
        out.place(PlaceKind::Slide, ++number);
        FlowText text(out, false);
        readPart(zip, slide, [&](XmlReader& xml) { text.read(xml); });
        out.newline();
        for (const Relationship& r : readRelationships(zip, slide)) { // the speaker's notes
            if (isKind(r.type, "notesSlide")) {
                FlowText notes(out, false);
                readPart(zip, r.target, [&](XmlReader& xml) { notes.read(xml); });
                out.newline();
            }
        }
    }
    return Status::Ok;
}

// ---- Excel ---------------------------------------------------------------------

// The shared strings: the text of most cells, by number. Phonetic runs
// (rPh, the readings above Japanese text) are left out.
std::vector<std::string> readSharedStrings(Zip& zip, const std::string& part, std::size_t maxBytes)
{
    std::vector<std::string> strings;
    std::size_t total = 0;
    readPart(zip, part, [&](XmlReader& xml) {
        std::string current;
        bool inItem = false;
        int inText = 0;
        int skip = 0;
        for (Token t = xml.next(); t != Token::Done; t = xml.next()) {
            if (t == Token::Start) {
                const std::string_view name = xml.name();
                if (skip > 0 || name == "rPh") {
                    ++skip;
                } else if (name == "si") {
                    inItem = true;
                    current.clear();
                } else if (name == "t") {
                    ++inText;
                }
            } else if (t == Token::End) {
                const std::string_view name = xml.name();
                if (skip > 0) {
                    --skip;
                } else if (name == "t") {
                    inText = std::max(0, inText - 1);
                } else if (name == "si") {
                    inItem = false;
                    total += current.size();
                    strings.push_back(total <= maxBytes ? std::move(current) : std::string());
                    current = std::string();
                }
            } else if (inItem && inText > 0 && skip == 0 && current.size() < maxBytes) {
                current.append(xml.text());
            }
        }
    });
    return strings;
}

std::uint32_t parseNumber(std::string_view s)
{
    std::uint32_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// "B12" → 12.
std::uint32_t rowOfCellReference(std::string_view ref)
{
    std::size_t i = 0;
    while (i < ref.size() && !(ref[i] >= '0' && ref[i] <= '9'))
        ++i;
    return parseNumber(ref.substr(i));
}

void readSheet(XmlReader& xml, const std::vector<std::string>& strings, std::uint16_t sheet, Writer& out)
{
    std::uint32_t row = 0;
    bool rowStarted = false; // has a cell with text
    std::string type; // of the cell: s (shared string), inlineStr, str, b, e, n
    std::string value;
    bool inValue = false;
    bool inInline = false;
    int inText = 0;
    int skip = 0;
    for (Token t = xml.next(); t != Token::Done && !out.full(); t = xml.next()) {
        if (t == Token::Start) {
            const std::string_view name = xml.name();
            if (skip > 0 || name == "rPh" || name == "f") {
                ++skip; // phonetic runs; formulas (the value is kept in <v>)
            } else if (name == "row") {
                const std::string r = xml.attribute("r");
                row = r.empty() ? row + 1 : parseNumber(r);
                rowStarted = false;
            } else if (name == "c") {
                type = xml.attribute("t");
                const std::string r = xml.attribute("r");
                if (!r.empty())
                    row = std::max(row, rowOfCellReference(r));
                value.clear();
            } else if (name == "v") {
                inValue = true;
            } else if (name == "is") {
                inInline = true;
            } else if (name == "t") {
                ++inText;
            }
        } else if (t == Token::End) {
            const std::string_view name = xml.name();
            if (skip > 0) {
                --skip;
            } else if (name == "v") {
                inValue = false;
            } else if (name == "is") {
                inInline = false;
            } else if (name == "t") {
                inText = std::max(0, inText - 1);
            } else if (name == "c") {
                std::string_view text;
                if (type == "s") {
                    const std::uint32_t i = parseNumber(value);
                    if (i < strings.size())
                        text = strings[i];
                } else if (type != "b" && type != "e") {
                    text = value; // inline string, formula string, number, ISO date
                }
                while (!text.empty() && (text.front() == ' ' || text.front() == '\n'))
                    text.remove_prefix(1);
                if (!text.empty()) {
                    if (!rowStarted) {
                        out.place(PlaceKind::Row, row, sheet);
                        rowStarted = true;
                    } else {
                        out.tab();
                    }
                    putCell(out, text);
                }
            } else if (name == "row") {
                out.newline();
            }
        } else if (skip == 0 && (inValue || (inInline && inText > 0))) {
            value.append(xml.text());
        }
    }
    out.newline();
}

Status readWorkbook(Zip& zip, const std::string& main, Writer& out)
{
    const std::vector<Relationship> rels = readRelationships(zip, main);
    struct Sheet {
        std::string name;
        std::string part;
    };
    std::vector<Sheet> sheets;
    if (!readPart(zip, main, [&](XmlReader& xml) {
            for (Token t = xml.next(); t != Token::Done; t = xml.next()) {
                if (t == Token::Start && xml.name() == "sheet") {
                    const Relationship* r = byId(rels, xml.prefixedAttribute("id"));
                    if (r && isKind(r->type, "worksheet")) // not chart or dialog sheets
                        sheets.push_back({xml.attribute("name"), r->target});
                }
            }
        }))
        return Status::Broken;
    std::vector<std::string> strings;
    for (const Relationship& r : rels) {
        if (isKind(r.type, "sharedStrings")) {
            strings = readSharedStrings(zip, r.target, kMaxSharedStringBytes);
            break;
        }
    }
    for (const Sheet& s : sheets) {
        if (out.full())
            break;
        const std::uint16_t sheet = out.addSheet(s.name);
        readPart(zip, s.part, [&](XmlReader& xml) { readSheet(xml, strings, sheet, out); });
    }
    return Status::Ok;
}

} // namespace

Status extractOoxml(Zip& zip, Writer& out, doctext::DocText& doc)
{
    std::string main;
    for (const Relationship& r : readRelationships(zip, "")) {
        if (isKind(r.type, "officeDocument")) {
            main = r.target;
            break;
        }
    }
    if (main.empty())
        return Status::Unsupported;
    if (main.ends_with(".bin"))
        return Status::Unsupported; // xlsb: binary sheets
    const auto index = zip.find(main);
    if (!index)
        return Status::Broken;
    const std::string root = rootElementOf(zip, *index);
    if (root == "document")
        return readWord(zip, main, out, doc);
    if (root == "workbook")
        return readWorkbook(zip, main, out);
    if (root == "presentation")
        return readPresentation(zip, main, out);
    return Status::Unsupported;
}

} // namespace ws::extract
