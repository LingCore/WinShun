// The rules of the winshun.expander plugin (缩写展开, GitHub #403): the
// abbreviations its setting lists and which one a run of typed characters
// ends with. Plain C++ (Windows' case mapping) that tests/tst_expander.cpp
// builds in again; Engine.cpp does the typing.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace expander {

// The longest abbreviation, in UTF-16 units; what is typed is kept to a
// little more (the character before it).
constexpr std::size_t kLongest = 32;

struct Phrase {
    std::wstring abbreviation; // as written
    std::wstring text; // what it becomes; its lines joined by '\n'
    int line = 0; // its first line in the setting, from 1
};

// A line that is not used, and why (shown under the keyword).
struct Problem {
    enum class Kind {
        NoEquals, // neither "abbreviation = text" nor a phrase's next line (none above it)
        NoAbbreviation, // "= text"
        NoText, // "abbreviation =" and nothing under it
        Spaces, // "c md = …": typed as one, an abbreviation has no space
        TooLong, // longer than kLongest
        NotTyped, // Chinese, Japanese or Korean, or full-width: the input method's, never seen
        Repeated, // the same abbreviation again: the first one counts
    };
    Kind kind = Kind::NoEquals;
    int line = 0;
    std::wstring abbreviation;
};

struct Phrases {
    std::vector<Phrase> list;
    std::vector<Problem> problems;
};

// The setting: a phrase a line, "abbreviation = text", split at the first
// "=" (or "＝"), spaces around both sides left out. A line with no "=" goes
// on the phrase above as its next line, as written but for the spaces at its
// end; blank lines between such lines are kept, the ones after the last left
// out. "abbreviation =" alone takes the lines under it. "#" at a line's start
// makes it a note.
Phrases parse(std::wstring_view setting);

// Letters (of any script), digits and "_": what an abbreviation that starts
// with one must not follow ("cmd" in "xcmd" is not one).
bool isWordChar(wchar_t c);

struct Match {
    const Phrase* phrase = nullptr;
    std::size_t typedLength = 0; // the abbreviation's UTF-16 units at the end of what was typed
    std::wstring text; // the phrase's text, in the case it was typed in
};

// The abbreviation `typed` ends with: the longest; at one length, one typed
// as written before one typed in another case. Another case: all in capitals
// (BTW: BY THE WAY) where the abbreviation has two letters at least and a
// small one, or the first letter a capital (Btw: By the way) where it is a
// small one; other mixtures are not it. One that starts with a letter or a
// digit must come after something else (or nothing).
std::optional<Match> find(const std::vector<Phrase>& phrases, std::wstring_view typed);

// Code points in `text` (a surrogate pair is one): the Backspaces it takes.
std::size_t codePoints(std::wstring_view text);

// Program names from "a.exe; b": lower case, file names only, ".exe" added
// where there is no extension, each once.
std::vector<std::wstring> programList(std::wstring_view text);
bool listed(const std::vector<std::wstring>& list, std::wstring_view path);

} // namespace expander
