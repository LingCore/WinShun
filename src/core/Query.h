#pragma once

#include "FileIndex.h"
#include "Pinyin.h"

#include <QString>
#include <QStringList>

#include <string>
#include <string_view>
#include <vector>

namespace qf {

// One whitespace-separated piece of the query.
//
//   report          name contains "report"
//   bg  baogao      Chinese names by pinyin too (报告): see Pinyin.h
//   "annual report" quoted: spaces are part of the term
//   !draft          name must NOT contain "draft"
//   *.pdf  a?c      wildcard match against the whole name
//   proj\readme     "readme" in the name, under a folder containing "proj"
//   ext:pdf,docx    only files with these extensions
struct QueryTerm {
    std::string text; // folded UTF-8; the last segment for path terms
    std::vector<std::string> ancestors; // folded folder segments, outermost first
    bool negated = false;
    bool wildcard = false;
    std::string literal; // wildcard terms: longest part without * or ?, a cheap pre-filter
};

struct ParsedQuery {
    std::vector<QueryTerm> terms;
    std::vector<std::string> extensions; // folded, without the dot
    QStringList highlights; // plain terms to highlight in result names

    bool isEmpty() const noexcept { return terms.empty() && extensions.empty(); }
};

ParsedQuery parseQuery(const QString& text);

// Decides whether a name matches and how well. Higher scores rank first;
// -1 means no match. Thread-safe (immutable after construction).
class NameMatcher {
public:
    explicit NameMatcher(const ParsedQuery& query);

    // Caller holds index.readLock().
    int match(const FileIndex& index, const Entry& entry) const;
    // Matches a full path that is not (necessarily) in the index, e.g. history.
    int matchPath(const QString& fullPath, bool isDir) const;
    // Matches a bare UTF-8 name with no folders or extension, such as an
    // app's display name: folder terms and ext: never match it.
    int matchName(std::string_view name) const;

private:
    struct Positive {
        QueryTerm term;
        pinyin::Matcher pinyin; // valid() when the term can be read as pinyin
        pinyin::Matcher commonPinyin; // the same with each character's most common reading only
        std::vector<pinyin::Matcher> folders; // the same for each ancestor segment
    };

    template <bool Padded, typename AncestorsMatch>
    int score(std::string_view name, bool isDir, bool han, std::size_t extLength, AncestorsMatch&& ancestorsMatch) const;

    std::vector<Positive> m_positive;
    std::vector<QueryTerm> m_negative;
    std::vector<std::string> m_extensions;
};

} // namespace qf
