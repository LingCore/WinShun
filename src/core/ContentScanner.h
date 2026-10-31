#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws {

enum class TextEncoding { Utf8, Utf16LE, Utf16BE, Ansi };

// Case-insensitive (ASCII letters) Boyer-Moore-Horspool over raw bytes.
class ByteFinder {
public:
    ByteFinder() = default;
    explicit ByteFinder(std::string needle); // folded internally

    bool empty() const noexcept { return m_needle.empty(); }
    std::size_t size() const noexcept { return m_needle.size(); }
    std::size_t find(const char* data, std::size_t length, std::size_t from) const noexcept;

private:
    std::string m_needle;
    std::array<std::size_t, 256> m_skip {};
};

struct ContentMatch {
    int line = 0; // 1-based
    QString snippet;
    QList<std::pair<qsizetype, qsizetype>> matches; // where the terms are in the snippet: start, length
    // For ranking (see contentRelevance): of the line shown.
    int termsOnLine = 0;
    std::uint32_t wordStarts = 0; // the terms in it at the start of a word ("fix", "fixed"; not "prefix"), one bit each
    bool longLine = false; // longer than is read around a match: packed code or data
};

// How well a file matches, higher first: its terms on one line and at the
// start of words, in its name, in your own folders and lately written count
// for it; a line too long to read (packed code, data) against it.
// `priority`: 0 your own folders, 1 elsewhere, 2 hidden, 3 system and program folders.
int contentRelevance(const ContentMatch& match, const QStringList& terms, QStringView fileName, int priority,
    std::int64_t ageSeconds);

// Finds whether a text file contains every one of some terms ("报告 2025",
// each a word or a phrase), anywhere in it, and the line to show for it: the
// first with the most of them, its snippet pieced together with where the
// others are, so every term shows.
//
// Files are streamed in chunks (memory stays flat regardless of file size).
// The encoding is detected per file: BOM (UTF-8 / UTF-16 LE / UTF-16 BE),
// otherwise valid UTF-8, otherwise the ANSI code page (GBK on Chinese
// Windows). Raw bytes are pre-filtered with the encoded terms and every
// candidate is verified on decoded text, so multi-byte encodings never
// produce false positives.
class ContentScanner {
public:
    using ReadFn = std::function<std::size_t(char* buffer, std::size_t capacity)>; // 0 = end of file
    using CancelFn = std::function<bool()>;

    // At most kMaxTerms; empty ones are left out.
    explicit ContentScanner(const QStringList& terms, unsigned ansiCodePage = 0 /* 0 = system */);
    // One phrase, spaces and all.
    explicit ContentScanner(const QString& phrase, unsigned ansiCodePage = 0)
        : ContentScanner(QStringList {phrase}, ansiCodePage)
    {
    }

    static constexpr std::size_t kMaxTerms = 32;

    bool isValid() const noexcept { return !m_terms.empty(); }
    unsigned ansiCodePage() const noexcept { return m_codePage; }

    // Reads at low I/O priority: a scan reads many files, and other programs
    // come first. `large`, if given, says whether the file was left unread for
    // having more than `maxBytes` (a search counts those).
    std::optional<ContentMatch> scanFile(
        std::wstring_view path, std::int64_t maxBytes, const CancelFn& cancelled, bool* large = nullptr,
        bool* unreadable = nullptr) const;
    std::optional<ContentMatch> scan(const ReadFn& read, std::size_t chunkSize, const CancelFn& cancelled) const;

    static TextEncoding detect(std::string_view head, std::size_t* bomLength);
    static bool isUtf8(std::string_view bytes, bool allowTruncatedTail);
    // The code page of files that are neither UTF-8 nor UTF-16 (GBK on Chinese Windows).
    static unsigned legacyCodePage();
    // How much of a file detect() looks at, and scan() reads at a time.
    static constexpr std::size_t kChunkBytes = 512 * 1024;

private:
    struct Term {
        QString text;
        ByteFinder utf8;
        ByteFinder utf16le;
        ByteFinder utf16be;
        std::optional<ByteFinder> ansi; // none: the code page cannot write it
    };

    QString decode(const char* data, std::size_t length, TextEncoding encoding) const;
    // Which terms a line has, one bit each.
    std::uint32_t termsIn(const QString& line) const;
    // Of those, the ones there at the start of a word.
    std::uint32_t wordStartsIn(const QString& line, std::uint32_t terms) const;
    // `others`: where the terms the line lacks first are.
    ContentMatch matchIn(const QString& line, int lineNumber, const std::vector<QString>& others) const;

    std::vector<Term> m_terms;
    unsigned m_codePage = 0;
};

} // namespace ws
