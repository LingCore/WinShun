#include "ContentScanner.h"

#include "TextUtil.h"
#include "Win32Util.h"

#include <QStringDecoder>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

namespace qf {

// ---- ByteFinder ---------------------------------------------------------

ByteFinder::ByteFinder(std::string needle)
    : m_needle(text::foldAscii(needle))
{
    const std::size_t m = m_needle.size();
    m_skip.fill(m == 0 ? 1 : m);
    for (std::size_t j = 0; j + 1 < m; ++j)
        m_skip[static_cast<unsigned char>(m_needle[j])] = m - 1 - j;
}

std::size_t ByteFinder::find(const char* data, std::size_t length, std::size_t from) const noexcept
{
    const std::size_t m = m_needle.size();
    if (m == 0 || length < m || from > length - m)
        return text::npos;
    const auto last = static_cast<unsigned char>(m_needle[m - 1]);
    std::size_t i = from;
    while (i <= length - m) {
        const unsigned char c = text::fold(data[i + m - 1]);
        if (c == last) {
            std::size_t j = m - 1;
            while (j > 0 && text::fold(data[i + j - 1]) == static_cast<unsigned char>(m_needle[j - 1]))
                --j;
            if (j == 0)
                return i;
        }
        i += m_skip[c];
    }
    return text::npos;
}

// ---- helpers --------------------------------------------------------------

namespace {

constexpr std::size_t kChunkBytes = 512 * 1024;
constexpr std::size_t kContextBytes = 600; // decoded around a candidate match

unsigned legacyCodePage(unsigned requested)
{
    if (requested != 0)
        return requested;
    const UINT acp = ::GetACP();
    if (acp != CP_UTF8)
        return acp;
    // "Use Unicode UTF-8 for worldwide language support" is on, yet legacy
    // .txt files are still in the regional code page. Guess it from the locale.
    const LANGID lang = ::GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(lang)) {
    case LANG_CHINESE:
        return SUBLANGID(lang) == SUBLANG_CHINESE_TRADITIONAL || SUBLANGID(lang) == SUBLANG_CHINESE_HONGKONG ? 950
                                                                                                             : 936;
    case LANG_JAPANESE:
        return 932;
    case LANG_KOREAN:
        return 949;
    default:
        return 1252;
    }
}

std::size_t unitOf(TextEncoding e) noexcept
{
    return e == TextEncoding::Utf16LE || e == TextEncoding::Utf16BE ? 2 : 1;
}

// `i + unit <= length` must hold.
bool isNewline(const char* d, std::size_t i, TextEncoding e) noexcept
{
    switch (e) {
    case TextEncoding::Utf16LE:
        return d[i] == '\n' && d[i + 1] == 0;
    case TextEncoding::Utf16BE:
        return d[i] == 0 && d[i + 1] == '\n';
    default:
        return d[i] == '\n';
    }
}

std::uint64_t countNewlines(const char* d, std::size_t n, TextEncoding e) noexcept
{
    if (unitOf(e) == 1)
        return static_cast<std::uint64_t>(std::count(d, d + n, '\n'));
    std::uint64_t lines = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2)
        lines += isNewline(d, i, e) ? 1 : 0;
    return lines;
}

// Reads until `capacity` bytes or end of file, so only the last chunk is short.
std::size_t fill(const ContentScanner::ReadFn& read, char* buffer, std::size_t capacity)
{
    std::size_t total = 0;
    while (total < capacity) {
        const std::size_t got = read(buffer + total, capacity - total);
        if (got == 0)
            break;
        total += got;
    }
    return total;
}

QString cleanLine(QString s)
{
    s.replace(u'\t', u' ');
    s.remove(u'\r');
    s.remove(u'\n');
    s.remove(QChar(0xFEFF));
    return s;
}

} // namespace

// ---- ContentScanner -------------------------------------------------------

ContentScanner::ContentScanner(const QString& needle, unsigned ansiCodePage)
    : m_needle(needle)
    , m_codePage(legacyCodePage(ansiCodePage))
{
    if (m_needle.isEmpty())
        return;

    m_utf8 = ByteFinder(m_needle.toUtf8().toStdString());

    std::string le(static_cast<std::size_t>(m_needle.size()) * 2, '\0');
    std::string be(le.size(), '\0');
    for (qsizetype i = 0; i < m_needle.size(); ++i) {
        const char16_t u = m_needle[i].unicode();
        le[2 * i] = static_cast<char>(u & 0xFF);
        le[2 * i + 1] = static_cast<char>(u >> 8);
        be[2 * i] = static_cast<char>(u >> 8);
        be[2 * i + 1] = static_cast<char>(u & 0xFF);
    }
    m_utf16le = ByteFinder(std::move(le));
    m_utf16be = ByteFinder(std::move(be));

    const auto* wide = reinterpret_cast<const wchar_t*>(m_needle.utf16());
    const int wideLen = static_cast<int>(m_needle.size());
    BOOL usedDefault = FALSE;
    const int n
        = ::WideCharToMultiByte(m_codePage, WC_NO_BEST_FIT_CHARS, wide, wideLen, nullptr, 0, nullptr, &usedDefault);
    if (n > 0 && !usedDefault) {
        std::string ansi(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(m_codePage, WC_NO_BEST_FIT_CHARS, wide, wideLen, ansi.data(), n, nullptr, &usedDefault);
        if (!usedDefault) {
            m_ansi = ByteFinder(std::move(ansi));
            m_ansiRepresentable = true;
        }
    }
}

bool ContentScanner::isUtf8(std::string_view b, bool allowTruncatedTail)
{
    const std::size_t n = b.size();
    std::size_t i = 0;
    while (i < n) {
        const auto c = static_cast<unsigned char>(b[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF)
            len = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            len = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            len = 4;
        else
            return false;
        const std::size_t avail = std::min(len, n - i);
        for (std::size_t k = 1; k < avail; ++k) {
            if ((static_cast<unsigned char>(b[i + k]) & 0xC0) != 0x80)
                return false;
        }
        if (avail < len)
            return allowTruncatedTail;
        i += len;
    }
    return true;
}

TextEncoding ContentScanner::detect(std::string_view head, std::size_t* bomLength)
{
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(head[i]); };
    std::size_t bom = 0;
    TextEncoding encoding = TextEncoding::Utf8;
    if (head.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) {
        bom = 3;
    } else if (head.size() >= 2 && at(0) == 0xFF && at(1) == 0xFE) {
        bom = 2;
        encoding = TextEncoding::Utf16LE;
    } else if (head.size() >= 2 && at(0) == 0xFE && at(1) == 0xFF) {
        bom = 2;
        encoding = TextEncoding::Utf16BE;
    } else if (!isUtf8(head, true)) {
        encoding = TextEncoding::Ansi;
    }
    if (bomLength)
        *bomLength = bom;
    return encoding;
}

QString ContentScanner::decode(const char* data, std::size_t length, TextEncoding encoding) const
{
    const auto len = static_cast<qsizetype>(length);
    switch (encoding) {
    case TextEncoding::Utf8:
        return QString::fromUtf8(data, len);
    case TextEncoding::Utf16LE: {
        QStringDecoder decoder(QStringConverter::Utf16LE);
        return decoder(QByteArrayView(data, len));
    }
    case TextEncoding::Utf16BE: {
        QStringDecoder decoder(QStringConverter::Utf16BE);
        return decoder(QByteArrayView(data, len));
    }
    case TextEncoding::Ansi: {
        const int n = ::MultiByteToWideChar(m_codePage, 0, data, static_cast<int>(length), nullptr, 0);
        if (n <= 0)
            return {};
        QString out(n, Qt::Uninitialized);
        ::MultiByteToWideChar(m_codePage, 0, data, static_cast<int>(length), reinterpret_cast<wchar_t*>(out.data()), n);
        return out;
    }
    }
    return {};
}

std::optional<ContentMatch> ContentScanner::scan(
    const ReadFn& read, std::size_t chunkSize, const CancelFn& cancelled) const
{
    if (!isValid())
        return std::nullopt;
    chunkSize = std::max<std::size_t>(chunkSize, 16) & ~std::size_t {1}; // even: keeps UTF-16 aligned
    const std::size_t longestNeedle = std::max({m_utf8.size(), m_utf16le.size(), m_ansi.size()});

    thread_local std::vector<char> buffer;
    buffer.resize(chunkSize + longestNeedle + 2);
    char* data = buffer.data();

    std::size_t windowLength = fill(read, data, chunkSize);
    if (windowLength == 0)
        return std::nullopt;

    std::size_t bom = 0;
    const TextEncoding encoding = detect({data, windowLength}, &bom);
    const ByteFinder* finder = nullptr;
    switch (encoding) {
    case TextEncoding::Utf8:
        finder = &m_utf8;
        break;
    case TextEncoding::Utf16LE:
        finder = &m_utf16le;
        break;
    case TextEncoding::Utf16BE:
        finder = &m_utf16be;
        break;
    case TextEncoding::Ansi:
        if (!m_ansiRepresentable)
            return std::nullopt; // the phrase cannot occur in this code page
        finder = &m_ansi;
        break;
    }
    const std::size_t unit = unitOf(encoding);
    const std::size_t needleBytes = finder->size();

    std::uint64_t linesBeforeWindow = 0;
    std::size_t windowFloor = bom; // first byte that belongs to the text
    for (;;) {
        if (cancelled && cancelled())
            return std::nullopt;

        for (std::size_t pos = finder->find(data, windowLength, windowFloor); pos != text::npos;
            pos = finder->find(data, windowLength, pos + 1)) {
            if (unit == 2 && (pos & 1))
                continue; // straddles two UTF-16 code units

            // Line bounds around the candidate, capped so huge lines stay cheap.
            std::size_t from = pos;
            bool atLineStart = false;
            while (from > windowFloor && pos - from < kContextBytes) {
                if (isNewline(data, from - unit, encoding)) {
                    atLineStart = true;
                    break;
                }
                from -= unit;
            }
            if (!atLineStart && encoding == TextEncoding::Utf8) {
                while (from < pos && (static_cast<unsigned char>(data[from]) & 0xC0) == 0x80)
                    ++from;
            }
            std::size_t to = pos + needleBytes;
            while (
                to + unit <= windowLength && to - pos < kContextBytes + needleBytes && !isNewline(data, to, encoding))
                to += unit;

            const QString line = cleanLine(decode(data + from, to - from, encoding));
            const qsizetype idx = line.indexOf(m_needle, 0, Qt::CaseInsensitive);
            if (idx < 0)
                continue; // byte-level false positive (e.g. a GBK trail byte)

            ContentMatch match;
            match.line = static_cast<int>(linesBeforeWindow + countNewlines(data, pos, encoding) + 1);

            qsizetype lead = 0;
            while (lead < idx && line[lead].isSpace())
                ++lead;
            constexpr qsizetype kBefore = 36;
            constexpr qsizetype kMaxChars = 160;
            qsizetype start = std::max(lead, idx - kBefore);
            if (start > 0 && start < idx && line[start].isLowSurrogate())
                ++start;
            qsizetype end = std::min(line.size(), std::max(start + kMaxChars, idx + m_needle.size()));
            while (end > idx + m_needle.size() && line[end - 1].isSpace())
                --end;
            const QString prefix = start > lead ? u"…"_s : QString();
            const QString suffix = end < line.size() ? u"…"_s : QString();
            match.snippet = prefix + line.mid(start, end - start) + suffix;
            match.matchStart = static_cast<int>(prefix.size() + idx - start);
            match.matchLength = static_cast<int>(m_needle.size());
            return match;
        }

        // Slide the window, keeping a tail so matches across chunks are found.
        std::size_t keep = needleBytes > unit ? needleBytes - unit : 0;
        keep = std::min(keep, windowLength - windowFloor);
        if (unit == 2)
            keep &= ~std::size_t {1};
        linesBeforeWindow += countNewlines(data, windowLength - keep, encoding);
        std::memmove(data, data + windowLength - keep, keep);
        const std::size_t got = fill(read, data + keep, chunkSize);
        if (got == 0)
            return std::nullopt;
        windowLength = keep + got;
        windowFloor = 0;
    }
}

std::optional<ContentMatch> ContentScanner::scanFile(
    const std::wstring& path, std::int64_t maxBytes, const CancelFn& cancelled) const
{
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid())
        return std::nullopt;

    FILE_BASIC_INFO basic {};
    constexpr DWORD kSkip = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        && (basic.FileAttributes & kSkip))
        return std::nullopt; // never trigger a cloud download

    LARGE_INTEGER size {};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || (maxBytes > 0 && size.QuadPart > maxBytes))
        return std::nullopt;

    const HANDLE h = file.get();
    const ReadFn read = [h](char* buffer, std::size_t capacity) -> std::size_t {
        DWORD got = 0;
        const auto request = static_cast<DWORD>(std::min<std::size_t>(capacity, 1u << 30));
        if (!::ReadFile(h, buffer, request, &got, nullptr))
            return 0;
        return got;
    };
    return scan(read, kChunkBytes, cancelled);
}

} // namespace qf
