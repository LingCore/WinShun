#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ws {

// What was typed, split at whitespace; "within quotes" is one word, spaces
// and all (the quotes go).
QStringList queryWords(const QString& text);

// Characters that name searches have no use for, but lines of code are full of.
bool looksLikeCode(QStringView text);

// What a content search looks for, every one somewhere in the file: the
// words ("报告 2025"), or a line of code as it is, quotes, spaces and all
// (printf("%d", *p)). As plain words only: contentPattern() reads the rest.
QStringList contentTerms(const QString& text);

// The text any match of a regular expression is sure to have in it, read off
// the expression: "abc\d+(def|xyz)" cannot match without "abc", and one of
// "def" and "xyz". A search looks this up in the content index and reads only
// the files that have it, as Zoekt and Google's codesearch do (GitHub #295);
// the expression itself is then run on those. Any: nothing can be said
// ("\d{11}", or an expression this does not make out), and every file is read.
//
// Text is in lower case, and holds for a match in any case. Letters other
// than ASCII that have a case are left out (the index does not fold them).
struct RegexNeed {
    enum class Kind : std::uint8_t {
        Any,
        Literal, // `text`, as it is
        All, // every one of `parts`
        OneOf, // at least one of `parts`
    };
    Kind kind = Kind::Any;
    QString text;
    std::vector<RegexNeed> parts;

    static RegexNeed of(QStringView pattern);
    bool isAny() const noexcept { return kind == Kind::Any; }
    // Whether a text has it (any case): true of every text the expression matches.
    bool in(QStringView text) const;
    // For the log and the tests: "abc", all(...), oneOf(...), any.
    QString toString() const;

    friend bool operator==(const RegexNeed&, const RegexNeed&) = default;
};

// One thing a content search looks for in a file: words as they are (any
// case), or a regular expression.
struct ContentNeedle {
    QString text; // the words, or the expression as typed
    bool regex = false;
    QRegularExpression expression; // regex: any case unless it says (?-i); ^ and $ at each line's ends
    RegexNeed need; // regex

    friend bool operator==(const ContentNeedle&, const ContentNeedle&) = default;
};

// What a content search looks for, as typed:
//
//   报告 2025          both, anywhere in the file
//   "年度 报告"        the phrase, spaces and all
//   马上|立即          either (also "马上 | 立即", as in a name search); quoted,
//                      "a|b" is the three characters
//   会议 马上|立即     会议, and either of the two
//   报告 | 总结 2026   报告, or both of the others
//   printf("%d", *p)   a line of code as it is, whatever is in it
//   regex:1[3-9]\d{9}  a regular expression (re: for short), matched within
//                      a line; regex:"a b" for one with spaces
//
// In regex mode (the search box's, Ctrl+R) every word is an expression.
//
// `ways`: each the needles (one bit each) a file must have all of; a file
// that has those of one way matches.
struct ContentPattern {
    static constexpr std::size_t kMaxNeedles = 32;
    static constexpr std::size_t kMaxWays = 16;

    std::vector<ContentNeedle> needles; // distinct
    std::vector<std::uint32_t> ways;
    QString error; // an expression that cannot be read: why (nothing is searched), as the footer says it
    QString errorReason; // the same in the regex engine's own words, for callers that are no people (MCP)

    bool isEmpty() const noexcept { return ways.empty(); }
    bool hasRegex() const noexcept;
    // The words among the needles (not the expressions): what to look for in a file's name.
    QStringList words() const;

    // Every one of these words or phrases, as they are.
    static ContentPattern allOf(const QStringList& terms);
};

// `words`: what was typed, without the folders named among it
// (splitContentQuery, which takes the same `regexMode`).
ContentPattern contentPattern(const QString& words, bool regexMode = false);

} // namespace ws
