#pragma once

// Colours for the text of a file in the preview (GitHub #291): which parts
// of a line are a comment, a string, a keyword…, told by a small scanner per
// family of languages, a line at a time. Not a parser: it is for reading a
// file at a glance, so it errs on the side of leaving text plain. Markdown is
// "rendered" the same way: its headings, emphasis, code and links are spans
// of their own, the marks around them dimmed.
//
// Lines come in order; what a line leaves open (a block comment, a string
// over several lines, a fenced block) is carried in State to the next. A
// long line shown in pieces is scanned piece by piece the same way.

#include <QString>
#include <QStringList>
#include <QStringView>

#include <vector>

namespace ws::syntax {

enum class Kind : quint8 {
    Comment,
    String,
    Number,
    Keyword,
    Type, // built-in types and constants (int, true, null)
    Key, // a JSON or YAML key, an INI name, a CSS property, an XML attribute
    Tag, // an XML or HTML element's name, an INI section
    Heading1, // Markdown from here on
    Heading2,
    Heading3, // and deeper
    Strong,
    Emphasis,
    Code,
    Link,
    Quote,
    Mark, // the marks themselves ("##", "**", "```"): dimmed
};
inline constexpr int kKinds = static_cast<int>(Kind::Mark) + 1;

struct Span {
    int start = 0; // UTF-16 units in the line
    int length = 0;
    Kind kind = Kind::Comment;

    bool operator==(const Span&) const = default;
};

enum class Language : quint8 {
    None,
    CFamily, // C, C++, C#, Java, JavaScript, TypeScript, Go, Rust, Kotlin, Swift, Dart, PHP, QML
    Python,
    Shell, // sh, PowerShell, Ruby, Perl, R, Makefile, Dockerfile: "#" comments
    Batch,
    Lua,
    Sql,
    Css,
    Json,
    Yaml,
    Ini, // ini, toml, properties, conf, reg
    Xml, // xml, html, svg, xaml, vue
    Markdown,
};

// By the file's name ("main.cpp", "Dockerfile"); None for one not known.
Language languageFor(QStringView fileName);

// What a line left open for the next.
struct State {
    enum Mode : quint8 { Plain, BlockComment, LongString, Fence, InTag };
    Mode mode = Plain;
    quint8 detail = 0; // BlockComment / LongString: which pair of the language's it is

    bool operator==(const State&) const = default;
};

// The spans of `line` (no line break in it), in order, never overlapping.
std::vector<Span> highlight(Language language, QStringView line, State& state);

// Where the words searched for are in `line`, whatever their case: start and
// length, in order, merged where they touch. A word of one letter or digit
// is not looked for (it would be everywhere).
struct Found {
    int start = 0;
    int length = 0;

    bool operator==(const Found&) const = default;
};
std::vector<Found> findWords(QStringView line, const QStringList& words);

} // namespace ws::syntax
