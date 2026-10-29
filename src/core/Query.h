#pragma once

#include "FileIndex.h"
#include "Pinyin.h"
#include "SearchTypes.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QRegularExpression>

#include <string>
#include <string_view>
#include <vector>

namespace ws {

// A leading :keyword, outside quotes. Unknown prefixes remain ordinary words.
struct FileTypeQuery {
    QString words;
    std::optional<int> type; // -2: folders; otherwise an index in FileTypes
    QList<int> types; // multiple file categories, joined by a comma
};
std::optional<QString> normalizedTypeKeyword(QStringView text);
FileTypeQuery splitFileTypeQuery(const QString& text, const FileTypes& types, QStringView folderKeyword);

// One whitespace-separated piece of the query.
//
//   report          name contains "report"
//   bg  baogao      Chinese names by pinyin too (报告): see Pinyin.h
//   "annual report" quoted: spaces are part of the term
//   !draft          name must NOT contain "draft"
//   !node_modules\  nothing inside a folder whose name contains "node_modules"
//   *.pdf  a?c      wildcard match against the whole name
//   proj\readme     "readme" in the name, under a folder containing "proj"
//   C:\My Files\a   a path from a drive (or share) is one term, spaces and all
//   ext:pdf,docx    only files with these extensions
//   size:>10mb      only files of such a size: >, >=, <, <=, 1mb..5mb (or 1mb-5mb),
//                   units b k/kb m/mb g/gb t/tb (1024 each), or empty, tiny (< 10 KB),
//                   small (< 100 KB), medium (< 1 MB), large (< 16 MB), huge (< 128 MB),
//                   gigantic (the rest), as Everything names them
//   dm:7d           written in the last 7 days (w: weeks, m: months, y: years);
//                   today, yesterday, thisweek, thismonth, thisyear; 2026, 2026-01,
//                   2026-01-15, >2026-01-15, <2026-02, 2026-01-15..2026-02-01
struct QueryTerm {
    std::string text; // folded UTF-8; the last segment for path terms
    std::vector<std::string> ancestors; // folded folder segments, outermost first
    bool negated = false;
    bool wildcard = false;
    std::string literal; // wildcard terms: longest part without * or ?, a cheap pre-filter
    bool regex = false;
    QRegularExpression expression; // compiled once; names only, no pinyin expansion
};

struct ParsedQuery {
    std::vector<ParsedQuery> alternatives; // OR of AND groups; filters belong to their group
    QString error;
    std::vector<QueryTerm> terms;
    std::vector<std::string> extensions; // folded, without the dot
    // Set by the search, not typed: files with these extensions never match
    // (Settings' skipped file types), but in NameMatcher::match(index, ...)
    // only; a favorite or a path asked for by name stays.
    std::vector<std::string> excludedExtensions; // folded, without the dot
    // The same for Settings' skipped file names (GitHub #132): folded
    // wildcards ("~$*"), each against a file's whole name.
    std::vector<std::string> excludedFileNames;
    EntryFilter filter; // size: and dm: (never folders)
    QStringList highlights; // plain terms to highlight in result names

    // Nothing to find by: a filter alone (size:>1gb) lists what fits it.
    bool isEmpty() const noexcept { return error.isEmpty() && alternatives.empty() && terms.empty() && extensions.empty() && !filter.any(); }
    bool hasFilters() const noexcept;
};

// What a size: or dm: token stands for (parseQuery takes them from the
// query); false for anything else, or one it cannot read (left in the query
// as a word). `now`: local time, for today, 7d and the like.
bool parseSizeToken(QStringView token, EntryFilter& filter);
bool parseDateToken(QStringView token, const QDateTime& now, EntryFilter& filter);
// A size in bytes written briefly: "1.2 MB", "356 KB", "0 B".
QString sizeText(std::uint64_t bytes);

// Whether a name (UTF-8, its extension and all, any case) is one of the
// patterns: folded, with * and ? (Settings' skipped file names).
bool nameMatchesAny(std::string_view name, const std::vector<std::string>& foldedPatterns) noexcept;

// What was typed, split at whitespace; "within quotes" is one word, spaces
// and all (the quotes go).
QStringList queryWords(const QString& text);

// Characters that name searches have no use for, but lines of code are full of.
bool looksLikeCode(QStringView text);

// What a content search looks for, every one somewhere in the file: the
// words ("报告 2025"), or a line of code as it is, quotes, spaces and all
// (printf("%d", *p)).
QStringList contentTerms(const QString& text);

// A folder a content search is to look in, named among its words.
//
//   年度报告 abc/        in every folder named abc (a full-width ／ or a \ will do)
//   年度报告 工作/abc/   ... named abc in a folder named 工作
//   年度报告 /abc        the same as abc/, but only as the last word, after others
//   D:\My Files\ 报告    in that folder (spaces and all, up to "\ "), or "%USERPROFILE%\Documents\"
//   "D:\My Files\" 报告  the same, quoted
struct ContentFolderSpec {
    QString typed; // as it was typed: "abc/", "/abc", "D:\My Files\"
    QString path; // from a drive or share, normalized ("D:\My Files", "D:\"); else empty, and:
    QStringList names; // the folder's name and those it is in, outermost first ("工作", "abc")
    friend bool operator==(const ContentFolderSpec&, const ContentFolderSpec&) = default;
};

struct ContentQuery {
    QString words; // the rest: what to find in the files (contentTerms)
    std::vector<ContentFolderSpec> folders;
};

// What a content search was typed as: its words, and the folders named
// among them (ContentFolderSpec). A line of code is all words: "a/" or
// "/usr/bin" in it are what it says.
ContentQuery splitContentQuery(const QString& text);

ParsedQuery parseQuery(const QString& text);

// The query read aloud: each term with Chinese characters as pinyin
// ("年度报高" → "niandubaogao"), for finding a name that sounds the same
// (年度报告) when nothing is spelled that way (the name search tries these
// only then). A character with several readings (行: xing, hang) gives a
// query per combination, the common readings first, up to a few; the rest
// take the most common one. Exclusions, wildcards, ext: and the terms with
// no Chinese stay as they are; the highlights are the pinyin, which the
// result list reads back into the names. Empty when no term has Chinese
// characters it can read.
std::vector<ParsedQuery> homophoneQueries(const ParsedQuery& query);

// Decides whether a name matches and how well. Higher scores rank first;
// -1 means no match. Thread-safe (immutable after construction).
//
// Several terms ("合同 2025", "src app"): each is in the name or, ranked
// lower, in the name of a folder the item is in; at least one is in the name.
class NameMatcher {
public:
    explicit NameMatcher(const ParsedQuery& query);

    // Which terms the folders of an index hold, kept while one search looks
    // at many entries (siblings share them): one per thread and query.
    class Folders {
    private:
        friend class NameMatcher;
        struct Slot {
            EntryId folder = kNoEntry;
            std::uint64_t terms = 0; // in it or any folder it is in
        };
        static constexpr std::size_t kSlots = 1024;
        std::vector<Slot> m_slots; // made when first needed
        std::vector<Folders> m_alternatives;
    };

    // Whether an entry with this name (stored in the index, so padded) may
    // match, whatever its folders and kind: false rules out every entry that
    // has it, so a search can look at each distinct name once and then only
    // at the entries whose name may match. `fileExtLength` as for a file;
    // `han`: whether it has Chinese characters.
    bool nameMayMatch(std::string_view name, std::size_t fileExtLength, bool han) const;
    // Whether nameMayMatch() ever says no (a query of folders alone does not).
    bool filtersNames() const noexcept;

    // Caller holds index.readLock().
    int match(const FileIndex& index, const Entry& entry, Folders& folders, EntryId id = kNoEntry) const;
    int match(const FileIndex& index, const Entry& entry) const;
    // Matches a full path that is not (necessarily) in the index, e.g. history.
    int matchPath(const QString& fullPath, bool isDir, FileTime modified = 0, FileSize size = kUnknownSize) const;
    // Matches a bare UTF-8 name with no folders or extension, such as an
    // app's display name: folder terms and ext: never match it.
    int matchName(std::string_view name) const;
    // Whether the query is one plain term that is the whole name (UTF-8),
    // with or without its extension: "readme" for "README.md".
    bool isExactName(std::string_view name, bool isDir) const;

private:
    bool m_invalid = false;
    EntryFilter m_filter;
    std::vector<NameMatcher> m_alternatives;
    struct Positive {
        QueryTerm term;
        pinyin::Matcher pinyin; // valid() when the term can be read as pinyin
        pinyin::Matcher commonPinyin; // the same with each character's most common reading only
        std::vector<pinyin::Matcher> folders; // the same for each ancestor segment
    };

    // `han()`: whether the name has Chinese characters (asked only when it
    // matters, as working it out reads the name).
    template <bool Padded, typename Han, typename AncestorsMatch, typename FolderTerms>
    int score(std::string_view name, bool isDir, Han&& han, std::size_t extLength, AncestorsMatch&& ancestorsMatch,
        FolderTerms&& folderTerms) const;
    // Which of m_folderTerms a folder's own name has.
    std::uint64_t termsInFolder(std::string_view name, bool han, bool padded) const;
    // ... it or any folder it is in.
    std::uint64_t termsInFolders(const FileIndex& index, EntryId folder, Folders* folders) const;

    std::vector<Positive> m_positive;
    // The longest term every matching name has (as it is, or as pinyin): a
    // quick test before the rest. -1: none such. `m_quickDecides`: nothing
    // else about a name can rule it out.
    int m_quick = -1;
    bool m_quickDecides = false;
    std::uint64_t m_folderTerms = 0; // m_positive's that may be in a folder's name instead, one bit each
    std::vector<QueryTerm> m_negative;
    std::vector<std::string> m_extensions;
    std::vector<std::string> m_excluded; // ParsedQuery::excludedExtensions
    std::size_t m_excludedLongest = 0; // of those: a longer extension is none of them
    std::vector<std::string> m_excludedNames; // ParsedQuery::excludedFileNames
};

} // namespace ws
