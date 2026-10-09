#pragma once

#include "DocText.h"

#include <cstdint>
#include <string_view>

namespace ws::extract {

// Builds a DocText: the text as UTF-8 lines, and the places they are at.
// Lines are never empty (a run of line ends makes one), runs of spaces and
// tabs make one, and spaces at the ends of lines are dropped. Control
// characters become spaces; characters that show nothing (zero-width space,
// byte order mark) are dropped. Stops adding text at `maxBytes`.
class Writer {
public:
    Writer(doctext::DocText& out, std::size_t maxBytes);

    // The lines from the next one with text on are at this place. A row
    // that follows on from the place before (the next row of the same
    // sheet, one line later) adds nothing: rows count up by themselves.
    void place(doctext::PlaceKind kind, std::uint32_t number, std::uint16_t sheet = doctext::Place::kNoSheet);
    std::uint16_t addSheet(std::string_view utf8Name);

    void put(char32_t c);
    void put(std::u16string_view utf16);
    void putUtf8(std::string_view utf8); // malformed bytes become U+FFFD
    void newline();
    void tab();
    void space();

    bool full() const noexcept { return m_out.truncated; }
    bool lineEmpty() const noexcept { return m_lineStart == m_out.text.size(); }
    char32_t last() const noexcept { return m_last; } // the last character on the line, 0 if none
    std::uint32_t lineNumber() const noexcept { return m_line; }
    std::size_t bytes() const noexcept { return m_out.text.size(); }
    // Ends the last line, drops places after the last line of text.
    void finish();

private:
    void append(char32_t c);
    void endLine();

    doctext::DocText& m_out;
    std::size_t m_max;
    std::size_t m_lineStart = 0;
    std::uint32_t m_line = 1; // of the line being written
    char32_t m_last = 0;
    bool m_pendingPlace = false;
    doctext::Place m_place;
    char16_t m_high = 0; // a high surrogate waiting for its pair
};

bool isCjk(char32_t c) noexcept; // ideographs, kana, hangul and CJK punctuation

} // namespace ws::extract
