#include "FileText.h"

#include "ContentIndex.h"
#include "ContentScanner.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "FileIndex.h"
#include "RemoteContent.h"
#include "Unc.h"
#include "Win32Util.h"

#include <QStringDecoder>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>

using namespace Qt::StringLiterals;

namespace ws::filetext {

namespace {

constexpr std::size_t kMaxTextFileBytes = 16u << 20;
constexpr std::size_t kBinaryProbe = 64u << 10; // a NUL in here: not text

// A document's text with a line where each page, slide or sheet begins.
QString withPlaces(const doctext::DocText& doc)
{
    const std::string_view all(doc.text);
    std::string out;
    out.reserve(all.size() + doc.places.size() * 12);
    std::size_t place = 0;
    std::uint16_t sheet = doctext::Place::kNoSheet;
    std::uint32_t line = 1;
    for (std::size_t at = 0; at < all.size(); ++line) {
        for (; place < doc.places.size() && doc.places[place].line <= line; ++place) {
            const doctext::Place& p = doc.places[place];
            if (p.line != line)
                continue;
            switch (p.kind) {
            case doctext::PlaceKind::Page:
                out += "[page " + std::to_string(p.number) + "]\n";
                break;
            case doctext::PlaceKind::Slide:
                out += "[slide " + std::to_string(p.number) + "]\n";
                break;
            case doctext::PlaceKind::Row:
                if (p.sheet != sheet && p.sheet < doc.sheets.size()) {
                    sheet = p.sheet;
                    out += "[sheet " + doc.sheets[sheet] + "]\n";
                }
                break;
            case doctext::PlaceKind::None:
                break;
            }
        }
        const std::size_t end = all.find('\n', at);
        const std::size_t next = end == std::string_view::npos ? all.size() : end + 1;
        out.append(all.substr(at, next - at));
        at = next;
    }
    return QString::fromUtf8(out);
}

QString decode(std::string_view bytes, TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:
        return QString::fromUtf8(bytes);
    case TextEncoding::Utf16LE:
        return QStringDecoder(QStringConverter::Utf16LE)(QByteArrayView(bytes));
    case TextEncoding::Utf16BE:
        return QStringDecoder(QStringConverter::Utf16BE)(QByteArrayView(bytes));
    case TextEncoding::Ansi: {
        const UINT codePage = ContentScanner::legacyCodePage();
        const int n = ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0)
            return {};
        QString out(n, Qt::Uninitialized);
        ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), reinterpret_cast<wchar_t*>(out.data()), n);
        return out;
    }
    }
    return {};
}

Result readTextFile(const std::wstring& path, std::uint64_t size)
{
    Result result;
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid())
        return result;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, kMaxTextFileBytes));
    std::string bytes(want, '\0');
    std::size_t got = 0;
    while (got < want) {
        DWORD n = 0;
        const DWORD ask = static_cast<DWORD>(std::min<std::size_t>(want - got, 1u << 20));
        if (!::ReadFile(file.get(), bytes.data() + got, ask, &n, nullptr))
            return result;
        if (n == 0)
            break;
        got += n;
    }
    bytes.resize(got);
    std::size_t bom = 0;
    const TextEncoding encoding = ContentScanner::detect(std::string_view(bytes).substr(0, ContentScanner::kChunkBytes), &bom);
    if (encoding != TextEncoding::Utf16LE && encoding != TextEncoding::Utf16BE
        && std::memchr(bytes.data(), 0, std::min(bytes.size(), kBinaryProbe))) {
        result.status = Status::NotText;
        return result;
    }
    result.text = decode(std::string_view(bytes).substr(bom), encoding);
    result.text.replace(u"\r\n"_s, u"\n"_s);
    result.cut = size > got;
    result.status = Status::Ok;
    return result;
}

} // namespace

Result read(const std::wstring& path, const Sources& sources, std::int64_t maxBytes, const std::function<bool()>& cancelled)
{
    Result result;
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = ::GetLastError();
        result.status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME
            ? Status::Missing
            : Status::Unreadable;
        return result;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        result.status = Status::Folder;
        return result;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    result.document = isDocumentPath(path);

    if (result.document && sources.index && sources.content) {
        std::optional<doctext::DocText> kept;
        {
            const auto lock = sources.index->readLock();
            // On a server or in WSL the text can be older than the file for a
            // while (RemoteContent reads a changed file again after a pause):
            // kept only when it is of the file as the index has it now (#114).
            if (const EntryId id = sources.index->findPath(path); id != kNoEntry
                && (!unc::isUnc(path) || sources.content->docStamp(id) == remotecontent::stampOf(*sources.index, id)))
                kept = sources.content->textOf(id);
        }
        if (kept) {
            result.status = Status::Ok;
            result.fromIndex = true;
            result.cut = kept->truncated;
            result.text = withPlaces(*kept);
            return result;
        }
    }

    if (data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
        result.status = Status::CloudOnly;
        return result;
    }
    if (!result.document)
        return readTextFile(path, size);

    if (static_cast<std::int64_t>(size) > maxBytes) {
        result.status = Status::TooLarge;
        return result;
    }
    if (!sources.extractor)
        return result;
    DocExtractor::Result read = sources.extractor->extractFile(path, maxBytes, cancelled);
    if (read.status != extractproto::Status::Ok) {
        result.status = read.large ? Status::TooLarge : Status::Unreadable;
        return result;
    }
    result.status = Status::Ok;
    result.cut = read.text.truncated;
    result.text = withPlaces(read.text);
    return result;
}

namespace {

// How much of `body` (no BOM) is whole lines, or failing a line end, whole
// characters: the rest was cut off by the read.
std::size_t wholePart(std::string_view body, TextEncoding encoding, unsigned codePage)
{
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(body[i]); };
    switch (encoding) {
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        const bool le = encoding == TextEncoding::Utf16LE;
        const std::size_t even = body.size() & ~std::size_t {1};
        for (std::size_t i = even; i >= 2; i -= 2) {
            if (at(i - 2 + (le ? 0 : 1)) == '\n' && at(i - 2 + (le ? 1 : 0)) == 0)
                return i;
        }
        // One long line: whole code units, not ending on a lead surrogate.
        if (even >= 2) {
            const unsigned unit = le ? at(even - 2) | (at(even - 1) << 8) : (at(even - 2) << 8) | at(even - 1);
            if (unit >= 0xD800 && unit <= 0xDBFF)
                return even - 2;
        }
        return even;
    }
    case TextEncoding::Utf8: {
        if (const std::size_t nl = body.rfind('\n'); nl != std::string_view::npos)
            return nl + 1;
        std::size_t i = body.size();
        while (i > 0 && (at(i - 1) & 0xC0) == 0x80)
            --i;
        if (i == 0)
            return 0;
        const unsigned lead = at(i - 1);
        const std::size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        return body.size() - (i - 1) >= need ? body.size() : i - 1;
    }
    case TextEncoding::Ansi: {
        // A trail byte of a double-byte code page is never below 0x40, so a
        // line feed always ends a line.
        if (const std::size_t nl = body.rfind('\n'); nl != std::string_view::npos)
            return nl + 1;
        std::size_t whole = 0;
        for (std::size_t i = 0; i < body.size();) {
            const std::size_t len = ::IsDBCSLeadByteEx(codePage, static_cast<BYTE>(at(i))) ? 2 : 1;
            if (i + len > body.size())
                break;
            i += len;
            whole = i;
        }
        return whole;
    }
    }
    return body.size();
}

bool isDoubleByteCodePage(unsigned codePage)
{
    return codePage == 932 || codePage == 936 || codePage == 949 || codePage == 950;
}

// Strictly decoded in `codePage`, or nothing when a byte does not fit it.
std::optional<QString> strictly(std::string_view bytes, unsigned codePage)
{
    if (bytes.empty())
        return QString();
    const int n = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0)
        return std::nullopt;
    QString text(n, Qt::Uninitialized);
    ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
        reinterpret_cast<wchar_t*>(text.data()), n);
    return text;
}

// The code page of a text that is neither UTF-8 nor UTF-16. The system's
// ANSI code page, except where that is a Western one (English Windows, or a
// Chinese one set to English for programs) and the bytes read as GBK, and
// as Chinese: every character beyond ASCII a CJK one. A Chinese user's old
// .txt files are GBK whatever the system locale says; Western text seldom
// passes (accented letters before a space or a full stop are not GBK).
unsigned ansiCodePageOf(std::string_view body)
{
    const unsigned system = ContentScanner::legacyCodePage();
    if (isDoubleByteCodePage(system))
        return system;
    const std::optional<QString> gbk = strictly(body, 936);
    if (!gbk)
        return system;
    int beyond = 0;
    int cjk = 0;
    for (const QChar c : *gbk) {
        const char16_t u = c.unicode();
        if (u < 0x80)
            continue;
        ++beyond;
        if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3000 && u <= 0x303F) || (u >= 0xFF00 && u <= 0xFFEF) || (u >= 0x2010 && u <= 0x201F))
            ++cjk;
    }
    return beyond > 0 && cjk == beyond ? 936 : system;
}

} // namespace

Head decodeHead(std::string_view bytes, std::uint64_t total)
{
    Head head;
    head.size = total;
    std::size_t bom = 0;
    // All of what was read decides it: a GBK character after the first
    // 512 KB would otherwise come out as UTF-8 gone wrong.
    const TextEncoding encoding = ContentScanner::detect(bytes, &bom);
    const bool wide = encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE;
    if (!wide && std::memchr(bytes.data(), 0, std::min(bytes.size(), kBinaryProbe))) {
        head.status = Status::NotText;
        return head;
    }
    std::string_view body = bytes.substr(bom);
    head.cut = total > bytes.size();
    // (Cut off mid-character, the last one would not be GBK: without it.)
    const unsigned codePage = encoding != TextEncoding::Ansi ? ContentScanner::legacyCodePage()
        : ansiCodePageOf(head.cut ? body.substr(0, wholePart(body, encoding, 936)) : body);
    if (head.cut)
        body = body.substr(0, wholePart(body, encoding, codePage));
    if (encoding == TextEncoding::Ansi && codePage != ContentScanner::legacyCodePage())
        head.text = strictly(body, codePage).value_or(QString());
    else
        head.text = decode(body, encoding);
    head.text.replace(u"\r\n"_s, u"\n"_s);
    switch (encoding) {
    case TextEncoding::Utf8: head.encoding = bom > 0 ? HeadEncoding::Utf8Bom : HeadEncoding::Utf8; break;
    case TextEncoding::Utf16LE: head.encoding = HeadEncoding::Utf16LE; break;
    case TextEncoding::Utf16BE: head.encoding = HeadEncoding::Utf16BE; break;
    case TextEncoding::Ansi:
        head.encoding = HeadEncoding::Ansi;
        head.codePage = codePage;
        break;
    }
    head.status = Status::Ok;
    return head;
}

Head readHead(const std::wstring& path, std::size_t maxBytes)
{
    Head head;
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(path).c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME)
            head.status = Status::Missing;
        return head;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        head.status = Status::Folder;
        return head;
    }
    if (data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
        head.status = Status::CloudOnly;
        return head;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid())
        return head;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, maxBytes));
    std::string bytes(want, '\0');
    std::size_t got = 0;
    while (got < want) {
        DWORD n = 0;
        if (!::ReadFile(file.get(), bytes.data() + got, static_cast<DWORD>(want - got), &n, nullptr))
            return head;
        if (n == 0)
            break;
        got += n;
    }
    bytes.resize(got);
    return decodeHead(bytes, std::max<std::uint64_t>(size, got));
}

} // namespace ws::filetext
