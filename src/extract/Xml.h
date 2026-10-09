#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws::extract {

// A pull reader for the XML inside documents, fed in chunks: elements by
// local name (the prefix is dropped: generators pick their own), their
// attributes, and character data. It never reads a DTD, so entities other
// than the five predefined ones and character references stay as written:
// nothing expands. UTF-8 only (what Office and OpenDocument write).
class XmlReader {
public:
    using Fill = std::function<std::size_t(char* buffer, std::size_t capacity)>; // 0: the end

    enum class Token { Start, End, Text, Done };

    explicit XmlReader(Fill fill);

    Token next();
    // Start and End: the element's local name. "<a/>" gives Start, then End.
    std::string_view name() const noexcept { return m_name; }
    // Start: an attribute's value by local name, entities decoded; empty if
    // none. One without a prefix comes first: <sldId id="256" r:id="rId2"/>.
    std::string attribute(std::string_view localName) const;
    // ... only one with a prefix: r:id.
    std::string prefixedAttribute(std::string_view localName) const;
    // Text: character data, entities decoded. Long runs come in pieces.
    std::string_view text() const noexcept { return m_text; }
    bool failed() const noexcept { return m_failed; }

private:
    bool more(); // reads another chunk; false at the end
    std::size_t available() const noexcept { return m_buf.size() - m_pos; }
    bool skipPast(std::string_view what); // for comments and the like: drops what it passes
    bool skipDeclaration(); // <!DOCTYPE ...>, with an internal subset
    Token readText();
    Token readCdata();
    Token readTag();

    Fill m_fill;
    std::string m_buf;
    std::size_t m_pos = 0;
    bool m_eof = false;
    bool m_failed = false;
    bool m_pendingEnd = false;
    bool m_inCdata = false;
    std::string m_tag; // the current start tag, which the views below point into
    std::string m_name;
    std::vector<std::pair<std::string_view, std::string_view>> m_attributes; // qualified name, raw value
    std::string m_text;
};

// Appends `raw` with the predefined entities and character references decoded.
void decodeXmlText(std::string_view raw, std::string& out);

} // namespace ws::extract
