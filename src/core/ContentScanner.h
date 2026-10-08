#pragma once

#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
    int matchStart = 0; // within snippet
    int matchLength = 0;
};

// Finds the first line of a text file containing a phrase.
//
// Files are streamed in chunks (memory stays flat regardless of file size).
// The encoding is detected per file: BOM (UTF-8 / UTF-16 LE / UTF-16 BE),
// otherwise valid UTF-8, otherwise the ANSI code page (GBK on Chinese
// Windows). Raw bytes are pre-filtered with the encoded needle and every
// candidate is verified on decoded text, so multi-byte encodings never
// produce false positives.
class ContentScanner {
public:
    using ReadFn = std::function<std::size_t(char* buffer, std::size_t capacity)>; // 0 = end of file
    using CancelFn = std::function<bool()>;

    explicit ContentScanner(const QString& needle, unsigned ansiCodePage = 0 /* 0 = system */);

    bool isValid() const noexcept { return !m_needle.isEmpty(); }
    unsigned ansiCodePage() const noexcept { return m_codePage; }

    // Reads at low I/O priority: a scan reads many files, and other programs come first.
    std::optional<ContentMatch> scanFile(std::wstring_view path, std::int64_t maxBytes, const CancelFn& cancelled) const;
    std::optional<ContentMatch> scan(const ReadFn& read, std::size_t chunkSize, const CancelFn& cancelled) const;

    static TextEncoding detect(std::string_view head, std::size_t* bomLength);
    static bool isUtf8(std::string_view bytes, bool allowTruncatedTail);
    // The code page of files that are neither UTF-8 nor UTF-16 (GBK on Chinese Windows).
    static unsigned legacyCodePage();
    // How much of a file detect() looks at, and scan() reads at a time.
    static constexpr std::size_t kChunkBytes = 512 * 1024;

private:
    QString decode(const char* data, std::size_t length, TextEncoding encoding) const;

    QString m_needle;
    unsigned m_codePage = 0;
    ByteFinder m_utf8;
    ByteFinder m_utf16le;
    ByteFinder m_utf16be;
    ByteFinder m_ansi;
    bool m_ansiRepresentable = false;
};

} // namespace ws
