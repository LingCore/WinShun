#include "AppCatalog.h"
#include "AppLogo.h"
#include "AutostartRun.h"
#include "ChangeWatcher.h"
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "DoubleClickDetector.h"
#include "MenuHotkey.h"
#include "MissingFiles.h"
#include "DoubleTapDetector.h"
#include "Extract.h"
#include "GameGuard.h"
#include "JournalBudget.h"
#include "KeyListenerProtocol.h"
#include "KeyPresses.h"
#include "ShortcutAgain.h"
#include "TapTeller.h"
#include "FileIndex.h"
#include "FileText.h"
#include "History.h"
#include "HotkeySteps.h"
#include "PasteKeys.h"
#include "IndexFolder.h"
#include "IndexService.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsBulk.h"
#include "NtfsIndexer.h"
#include "PathText.h"
#include "Pinyin.h"
#include "Query.h"
#include "Release.h"
#include "SearchEngine.h"
#include "SearchTyping.h"
#include "ServiceLocation.h"
#include "ServiceVolumeAccess.h"
#include "SettingsMatch.h"
#include "ShareIndexer.h"
#include "ShareLister.h"
#include "ShareReach.h"
#include "Snapshot.h"
#include "SwitchCommand.h"
#include "SystemCatalog.h"
#include "TextUtil.h"
#include "Unc.h"
#include "VolumeProtocol.h"
#include "WebShortcut.h"
#include "WebSuggest.h"
#include "Win32Util.h"

#include <QRegularExpression>
#include "Writer.h"
#include "Wtf8.h"
#include "Xml.h"
#include "miniz.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <sddl.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

// Feeds `data` to the scanner `step` bytes per read call.
ContentScanner::ReadFn reader(QByteArray data, std::size_t step = 1u << 20)
{
    auto offset = std::make_shared<qsizetype>(0);
    return [data = std::move(data), offset, step](char* buffer, std::size_t capacity) -> std::size_t {
        const qsizetype n = std::min<qsizetype>(
            {static_cast<qsizetype>(capacity), static_cast<qsizetype>(step), data.size() - *offset});
        if (n <= 0)
            return 0;
        std::memcpy(buffer, data.constData() + *offset, static_cast<std::size_t>(n));
        *offset += n;
        return static_cast<std::size_t>(n);
    };
}

QByteArray encode(const QString& text, unsigned codePage)
{
    const auto* wide = reinterpret_cast<const wchar_t*>(text.utf16());
    const int n = ::WideCharToMultiByte(codePage, 0, wide, static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    QByteArray out(n, Qt::Uninitialized);
    ::WideCharToMultiByte(codePage, 0, wide, static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

QByteArray utf16le(const QString& text, bool bom)
{
    QByteArray out;
    if (bom)
        out.append("\xFF\xFE", 2);
    for (QChar c : text) {
        out.append(static_cast<char>(c.unicode() & 0xFF));
        out.append(static_cast<char>(c.unicode() >> 8));
    }
    return out;
}

template <typename T> void put(std::vector<std::byte>& data, std::size_t offset, T value)
{
    std::memcpy(data.data() + offset, &value, sizeof value);
}

// The grams of `bytes`, fed `chunk` bytes at a time.
std::vector<grams::Key> gramsOf(const QByteArray& bytes, TextEncoding encoding, std::size_t chunk = 1u << 20)
{
    grams::Collector collector(encoding, 936);
    for (qsizetype i = 0; i < bytes.size(); i += static_cast<qsizetype>(chunk))
        collector.feed(bytes.constData() + i, std::min<std::size_t>(chunk, static_cast<std::size_t>(bytes.size() - i)));
    return collector.finish();
}

std::vector<grams::Key> gramsOf(const QString& text)
{
    return gramsOf(text.toUtf8(), TextEncoding::Utf8);
}

bool hasAll(const std::vector<grams::Key>& keys, const QString& phrase)
{
    const std::vector<grams::Key> wanted = grams::ofPhrase(phrase);
    return !wanted.empty() && std::includes(keys.begin(), keys.end(), wanted.begin(), wanted.end());
}

// The entries a lookup says contain the phrase, and all it knows.
std::vector<EntryId> matchesOf(const ContentIndex& content, const QString& phrase)
{
    std::vector<EntryId> out;
    for (const std::uint32_t k : content.lookup(phrase).known) {
        if (k & ContentIndex::Lookup::kMatch)
            out.push_back(k & ~ContentIndex::Lookup::kMatch);
    }
    return out;
}

std::vector<EntryId> knownOf(const ContentIndex& content, const QString& phrase)
{
    std::vector<EntryId> out;
    for (const std::uint32_t k : content.lookup(phrase).known)
        out.push_back(k & ~ContentIndex::Lookup::kMatch);
    return out;
}

// ---- documents ------------------------------------------------------------------

// A zip archive (stored, or deflated) of these entries.
QByteArray makeZip(const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    for (const auto& [name, data] : entries)
        mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL);
    void* buffer = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    QByteArray out(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_zip_writer_end(&zip);
    return out;
}

// A compound file (version 3) with these streams in its root: those under
// 4096 bytes in the mini stream, as Office writes them.
QByteArray makeCfb(const std::vector<std::pair<std::u16string, QByteArray>>& streams)
{
    constexpr quint32 kEnd = 0xFFFFFFFE;
    constexpr quint32 kFree = 0xFFFFFFFF;
    constexpr quint32 kFatSector = 0xFFFFFFFD;
    std::vector<QByteArray> sectors;
    std::vector<quint32> fat;
    const auto chain = [&](const QByteArray& data) -> quint32 {
        if (data.isEmpty())
            return kEnd;
        const auto start = static_cast<quint32>(sectors.size());
        for (qsizetype at = 0; at < data.size(); at += 512) {
            QByteArray s = data.mid(at, 512);
            s.resize(512, '\0');
            sectors.push_back(s);
            fat.push_back(static_cast<quint32>(sectors.size()));
        }
        fat.back() = kEnd;
        return start;
    };
    const auto u32 = [](QByteArray& b, qsizetype at, quint32 v) { std::memcpy(b.data() + at, &v, 4); };

    QByteArray mini;
    std::vector<quint32> miniFat;
    std::vector<quint32> starts(streams.size(), kEnd);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const QByteArray& data = streams[i].second;
        if (data.size() >= 4096 || data.isEmpty())
            continue;
        const auto first = static_cast<quint32>(mini.size() / 64);
        QByteArray padded = data;
        padded.resize((data.size() + 63) / 64 * 64, '\0');
        mini += padded;
        for (qsizetype k = 0; k < padded.size() / 64; ++k)
            miniFat.push_back(first + static_cast<quint32>(k) + 1);
        miniFat.back() = kEnd;
        starts[i] = first;
    }
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].second.size() >= 4096)
            starts[i] = chain(streams[i].second);
    }
    const quint32 miniStart = chain(mini);
    QByteArray miniFatBytes(static_cast<qsizetype>(miniFat.size() * 4), '\0');
    if (!miniFat.empty())
        std::memcpy(miniFatBytes.data(), miniFat.data(), miniFatBytes.size());
    const quint32 miniFatStart = chain(miniFatBytes);

    QByteArray directory;
    const auto entry = [&](std::u16string_view name, quint8 type, quint32 child, quint32 right, quint32 start,
                           quint32 size) {
        QByteArray e(128, '\0');
        std::memcpy(e.data(), name.data(), name.size() * 2);
        const auto length = static_cast<quint16>((name.size() + 1) * 2);
        std::memcpy(e.data() + 0x40, &length, 2);
        e[0x42] = static_cast<char>(type);
        e[0x43] = 1;
        u32(e, 0x44, kFree);
        u32(e, 0x48, right);
        u32(e, 0x4C, child);
        u32(e, 0x74, start);
        u32(e, 0x78, size);
        directory += e;
    };
    entry(u"Root Entry", 5, streams.empty() ? kFree : 1, kFree, miniStart, static_cast<quint32>(mini.size()));
    for (std::size_t i = 0; i < streams.size(); ++i)
        entry(streams[i].first, 2, kFree, i + 1 < streams.size() ? static_cast<quint32>(i + 2) : kFree, starts[i],
            static_cast<quint32>(streams[i].second.size()));
    const quint32 directoryStart = chain(directory);

    const auto dataSectors = static_cast<quint32>(sectors.size());
    quint32 fatSectors = 1;
    while (dataSectors + fatSectors > fatSectors * 128)
        ++fatSectors;
    for (quint32 k = 0; k < fatSectors; ++k)
        fat.push_back(kFatSector);
    fat.resize(fatSectors * 128, kFree);

    QByteArray header(512, '\0');
    std::memcpy(header.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    const quint16 fields[] = {0x3E, 3, 0xFFFE, 9, 6};
    std::memcpy(header.data() + 0x18, fields, sizeof fields);
    u32(header, 0x2C, fatSectors);
    u32(header, 0x30, directoryStart);
    u32(header, 0x38, 4096);
    u32(header, 0x3C, miniFatStart);
    u32(header, 0x40, static_cast<quint32>((miniFatBytes.size() + 511) / 512));
    u32(header, 0x44, kEnd);
    for (quint32 k = 0; k < 109; ++k)
        u32(header, 0x4C + 4 * k, k < fatSectors ? dataSectors + k : kFree);
    QByteArray out = header;
    for (const QByteArray& s : sectors)
        out += s;
    out.append(reinterpret_cast<const char*>(fat.data()), static_cast<qsizetype>(fat.size() * 4));
    return out;
}

// A PDF of pages, each a list of lines (Helvetica, so ASCII only), with a
// cross-reference table that points right.
QByteArray makePdf(const std::vector<std::vector<std::string>>& pages)
{
    std::vector<std::string> objects;
    const auto pageCount = pages.size();
    std::string kids;
    for (std::size_t p = 0; p < pageCount; ++p)
        kids += std::to_string(3 + 2 * p) + " 0 R ";
    const std::size_t font = 3 + 2 * pageCount;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageCount) + " >>");
    for (std::size_t p = 0; p < pageCount; ++p) {
        std::string content = "BT /F1 12 Tf 20 180 Td";
        for (const std::string& line : pages[p])
            content += " (" + line + ") Tj 0 -20 Td";
        content += " ET";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents " + std::to_string(4 + 2 * p)
            + " 0 R /Resources << /Font << /F1 " + std::to_string(font) + " 0 R >> >> >>");
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");
    }
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t o : offsets) {
        char line[24];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
        + std::to_string(xref) + "\n%%EOF\n";
    return QByteArray::fromStdString(pdf);
}

QByteArray le16(std::initializer_list<quint16> values)
{
    QByteArray out;
    for (const quint16 v : values)
        out.append(reinterpret_cast<const char*>(&v), 2);
    return out;
}

QByteArray le32(quint32 v)
{
    return QByteArray(reinterpret_cast<const char*>(&v), 4);
}

QByteArray utf16(std::u16string_view s)
{
    return QByteArray(reinterpret_cast<const char*>(s.data()), static_cast<qsizetype>(s.size() * 2));
}

// One BIFF record: type, length, data.
QByteArray biff(quint16 type, const QByteArray& data)
{
    return le16({type, static_cast<quint16>(data.size())}) + data;
}

// A PowerPoint record (ver/instance, type, length) around `body`.
QByteArray pptRecord(quint16 verInstance, quint16 type, const QByteArray& body)
{
    return le16({verInstance, type}) + le32(static_cast<quint32>(body.size())) + body;
}

struct Extracted {
    extract::Status status;
    doctext::DocText text;
};

Extracted extractBytes(const QByteArray& bytes, std::size_t maxText = 16u << 20)
{
    Extracted out;
    extract::MemorySource source({bytes.constData(), static_cast<std::size_t>(bytes.size())});
    extract::Options options;
    options.maxText = maxText;
    options.codePage = 936;
    out.status = extract::extract(source, options, out.text);
    return out;
}

// The lines of a text, with each one's place: "page 2|text".
QStringList placedLines(const doctext::DocText& doc)
{
    QStringList out;
    const QString text = QString::fromUtf8(doc.text);
    const QStringList lines = text.split(u'\n', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const doctext::Location where = doctext::locate(doc, static_cast<std::uint32_t>(i + 1));
        QString place;
        switch (where.kind) {
        case doctext::PlaceKind::None:
            place = u"-"_s;
            break;
        case doctext::PlaceKind::Page:
            place = u"page %1"_s.arg(where.number);
            break;
        case doctext::PlaceKind::Slide:
            place = u"slide %1"_s.arg(where.number);
            break;
        case doctext::PlaceKind::Row:
            place = QString::fromUtf8(where.sheet) + u"!%1"_s.arg(where.number);
            break;
        }
        out.append(place + u'|' + lines[i]);
    }
    return out;
}

// A 1 KB MFT file record as NTFS writes it, update sequence included.
class RecordBuilder {
public:
    explicit RecordBuilder(std::uint16_t flags, std::uint64_t baseRecord = 0, std::size_t size = 1024)
        : m_data(size)
        , m_strides(size / 512)
        , m_pos((0x30 + 2 * (m_strides + 1) + 7) & ~std::size_t {7})
    {
        put<std::uint32_t>(m_data, 0, 0x454C4946); // "FILE"
        put<std::uint16_t>(m_data, 0x04, 0x30); // update sequence array
        put<std::uint16_t>(m_data, 0x06, static_cast<std::uint16_t>(m_strides + 1)); // check value + one per 512 bytes
        put<std::uint16_t>(m_data, 0x10, 1); // sequence number
        put<std::uint16_t>(m_data, 0x14, static_cast<std::uint16_t>(m_pos)); // first attribute
        put<std::uint16_t>(m_data, 0x16, flags);
        put<std::uint64_t>(m_data, 0x20, baseRecord);
    }

    void standardInformation(std::uint32_t attributes)
    {
        std::vector<std::byte> value(0x48);
        put(value, 0x20, attributes);
        attribute(0x10, value);
    }

    void fileName(std::uint64_t parent, std::u16string_view name, std::uint8_t nameSpace)
    {
        std::vector<std::byte> value(0x42 + name.size() * 2);
        put(value, 0, parent);
        value[0x40] = static_cast<std::byte>(name.size());
        value[0x41] = static_cast<std::byte>(nameSpace);
        std::memcpy(value.data() + 0x42, name.data(), name.size() * 2);
        attribute(0x30, value);
    }

    // An unnamed non-resident attribute of `size` bytes: where they are, as a run list.
    void nonResident(std::uint32_t type, const std::vector<std::byte>& runs, std::uint64_t lastVcn, std::uint64_t size = 0,
        std::uint64_t firstVcn = 0)
    {
        const std::size_t length = (0x40 + runs.size() + 7) & ~std::size_t {7};
        put(m_data, m_pos, type);
        put(m_data, m_pos + 4, static_cast<std::uint32_t>(length));
        m_data[m_pos + 8] = std::byte {1};
        put<std::uint64_t>(m_data, m_pos + 0x10, firstVcn);
        put<std::uint64_t>(m_data, m_pos + 0x18, lastVcn);
        put<std::uint16_t>(m_data, m_pos + 0x20, 0x40);
        put<std::uint64_t>(m_data, m_pos + 0x28, size); // allocated
        put<std::uint64_t>(m_data, m_pos + 0x30, size); // data
        put<std::uint64_t>(m_data, m_pos + 0x38, size); // initialized
        std::memcpy(m_data.data() + m_pos + 0x40, runs.data(), runs.size());
        m_pos += length;
    }

    void resident(std::uint32_t type, const std::vector<std::byte>& value) { attribute(type, value); }

    // An unnamed non-resident attribute with its sizes and flags given apart
    // (a file's $DATA: the valid data length short of the size, compressed...).
    void nonResidentData(const std::vector<std::byte>& runs, std::uint64_t lastVcn, std::uint64_t allocated,
        std::uint64_t size, std::uint64_t initialized, std::uint16_t flags)
    {
        const std::size_t at = m_pos;
        nonResident(0x80, runs, lastVcn, allocated);
        put<std::uint16_t>(m_data, at + 0x0C, flags);
        put<std::uint64_t>(m_data, at + 0x30, size);
        put<std::uint64_t>(m_data, at + 0x38, initialized);
    }

    std::vector<std::byte> finish()
    {
        put<std::uint32_t>(m_data, m_pos, 0xFFFF'FFFF);
        put<std::uint32_t>(m_data, 0x18, static_cast<std::uint32_t>(m_pos + 8)); // bytes in use
        put<std::uint32_t>(m_data, 0x1C, static_cast<std::uint32_t>(m_data.size()));
        // Each 512-byte stride ends in the check value; the real bytes move to the array.
        put<std::uint16_t>(m_data, 0x30, 0x0042);
        for (std::size_t i = 1; i <= m_strides; ++i) {
            std::memcpy(m_data.data() + 0x30 + i * 2, m_data.data() + i * 512 - 2, 2);
            put<std::uint16_t>(m_data, i * 512 - 2, 0x0042);
        }
        return m_data;
    }

private:
    void attribute(std::uint32_t type, const std::vector<std::byte>& value)
    {
        const std::size_t length = (0x18 + value.size() + 7) & ~std::size_t {7};
        put(m_data, m_pos, type);
        put(m_data, m_pos + 4, static_cast<std::uint32_t>(length));
        put(m_data, m_pos + 0x10, static_cast<std::uint32_t>(value.size()));
        put<std::uint16_t>(m_data, m_pos + 0x14, 0x18);
        std::memcpy(m_data.data() + m_pos + 0x18, value.data(), value.size());
        m_pos += length;
    }

    std::vector<std::byte> m_data;
    std::size_t m_strides;
    std::size_t m_pos;
};

// The run list (mapping pairs) of extents, as NTFS writes it.
std::vector<std::byte> runList(const std::vector<ntfs::Extent>& extents, std::uint64_t clusterSize)
{
    const auto append = [](std::vector<std::byte>& out, std::uint64_t value, unsigned bytes) {
        for (unsigned b = 0; b < bytes; ++b)
            out.push_back(static_cast<std::byte>(value >> (8 * b)));
    };
    std::vector<std::byte> out;
    std::int64_t lcn = 0;
    for (const ntfs::Extent& e : extents) {
        const std::uint64_t length = e.length / clusterSize;
        const std::int64_t delta = static_cast<std::int64_t>(e.offset / clusterSize) - lcn;
        lcn += delta;
        unsigned lengthBytes = 1;
        while (length >> (8 * lengthBytes))
            ++lengthBytes;
        unsigned deltaBytes = 1; // signed: the top bit of the last byte is the sign
        while (deltaBytes < 8 && (delta < -(std::int64_t {1} << (8 * deltaBytes - 1))
                   || delta >= (std::int64_t {1} << (8 * deltaBytes - 1))))
            ++deltaBytes;
        out.push_back(static_cast<std::byte>(deltaBytes << 4 | lengthBytes));
        append(out, length, lengthBytes);
        append(out, static_cast<std::uint64_t>(delta), deltaBytes);
    }
    out.push_back(std::byte {0});
    return out;
}

// Appends a USN_RECORD (version 2, or another version to be skipped).
void appendUsn(std::vector<std::byte>& buffer, std::uint64_t file, std::uint64_t parent, std::int64_t usn,
    std::uint32_t reason, std::uint32_t attributes, std::u16string_view name, std::uint16_t major = 2)
{
    const std::size_t offset = buffer.size();
    const std::size_t length = (60 + name.size() * 2 + 7) & ~std::size_t {7};
    buffer.resize(offset + length);
    put(buffer, offset, static_cast<std::uint32_t>(length));
    put(buffer, offset + 4, major);
    put(buffer, offset + 8, file);
    put(buffer, offset + 16, parent);
    put(buffer, offset + 24, usn);
    put(buffer, offset + 40, reason);
    put(buffer, offset + 52, attributes);
    put(buffer, offset + 56, static_cast<std::uint16_t>(name.size() * 2));
    put(buffer, offset + 58, std::uint16_t {60});
    std::memcpy(buffer.data() + offset + 60, name.data(), name.size() * 2);
}

// The NTFS record number of a folder on disk.
std::uint32_t recordOfPath(const QString& path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE h = ::CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info {};
    const bool ok = h != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(h, &info);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
    return ok ? ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow) : 0;
}

// C:\Users\me\Documents\report.docx etc.
struct SampleTree {
    FileIndex index;
    EntryId root, users, me, docs, report, notes, windows;

    SampleTree()
    {
        root = index.addRoot("C:");
        users = index.add(root, "Users", EntryFlag::Directory);
        me = index.add(users, "me", EntryFlag::Directory);
        docs = index.add(me, "Documents", EntryFlag::Directory);
        report = index.add(docs, "Report.docx", 0);
        notes = index.add(docs, "notes.txt", 0);
        windows = index.add(root, "Windows", EntryFlag::Directory | EntryFlag::LowPriority);
    }
};

// The places of Windows' Settings index that the system switches share a
// command with (as Windows lists them, with the keywords it has for them),
// and the switches of places.txt as shipped, applied the way loadPlaces does.
PlaceList switchPlaces()
{
    const auto page = [](const QString& name, const QString& command, const QStringList& keywords) {
        PlaceInfo p;
        p.name = name;
        p.key = name;
        p.command = command;
        p.kind = AppKind::Setting;
        p.keywords = keywords;
        p.page = true;
        return p;
    };
    PlaceList places {
        page(u"蓝牙和其他设备"_s, u"ms-settings:bluetooth"_s, {u"蓝牙"_s, u"bluetooth"_s, u"设备"_s}),
        page(u"颜色"_s, u"ms-settings:colors"_s, {u"深色模式"_s, u"强调色"_s}),
        page(u"声音"_s, u"ms-settings:sound"_s, {u"音量"_s, u"扬声器"_s}),
        page(u"夜灯"_s, u"ms-settings:nightlight"_s, {u"夜间模式"_s}),
        page(u"文件夹选项"_s, uR"(%windir%\system32\control.exe /name Microsoft.FolderOptions)"_s, {u"选项"_s}),
        page(u"Wi-Fi"_s, u"ms-settings:network-wifi"_s, {u"wifi"_s, u"无线网络"_s}),
        page(u"电池"_s, u"ms-settings:batterysaver"_s, {u"电源"_s, u"睡眠"_s}),
    };
    QFile file(u":/winshun/places.txt"_s);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        applyPlaceExtras(places, QString::fromUtf8(file.readAll()), true);
    for (PlaceInfo& p : places)
        p.prepare();
    return places;
}

// ---- GitHub #52: an index of the chosen folders, walked for real ----------

// Whether `path` is in the index (with its read lock, as the app looks).
bool indexedIn(const IndexService& service, const QString& path)
{
    const std::shared_ptr<FileIndex> index = service.index();
    const auto lock = index->readLock();
    return index->findPath(QDir::toNativeSeparators(path).toStdWString()) != kNoEntry;
}

// T\docs\a.txt, T\docs\sub\b.txt, T\pics\p.png, T\other\o.txt
bool makeScopeTree(const QString& t)
{
    QDir base(t);
    if (!base.mkpath(u"docs/sub"_s) || !base.mkpath(u"pics"_s) || !base.mkpath(u"other"_s))
        return false;
    for (const QString& file : {u"docs/a.txt"_s, u"docs/sub/b.txt"_s, u"pics/p.png"_s, u"other/o.txt"_s}) {
        QFile f(base.filePath(file));
        if (!f.open(QIODevice::WriteOnly))
            return false;
    }
    return true;
}

// Rules that leave out every other volume and, on `branch`'s, every folder
// but the ones on the way to it: its disk walked whole in a moment.
CrawlRules branchRules(const QString& branch)
{
    CrawlRules rules;
    const QString native = QDir::toNativeSeparators(branch);
    const std::wstring root = native.left(2).toStdWString();
    for (const VolumeInfo& v : listLocalVolumes(true)) {
        if (!win32::equalsIgnoreCase(v.root, root))
            rules.excludedPaths.push_back(v.root); // "E:": the whole drive
    }
    QString ancestor = native.left(2);
    for (const QString& next : native.mid(3).split(u'\\', Qt::SkipEmptyParts)) {
        const QFileInfoList siblings
            = QDir(ancestor + u'\\').entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo& sub : siblings) {
            if (sub.fileName().compare(next, Qt::CaseInsensitive) != 0)
                rules.excludedPaths.push_back(QDir::toNativeSeparators(sub.absoluteFilePath()).toStdWString());
        }
        ancestor += u'\\' + next;
    }
    return rules;
}

// Every volume walked and watched (no way to the journals), at once.
IndexService::Options walkedOptions(std::optional<std::vector<std::wstring>> folders, CrawlRules rules = {})
{
    IndexService::Options options;
    options.rules = std::move(rules);
    options.content.enabled = false;
    options.volumeAccess = std::make_shared<ntfs::NoVolumeAccess>();
    options.rescanOnStartup = false;
    options.folders = std::move(folders);
    return options;
}

// The index service, there but not answering (it is being updated, say).
class ServiceDown final : public ntfs::VolumeAccess {
public:
    explicit ServiceDown(Kind kind = Kind::Service) : m_kind(kind) { }
    Kind kind() const noexcept override { return m_kind; }
    std::unique_ptr<ntfs::RecordSource> openMft(std::wstring_view, std::uint32_t, bool) override { return nullptr; }
    std::optional<ntfs::JournalInfo> queryJournal(std::wstring_view, std::uint32_t) override { return std::nullopt; }
    std::unique_ptr<ntfs::JournalSource> openJournal(std::wstring_view, std::uint32_t, std::uint64_t) override
    {
        return nullptr;
    }
    void closeVolume(std::wstring_view) override { }
    std::unique_ptr<ntfs::BulkSource> openBulkRead(std::wstring_view, std::uint32_t, bool) override { return nullptr; }
private:
    Kind m_kind;
};

// Junctions need no elevation; every path and target belongs to the test.
bool makeJunction(const QString& link, const QString& target)
{
    if (!QDir().mkpath(link))
        return false;
    const auto linkPath = QDir::toNativeSeparators(link).toStdWString();
    const auto targetPath = QDir::toNativeSeparators(target).toStdWString();
    const std::wstring substitute = L"\\??\\" + targetPath;
    struct JunctionData {
        DWORD tag;
        WORD bytes;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t names[1];
    };
    const auto nameBytes = (substitute.size() + 1 + targetPath.size() + 1) * sizeof(wchar_t);
    std::vector<std::byte> buffer(offsetof(JunctionData, names) + nameBytes);
    auto* data = reinterpret_cast<JunctionData*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->bytes = WORD(buffer.size() - 8);
    data->substituteLength = WORD(substitute.size() * sizeof(wchar_t));
    data->printOffset = WORD((substitute.size() + 1) * sizeof(wchar_t));
    data->printLength = WORD(targetPath.size() * sizeof(wchar_t));
    std::memcpy(data->names, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<std::byte*>(data->names) + data->printOffset, targetPath.c_str(), (targetPath.size() + 1) * sizeof(wchar_t));
    win32::UniqueHandle handle(::CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    DWORD returned = 0;
    return handle.valid() && ::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, data, DWORD(buffer.size()), nullptr, 0, &returned, nullptr);
}

using FolderState = IndexService::FolderStatus::State;

// The chosen folders' states, in their order.
std::vector<FolderState> folderStates(const IndexService& service)
{
    std::vector<FolderState> states;
    for (const IndexService::FolderStatus& s : service.folderStatuses())
        states.push_back(s.state);
    return states;
}

bool allReady(const IndexService& service)
{
    const std::vector<FolderState> states = folderStates(service);
    return service.state() == IndexService::State::Ready
        && std::all_of(states.begin(), states.end(), [](FolderState s) { return s == FolderState::Ready; });
}

// A drive letter no disk has here ("Q:"): for a chosen folder whose disk is away.
std::wstring absentDrive()
{
    const DWORD drives = ::GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter > L'D'; --letter) {
        if (!(drives & (1u << (letter - L'A'))))
            return {letter, L':'};
    }
    return {};
}

// The volume `path` is on, as the index lists it.
std::optional<VolumeInfo> volumeOfPath(const QString& path)
{
    const std::wstring root = QDir::toNativeSeparators(path).left(2).toStdWString();
    for (const VolumeInfo& v : listLocalVolumes(true)) {
        if (win32::equalsIgnoreCase(v.root, root))
            return v;
    }
    return std::nullopt;
}

// The snapshot in `store` saved again as it is: its scope.json no longer goes with it.
bool resaveSnapshot(const QString& store, const std::function<void(snapshot::Contents&)>& change = {})
{
    const QString path = indexfolder::snapshotPath(store);
    std::optional<snapshot::Contents> contents = snapshot::load(path);
    if (!contents || !contents->rules)
        return false;
    if (change)
        change(*contents);
    const auto lock = contents->index->readLock();
    return snapshot::save(*contents->index, contents->volumes, contents->journals, *contents->rules, path);
}

// A hold (searchtyping::Hold) played out the way platform/TypeToSearch drives
// it: the input queue, where each key goes down the hook chain alone and
// SendInput's keys queue up behind the ones already waiting; the hook
// (Hold::key, then Hold::went once a key got to the window, and ownPassed);
// the message that the keys typed again went by, handled once the keys
// waiting have gone; the timer; and the window in front: every key that got
// there, in order.
class HoldRun {
public:
    using Hold = searchtyping::Hold;
    struct Key {
        unsigned vk = 0;
        unsigned scanCode = 0;
        bool press = true;
        bool own = false;
        bool extended = false;
    };

    Hold hold;
    Hold::Front front {.ours = true, .sourceFocused = true};
    std::uint32_t now = 10'000; // GetTickCount
    std::uint32_t startedAt = 0;
    std::vector<Key> window; // what got to the window in front
    std::vector<std::vector<searchtyping::Stroke>> batches; // every Step's keys sent
    bool ended = false;
    std::uint32_t endedAt = 0;
    int asksAgain = 0;
    int masksNow = 0; // masks the hook sent at once (Take::YesMask)
    bool looking = false; // the front looked at again shortly
    std::optional<unsigned> timer; // armed: ms after timerFrom
    std::uint32_t timerFrom = 0;
    std::optional<std::size_t> nextSendPutsIn; // the next SendInput puts in only this many
    std::function<bool(const Key&)> takenLater; // a hook after this one takes the key
    std::function<bool(const Key&)> lostOnTheWay; // ... a hook before it: never seen here, nor in the window
    // What a key types with AltGr: the layout's character (the platform asks ToUnicodeEx).
    std::function<std::u16string(unsigned vk)> altGr = [](unsigned) { return std::u16string(); };

    static unsigned scanOf(unsigned vk)
    {
        switch (vk) {
        case searchtyping::kVkLShift:
            return 0x2A;
        case searchtyping::kVkRShift:
            return searchtyping::kRightShiftScan;
        case searchtyping::kVkCapital:
            return 0x3A;
        case searchtyping::kVkLControl:
            return 0x1D;
        case searchtyping::kVkRMenu:
        case searchtyping::kVkLMenu:
            return 0x38;
        default:
            return vk; // letters: distinct, and none of the above
        }
    }

    // A key that got to the window before the hold (Shift held down already).
    void before(unsigned vk, bool press) { window.push_back({vk, scanOf(vk), press}); }
    // The first key, taken where the hook starts a hold.
    void start(unsigned vk, bool forFolder = false, searchtyping::Shifts shifts = {})
    {
        startedAt = now;
        apply(hold.begin(forFolder, shifts, now));
        hook({vk, scanOf(vk), true});
        run();
    }
    void press(unsigned vk, unsigned scanCode = 0, bool extended = false)
    {
        m_queue.push_back({vk, scanCode ? scanCode : scanOf(vk), true, false, extended});
    }
    void release(unsigned vk, unsigned scanCode = 0, bool extended = false)
    {
        m_queue.push_back({vk, scanCode ? scanCode : scanOf(vk), false, false, extended});
    }
    void tap(unsigned vk)
    {
        press(vk);
        release(vk);
    }
    // Letters pressed and let go, one after another (Shift as it is).
    void letters(const char* text)
    {
        for (; *text; ++text)
            tap(static_cast<unsigned>(std::toupper(static_cast<unsigned char>(*text))));
    }
    // AltGr down (the left Ctrl Windows makes up, then the right Alt) or up.
    void altGrKey(bool down)
    {
        const Key ctrl {searchtyping::kVkLControl, 0x1D | searchtyping::kSimulatedScan, down};
        const Key alt {searchtyping::kVkRMenu, 0x38, down, false, true};
        m_queue.push_back(down ? ctrl : alt);
        m_queue.push_back(down ? alt : ctrl);
    }
    // The user's keys queued now come in right after the `n`-th of Win顺's
    // keys from now has gone by (among a batch typed again).
    void during(int n)
    {
        while (!m_queue.empty()) {
            m_among.emplace(m_ownSeen + n, m_queue.front());
            m_queue.pop_front();
        }
    }

    // Everything queued goes down the chain, then the posted message (unless
    // postponed: the test changes what is in front first, passedNow).
    void run()
    {
        while (!m_queue.empty()) {
            const Key key = m_queue.front();
            m_queue.pop_front();
            hook(key);
        }
        if (!postponePassed)
            passedNow();
    }
    bool postponePassed = false;
    void passedNow()
    {
        if (std::exchange(m_passed, false)) {
            apply(hold.passed(front, now));
            run();
        }
    }
    void deliver(bool ready = true)
    {
        apply(hold.deliver(ready, front, now));
        run();
    }
    void giveBack()
    {
        apply(hold.giveBack(front, now));
        run();
    }
    void lookAgainAt(unsigned msSinceStart)
    {
        now = startedAt + msSinceStart;
        apply(hold.lookAgain(front, now));
        run();
    }
    // Win顺 lets go of the hold (TypeToSearch goes); the hook goes with it.
    void stop()
    {
        apply(hold.stop());
        run();
    }
    // The timer comes when due.
    void fire()
    {
        if (!timer)
            return;
        now = timerFrom + *timer;
        timer.reset();
        apply(hold.timeout(front, now));
        run();
    }

    searchtyping::Shifts windowShift() const
    {
        searchtyping::Shifts shift;
        for (const Key& key : window)
            shift.seen(key.vk, key.press);
        return shift;
    }
    // What the window typed: letters with Shift and Caps Lock as it had them
    // at each, characters as they are, other keys as <vk>.
    QString typed() const
    {
        searchtyping::Shifts shift;
        bool capsLock = false;
        QString text;
        for (const Key& key : window) {
            if (searchtyping::isShift(key.vk)) {
                shift.seen(key.vk, key.press);
            } else if (!key.press || key.vk == kMaskKey || searchtyping::isModifier(key.vk)) {
                if (key.press && key.vk == searchtyping::kVkCapital)
                    capsLock = !capsLock;
            } else if (key.vk == searchtyping::kVkPacket) {
                text += QChar(static_cast<char16_t>(key.scanCode));
            } else if (key.vk >= 'A' && key.vk <= 'Z') {
                text += QChar(static_cast<char16_t>(shift.any() != capsLock ? key.vk : key.vk + 0x20));
            } else {
                text += u'<' + QString::number(key.vk, 16) + u'>';
            }
        }
        return text;
    }
    // Shift pressed and let go with no other key pressed between, as the window saw it.
    int taps() const
    {
        searchtyping::Shifts shift;
        bool alone = false;
        int taps = 0;
        for (const Key& key : window) {
            if (searchtyping::isShift(key.vk)) {
                const bool wasDown = key.vk == searchtyping::kVkRShift ? shift.right : shift.left;
                if (key.press && !wasDown)
                    alone = true;
                else if (!key.press && alone)
                    ++taps, alone = false;
                shift.seen(key.vk, key.press);
            } else if (key.press) {
                alone = false;
            }
        }
        return taps;
    }
    // Keys other than Shift the window still has down, as <vk>.
    QString stuck() const
    {
        std::set<unsigned> down;
        for (const Key& key : window) {
            if (searchtyping::isShift(key.vk) || key.vk == searchtyping::kVkPacket)
                continue;
            if (key.press)
                down.insert(key.vk);
            else
                down.erase(key.vk);
        }
        QString text;
        for (const unsigned vk : down)
            text += u'<' + QString::number(vk, 16) + u'>';
        return text;
    }
    // The keys the window got, by name: "A+ LShift- mask+ char(ż)+ <24>-".
    QString trace() const
    {
        QStringList names;
        for (const Key& key : window) {
            QString name;
            switch (key.vk) {
            case searchtyping::kVkLShift:
                name = u"LShift"_s;
                break;
            case searchtyping::kVkRShift:
                name = u"RShift"_s;
                break;
            case searchtyping::kVkLControl:
                name = (key.scanCode & searchtyping::kSimulatedScan) ? u"AltGrCtrl"_s : u"LCtrl"_s;
                break;
            case searchtyping::kVkRMenu:
                name = u"RAlt"_s;
                break;
            case searchtyping::kVkCapital:
                name = u"Caps"_s;
                break;
            case kMaskKey:
                name = u"mask"_s;
                break;
            case searchtyping::kVkPacket:
                name = u"char("_s + QChar(static_cast<char16_t>(key.scanCode)) + u')';
                break;
            default:
                name = key.vk >= 'A' && key.vk <= 'Z' ? QString(QChar(static_cast<char16_t>(key.vk)))
                                                      : u'<' + QString::number(key.vk, 16) + u'>';
                break;
            }
            names << name + QLatin1Char(key.press ? '+' : '-');
        }
        return names.join(u' ');
    }
    // Win顺's own Shift keys sent, all batches.
    int ownShifts() const
    {
        int n = 0;
        for (const auto& batch : batches)
            n += static_cast<int>(std::ranges::count_if(batch, [](const auto& s) { return searchtyping::isShift(s.vk); }));
        return n;
    }

private:
    void hook(const Key& key)
    {
        if (key.own && lostOnTheWay && lostOnTheWay(key))
            return;
        const Hold::KeyEvent event {key.vk, key.scanCode, key.extended, key.press, key.own};
        if (!key.own && hold.holding()) {
            const Hold::Take take = hold.key(event,
                [this](const Hold::KeyEvent& k, searchtyping::Shifts, bool) { return altGr(k.vk); });
            if (take == Hold::Take::YesMask) {
                // Sent from the hook: behind the keys of Win顺's already
                // waiting, ahead of the user's next (they come later).
                ++masksNow;
                const auto next = std::ranges::find_if(m_queue, [](const Key& k) { return !k.own; });
                const Key mask[] {{kMaskKey, 0, true, true}, {kMaskKey, 0, false, true}};
                m_queue.insert(next, std::begin(mask), std::end(mask));
            }
            if (take != Hold::Take::No)
                return;
        }
        const bool got = !(takenLater && takenLater(key));
        if (got)
            window.push_back(key);
        if (key.own && hold.ownPassed(key.vk, !key.press))
            m_passed = true; // posted: handled once the keys waiting have gone
        if (got)
            hold.went(event);
        if (key.own) {
            ++m_ownSeen;
            const auto [from, to] = m_among.equal_range(m_ownSeen);
            std::vector<Key> next;
            for (auto it = from; it != to; ++it)
                next.push_back(it->second);
            m_among.erase(from, to);
            for (auto it = next.rbegin(); it != next.rend(); ++it)
                m_queue.push_front(*it);
        }
    }
    void apply(Hold::Step step)
    {
        if (step.askAgain)
            ++asksAgain;
        if (step.lookAgain)
            looking = true;
        else if (!step.send.empty() || step.ended)
            looking = false;
        if (step.timerMs) {
            timer = step.timerMs;
            timerFrom = now;
        }
        if (!step.send.empty()) {
            batches.push_back(step.send);
            const std::size_t putIn = std::min(nextSendPutsIn.value_or(step.send.size()), step.send.size());
            nextSendPutsIn.reset();
            for (std::size_t i = 0; i < putIn; ++i) {
                const searchtyping::Stroke& s = step.send[i];
                m_queue.push_back({s.vk, s.scanCode, !s.release, true, s.extended});
            }
            if (putIn < step.send.size())
                apply(hold.sent(putIn));
        }
        if (step.ended) {
            ended = true;
            endedAt = now;
            timer.reset();
        }
    }

    std::deque<Key> m_queue;
    std::multimap<int, Key> m_among;
    int m_ownSeen = 0;
    bool m_passed = false;
};

// A server on 127.0.0.1 for share::probeAt (GitHub #116), answering each
// framing of the probe's NEGOTIATE as it is told: in SMB 2 or SMB 1, not at
// all (the connection kept open, as a proxy in TUN mode does), or by hanging
// up. On another loopback address (127.0.0.2, in host order) and a given
// port, for a server with two addresses; port() 0 when that is taken.
class FakeSmb {
public:
    enum class Then : std::uint8_t { Smb2, Smb1, Quiet, HangUp };

    FakeSmb(Then smb1Framing, Then smb2Framing, std::uint32_t address = INADDR_LOOPBACK, std::uint16_t port = 0)
        : m_smb1Framing(smb1Framing)
        , m_smb2Framing(smb2Framing)
    {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        m_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at {};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(address);
        at.sin_port = htons(port);
        int length = sizeof at;
        if (::bind(m_listener, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0
            && ::getsockname(m_listener, reinterpret_cast<sockaddr*>(&at), &length) == 0 && ::listen(m_listener, 16) == 0)
            m_port = ntohs(at.sin_port);
        m_accepting = std::thread([this] {
            for (;;) {
                const SOCKET s = ::accept(m_listener, nullptr, nullptr);
                if (s == INVALID_SOCKET)
                    return; // closed: the end
                std::lock_guard lock(m_mutex);
                m_open.push_back(s);
                m_serving.emplace_back([this, s] { serve(s); });
            }
        });
    }
    ~FakeSmb()
    {
        ::closesocket(m_listener);
        m_accepting.join();
        for (const SOCKET s : m_open)
            ::shutdown(s, SD_BOTH); // a quiet one's recv() returns
        for (std::thread& t : m_serving)
            t.join();
        for (const SOCKET s : m_open)
            ::closesocket(s);
        ::WSACleanup();
    }
    FakeSmb(const FakeSmb&) = delete;
    FakeSmb& operator=(const FakeSmb&) = delete;

    std::uint16_t port() const { return m_port; }
    int asked(bool smb2Framing) const { return (smb2Framing ? m_smb2Asked : m_smb1Asked).load(); }

private:
    void serve(SOCKET s)
    {
        unsigned char head[5] {}; // the NetBIOS header, then 0xFF (SMB 1 framing) or 0xFE
        for (int got = 0; got < 5;) {
            const int n = ::recv(s, reinterpret_cast<char*>(head) + got, 5 - got, 0);
            if (n <= 0)
                return;
            got += n;
        }
        const bool smb2 = head[4] == 0xFE;
        ++(smb2 ? m_smb2Asked : m_smb1Asked);
        const Then then = smb2 ? m_smb2Framing : m_smb1Framing;
        if (then == Then::HangUp) {
            ::shutdown(s, SD_BOTH);
            return;
        }
        if (then != Then::Quiet) {
            const unsigned char reply[8] {0, 0, 0, 4, static_cast<unsigned char>(then == Then::Smb2 ? 0xFE : 0xFF), 'S', 'M', 'B'};
            ::send(s, reinterpret_cast<const char*>(reply), sizeof reply, 0);
        }
        char c = 0;
        while (::recv(s, &c, 1, 0) > 0) {
        } // until the probe hangs up (or the end)
    }

    Then m_smb1Framing;
    Then m_smb2Framing;
    SOCKET m_listener = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::thread m_accepting;
    std::mutex m_mutex;
    std::vector<SOCKET> m_open;
    std::vector<std::thread> m_serving;
    std::atomic<int> m_smb1Asked {0};
    std::atomic<int> m_smb2Asked {0};
};

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

private slots:
    void pathFromText()
    {
        using pathtext::pathFromText;
        QCOMPARE(pathFromText(u"C:\\Users\\me\\file.txt"_s), u"C:\\Users\\me\\file.txt"_s);
        QCOMPARE(pathFromText(u"  \"C:\\Program Files\\App\"  \r\n"_s), u"C:\\Program Files\\App"_s); // Copy as path
        QCOMPARE(pathFromText(u"\n\nD:/work/docs/\nsecond line"_s), u"D:\\work\\docs"_s);
        QCOMPARE(pathFromText(u"C:\\\\Users\\\\me\\\\"_s), u"C:\\Users\\me"_s); // doubled, as in source code
        QCOMPARE(pathFromText(u"e:"_s), u"e:\\"_s);
        QCOMPARE(pathFromText(u"C:\\"_s), u"C:\\"_s);
        QCOMPARE(pathFromText(u"file:///C:/Users/me/a%20b.txt"_s), u"C:\\Users\\me\\a b.txt"_s);
        QCOMPARE(pathFromText(u"\\\\server\\share\\folder\\"_s), u"\\\\server\\share\\folder"_s);
        QCOMPARE(pathFromText(u"%SystemRoot%\\System32"_s), qEnvironmentVariable("SystemRoot") + u"\\System32"_s);
        QCOMPARE(pathFromText(u"C:\\100%NOSUCHVARIABLE%\\x"_s), u"C:\\100%NOSUCHVARIABLE%\\x"_s);
        QVERIFY(pathFromText(u"Users\\me"_s).isEmpty()); // relative
        QVERIFY(pathFromText(u"\\\\server"_s).isEmpty()); // no share
        QVERIFY(pathFromText(u"C:\\a?b"_s).isEmpty());
        QVERIFY(pathFromText(u"C:\\a:b"_s).isEmpty());
        QVERIFY(pathFromText(u"https://example.com/a"_s).isEmpty());
        QVERIFY(pathFromText(u"hello world"_s).isEmpty());
        QVERIFY(pathFromText({}).isEmpty());
    }

    void filterExtensions()
    {
        using pathtext::filterExtensions;
        QCOMPARE(filterExtensions(u"Text files (*.txt;*.LOG)"_s), (QStringList {u"txt"_s, u"log"_s}));
        QCOMPARE(filterExtensions(u"图片 (*.png, *.jpg; *.png)"_s), (QStringList {u"png"_s, u"jpg"_s}));
        QCOMPARE(filterExtensions(u"*.tar.gz"_s), (QStringList {u"tar.gz"_s}));
        QVERIFY(filterExtensions(u"所有文件 (*.*)"_s).isEmpty());
        QVERIFY(filterExtensions(u"All files (*)"_s).isEmpty());
        QVERIFY(filterExtensions(u"Web pages (*.htm*)"_s).isEmpty());
        QVERIFY(filterExtensions(u"Word 文档"_s).isEmpty());
    }

    // The preview's text (filetext::decodeHead): the encoding as the content
    // search finds it, and a cut start ends on a whole line or character.
    void previewTextHead()
    {
        using filetext::decodeHead;
        using filetext::HeadEncoding;
        const auto bytes = [](std::initializer_list<int> list) {
            std::string s;
            for (const int b : list)
                s.push_back(static_cast<char>(b));
            return s;
        };
        // Whole file, UTF-8 with CRLF.
        filetext::Head h = decodeHead("one\r\ntwo\r\n", 10);
        QCOMPARE(h.status, filetext::Status::Ok);
        QCOMPARE(h.text, u"one\ntwo\n"_s);
        QCOMPARE(h.encoding, HeadEncoding::Utf8);
        QVERIFY(!h.cut);
        // UTF-8 with a BOM.
        h = decodeHead(bytes({0xEF, 0xBB, 0xBF, 'h', 'i'}), 5);
        QCOMPARE(h.text, u"hi"_s);
        QCOMPARE(h.encoding, HeadEncoding::Utf8Bom);
        // Cut: back to the last line end.
        h = decodeHead("line 1\nline 2 cut he", 1000);
        QVERIFY(h.cut);
        QCOMPARE(h.text, u"line 1\n"_s);
        // One long line cut inside a character (好 is E5 A5 BD): the whole ones only.
        h = decodeHead(bytes({'a', 0xE4, 0xBD, 0xA0, 0xE5, 0xA5}), 100);
        QCOMPARE(h.encoding, HeadEncoding::Utf8);
        QCOMPARE(h.text, u"a你"_s);
        // UTF-16 LE with a BOM, cut after a line.
        h = decodeHead(bytes({0xFF, 0xFE, 'x', 0, '\n', 0, 'y', 0}), 100);
        QCOMPARE(h.encoding, HeadEncoding::Utf16LE);
        QCOMPARE(h.text, u"x\n"_s);
        // A NUL early on: not text.
        QCOMPARE(decodeHead(bytes({'M', 'Z', 0, 0, 3}), 5).status, filetext::Status::NotText);
        // GBK: 你好 is C4 E3 BA C3; cut inside the second. On a Chinese
        // Windows as its ANSI code page, on a Western one because every
        // character beyond ASCII comes out Chinese.
        const unsigned system = ContentScanner::legacyCodePage();
        if (system == 936 || system == 1252) {
            h = decodeHead(bytes({0xC4, 0xE3, 0xBA, 0xC3}), 4);
            QCOMPARE(h.encoding, HeadEncoding::Ansi);
            QCOMPARE(h.codePage, 936u);
            QCOMPARE(h.text, u"你好"_s);
            h = decodeHead(bytes({0xC4, 0xE3, 0xBA}), 50);
            QCOMPARE(h.text, u"你"_s);
            QCOMPARE(h.codePage, 936u);
        }
        if (system == 1252) {
            // Western text stays Western: "café." (E9 then a full stop is no GBK pair).
            h = decodeHead(bytes({'c', 'a', 'f', 0xE9, '.'}), 5);
            QCOMPARE(h.codePage, 1252u);
            QCOMPARE(h.text, u"café."_s);
        }
        // From the disk (Windows paths, as the preview gives them): a folder, a missing file.
        QCOMPARE(filetext::readHead(QDir::toNativeSeparators(QDir::tempPath()).toStdWString(), 1024).status, filetext::Status::Folder);
        QCOMPARE(filetext::readHead(L"C:\\no\\such\\file.txt", 1024).status, filetext::Status::Missing);
    }

    // GitHub #52: with only some folders indexed, is a folder among them, and
    // what the launcher says of one that is not.
    void chosenFolders()
    {
        const QStringList chosen {u"C:\\Users\\me\\Documents"_s, u"D:"_s};
        QVERIFY(pathtext::inChosenFolders(chosen, u"C:\\Users\\me\\Documents"_s));
        QVERIFY(pathtext::inChosenFolders(chosen, u"c:\\users\\ME\\documents\\Work\\"_s)); // case, a backslash at the end
        QVERIFY(pathtext::inChosenFolders(chosen, u"D:\\"_s)); // a whole drive: its root, and
        QVERIFY(pathtext::inChosenFolders(chosen, u"d:\\any\\where"_s)); // all it holds
        QVERIFY(!pathtext::inChosenFolders(chosen, u"C:\\Users\\me"_s)); // above one: its other contents are not
        QVERIFY(!pathtext::inChosenFolders(chosen, u"C:\\Users\\me\\Documents2"_s)); // a longer name is another folder
        QVERIFY(!pathtext::inChosenFolders(chosen, u"E:\\"_s));
        QVERIFY(!pathtext::inChosenFolders({}, u"C:\\"_s));

        // What is typed: the folder and the name after it, only when it is not indexed.
        pathtext::TypedPath typed = pathtext::typedOutside(u"C:\\Users\\me\\Downloads\\ report"_s, chosen);
        QCOMPARE(typed.folder, u"C:\\Users\\me\\Downloads"_s);
        QCOMPARE(typed.name.trimmed(), u"report"_s);
        QVERIFY(pathtext::typedOutside(u"C:\\Users\\me\\Documents\\ report"_s, chosen).folder.isEmpty());
        QVERIFY(pathtext::typedOutside(u"D:\\work\\ report"_s, chosen).folder.isEmpty());
        QVERIFY(pathtext::typedOutside(u"report"_s, chosen).folder.isEmpty()); // no path
        QVERIFY(pathtext::typedOutside(u"abc/"_s, chosen).folder.isEmpty()); // a name along the way, not a place
        QVERIFY(pathtext::typedOutside(u"\\\\nas\\share\\x\\ report"_s, chosen).folder.isEmpty()); // the network has its own list
        QCOMPARE(pathtext::typedOutside(u"E:\\"_s, chosen).folder, u"E:\\"_s);

        // The note: outside beats the general one; the general one once, and
        // only for few rows found of something typed.
        using pathtext::ScopeNote;
        QCOMPARE(pathtext::scopeNote(false, true, false, 0, 8, 5), ScopeNote::None); // whole disks: never
        QCOMPARE(pathtext::scopeNote(true, true, true, 20, 8, 5), ScopeNote::Outside);
        QCOMPARE(pathtext::scopeNote(true, false, false, 0, 8, 5), ScopeNote::Chosen);
        QCOMPARE(pathtext::scopeNote(true, false, false, 7, 8, 2), ScopeNote::Chosen);
        QCOMPARE(pathtext::scopeNote(true, false, false, 8, 8, 5), ScopeNote::None); // the window is full
        QCOMPARE(pathtext::scopeNote(true, false, false, 0, 8, 1), ScopeNote::None); // one letter
        QCOMPARE(pathtext::scopeNote(true, false, true, 0, 8, 5), ScopeNote::None); // closed for good
    }

    // GitHub #52: a folder the index does not hold, walked on the disk for
    // what is typed (the drive walk, for any folder).
    void walkAFolder()
    {
        QTemporaryDir dir;
        const auto touch = [&](const QString& relative) {
            const QString path = dir.filePath(relative);
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        };
        touch(u"a/b/report.txt"_s);
        touch(u"other.txt"_s);
        touch(u"deep/er/report-two.txt"_s);
        touch(u"Report-top.txt"_s);

        SearchEngine engine(nullptr);
        QSignalSpy rows(&engine, &SearchEngine::driveResults);
        QSignalSpy progress(&engine, &SearchEngine::driveProgress);
        const auto walk = [&](const QString& folder, const QString& text, int limit = 100) {
            rows.clear();
            progress.clear();
            SearchEngine::Request request;
            request.text = text;
            request.limit = limit;
            request.walkFolder = folder;
            const quint64 id = engine.submit(std::move(request));
            const auto finished = [&] {
                return !progress.isEmpty() && progress.last().at(2).toBool() && progress.last().at(0).toULongLong() == id;
            };
            if (!QTest::qWaitFor(finished, 5000))
                return QStringList {u"(never finished)"_s};
            QStringList paths;
            for (const QList<QVariant>& batch : rows) {
                for (const SearchResult& r : batch.at(1).value<SearchResults>())
                    paths.append(QDir::fromNativeSeparators(r.path));
            }
            paths.sort();
            return paths;
        };
        const QString top = dir.path(); // '/' separated, as walk() gives paths back
        QStringList reports {top + u"/Report-top.txt"_s, top + u"/a/b/report.txt"_s, top + u"/deep/er/report-two.txt"_s};
        reports.sort();
        QCOMPARE(walk(QDir::toNativeSeparators(dir.path()), u"report"_s), reports);
        QCOMPARE(walk(QDir::toNativeSeparators(dir.path()) + u'\\', u"report"_s), reports); // a backslash at the end
        QVERIFY(progress.last().at(1).toInt() >= 8); // it looked at every file and folder
        QCOMPARE(walk(QDir::toNativeSeparators(dir.filePath(u"a"_s)), u"report"_s), QStringList {top + u"/a/b/report.txt"_s}); // only below the folder
        QCOMPARE(walk(QDir::toNativeSeparators(dir.path()), u"report"_s, 1).size(), 1); // stops at the limit
        QCOMPARE(walk(QDir::toNativeSeparators(dir.filePath(u"missing"_s)), u"report"_s), QStringList {}); // not there: nothing, and it ends
    }

    void folderFromAddress()
    {
        using pathtext::folderFromAddress;
        QCOMPARE(folderFromAddress(u"地址: C:\\Windows\\System32"_s), u"C:\\Windows\\System32"_s);
        QCOMPARE(folderFromAddress(u"Address: D:\\"_s), u"D:\\"_s);
        QCOMPARE(folderFromAddress(u"Adresse : \\\\nas\\photos\\2024"_s), u"\\\\nas\\photos\\2024"_s);
        QVERIFY(folderFromAddress(u"地址: 此电脑"_s).isEmpty());
        QVERIFY(folderFromAddress(u"Address: Libraries\\Documents"_s).isEmpty());
        QVERIFY(pathtext::sameFolder(u"c:\\users\\ME\\"_s, u"C:\\Users\\me"_s));
        QVERIFY(!pathtext::sameFolder(u"C:\\Users"_s, u"C:\\Users\\me"_s));
        QVERIFY(!pathtext::sameFolder({}, {}));
    }

    // What Total Commander and Directory Opus say they show (filemanager::).
    void fileManagerText()
    {
        using pathtext::folderFromTotalCommander;
        QCOMPARE(folderFromTotalCommander(u"c:\\Windows\\*.*"_s), u"C:\\Windows"_s);
        QCOMPARE(folderFromTotalCommander(u"d:\\*.*"_s), u"D:\\"_s);
        QCOMPARE(folderFromTotalCommander(u"c:\\文档\\报告 2024\\*.txt"_s), u"C:\\文档\\报告 2024"_s);
        QCOMPARE(folderFromTotalCommander(u"\\\\nas\\photos\\*.*"_s), u"\\\\nas\\photos"_s);
        QCOMPARE(folderFromTotalCommander(u"c:\\Windows"_s), u"C:\\Windows"_s); // no filter shown
        QVERIFY(folderFromTotalCommander(u"\\\\\\Uninstaller64\\*.*"_s).isEmpty()); // a plugin's
        QVERIFY(folderFromTotalCommander(u"ftp://example.com/pub/*.*"_s).isEmpty());
        QVERIFY(folderFromTotalCommander({}).isEmpty());

        // As dopusrt /info writes it (UTF-8 with a BOM once a name is not ASCII).
        const QByteArray xml = QByteArray("\xEF\xBB\xBF")
            + QString::fromUtf16(uR"(<?xml version="1.0" encoding="UTF-8"?>
<results command="paths" result="1">
	<path active_lister="1" display_path="C:\" lister="0x6185e" side="1" tab="0x3185a">C:\</path>
	<path active_lister="1" active_tab="1" display_path="C:\用户\me\A &amp; B" lister="0x6185e" side="1" tab="0x518c0" tab_state="1">C:\Users\me\A &amp; B</path>
	<path active_lister="1" active_tab="2" display_path="D:\" lister="0x6185e" side="2" tab="0x1187a" tab_state="2">D:\</path>
	<path display_path="FTP" lister="0x7f00a" side="1" tab="0x2" tab_state="1">ftp://example.com/</path>
	<path display_path="Windows" lister="0x7f00a" side="1" tab="0x3">C:\Windows</path>
</results>
)").toUtf8();
        const QList<pathtext::OpusTab> tabs = pathtext::opusTabs(xml);
        QCOMPARE(tabs.size(), 4); // not the FTP site
        QCOMPARE(tabs[0].path, u"C:\\"_s);
        QCOMPARE(tabs[0].lister, quintptr(0x6185e));
        QCOMPARE(tabs[0].state, 0);
        QCOMPARE(tabs[1].path, u"C:\\Users\\me\\A & B"_s); // the real path, not the display one
        QCOMPARE(tabs[1].state, 1);
        QCOMPARE(tabs[1].tab, quintptr(0x518c0)); // the actual pane, not just the active lister
        QCOMPARE(tabs[2].state, 2);
        QCOMPARE(tabs[3].lister, quintptr(0x7f00a));
        QVERIFY(pathtext::opusTabs("<results command=\"paths\" result=\"0\"/>").isEmpty());
        QVERIFY(pathtext::opusTabs("not xml").isEmpty());
    }

    // What XYplorer answers (filemanager::), and what goes into its scripts.
    void xyplorerText()
    {
        using pathtext::xyplorerTabs;
        // Both panes shown: the active pane's tab on top (state 1), the
        // other pane's (2), the rest (0). pathreal() gives "C:" for a drive,
        // special places stay as they are ("此电脑"), a quick search keeps
        // its "?pattern".
        QList<pathtext::XYplorerTab> tabs = xyplorerTabs(
            u"1\r\n|F:\\wsfmtest\\子文件夹 甲|此电脑|c:|C:\\Users\\me?*.bmp\r\n|D:\\Work|\\\\nas\\photos\\2024|"_s);
        QCOMPARE(tabs.size(), 5);
        QCOMPARE(tabs[0].path, u"F:\\wsfmtest\\子文件夹 甲"_s);
        QCOMPARE(tabs[0].state, 1);
        QCOMPARE(tabs[1].path, u"C:\\"_s); // the drive, as a folder
        QCOMPARE(tabs[1].state, 0);
        QCOMPARE(tabs[2].path, u"C:\\Users\\me"_s); // the folder searched
        QCOMPARE(tabs[3].path, u"D:\\Work"_s);
        QCOMPARE(tabs[3].state, 2);
        QCOMPARE(tabs[4].path, u"\\\\nas\\photos\\2024"_s);
        QCOMPARE(tabs[4].state, 0);
        // One pane shown: the other's tabs are not on view.
        tabs = xyplorerTabs(u"0\n|此电脑|C:\\Windows\n|D:\\|E:\\"_s);
        QCOMPARE(tabs.size(), 1);
        QCOMPARE(tabs[0].path, u"C:\\Windows"_s);
        QCOMPARE(tabs[0].state, 1);
        QVERIFY(xyplorerTabs(u"1\n|此电脑\n|"_s).isEmpty());
        QVERIFY(xyplorerTabs({}).isEmpty());
        QVERIFY(xyplorerTabs(u"garbage"_s).isEmpty());

        QCOMPARE(pathtext::xyplorerQuoted(u"C:\\Users\\me"_s), u"'C:\\Users\\me'"_s);
        QCOMPARE(pathtext::xyplorerQuoted(u"D:\\John's 100%"_s), u"'D:\\John''s 100%'"_s);
        QCOMPARE(pathtext::xyplorerQuoted({}), u"''"_s);
    }

    // What Pulse keeps in its session.json (filemanager::, GitHub #41).
    void pulseText()
    {
        using pathtext::pulseWindows;
        // As Pulse 1.0.56 wrote it (two windows, the one used last first; the
        // other split in two, the right pane with the focus, and a tab behind).
        const QByteArray session = QString::fromUtf16(uR"json({
  "version":8,
  "left":500,
  "top":250,
  "right":1500,
  "bottom":1000,
  "maximized":false,
  "dark":true,
  "path":"\\\\?\\F:\\wspulse\\t\\丁",
  "sidebarCollapsed":0,
  "sidebarHidden":0,
  "sidebarOrder":"6,7,1,0,2,4,3,5",
  "quickAccessHidden":0,
  "starredExpanded":true,
  "detailsPanel":0,
  "detailsPanelWidth":340,
  "detailsPreviewOnly":false,
  "detailsPreview":true,
  "tray":[
],
  "undo":[
],
  "activeTab":0,
  "tabGroups":[],
  "layoutTabs":[{"pinned":false,"group":0,"marker_rgb":0,"title":"","layout":0,"focused":0,"target":-1,"splitRatios":"","panes":[{"path":"\\\\?\\F:\\wspulse\\t\\丁","view":"details","cols":"0,0,0","searchCols":"0,0,0,0","colLayout":false,"colWidths":""}]}],
  "focusedWindow":0,
  "windows":[
    {"left":500,"top":250,"right":1500,"bottom":1000,"maximized":false,
     "sidebarCollapsed":0,"sidebarHidden":0,"sidebarOrder":"6,7,1,0,2,4,3,5","quickAccessHidden":0,"starredExpanded":true,
     "detailsPanel":0,"detailsPanelWidth":340,"detailsPreviewOnly":false,"detailsPreview":true,
     "windowName":"","windowColor":1,
     "activeTab":0,
     "tabGroups":[],
     "layoutTabs":[{"pinned":false,"group":0,"marker_rgb":0,"title":"","layout":0,"focused":0,"target":-1,"splitRatios":"","panes":[{"path":"\\\\?\\F:\\wspulse\\t\\丁","view":"details","cols":"0,0,0","searchCols":"0,0,0,0","colLayout":false,"colWidths":""}]}]},
    {"left":100,"top":100,"right":1300,"bottom":900,"maximized":false,
     "sidebarCollapsed":0,"sidebarHidden":0,"sidebarOrder":"6,7,1,0,2,4,3,5","quickAccessHidden":0,"starredExpanded":true,
     "detailsPanel":0,"detailsPanelWidth":340,"detailsPreviewOnly":false,"detailsPreview":true,
     "windowName":"","windowColor":0,
     "activeTab":0,
     "tabGroups":[],
     "layoutTabs":[{"pinned":false,"group":0,"marker_rgb":0,"title":"","layout":1,"focused":1,"target":-1,"splitRatios":"5000","panes":[{"path":"\\\\?\\F:\\wspulse\\t\\甲 folder","view":"details","cols":"0,0,0","searchCols":"0,0,0,0","colLayout":false,"colWidths":""},{"path":"\\\\?\\F:\\wspulse\\t\\乙","view":"details","cols":"0,0,0","searchCols":"0,0,0,0","colLayout":false,"colWidths":""}]},{"pinned":false,"group":0,"marker_rgb":0,"title":"","layout":0,"focused":0,"target":-1,"splitRatios":"","panes":[{"path":"\\\\?\\F:\\wspulse\\t\\丙\\子 文件夹","view":"details","cols":"0,0,0","searchCols":"0,0,0,0","colLayout":false,"colWidths":""}]}]}
  ]
}
)json").toUtf8();
        QList<pathtext::PulseWindow> windows = pulseWindows(session);
        QCOMPARE(windows.size(), 2); // windows[] only, not the top level's copy of the first
        QCOMPARE(windows[0].left, 500);
        QCOMPARE(windows[0].top, 250);
        QCOMPARE(windows[0].right, 1500);
        QCOMPARE(windows[0].bottom, 1000);
        QCOMPARE(windows[0].frontName, u"丁"_s);
        QCOMPARE(windows[0].folders.size(), 1);
        QCOMPARE(windows[0].folders[0].path, u"F:\\wspulse\\t\\丁"_s); // without "\\?\"
        QCOMPARE(windows[0].folders[0].state, 1);
        QCOMPARE(windows[1].left, 100);
        QCOMPARE(windows[1].frontName, u"乙"_s); // the pane with the focus
        QCOMPARE(windows[1].folders.size(), 3);
        QCOMPARE(windows[1].folders[0].path, u"F:\\wspulse\\t\\乙"_s);
        QCOMPARE(windows[1].folders[0].state, 1);
        QCOMPARE(windows[1].folders[1].path, u"F:\\wspulse\\t\\甲 folder"_s); // the other pane
        QCOMPARE(windows[1].folders[1].state, 2);
        QCOMPARE(windows[1].folders[2].path, u"F:\\wspulse\\t\\丙\\子 文件夹"_s); // the tab behind
        QCOMPARE(windows[1].folders[2].state, 0);
        QCOMPARE(pathtext::pulseTitle(windows[0].frontName), u"丁 \u2014 Pulse"_s);

        // The second tab in front, showing a share; a drive and Pulse's own places behind.
        windows = pulseWindows(QByteArray(R"({"windows":[{"left":-8,"top":0,"right":800,"bottom":600,"activeTab":1,"layoutTabs":[
            {"focused":0,"panes":[{"path":"pulse:recycle"},{"path":"\\\\?\\D:\\"}]},
            {"focused":0,"panes":[{"path":"\\\\?\\UNC\\nas\\photos\\2024"}]},
            {"focused":0,"panes":[{"path":""}]}]}]})"));
        QCOMPARE(windows.size(), 1);
        QCOMPARE(windows[0].left, -8);
        QCOMPARE(windows[0].frontName, u"2024"_s);
        QCOMPARE(windows[0].folders.size(), 2);
        QCOMPARE(windows[0].folders[0].path, u"\\\\nas\\photos\\2024"_s);
        QCOMPARE(windows[0].folders[0].state, 1);
        QCOMPARE(windows[0].folders[1].path, u"D:\\"_s);
        QCOMPARE(windows[0].folders[1].state, 0);
        // A drive in front is named "D:"; This PC in front has no name to check.
        windows = pulseWindows(QByteArray(R"({"windows":[{"activeTab":0,"layoutTabs":[{"focused":0,"panes":[{"path":"\\\\?\\D:\\"}]}]},
            {"activeTab":0,"layoutTabs":[{"focused":0,"panes":[{"path":""}]}]}]})"));
        QCOMPARE(windows.size(), 2);
        QCOMPARE(windows[0].frontName, u"D:"_s);
        QVERIFY(windows[1].frontName.isEmpty());
        QVERIFY(windows[1].folders.isEmpty());

        // Version 7 and before: one window, at the top level; a pane out of range is none.
        windows = pulseWindows(QByteArray(R"({"version":7,"left":10,"top":20,"right":30,"bottom":40,"activeTab":0,
            "layoutTabs":[{"focused":5,"panes":[{"path":"C:\\Work"}]}]})"));
        QCOMPARE(windows.size(), 1);
        QCOMPARE(windows[0].top, 20);
        QVERIFY(windows[0].frontName.isEmpty()); // no pane has the focus
        QCOMPARE(windows[0].folders.size(), 1);
        QCOMPARE(windows[0].folders[0].path, u"C:\\Work"_s);
        QCOMPARE(windows[0].folders[0].state, 2);
        QVERIFY(pulseWindows("not json").isEmpty());
        QVERIFY(pulseWindows({}).isEmpty());
        QVERIFY(pulseWindows(R"({"version":8})").isEmpty());
    }

    // A command of the user's own for another file manager, with {path}.
    void customFileManagerCommand()
    {
        using pathtext::commandArgument;
        using pathtext::customCommand;
        QCOMPARE(commandArgument(u"C:\\Tools\\x.exe"_s), u"C:\\Tools\\x.exe"_s);
        QCOMPARE(commandArgument(u"C:\\"_s), u"C:\\"_s); // no quotes: nothing to escape
        QCOMPARE(commandArgument(u"C:\\Program Files"_s), u"\"C:\\Program Files\""_s);
        QCOMPARE(commandArgument(u"D:\\A B\\"_s), u"\"D:\\A B\\\\\""_s); // \" would be a quote
        QCOMPARE(commandArgument(u"F:\\wsfmtest\\子文件夹 甲"_s), u"\"F:\\wsfmtest\\子文件夹 甲\""_s);
        QCOMPARE(commandArgument({}), u"\"\""_s);

        pathtext::CustomCommand c = customCommand(u"\"C:\\Tools\\My Manager\\fm.exe\" --open {path}"_s, u"D:\\A B"_s);
        QCOMPARE(c.program, u"C:\\Tools\\My Manager\\fm.exe"_s);
        QCOMPARE(c.arguments, u"--open \"D:\\A B\""_s);
        // Quoted by the user: not twice. Twice in the line: both.
        c = customCommand(u"fm.exe \"{path}\" /x={path}"_s, u"D:\\A B"_s);
        QCOMPARE(c.program, u"fm.exe"_s);
        QCOMPARE(c.arguments, u"\"D:\\A B\" /x=\"D:\\A B\""_s);
        // No {path}: the path goes last.
        c = customCommand(u"  C:\\fm.exe  "_s, u"C:\\"_s);
        QCOMPARE(c.program, u"C:\\fm.exe"_s);
        QCOMPARE(c.arguments, u"C:\\"_s);
        c = customCommand(u"C:\\fm.exe /n"_s, u"C:\\x y"_s);
        QCOMPARE(c.arguments, u"/n \"C:\\x y\""_s);
        // %VARIABLES% in the line, not in the path.
        qputenv("WS_TEST_FM", "C:\\Tools");
        c = customCommand(u"\"%WS_TEST_FM%\\fm.exe\" {path}"_s, u"%TEMP%"_s);
        QCOMPARE(c.program, u"C:\\Tools\\fm.exe"_s);
        QCOMPARE(c.arguments, u"%TEMP%"_s);
        qunsetenv("WS_TEST_FM");
        QVERIFY(customCommand({}, u"C:\\"_s).program.isEmpty());
        QVERIFY(customCommand(u"\"unclosed"_s, u"C:\\"_s).program.isEmpty());
        QVERIFY(customCommand(u"\"\" {path}"_s, u"C:\\"_s).program.isEmpty());
    }

    void opusBlankClick()
    {
        using pathtext::opusBlankClickEvent;
        using pathtext::withOpusBlankClick;
        // As Opus 13 writes prefs.oxc; another <bg_commands> outside <dblclk> stays out of it.
        const QByteArray before = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<prefs version=\"6\">\n"
                                  "\t<other><bg_commands><event command=\"x\" enable=\"yes\" id=\"0\" /></bg_commands></other>\n"
                                  "\t<dblclk flags=\"0x405\">\n\t\t<layout />\n\t\t<text />\n"
                                  "\t\t<bg_commands single_middle_click=\"no\">\n"
                                  "\t\t\t<event command=\"Go UP\" enable=\"yes\" id=\"1\" />\n"
                                  "\t\t\t<event command=\"Go UP BACK\" enable=\"no\" id=\"0\" />\n"
                                  "\t\t</bg_commands>\n\t\t<mouse_hook_delay>0</mouse_hook_delay>\n"
                                  "\t</dblclk>\n\t<名字>文档</名字>\n</prefs>\n";
        pathtext::OpusBlankClick click = pathtext::opusBlankClick(before);
        QVERIFY(click.hasPlace);
        QCOMPARE(click.element, "<event command=\"Go UP BACK\" enable=\"no\" id=\"0\" />");
        QCOMPARE(click.command, u"Go UP BACK"_s);
        QVERIFY(!click.enabled);

        // Quotes, & and > in the command; only the element changes.
        const QString ours = uR"("C:\A & B\WinShunSearch.exe" --folder-menu "{sourcepath$|noterm}" >x)"_s;
        const QByteArray event = opusBlankClickEvent(ours);
        QCOMPARE(event, R"(<event command="&quot;C:\A &amp; B\WinShunSearch.exe&quot; --folder-menu &quot;{sourcepath$|noterm}&quot; &gt;x" enable="yes" id="0" />)");
        const QByteArray set = withOpusBlankClick(before, event);
        QCOMPARE(QByteArray(set).replace(event, click.element), before);
        click = pathtext::opusBlankClick(set);
        QCOMPARE(click.command, ours);
        QVERIFY(click.enabled);
        QCOMPARE(withOpusBlankClick(set, "<event command=\"Go UP BACK\" enable=\"no\" id=\"0\" />"), before);
        // Taken away with its line; a later read finds none.
        const QByteArray removed = withOpusBlankClick(set, {});
        QCOMPARE(QByteArray(removed).replace("\t\t\t<event command=\"Go UP\" enable=\"yes\" id=\"1\" />\n",
                     "\t\t\t<event command=\"Go UP\" enable=\"yes\" id=\"1\" />\n\t\t\t" + click.element + '\n'),
            set);
        QVERIFY(pathtext::opusBlankClick(removed).element.isEmpty());
        QVERIFY(pathtext::opusBlankClick(removed).hasPlace);

        // No <bg_commands> yet: one before <mouse_hook_delay>; a <bg_commands/> opened up.
        const QByteArray bare = "<prefs><dblclk flags=\"0x405\"><text /><mouse_hook_delay>0</mouse_hook_delay></dblclk></prefs>";
        QCOMPARE(withOpusBlankClick(bare, event),
            "<prefs><dblclk flags=\"0x405\"><text /><bg_commands>" + event
                + "</bg_commands><mouse_hook_delay>0</mouse_hook_delay></dblclk></prefs>");
        QCOMPARE(pathtext::opusBlankClick(withOpusBlankClick(bare, event)).command, ours);
        QCOMPARE(withOpusBlankClick("<prefs><dblclk><bg_commands single_middle_click=\"no\"/></dblclk></prefs>", event),
            "<prefs><dblclk><bg_commands single_middle_click=\"no\">" + event + "</bg_commands></dblclk></prefs>");
        QCOMPARE(withOpusBlankClick("<prefs><dblclk></dblclk></prefs>", event),
            "<prefs><dblclk><bg_commands>" + event + "</bg_commands></dblclk></prefs>");
        QCOMPARE(withOpusBlankClick(bare, {}), bare); // nothing to take away

        // No place for it: no <dblclk>, an empty one, not XML.
        QVERIFY(withOpusBlankClick("<prefs><dblclkx></dblclkx></prefs>", event).isEmpty());
        QVERIFY(withOpusBlankClick("<prefs><dblclk /></prefs>", event).isEmpty());
        QVERIFY(withOpusBlankClick("<prefs><dblclk>", event).isEmpty());
        QVERIFY(!pathtext::opusBlankClick("<prefs/>").hasPlace);
        QVERIFY(withOpusBlankClick({}, event).isEmpty());
    }

    void splitTyped()
    {
        const auto split = [](const QString& text) {
            const pathtext::TypedPath typed = pathtext::splitTyped(text);
            return QStringList {typed.folder, typed.name};
        };
        QCOMPARE(split(u"D:\\Pro"_s), (QStringList {u"D:\\"_s, u"Pro"_s}));
        QCOMPARE(split(u"D:\\Projects\\"_s), (QStringList {u"D:\\Projects"_s, QString()}));
        QCOMPARE(split(u"D:/work/docs"_s), (QStringList {u"D:\\work"_s, u"docs"_s}));
        QCOMPARE(split(u"d:"_s), (QStringList {u"d:\\"_s, QString()}));
        QCOMPARE(split(u"\"C:\\Program Files\\Wi\""_s), (QStringList {u"C:\\Program Files"_s, u"Wi"_s}));
        QCOMPARE(split(u"\\\\server\\share\\"_s), (QStringList {u"\\\\server\\share"_s, QString()}));
        QCOMPARE(split(u"%SystemRoot%\\Sys"_s), (QStringList {qEnvironmentVariable("SystemRoot"), u"Sys"_s}));
        QVERIFY(pathtext::splitTyped(u"\\\\server\\sha"_s).folder.isEmpty()); // no share yet
        QVERIFY(pathtext::splitTyped(u"D:\\*.pdf"_s).folder.isEmpty()); // for the search
        QVERIFY(pathtext::splitTyped(u"report"_s).folder.isEmpty());
        QVERIFY(pathtext::splitTyped(u"docs\\report"_s).folder.isEmpty()); // a folder name in a search
    }

    void splitRelative()
    {
        const auto names = [](const QString& text) { return pathtext::splitRelative(text).names; };
        QCOMPARE(names(u"tools/screenshots/work/out/covers/"_s),
            (QStringList {u"tools"_s, u"screenshots"_s, u"work"_s, u"out"_s, u"covers"_s}));
        QVERIFY(pathtext::splitRelative(u"tools/out/"_s).folder);
        QCOMPARE(names(u" src\\app\\App.cpp "_s), (QStringList {u"src"_s, u"app"_s, u"App.cpp"_s}));
        QVERIFY(!pathtext::splitRelative(u"src\\app\\App.cpp"_s).folder);
        QCOMPARE(names(u"/Tencent Files//x\\"_s), (QStringList {u"Tencent Files"_s, u"x"_s}));
        QCOMPARE(names(u"covers/"_s), QStringList {u"covers"_s});
        QVERIFY(names(u"covers"_s).isEmpty()); // a name, not a path
        QVERIFY(names(u"D:\\work\\docs"_s).isEmpty()); // splitTyped's
        QVERIFY(names(u"\\\\server\\share"_s).isEmpty());
        QVERIFY(names(u"src/*.cpp"_s).isEmpty());
        QVERIFY(names(u"!node_modules\\"_s).isEmpty());
        QVERIFY(names(u"covers/ png"_s).isEmpty()); // two terms
        QVERIFY(names(u"../out"_s).isEmpty());
        QVERIFY(names(u"/"_s).isEmpty());
    }

    void listTyped()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = QDir::toNativeSeparators(tmp.path());
        for (const QString& name : {u"Alpha"_s, u"alpha10"_s, u"alpha2"_s, u"Beta"_s, u"项目资料"_s, u"secret"_s})
            QVERIFY(QDir(root).mkdir(name));
        for (const QString& name : {u"alpha.txt"_s, u"notes.md"_s}) {
            QFile file(root + u'\\' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(::SetFileAttributesW(reinterpret_cast<const wchar_t*>((root + u"\\secret"_s).utf16()), FILE_ATTRIBUTE_HIDDEN));
        const auto names = [](const SearchResults& rows) {
            QStringList out;
            for (const SearchResult& r : rows)
                out.append(r.name + (r.isDir ? u"\\"_s : QString()));
            return out;
        };
        const QString self = QDir(root).dirName() + u'\\';

        // Nothing typed: the folder itself, its folders, its files; no hidden ones.
        const SearchResults all = pathtext::listTyped({root, {}}, false, 50);
        QCOMPARE(names(all).first(5), (QStringList {self, u"Alpha\\"_s, u"alpha2\\"_s, u"alpha10\\"_s, u"Beta\\"_s}));
        QCOMPARE(names(all).last(2), (QStringList {u"alpha.txt"_s, u"notes.md"_s}));
        QCOMPARE(all.size(), 8);
        QCOMPARE(all[0].path, root);
        QCOMPARE(all[1].path, root + u"\\Alpha"_s);

        QCOMPARE(names(pathtext::listTyped({root, u"alp"_s}, false, 50)),
            (QStringList {u"Alpha\\"_s, u"alpha2\\"_s, u"alpha10\\"_s, u"alpha.txt"_s}));
        QCOMPARE(names(pathtext::listTyped({root, u"alp"_s}, true, 50)),
            (QStringList {u"Alpha\\"_s, u"alpha2\\"_s, u"alpha10\\"_s}));
        QCOMPARE(names(pathtext::listTyped({root, u"xm"_s}, false, 50)), (QStringList {u"项目资料\\"_s})); // pinyin
        QCOMPARE(names(pathtext::listTyped({root, {}}, false, 2)), (QStringList {self, u"Alpha\\"_s}));
        QVERIFY(pathtext::listTyped({root + u"\\nothing here"_s, {}}, false, 50).isEmpty());
    }

    void webKeyword()
    {
        const WebShortcuts shortcuts {
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}},
            {u"blog"_s, u"Blog"_s, u"https://example.com/blog"_s, {}}, // a page: takes no words
        };
        const auto typed = [&](const QString& text) {
            const auto t = typedKeyword(shortcuts, text);
            return t ? QStringList {QString::number(t->index), t->words} : QStringList();
        };
        QCOMPARE(typed(u"gh"_s), (QStringList {u"0"_s, QString()}));
        QCOMPARE(typed(u"  GH  "_s), (QStringList {u"0"_s, QString()})); // any case, spaces around
        QCOMPARE(typed(u"gh  Win Shun "_s), (QStringList {u"0"_s, u"Win Shun"_s}));
        QCOMPARE(typed(u"gh \"exact words\""_s), (QStringList {u"0"_s, u"\"exact words\""_s})); // as typed
        QCOMPARE(typed(u"blog"_s), (QStringList {u"1"_s, QString()}));
        QVERIFY(typed(u"blog notes"_s).isEmpty()); // an ordinary query then
        QVERIFY(typed(u"ghost"_s).isEmpty());
        QVERIFY(typed(u"g"_s).isEmpty());
        QVERIFY(typed(u"report gh"_s).isEmpty());
        QVERIFY(typed({}).isEmpty());
        QVERIFY(!typedKeyword({}, u"gh"_s));
    }

    void webAddresses()
    {
        const WebShortcut github {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s&type=code"_s, {}};
        QVERIFY(github.searches());
        QCOMPARE(github.searchUrl(u"Win Shun/中文&x"_s),
            u"https://github.com/search?q=Win%20Shun%2F%E4%B8%AD%E6%96%87%26x&type=code"_s);
        QCOMPARE(github.homeUrl(), u"https://github.com/"_s); // its site
        const WebShortcut bilibili {u"bili"_s, u"哔哩哔哩"_s, u"https://search.bilibili.com/all?keyword=%s"_s,
            u"https://www.bilibili.com/"_s};
        QCOMPARE(bilibili.homeUrl(), u"https://www.bilibili.com/"_s);
        const WebShortcut page {u"blog"_s, u"Blog"_s, u"https://example.com/blog"_s, {}};
        QVERIFY(!page.searches());
        QCOMPARE(page.homeUrl(), u"https://example.com/blog"_s);
        const WebShortcut app {u"ob"_s, u"Obsidian"_s, u"obsidian://search?query=%s"_s, {}};
        QCOMPARE(app.homeUrl(), u"obsidian://search?query="_s); // an app's link, with no words

        QCOMPARE(normalizeWebUrl(u"  github.com/search?q=%s "_s), u"https://github.com/search?q=%s"_s);
        // Searched for "%s" itself, then copied from the address bar.
        QCOMPARE(normalizeWebUrl(u"https://www.baidu.com/s?wd=%25s&rsv_spt=1"_s), u"https://www.baidu.com/s?wd=%s&rsv_spt=1"_s);
        QCOMPARE(normalizeWebUrl(u"obsidian://search?query=%s"_s), u"obsidian://search?query=%s"_s);
        QCOMPARE(normalizeWebUrl(u"HTTP://Example.com"_s), u"HTTP://Example.com"_s);
        QCOMPARE(normalizeWebUrl(u"wiki.example.com:8080/find?q=%s"_s), u"https://wiki.example.com:8080/find?q=%s"_s);
        QVERIFY(normalizeWebUrl(u"file:///C:/notes/%s"_s).isEmpty()); // could start a program
        QVERIFY(normalizeWebUrl(u"C:\\Windows\\%s"_s).isEmpty());
        QVERIFY(normalizeWebUrl(u"https://"_s).isEmpty());
        QVERIFY(normalizeWebUrl(u"   "_s).isEmpty());

        QCOMPARE(displayWebUrl(u"https://www.baidu.com/s?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5"_s),
            u"baidu.com/s?wd=天气 预报"_s);
        QCOMPARE(displayWebUrl(u"https://github.com/"_s), u"github.com"_s);
        QCOMPARE(displayWebUrl(u"https://github.com/search?q=%s&type=code"_s), u"github.com/search?q=%s&type=code"_s);
        QCOMPARE(displayWebUrl(u"obsidian://search?query=a%20b"_s), u"obsidian://search?query=a b"_s);

        QVERIFY(isValidWebKeyword(u"gh"_s));
        QVERIFY(isValidWebKeyword(u"知乎"_s));
        QVERIFY(!isValidWebKeyword(u"g h"_s));
        QVERIFY(!isValidWebKeyword(u"g\"h"_s));
        QVERIFY(!isValidWebKeyword(QString()));

        QVERIFY(isWebPath(webPath(u"gh"_s, u"WinShun"_s)));
        QVERIFY(webPath(u"gh"_s) != webPath(u"gh"_s, u"WinShun"_s)); // rows of their own
        QVERIFY(!isWebPath(u"C:\\gh"_s));
    }

    void webByName()
    {
        const WebShortcuts shortcuts {
            {u"bd"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, {}},
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}},
            {u"zh"_s, u"知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, {}},
        };
        const auto found = [&](const QString& text) {
            QStringList names;
            for (const WebHit& hit : searchWebShortcuts(shortcuts, NameMatcher(ws::parseQuery(text))))
                names.append(shortcuts[hit.index].name);
            return names;
        };
        QCOMPARE(found(u"github"_s), (QStringList {u"GitHub"_s}));
        QCOMPARE(found(u"baidu"_s), (QStringList {u"百度"_s})); // pinyin
        QCOMPARE(found(u"bd"_s), (QStringList {u"百度"_s})); // initials
        QVERIFY(found(u"!github"_s).isEmpty()); // exclusions alone list nothing
        QVERIFY(found(u"report"_s).isEmpty());

    }

    void webDefault()
    {
        // Ten sites most used on the Chinese web, and "winshun" for the repository.
        const WebShortcuts shortcuts = defaultWebShortcuts();
        QCOMPARE(shortcuts.size(), 11);
        QStringList keywords;
        QStringList sites;
        for (const WebShortcut& s : shortcuts) {
            QCOMPARE(normalizeWebUrl(s.url), s.url); // as the settings window would keep them
            QVERIFY(isValidWebKeyword(s.keyword));
            // Whole words: two letters typed alone would put a site above the files.
            QVERIFY2(s.keyword.size() >= 3, qPrintable(s.keyword));
            // Not the winshun.web plugin's: its keyword mode would win.
            QVERIFY(!QStringList({u"bd"_s, u"gh"_s, u"yt"_s, u"wiki"_s, u"bing"_s, u"bili"_s}).contains(s.keyword));
            QVERIFY(!keywords.contains(s.keyword));
            keywords.append(s.keyword);
            if (s.searches()) {
                QVERIFY2(!s.home.isEmpty(), qPrintable(s.keyword)); // the site, for the keyword alone and its icon
                QVERIFY(!sites.contains(siteIconKey(s.homeUrl()))); // an icon each
                sites.append(siteIconKey(s.homeUrl()));
            }
        }
        QCOMPARE(sites.size(), 10);
        // Suggestions where the site answers a plain request (checked live 2026-10-15), none elsewhere.
        QStringList suggesting;
        for (const WebShortcut& s : shortcuts) {
            QCOMPARE(s.suggest, knownSuggestUrl(s.url));
            if (!s.suggest.isEmpty()) {
                QCOMPARE(normalizeSuggestUrl(s.suggest), s.suggest);
                suggesting.append(s.keyword);
            }
        }
        QCOMPARE(suggesting, (QStringList {u"baidu"_s, u"biying"_s, u"bilibili"_s, u"taobao"_s}));
        // The OCR's 网页搜索: the first of the list that searches.
        QCOMPARE(defaultWebSearchUrl(shortcuts, u" 天气 预报 "_s), u"https://www.baidu.com/s?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5"_s);
        QVERIFY(defaultWebSearchUrl(shortcuts, u"  "_s).isEmpty());
        {
            WebShortcuts own = shortcuts;
            std::rotate(own.begin(), own.begin() + 1, own.end()); // 必应 on top
            QCOMPARE(defaultWebSearchUrl(own, u"a&b"_s), u"https://www.bing.com/search?q=a%26b"_s);
            WebShortcuts pages {{u"page"_s, u"A page"_s, u"https://example.com/"_s, {}}, own[1]};
            QCOMPARE(defaultWebSearchUrl(pages, u"x"_s), pages[1].searchUrl(u"x"_s)); // a page does not search: the next one does
            QCOMPARE(defaultWebSearchUrl({}, u"x y"_s), u"https://www.bing.com/search?q=x%20y"_s); // none: Bing's
        }
        const auto typed = typedKeyword(shortcuts, u"baidu 天气 预报"_s);
        QVERIFY(typed);
        QCOMPARE(shortcuts[typed->index].searchUrl(typed->words), u"https://www.baidu.com/s?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5"_s);
        QCOMPARE(shortcuts[typed->index].suggestUrl(typed->words),
            u"https://suggestion.baidu.com/su?wd=%E5%A4%A9%E6%B0%94%20%E9%A2%84%E6%8A%A5&action=opensearch"_s);
        QVERIFY(!typedKeyword(shortcuts, u"bd"_s)); // found by its name instead (pinyin initials)
        const auto byName = searchWebShortcuts(shortcuts, NameMatcher(ws::parseQuery(u"jd"_s)));
        QVERIFY(!byName.empty());
        QCOMPARE(shortcuts[byName.front().index].name, u"京东"_s);

        const auto winshun = typedKeyword(shortcuts, u"WinSHUN"_s);
        QVERIFY(winshun);
        QCOMPARE(shortcuts[winshun->index].homeUrl(), u"https://github.com/LingCore/WinShun"_s);
        QVERIFY(winshun->words.isEmpty());
        QVERIFY(!typedKeyword(shortcuts, u"winshun 设置"_s)); // a page: an ordinary query then
    }

    void webNewDefaults()
    {
        const WebShortcuts defaults = defaultWebShortcuts();
        // Written by 0.6.0: "winshun" alone. The ten come after it.
        const WebShortcuts old {defaults.last()};
        WebShortcuts merged = withNewDefaults(old, defaults);
        QCOMPARE(merged.size(), 11);
        QCOMPARE(merged.first().keyword, u"winshun"_s);
        // The user's own: a keyword taken (theirs stays), a site already there
        // under another keyword ("bd" for 百度), nothing twice.
        const WebShortcuts own {
            {u"zhihu"_s, u"我的知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, {}},
            {u"bd"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, u"https://www.baidu.com/"_s},
        };
        merged = withNewDefaults(own, defaults);
        QCOMPARE(merged.size(), 2 + 11 - 2);
        QCOMPARE(merged[0].name, u"我的知乎"_s);
        QCOMPARE(std::count_if(merged.begin(), merged.end(), [](const WebShortcut& s) { return s.name == u"百度"; }),
            std::ptrdiff_t(1));
        QCOMPARE(withNewDefaults(merged, defaults), merged); // once is all
    }

    void webFile()
    {
        QVERIFY(isValidCustomIconName(u"0123456789abcdef.png"));
        QVERIFY(!isValidCustomIconName(u"0123456789ABCDEF.png")); // as Win顺 names them: lower case
        QVERIFY(!isValidCustomIconName(u"..\\..\\evil1234.png"));
        QVERIFY(!isValidCustomIconName(u"C:\\icons\\gh.png"));
        QVERIFY(!isValidCustomIconName(u""));

        WebShortcuts shortcuts {
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}, u"0123456789abcdef.png"_s,
                u"https://example.com/sug?q=%s"_s},
            {u"zhihu"_s, u"知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, u"https://www.zhihu.com/"_s, {}},
            {u"gone"_s, u"图标没了"_s, u"https://example.com/"_s, {}, u"fedcba9876543210.png"_s},
        };
        const QByteArray png("\x89PNG fake", 9);
        const QByteArray json = writeWebShortcutsFile(shortcuts, [&](const QString& icon) {
            return icon == u"0123456789abcdef.png" ? png : QByteArray(); // the other's file is gone
        });
        const std::optional<WebShortcutFile> read = readWebShortcutsFile(json);
        QVERIFY(read);
        QCOMPARE(read->skipped, 0);
        QCOMPARE(read->entries.size(), 3);
        QCOMPARE(read->entries[0].iconData, png);
        QVERIFY(read->entries[0].shortcut.icon.isEmpty()); // a name of this computer's: the picture travels instead
        QCOMPARE(read->entries[1].shortcut.home, u"https://www.zhihu.com/"_s);
        QCOMPARE(read->entries[1].shortcut.name, u"知乎"_s);
        QVERIFY(read->entries[2].iconData.isEmpty());
        QCOMPARE(read->entries[0].shortcut.suggest, u"https://example.com/sug?q=%s"_s);
        QVERIFY(read->entries[1].shortcut.suggest.isEmpty()); // written empty: none, not the one known for the site

        // Tidied as the settings file is; the unusable skipped; the later of two keywords wins.
        // Not a raw string: moc takes the // of its addresses for a comment and loses the class (pitfalls).
        const QByteArray hand = "{\"format\": \"winshun-web-shortcuts\", \"version\": 1, \"shortcuts\": ["
                                "{\"keyword\": \"bd\", \"url\": \"baidu.com/s?wd=%s\"},"
                                "{\"keyword\": \"two words\", \"url\": \"https://example.com/\"},"
                                "{\"keyword\": \"x\", \"url\": \"file:///C:/Windows/\"},"
                                "{\"keyword\": \"BD\", \"name\": \"百度\", \"url\": \"https://www.baidu.com/s?wd=%s\","
                                " \"icon\": \"data:image/png;base64,iVBORw==\"},"
                                "{\"keyword\": \"y\", \"url\": \"https://y.example/\", \"icon\": \"C:/Windows/a.png\"}"
                                "]}";
        const std::optional<WebShortcutFile> tidied = readWebShortcutsFile(hand);
        QVERIFY(tidied);
        QCOMPARE(tidied->skipped, 2);
        QCOMPARE(tidied->entries.size(), 2);
        QCOMPARE(tidied->entries[0].shortcut.keyword, u"BD"_s);
        QCOMPARE(tidied->entries[0].shortcut.name, u"百度"_s);
        QCOMPARE(tidied->entries[0].iconData, QByteArray::fromBase64("iVBORw=="));
        QVERIFY(tidied->entries[1].iconData.isEmpty()); // a path is no picture
        QCOMPARE(tidied->entries[1].shortcut.name, u"y"_s);
        // A file from before suggestions: the one known for the site, if any.
        QCOMPARE(tidied->entries[0].shortcut.suggest, knownSuggestUrl(u"https://www.baidu.com/s?wd=%s"_s));
        QVERIFY(!tidied->entries[0].shortcut.suggest.isEmpty());
        QVERIFY(tidied->entries[1].shortcut.suggest.isEmpty());

        bool newer = false;
        QVERIFY(!readWebShortcutsFile("{}", &newer));
        QVERIFY(!newer);
        QVERIFY(!readWebShortcutsFile("not json"));
        QVERIFY(!readWebShortcutsFile("{\"format\": \"winshun-web-shortcuts\", \"version\": 2, \"shortcuts\": []}", &newer));
        QVERIFY(newer);
    }

    void webMerge()
    {
        WebShortcuts own {
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}, {}},
            {u"zhihu"_s, u"知乎"_s, u"https://www.zhihu.com/search?q=%s"_s, {}, {}},
        };
        const WebShortcuts file {
            {u"ZHIHU"_s, u"知乎"_s, u"https://www.zhihu.com/search?type=content&q=%s"_s, {}, {}},
            {u"gh"_s, u"GitHub"_s, u"https://github.com/search?q=%s"_s, {}, {}},
            {u"bd"_s, u"百度"_s, u"https://www.baidu.com/s?wd=%s"_s, {}, {}},
        };
        const WebMerge merge = mergeWebShortcuts(own, file);
        QCOMPARE(merge.added, 1);
        QCOMPARE(merge.updated, 1);
        QCOMPARE(merge.unchanged, 1);
        QCOMPARE(own.size(), 3);
        QCOMPARE(own[1].keyword, u"ZHIHU"_s); // in its place
        QCOMPARE(own[2].keyword, u"bd"_s);
        const WebShortcuts again = own;
        QCOMPARE(mergeWebShortcuts(own, file).unchanged, 3);
        QCOMPARE(own, again);
    }

    void siteIcons()
    {
        QCOMPARE(siteIconKey(u"https://Fanyi.Baidu.com/"_s), u"fanyi.baidu.com"_s);
        QCOMPARE(siteIconKey(u"http://example.com:8080/x"_s), u"example.com"_s);
        QVERIFY(siteIconKey(u"obsidian://search?query="_s).isEmpty());

        // As www.bilibili.com and www.zhihu.com have them (2026-10), and the
        // kinds left out: one colour, other relations, attributes alike.
        // (Not a raw string literal: moc reads this file and stumbles on one.)
        const QByteArray html = "<html><head>\n"
                                "<link rel=\"stylesheet\" href=\"/a.css\">\n"
                                "<link rel=\"shortcut icon\" href=\"//i0.hdslb.com/images/favicon.ico\" />\n"
                                "<link rel=\"mask-icon\" href=\"/pinned.svg\" color=\"#000\">\n"
                                "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/favicon.svg\">\n"
                                "<LINK REL='apple-touch-icon' HREF='https://static.zhihu.com/touch-120.png' sizes='120x120'>\n"
                                "<link data-rh=\"true\" rel=\"apple-touch-icon\" href=\"https://static.zhihu.com/touch-152.png\" sizes=\"152x152\"/>\n"
                                "<link rel=\"icon\" sizes=\"16x16 196x196\" href=\"icons/big.png?v=1&amp;x=2\">\n"
                                "<link rel=\"apple-touch-icon-precomposed\" href=/touch.png>\n"
                                "<link rel=\"icon\" href=\"data:image/png;base64,AAAA\">\n"
                                "<link rel=icon mce_href=\"/wrong.ico\" href=\"/right.ico\">\n"
                                "</head></html>";
        const SiteIconCandidates found = siteIconCandidates(html, u"https://www.example.com/start/page"_s);
        QCOMPARE(found.icons,
            (QStringList {
                u"https://www.example.com/start/icons/big.png?v=1&x=2"_s, // declared large
                u"https://www.example.com/favicon.svg"_s, // SVG: any size, after the large ones (not mask-icon's)
                u"https://i0.hdslb.com/images/favicon.ico"_s, // the other icons, as the page has them
                u"https://www.example.com/right.ico"_s,
                u"https://www.example.com/favicon.ico"_s, // the site's, last
            }));
        QCOMPARE(found.touchIcons,
            (QStringList {
                u"https://www.example.com/touch.png"_s, // size unknown: taken as 180
                u"https://static.zhihu.com/touch-152.png"_s,
                u"https://static.zhihu.com/touch-120.png"_s,
            }));
        // No page (it could not be read): the site's favicon alone.
        QCOMPARE(siteIconCandidates({}, u"https://weibo.com/"_s).icons, (QStringList {u"https://weibo.com/favicon.ico"_s}));
        QVERIFY(siteIconCandidates({}, u"https://weibo.com/"_s).touchIcons.isEmpty());
        QVERIFY(siteIconCandidates({}, u"obsidian://x"_s).icons.isEmpty());
    }

    void webSuggestAddresses()
    {
        QCOMPARE(normalizeSuggestUrl(u" api.bing.com/osjson.aspx?query=%s "_s), u"https://api.bing.com/osjson.aspx?query=%s"_s);
        QCOMPARE(normalizeSuggestUrl(u"https://example.com/sug?q={query}"_s), u"https://example.com/sug?q=%s"_s); // Listary's
        QCOMPARE(normalizeSuggestUrl(u"https://example.com/sug?q=%25s"_s), u"https://example.com/sug?q=%s"_s);
        QVERIFY(normalizeSuggestUrl(u"https://example.com/sug"_s).isEmpty()); // no words: nothing to suggest for
        QVERIFY(normalizeSuggestUrl(u"obsidian://search?query=%s"_s).isEmpty()); // read by Win顺: http(s) only
        QVERIFY(normalizeSuggestUrl(u"file:///C:/x?%s"_s).isEmpty());
        QVERIFY(normalizeSuggestUrl({}).isEmpty());

        QCOMPARE(knownSuggestUrl(u"https://cn.bing.com/search?q=%s"_s), u"https://api.bing.com/osjson.aspx?query=%s"_s);
        QVERIFY(knownSuggestUrl(u"https://fanyi.baidu.com/x?q=%s"_s).isEmpty()); // not 百度's search
        QVERIFY(knownSuggestUrl(u"https://www.zhihu.com/search?q=%s"_s).isEmpty()); // 403 without signing in

        WebShortcut page {u"blog"_s, u"Blog"_s, u"https://example.com/blog"_s, {}, {}, u"https://example.com/s?q=%s"_s};
        QVERIFY(page.suggestUrl(u"x"_s).isEmpty()); // a page takes no words
        const WebShortcut site {u"ex"_s, u"Ex"_s, u"https://example.com/?q=%s"_s, {}, {}, u"https://example.com/s?q=%s&n=5"_s};
        QCOMPARE(site.suggestUrl(u"a b&c"_s), u"https://example.com/s?q=a%20b%26c&n=5"_s);
    }

    void webSuggestReplies()
    {
        using ws::websuggest::decodeReply;
        using ws::websuggest::parse;
        // 百度's JSONP, in GBK: unquoted keys, as its su?wd= sends it (显卡, 显卡天梯图).
        const QByteArray gbk = "window.baidu.sug({q:\"\xCF\xD4\xBF\xA8\",p:false,s:[\"\xCF\xD4\xBF\xA8\xCC\xEC\xCC\xDD\xCD\xBC\","
                               "\"\xCF\xD4\xBF\xA8\"]});";
        const QString text = decodeReply(gbk, u"text/javascript; charset=gbk");
        QVERIFY(text.contains(u"显卡天梯图"_s));
        // The word typed is left out: the row above searches it.
        QCOMPARE(parse(text, u"显卡"_s), (QStringList {u"显卡天梯图"_s}));
        // No charset named, not UTF-8: GB18030 all the same.
        QCOMPARE(decodeReply(QByteArray("\xCF\xD4\xBF\xA8"), {}), u"显卡"_s);
        QCOMPARE(decodeReply(QByteArray("\xE6\x98\xBE\xE5\x8D\xA1"), {}), u"显卡"_s); // UTF-8 stays UTF-8
        QCOMPARE(decodeReply(QByteArray("\xEF\xBB\xBF[1]"), u"text/plain; charset=gbk"), u"[1]"_s); // a BOM wins

        // OpenSearch (必应, Google: extra members after the list), and the cap.
        QCOMPARE(parse(u"[\"gpu\",[\"gpu z\",\"GPU Z\",\"gpu\",\"gpu test\",\" gpu  tweak \",\"e\",\"f\",\"g\"],[],{\"x\":[[512]]}]", u"gpu"_s),
            (QStringList {u"gpu z"_s, u"gpu test"_s, u"gpu tweak"_s, u"e"_s, u"f"_s}));
        QCOMPARE(parse(u"[\"x\",[\"a\",\"b\"]]", u"x"_s, 1), (QStringList {u"a"_s}));
        // JSONP around OpenSearch, a callback with dots, and a trailing semicolon.
        QCOMPARE(parse(u"jQuery.cb_1( [\"q\", [\"a\"]] );", u"q"_s), (QStringList {u"a"_s}));
        // 淘宝: arrays of [word, count].
        QCOMPARE(parse(u"{\"result\":[[\"显卡支架\",\"100\"],[\"显卡二手\",\"100\"]]}", u"显卡"_s),
            (QStringList {u"显卡支架"_s, u"显卡二手"_s}));
        // 哔哩哔哩: objects, "value" plain (its "name" carries markup).
        QCOMPARE(parse(u"{\"code\":0,\"result\":{\"tag\":[{\"value\":\"显卡推荐\",\"name\":\"<em class=\\\"h\\\">显卡</em>推荐\"}]}}", u"显卡"_s),
            (QStringList {u"显卡推荐"_s}));
        // 百度's sugrec: objects under g, the word under q.
        QCOMPARE(parse(u"{\"q\":\"显卡\",\"p\":false,\"g\":[{\"type\":\"sug\",\"q\":\"显卡价格\"}]}", u"显卡"_s),
            (QStringList {u"显卡价格"_s}));
        // JavaScript's 'strings', with a quote of the other kind inside, and an escaped one.
        QCOMPARE(parse(u"cb({s:['say \"hi\"', 'it\\'s']})", u""_s), (QStringList {u"say \"hi\""_s, u"it's"_s}));
        // A key-like word inside a string is left alone.
        QCOMPARE(parse(u"cb({s:[\"a, b: c\"]})", u""_s), (QStringList {u"a, b: c"_s}));

        // Anything else: nothing, quietly.
        QVERIFY(parse(u"<!DOCTYPE html><html>Sina Visitor System</html>", u"x"_s).isEmpty());
        QVERIFY(parse(u"", u"x"_s).isEmpty());
        QVERIFY(parse(u"{\"code\":403,\"message\":\"forbidden\"}", u"x"_s).isEmpty());
        QVERIFY(parse(u"[1, 2, 3]", u"x"_s).isEmpty());
        QVERIFY(parse(u"alert(1)", u"x"_s).isEmpty());
        QVERIFY(parse(u"x = (function(){})()", u"x"_s).isEmpty());
    }

    void webSuggestCache()
    {
        using ws::websuggest::Cache;
        Cache cache;
        const QString bing = u"https://api.bing.com/osjson.aspx?query=%s"_s;
        const QString mine = u"https://example.com/sug?q=%s"_s;
        cache.insert(bing, u"显卡"_s, {u"显卡驱动"_s}, 1000);
        QCOMPARE(cache.find(bing, u"显卡"_s, 1000 + Cache::kKeepMs), (QStringList {u"显卡驱动"_s}));
        QVERIFY(!cache.find(bing, u"显卡"_s, 1001 + Cache::kKeepMs)); // gone stale
        QVERIFY(!cache.find(bing, u"显"_s, 1000)); // other words
        // The address changed in the settings (the Lertaro bug): the same words
        // from the new address are not the old answer.
        QVERIFY(!cache.find(mine, u"显卡"_s, 1000));
        cache.insert(mine, u"显卡"_s, {}, 1000); // "no suggestions" is an answer too
        QCOMPARE(cache.find(mine, u"显卡"_s, 1000), QStringList());
        QCOMPARE(cache.find(bing, u"显卡"_s, 1000), (QStringList {u"显卡驱动"_s}));
        cache.clear();
        QVERIFY(!cache.find(bing, u"显卡"_s, 1000));
        QCOMPARE(cache.size(), 0);
        // Full: what went stale goes, then the oldest.
        for (int i = 0; i < Cache::kMaxEntries + 10; ++i)
            cache.insert(bing, QString::number(i), {}, i);
        QVERIFY(cache.size() <= Cache::kMaxEntries);
        QVERIFY(!cache.find(bing, u"0"_s, 300));
        QVERIFY(cache.find(bing, QString::number(Cache::kMaxEntries + 9), Cache::kMaxEntries + 9));
    }

    void releaseVersions()
    {
        QVERIFY(release::isNewer(u"0.2.0"_s, u"0.1.0"_s));
        QVERIFY(release::isNewer(u"v0.10.0"_s, u"0.9.9"_s)); // numbers, not text
        QVERIFY(release::isNewer(u"1.0"_s, u"0.9.9"_s));
        QVERIFY(!release::isNewer(u"0.2"_s, u"0.2.0"_s));
        QVERIFY(!release::isNewer(u"0.2.0-beta"_s, u"0.2.0"_s)); // the suffix is not looked at
        QVERIFY(!release::isNewer(u"0.1.9"_s, u"0.2.0"_s));
        QCOMPARE(release::normalized(u" V1.2.3 "_s), u"1.2.3"_s);
    }

    void releaseParse()
    {
        // Not raw string literals: moc cannot parse them.
        const auto info = release::parse("{\"tag_name\": \"v0.2.1\", "
                                         "\"html_url\": \"https://github.com/x/y/releases/tag/v0.2.1\", "
                                         "\"body\": \"notes\", \"assets\": []}");
        QVERIFY(info);
        QCOMPARE(info->version, u"0.2.1"_s);
        QCOMPARE(info->pageUrl, u"https://github.com/x/y/releases/tag/v0.2.1"_s);
        QCOMPARE(info->notes, u"notes"_s);
        QVERIFY(!release::parse("{\"message\": \"Not Found\"}"));
        QVERIFY(!release::parse("not json"));
    }

    void releaseNotes()
    {
        const QString notes = u"界面有了英文，还能切换主题。**Now** in English, with themes.\n"
                              u"\n"
                              u"## 下载 · Download\n"
                              u"\n"
                              u"Something.\n"
                              u"\n"
                              u"## 新功能 · What's new\n"
                              u"\n"
                              u"- 🌐 **中英双语**：随时切换。\n"
                              u"  **English and Chinese**: switch any time.\n"
                              u"- 没有图标的一条\n"
                              u"- 🎨 主题\n"
                              u"  Themes\n"
                              u"  in two lines\n"
                              u"\n"
                              u"## 其他 · Other\n"
                              u"- 不算\n"_s;
        QCOMPARE(release::summary(notes, true), u"界面有了英文，还能切换主题。"_s);
        QCOMPARE(release::summary(notes, false), u"**Now** in English, with themes."_s);
        const QList<release::Highlight> zh = release::highlights(notes, true);
        QCOMPARE(zh.size(), 3);
        QCOMPARE(zh[0], (release::Highlight {u"🌐"_s, u"**中英双语**：随时切换。"_s}));
        QCOMPARE(zh[1], (release::Highlight {QString(), u"没有图标的一条"_s}));
        const QList<release::Highlight> en = release::highlights(notes, false);
        QCOMPARE(en[0].text, u"**English and Chinese**: switch any time."_s);
        QCOMPARE(en[1].text, u"没有图标的一条"_s); // no English line: the Chinese one
        QCOMPARE(en[2], (release::Highlight {u"🎨"_s, u"Themes in two lines"_s}));
        QCOMPARE(release::summary(u"## Only headings"_s, true), QString());
        QCOMPARE(release::summary(u"No Chinese at all."_s, true), u"No Chinese at all."_s);
    }

    void wtf8RoundTrip()
    {
        const std::u16string samples[]
            = {u"plain.txt", u"中文文件名.txt", u"emoji 😀.png", std::u16string(1, char16_t(0xD800)) + u"x"};
        for (const auto& s : samples) {
            const std::string bytes = wtf8::fromUtf16(s);
            std::u16string back;
            wtf8::decodeAppend(back, bytes);
            QCOMPARE(back, s);
        }
        QCOMPARE(wtf8::toQString(wtf8::fromUtf16(u"报告.docx")), u"报告.docx"_s);
    }

    void foldAndFind()
    {
        QCOMPARE(text::foldAscii("ReadMe.TXT"), std::string("readme.txt"));
        QCOMPARE(text::findFolded("MyReport.docx", "report"), std::size_t {2});
        QCOMPARE(text::findFolded("abc", "abcd"), text::npos);
        QVERIFY(text::equalsFolded("ABC", "abc"));
        const std::string chinese = wtf8::fromUtf16(u"年度报告");
        QCOMPARE(text::findFolded(chinese, wtf8::fromUtf16(u"报告")), std::size_t {6});
    }

    void simdFindMatchesScalar()
    {
        std::mt19937 rng(42);
        const std::string alphabet = "abcABC._ 1" + wtf8::fromUtf16(u"报告");
        const auto randomString = [&](std::size_t maxLength) {
            std::string s(rng() % (maxLength + 1), '\0');
            for (char& c : s)
                c = alphabet[rng() % alphabet.size()];
            return s;
        };
        for (int iteration = 0; iteration < 50000; ++iteration) {
            const std::string hay = randomString(40);
            const std::string needle = text::foldAscii(randomString(5));
            const std::string padded = hay + std::string(text::kReadPastEnd, 'a'); // padding that could match
            const std::string_view view(padded.data(), hay.size());
            QCOMPARE(text::findFoldedPadded(view, needle), text::findFolded(view, needle));
        }
    }

    void glob()
    {
        QVERIFY(text::globMatch("Report.PDF", "*.pdf"));
        QVERIFY(text::globMatch("a1c", "a?c"));
        QVERIFY(!text::globMatch("abc.txt", "*.pdf"));
        QVERIFY(text::globMatch(wtf8::fromUtf16(u"报告.txt"), "??.txt")); // '?' is one character, not one byte
        QVERIFY(text::globMatch("anything", "*"));
    }

    void wordStart()
    {
        QVERIFY(text::isWordStart("my_report", 3));
        QVERIFY(text::isWordStart("myReport", 2));
        QVERIFY(text::isWordStart("v2final", 2));
        QVERIFY(!text::isWordStart("deportation", 2));
    }

    void parseQuery()
    {
        const ParsedQuery q = ws::parseQuery(uR"(report "annual plan" !draft ext:pdf,.docx proj\read)"_s);
        QCOMPARE(q.terms.size(), std::size_t {4});
        QCOMPARE(q.terms[0].text, std::string("report"));
        QCOMPARE(q.terms[1].text, std::string("annual plan"));
        QVERIFY(q.terms[2].negated);
        QCOMPARE(q.terms[3].text, std::string("read"));
        QCOMPARE(q.terms[3].ancestors, std::vector<std::string> {"proj"});
        QCOMPARE(q.extensions, (std::vector<std::string> {"pdf", "docx"}));
        QCOMPARE(q.highlights, (QStringList {u"report"_s, u"annual plan"_s, u"read"_s}));

        QVERIFY(ws::parseQuery(u"   "_s).isEmpty());
        QVERIFY(ws::parseQuery(u"*.TXT"_s).terms[0].wildcard);
        QCOMPARE(ws::parseQuery(u"rep*2024?.PDF"_s).terms[0].literal, std::string("2024"));

        // A whole path is one term: the space is in a folder's name.
        const ParsedQuery path = ws::parseQuery(uR"(C:\Users\me\Tencent Files\nt_qq\emoji)"_s);
        QCOMPARE(path.terms.size(), std::size_t {1});
        QCOMPARE(path.terms[0].text, std::string("emoji"));
        QCOMPARE(path.terms[0].ancestors, (std::vector<std::string> {"c:", "users", "me", "tencent files", "nt_qq"}));
        QCOMPARE(path.highlights, (QStringList {u"emoji"_s}));
        QVERIFY(NameMatcher(path).matchPath(uR"(C:\Users\me\Tencent Files\nt_qq\emoji)"_s, true) >= 0);
        QCOMPARE(ws::parseQuery(uR"(D:\Projects\ ext:pdf)"_s).extensions, std::vector<std::string> {"pdf"}); // no path: as before
    }

    // size: and dm: narrow by size and time (EntryFilter); what cannot be
    // read stays a word.
    void parseQueryFilters()
    {
        const std::uint64_t mb = 1 << 20;
        ParsedQuery q = ws::parseQuery(u"report size:>10mb"_s);
        QCOMPARE(q.terms.size(), std::size_t {1});
        QVERIFY(q.filter.bySize() && !q.filter.byTime());
        QCOMPARE(q.filter.minSize, packSize(10 * mb + 1));
        QCOMPARE(q.filter.maxSize, kUnknownSize);
        QVERIFY(!q.isEmpty());
        QVERIFY(!ws::parseQuery(u"size:>1gb"_s).isEmpty()); // a filter alone lists what fits it
        q = ws::parseQuery(u"SIZE:1mb..5MB"_s);
        QCOMPARE(q.filter.minSize, packSize(mb));
        QCOMPARE(q.filter.maxSize, packSize(5 * mb));
        q = ws::parseQuery(u"size:<=1.5k"_s);
        QCOMPARE(q.filter.minSize, FileSize {0});
        QCOMPARE(q.filter.maxSize, packSize(1536));
        q = ws::parseQuery(u"size:large"_s);
        QCOMPARE(q.filter.minSize, packSize(mb));
        QCOMPARE(q.filter.maxSize, packSize(16 * mb - 1));
        q = ws::parseQuery(u"size:>100mb size:<1gb"_s); // both: the narrower bounds
        QCOMPARE(q.filter.minSize, packSize(100 * mb + 1));
        QCOMPARE(q.filter.maxSize, packSize(1024 * mb - 1));
        q = ws::parseQuery(u"size:abc"_s); // not a size: a word
        QCOMPARE(q.terms.size(), std::size_t {1});
        QVERIFY(!q.filter.any());

        const QDateTime now(QDate(2026, 10, 15), QTime(14, 30));
        EntryFilter f;
        QVERIFY(ws::parseDateToken(u"dm:today", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(QDate(2026, 10, 15).startOfDay().toSecsSinceEpoch()));
        QCOMPARE(f.before, FileTime {0});
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:7d", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(now.addDays(-7).toSecsSinceEpoch()));
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:2026-01", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(QDate(2026, 1, 1).startOfDay().toSecsSinceEpoch()));
        QCOMPARE(f.before, static_cast<FileTime>(QDate(2026, 2, 1).startOfDay().toSecsSinceEpoch()));
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:2025-12-30..2026-01-02", now, f));
        QCOMPARE(f.after, static_cast<FileTime>(QDate(2025, 12, 30).startOfDay().toSecsSinceEpoch()));
        QCOMPARE(f.before, static_cast<FileTime>(QDate(2026, 1, 3).startOfDay().toSecsSinceEpoch()));
        f = {};
        QVERIFY(ws::parseDateToken(u"dm:<2026", now, f));
        QCOMPARE(f.after, FileTime {0});
        QCOMPARE(f.before, static_cast<FileTime>(QDate(2026, 1, 1).startOfDay().toSecsSinceEpoch()));
        f = {};
        QVERIFY(!ws::parseDateToken(u"dm:soon", now, f));
        QVERIFY(!ws::parseDateToken(u"dm:2026-13", now, f));
        QVERIFY(ws::parseQuery(u"dm:yesterday"_s).filter.byTime());
        QCOMPARE(ws::parseQuery(u"dm:never"_s).terms.size(), std::size_t {1});

        // What the filter lets through: bounds on the time leave out a time
        // not known (1970, 1980: knownTime); on the size, folders and sizes
        // not known.
        EntryFilter bounds;
        bounds.after = 1'700'000'000;
        QVERIFY(bounds.fits(1'700'000'000, kUnknownSize, true));
        QVERIFY(!bounds.fits(1'699'999'999, kUnknownSize, true));
        QVERIFY(!bounds.fits(0, kUnknownSize, false));
        QVERIFY(!bounds.fits(10, kUnknownSize, false)); // 1970: not known
        bounds.before = 1'700'000'100;
        QVERIFY(bounds.fits(1'700'000'099, kUnknownSize, false));
        QVERIFY(!bounds.fits(1'700'000'100, kUnknownSize, false));
        EntryFilter sizes;
        sizes.minSize = packSize(1000);
        QVERIFY(sizes.fits(0, packSize(1000), false));
        QVERIFY(!sizes.fits(0, packSize(999), false));
        QVERIFY(!sizes.fits(0, kUnknownSize, false));
        QVERIFY(!sizes.fits(0, kUnknownSize, true));

        QCOMPARE(ws::sizeText(0), u"0 B"_s);
        QCOMPARE(ws::sizeText(999), u"999 B"_s);
        QCOMPARE(ws::sizeText(1536), u"1.50 KB"_s);
        QCOMPARE(ws::sizeText(12 * mb + mb / 3), u"12.3 MB"_s);
        QCOMPARE(ws::sizeText(1'300'000'000), u"1.21 GB"_s);
        // Packed sizes: exact to 16 MB, close above, in order always.
        QCOMPARE(unpackSize(packSize(0)), std::uint64_t {0});
        QCOMPARE(unpackSize(packSize(16'777'215)), std::uint64_t {16'777'215});
        QVERIFY(packSize(16'777'217) > packSize(16'777'215));
        const std::uint64_t big = 7'000'000'000'000ull;
        QVERIFY(unpackSize(packSize(big)) > big - big / 100'000 && unpackSize(packSize(big)) < big + big / 100'000);
        QVERIFY(packSize(big) < kUnknownSize && packSize(big) > packSize(1 << 30));
    }

    // The panel's filters and column orders (NameSearchOptions::filter,
    // order): every search is a pass over the index, so a synthetic one of
    // files with known times and sizes says what must come out.
    void nameSearchFiltersAndOrders()
    {
        FileIndex index;
        const EntryId root = index.addRoot("D:");
        const EntryId work = index.add(root, "work", EntryFlag::Directory);
        const EntryId old = index.add(root, "old", EntryFlag::Directory);
        constexpr FileTime day = 86'400;
        constexpr FileTime t0 = 1'760'000'000; // 2025-10
        struct Spec {
            const char* name;
            EntryId parent;
            FileTime modified;
            std::uint64_t size; // kUnknownSize bits: not known
        };
        const Spec specs[] = {
            {"a.txt", work, t0 + 10 * day, 100},
            {"b.txt", work, t0 + 9 * day, 2'000'000},
            {"c.txt", old, t0 + 1 * day, 50'000},
            {"d.txt", old, t0 + 20 * day, 0xFFFF'FFFFull},
            {"e.txt", old, 0, 300}, // never known when
            {"f.log", work, t0 + 5 * day, 400},
        };
        std::vector<EntryId> ids;
        for (const Spec& s : specs) {
            const EntryId id = index.add(s.parent, s.name, 0);
            index.setModified(id, s.modified);
            if (s.size != 0xFFFF'FFFFull)
                index.setSize(id, packSize(s.size));
            ids.push_back(id);
        }
        const EntryId sub = index.add(work, "sub", EntryFlag::Directory);
        index.setModified(sub, t0 + 8 * day);
        WorkerPool pool(4);
        const auto names = [&](const NameSearchOutput& out) {
            QStringList list;
            for (const NameHit& h : out.hits)
                list.append(QString::fromUtf8(index.name(h.id)));
            return list;
        };
        const NameMatcher everything(ws::parseQuery(u""_s)); // the panel with nothing typed (browse)
        const NameMatcher txt(ws::parseQuery(u"ext:txt"_s));

        index.setCreated(ids[0], t0 + 2 * day);
        index.setCreated(ids[1], t0 + 3 * day);
        index.setCreated(ids[2], t0 + day);
        const auto created = searchNames(index, txt, {.limit = 2, .order = {SortKey::Created, true}}, pool, {});
        QCOMPARE(names(created), (QStringList {u"b.txt"_s, u"a.txt"_s}));
        const auto oldestCreated = searchNames(index, txt, {.limit = 3, .order = {SortKey::Created, false}}, pool, {});
        QCOMPARE(names(oldestCreated), (QStringList {u"c.txt"_s, u"a.txt"_s, u"b.txt"_s}));

        NameSearchOptions o;
        o.limit = 100;
        o.order = {SortKey::Name, false};
        auto out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {9}); // two folders, the sub folder, six files
        QCOMPARE(names(out), (QStringList {u"a.txt"_s, u"b.txt"_s, u"c.txt"_s, u"d.txt"_s, u"e.txt"_s, u"f.log"_s, u"old"_s, u"sub"_s, u"work"_s}));
        o.order = {SortKey::Name, true};
        QCOMPARE(names(searchNames(index, everything, o, pool, {})).first(), u"work"_s);

        // By size: the largest first; a size not known (and every folder) last.
        o.order = {SortKey::Size, true};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out).first(3), (QStringList {u"b.txt"_s, u"c.txt"_s, u"e.txt"_s}));
        QCOMPARE(names(out).last(), u"d.txt"_s);
        o.order = {SortKey::Size, false};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out).first(2), (QStringList {u"a.txt"_s, u"e.txt"_s}));
        QCOMPARE(names(out).last(), u"d.txt"_s);
        QCOMPARE(out.hits.front().size, packSize(100));

        // By time: a time not known last either way.
        o.order = {SortKey::Modified, true};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"d.txt"_s, u"a.txt"_s, u"b.txt"_s, u"c.txt"_s, u"e.txt"_s}));
        o.order = {SortKey::Modified, false};
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"c.txt"_s, u"b.txt"_s, u"a.txt"_s, u"d.txt"_s, u"e.txt"_s}));

        // By path: a folder's files together (its path, then the name), folders by their own.
        o.order = {SortKey::Path, false};
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"old"_s, u"work"_s, u"c.txt"_s, u"d.txt"_s, u"e.txt"_s, u"a.txt"_s, u"b.txt"_s, u"f.log"_s, u"sub"_s}));

        // By type (#44): the extension, then the name; folders have none.
        o.order = {SortKey::Type, false};
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"old"_s, u"sub"_s, u"work"_s, u"f.log"_s, u"a.txt"_s, u"b.txt"_s, u"c.txt"_s, u"d.txt"_s, u"e.txt"_s}));
        o.order = {SortKey::Type, true};
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(names(out).first(2), (QStringList {u"e.txt"_s, u"d.txt"_s}));
        QCOMPARE(names(out).last(), u"old"_s);

        // The kinds apart (the launcher's sorts): each kind keeps its own best
        // `limit`, in the order, where one list would cut folders off.
        o.order = {SortKey::Name, false};
        o.limit = 3;
        o.kindsApart = false;
        QCOMPARE(names(searchNames(index, everything, o, pool, {})), (QStringList {u"a.txt"_s, u"b.txt"_s, u"c.txt"_s}));
        o.kindsApart = true;
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.hits.size(), std::size_t {6});
        QCOMPARE(names(out).first(3), (QStringList {u"a.txt"_s, u"b.txt"_s, u"c.txt"_s}));
        QCOMPARE(names(out).last(3), (QStringList {u"old"_s, u"sub"_s, u"work"_s}));
        o.kindsApart = false;
        o.limit = 100;

        // Filters: when written (a time not known left out), how large
        // (folders and sizes not known left out), under which folders.
        o.order = {};
        o.filter.after = t0 + 5 * day;
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {5}); // a, b, d, f, sub
        o.filter.before = t0 + 10 * day;
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {3}); // b, f, sub
        o.filter = {};
        o.filter.minSize = packSize(300);
        o.filter.maxSize = packSize(50'000);
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {3}); // c, e, f
        o.filter = {};
        o.filter.folders = {old};
        out = searchNames(index, everything, o, pool, {});
        QCOMPARE(out.totalMatches, std::size_t {3});
        o.filter.folders = {old, work};
        std::ranges::sort(o.filter.folders);
        o.filter.minSize = packSize(1000);
        out = searchNames(index, txt, o, pool, {});
        QCOMPARE(names(out), (QStringList {u"b.txt"_s, u"c.txt"_s}));
        // A page more comes in the same order (ids break every tie).
        o.filter = {};
        o.order = {SortKey::Name, false};
        o.limit = 3;
        const auto page1 = names(searchNames(index, everything, o, pool, {}));
        o.limit = 6;
        QCOMPARE(names(searchNames(index, everything, o, pool, {})).first(3), page1);
    }

    // The query read aloud (homophoneQueries), for names that sound the same.
    void homophoneQueries()
    {
        // The term as pinyin (the most common readings first: 度 is also
        // duo), the rest of the query as it was.
        const std::vector<ParsedQuery> spoken = ws::homophoneQueries(ws::parseQuery(u"年度报高 2025 !草稿 ext:docx"_s));
        QCOMPARE(spoken.size(), std::size_t {2});
        QCOMPARE(spoken[1].terms[0].text, std::string("nianduobaogao"));
        QCOMPARE(spoken[0].terms.size(), std::size_t {3});
        QCOMPARE(spoken[0].terms[0].text, std::string("niandubaogao"));
        QCOMPARE(spoken[0].terms[1].text, std::string("2025"));
        QVERIFY(spoken[0].terms[2].negated);
        QCOMPARE(spoken[0].terms[2].text, std::string("草稿")); // exclusions stay as typed
        QCOMPARE(spoken[0].extensions, std::vector<std::string> {"docx"});
        QCOMPARE(spoken[0].highlights, (QStringList {u"niandubaogao"_s, u"2025"_s}));
        // Other characters in the term stay: "报高v2" reads "baogaov2".
        QCOMPARE(ws::homophoneQueries(ws::parseQuery(u"报高v2"_s))[0].terms[0].text, std::string("baogaov2"));
        // A character with several readings: a query per reading, the most
        // common first (行: xing, then hang).
        const std::vector<ParsedQuery> bank = ws::homophoneQueries(ws::parseQuery(u"银行"_s));
        QVERIFY(bank.size() >= 2);
        QCOMPARE(bank[0].terms[0].text, std::string("yinxing"));
        QVERIFY(std::ranges::any_of(bank, [](const ParsedQuery& q) { return q.terms[0].text == "yinhang"; }));
        // Two such characters: every combination, up to the few allowed.
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"银行长"_s)).size() >= 4);
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"行行行行行行"_s)).size() <= 8);
        // Nothing to read: no queries. A wildcard term is not read either.
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"report 2025"_s)).empty());
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u"报告*"_s)).empty());
        QVERIFY(ws::homophoneQueries(ws::parseQuery(u""_s)).empty());

        // In an index: 年度报高 is nowhere, read aloud it finds 年度报告 (and
        // not 其他); the pinyin highlights read back into the name.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId annual = index.add(root, "年度报告.docx", 0);
        index.add(root, "其他.txt", 0);
        index.add(root, "年度计划.txt", 0);
        const ParsedQuery typed = ws::parseQuery(u"年度报高"_s);
        WorkerPool pool(2);
        QCOMPARE(searchNames(index, NameMatcher(typed), {.limit = 10}, pool, {}).hits.size(), std::size_t {0});
        const auto found = searchNames(index, NameMatcher(ws::homophoneQueries(typed)[0]), {.limit = 10}, pool, {});
        QCOMPARE(found.hits.size(), std::size_t {1});
        QCOMPARE(found.hits[0].id, annual);
        QVERIFY(pinyin::Matcher("niandubaogao").findUtf16(u"年度报告.docx").has_value());
    }

    void matcherRanking()
    {
        const NameMatcher m(ws::parseQuery(u"report"_s));
        const int exact = m.matchPath(u"C:\\report"_s, true);
        const int stem = m.matchPath(u"C:\\Report.docx"_s, false);
        const int prefix = m.matchPath(u"C:\\report_2024_final.docx"_s, false);
        const int word = m.matchPath(u"C:\\my_report.txt"_s, false);
        const int inner = m.matchPath(u"C:\\unreported.pdf"_s, false);
        QVERIFY(exact > stem);
        QVERIFY(stem > prefix);
        QVERIFY(prefix > word);
        QVERIFY(word > inner);
        QCOMPARE(m.matchPath(u"C:\\summary.txt"_s, false), -1);

        const NameMatcher negated(ws::parseQuery(u"report !draft"_s));
        QCOMPARE(negated.matchPath(u"C:\\report_draft.txt"_s, false), -1);

        const NameMatcher ext(ws::parseQuery(u"ext:pdf"_s));
        QVERIFY(ext.matchPath(u"C:\\a.PDF"_s, false) >= 0);
        QCOMPARE(ext.matchPath(u"C:\\a.pdf"_s, true), -1); // folders have no extension

        const NameMatcher path(ws::parseQuery(u"users\\notes"_s));
        QVERIFY(path.matchPath(u"C:\\Users\\me\\notes.txt"_s, false) >= 0);
        QCOMPARE(path.matchPath(u"D:\\Other\\notes.txt"_s, false), -1);
    }

    // Folders named among the words of a content search (ContentFolderSpec).
    void contentFolderSpecs()
    {
        using Spec = ws::ContentFolderSpec;
        const auto split = [](const QString& text) { return ws::splitContentQuery(text); };
        const auto names = [](const QString& typed, QStringList n) { return Spec {typed, {}, std::move(n)}; };

        ws::ContentQuery q = split(u"年度报告 abc/"_s);
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"abc/"_s, {u"abc"_s})}));
        q = split(u"abc\\ 年度报告"_s); // first, with a backslash
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"abc\\"_s, {u"abc"_s})}));
        const QString wide = u"年度报告 工作"_s + QChar(0xFF0F) + u"abc"_s + QChar(0xFF0F); // as a Chinese layout types /
        q = split(wide);
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders.size(), std::size_t {1});
        QCOMPARE(q.folders[0].names, (QStringList {u"工作"_s, u"abc"_s}));
        q = split(u"年度报告 2025 工作/abc/"_s);
        QCOMPARE(q.words, u"年度报告 2025"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"工作/abc/"_s, {u"工作"_s, u"abc"_s})}));

        // "/abc": only last, after something to find.
        q = split(u"年度报告 /abc"_s);
        QCOMPARE(q.words, u"年度报告"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"/abc"_s, {u"abc"_s})}));
        q = split(u"/abc 年度报告"_s);
        QCOMPARE(q.words, u"/abc 年度报告"_s);
        QVERIFY(q.folders.empty());
        QCOMPARE(split(u"/usr"_s).words, u"/usr"_s);
        QVERIFY(split(u"/usr"_s).folders.empty());
        QVERIFY(split(u"GET /api/users"_s).folders.empty()); // more than one name: a route
        // A folder alone: where to look, nothing to find yet.
        q = split(u"abc/"_s);
        QVERIFY(q.words.isEmpty());
        QCOMPARE(q.folders, (std::vector<Spec> {names(u"abc/"_s, {u"abc"_s})}));

        // Paths, spaces and all, up to "\ ".
        q = split(u"D:\\My Files\\ 报告 2025"_s);
        QCOMPARE(q.words, u"报告 2025"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {{u"D:\\My Files\\"_s, u"D:\\My Files"_s, {}}}));
        q = split(u"报告 D:/Projects/"_s);
        QCOMPARE(q.words, u"报告"_s);
        QCOMPARE(q.folders.size(), std::size_t {1});
        QCOMPARE(q.folders[0].path, u"D:\\Projects"_s);
        q = split(u"\"D:\\My Files\\\" \"年度 报告\""_s);
        QCOMPARE(q.words, u"\"年度 报告\""_s);
        QCOMPARE(q.folders.size(), std::size_t {1});
        QCOMPARE(q.folders[0].path, u"D:\\My Files"_s);
        QCOMPARE(split(u"D:\\ 报告"_s).folders[0].path, u"D:\\"_s);
        // A folder whose name looks like code is still a folder; code after it stays one phrase.
        q = split(u"C:\\Program Files (x86)\\Steam\\ 存档 2025"_s);
        QCOMPARE(q.words, u"存档 2025"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {{u"C:\\Program Files (x86)\\Steam\\"_s, u"C:\\Program Files (x86)\\Steam"_s, {}}}));
        q = split(u" D:\\src [old]\\ if (a == b)"_s);
        QCOMPARE(q.words, u"if (a == b)"_s);
        QCOMPARE(q.folders, (std::vector<Spec> {{u"D:\\src [old]\\"_s, u"D:\\src [old]"_s, {}}}));
        QCOMPARE(split(u"x = 1; D:\\a\\"_s).words, u"x = 1; D:\\a\\"_s); // code first: all of it
        QCOMPARE(split(u"\\\\nas\\docs\\ 报告"_s).folders[0].path, u"\\\\nas\\docs"_s);
        qputenv("WS_TEST_HOME", "C:\\Users\\me");
        QCOMPARE(split(u"%WS_TEST_HOME%\\Documents\\ 报告"_s).folders[0].path, u"C:\\Users\\me\\Documents"_s);
        qunsetenv("WS_TEST_HOME");
        q = split(u"D:\\Projects\\rep"_s); // a path being typed: no folder said
        QCOMPARE(q.words, u"D:\\Projects\\rep"_s);
        QVERIFY(q.folders.empty());

        // Not folders: what the search finds as it is.
        for (const QString& text : {u"printf(\"a/\")"_s, u"x = a/ b;"_s, u"see http://x.com/"_s, u"\"abc/\" 报告"_s,
                 u"!tmp/ 报告"_s, u"*.log/ x"_s, u"a/../ x"_s, u"报告"_s}) {
            q = split(text);
            QVERIFY2(q.folders.empty(), qPrintable(text));
            QCOMPARE(q.words, text);
        }
    }

    void matcherTermsInFolders()
    {
        QCOMPARE(ws::queryWords(u"  报告 \"annual plan\"\tx "_s), (QStringList {u"报告"_s, u"annual plan"_s, u"x"_s}));
        // Content: the words, or a line of code just as it is.
        QCOMPARE(ws::contentTerms(u" 报告  2025 "_s), (QStringList {u"报告"_s, u"2025"_s}));
        QCOMPARE(ws::contentTerms(u"\"annual plan\" 2025"_s), (QStringList {u"annual plan"_s, u"2025"_s}));
        QCOMPARE(ws::contentTerms(u"printf(\"%d\", *p)"_s), QStringList {u"printf(\"%d\", *p)"_s});
        QCOMPARE(ws::contentTerms(u"const auto lock = index->readLock();"_s),
            QStringList {u"const auto lock = index->readLock();"_s});
        QCOMPARE(ws::contentTerms(u"\"if (a == b)\""_s), QStringList {u"if (a == b)"_s});
        QCOMPARE(ws::contentTerms(u"\"年度 报"_s), QStringList {u"年度 报"_s}); // the closing quote not typed yet
        QVERIFY(ws::contentTerms(u"   "_s).isEmpty());

        // Each term in the name or in a folder above it, in either order; at
        // least one in the name.
        for (const QString& q : {u"src app"_s, u"app src"_s}) {
            const NameMatcher m(ws::parseQuery(q));
            QVERIFY(m.matchPath(u"F:\\proj\\src\\app"_s, true) >= 0);
            QVERIFY(m.matchPath(u"F:\\proj\\src\\app\\App.cpp"_s, false) >= 0);
            QCOMPARE(m.matchPath(u"F:\\proj\\src\\other.txt"_s, false), -1); // none in the name
            QCOMPARE(m.matchPath(u"F:\\proj\\lib\\app.txt"_s, false), -1); // "src" nowhere
            QVERIFY(m.matchPath(u"F:\\src_app.txt"_s, false) > m.matchPath(u"F:\\src\\app.txt"_s, false));
            QCOMPARE(m.matchName("app"), -1); // an app's name has no folders
        }
        // One term: in the name, as before.
        QCOMPARE(NameMatcher(ws::parseQuery(u"src"_s)).matchPath(u"F:\\src\\app.txt"_s, false), -1);
        // With an extension: the word may be the folder's.
        const NameMatcher pdfs(ws::parseQuery(u"合同 ext:pdf"_s));
        QVERIFY(pdfs.matchPath(u"D:\\合同\\扫描1.pdf"_s, false) >= 0);
        QCOMPARE(pdfs.matchPath(u"D:\\合同\\扫描1.jpg"_s, false), -1);
        // Wildcards and exclusions stay with the name.
        QCOMPARE(NameMatcher(ws::parseQuery(u"*.txt docs"_s)).matchPath(u"C:\\a.txt\\docs.md"_s, false), -1);
        QVERIFY(NameMatcher(ws::parseQuery(u"notes !draft"_s)).matchPath(u"C:\\draft\\notes.txt"_s, false) >= 0);

        // In the index, folders by pinyin too, remembered across entries.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId project = index.add(root, "项目资料", EntryFlag::Directory);
        const EntryId year = index.add(project, "2025", EntryFlag::Directory);
        const EntryId plan = index.add(year, "方案.doc", 0);
        const EntryId notes = index.add(year, "notes.txt", 0);
        const EntryId elsewhere = index.add(root, "方案.doc", 0);
        const NameMatcher m(ws::parseQuery(u"xmzl 方案"_s));
        NameMatcher::Folders folders;
        QVERIFY(m.match(index, index.entry(plan), folders) >= 0);
        QCOMPARE(m.match(index, index.entry(notes), folders), -1);
        QCOMPARE(m.match(index, index.entry(elsewhere), folders), -1);
        QVERIFY(m.match(index, index.entry(plan), folders) >= 0); // from what it noted
        QVERIFY(NameMatcher(ws::parseQuery(u"2025 方案"_s)).match(index, index.entry(plan)) >= 0);
    }

    void bigFolderLookups()
    {
        // Writers' lookups in a folder with thousands of children go through a
        // hash table; it must stay in step with adds, removals, renames and moves.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId big = index.add(root, "big", EntryFlag::Directory);
        const EntryId other = index.add(root, "other", EntryFlag::Directory);
        std::vector<EntryId> ids;
        for (int i = 0; i < 3000; ++i)
            ids.push_back(index.add(big, "file" + std::to_string(i) + ".txt", 0));
        const auto linear = [&](EntryId parent, std::string_view name) {
            for (EntryId c = index.entry(parent).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
                if (index.name(c) == name)
                    return c;
            }
            return kNoEntry;
        };

        QCOMPARE(index.childForUpdate(big, "file1234.txt", true), ids[1234]); // builds the table
        QCOMPARE(index.childForUpdate(big, "FILE1234.TXT", false), ids[1234]);
        QCOMPARE(index.childForUpdate(big, "FILE1234.TXT", true), kNoEntry);
        QCOMPARE(index.findChild(big, "File7.TXT"), ids[7]); // readers use it too

        index.remove(ids[10]);
        QCOMPARE(index.childForUpdate(big, "file10.txt", false), kNoEntry);
        const EntryId added = index.add(big, "new.txt", 0);
        QCOMPARE(index.childForUpdate(big, "NEW.txt", false), added);
        QVERIFY(index.move(ids[20], big, "renamed.txt"));
        QCOMPARE(index.childForUpdate(big, "file20.txt", false), kNoEntry);
        QCOMPARE(index.childForUpdate(big, "renamed.txt", true), ids[20]);
        QVERIFY(index.move(ids[30], other, "moved.txt"));
        QCOMPARE(index.childForUpdate(big, "file30.txt", false), kNoEntry);
        QCOMPARE(index.childForUpdate(other, "moved.txt", true), ids[30]);
        const EntryId upper = index.add(big, "FILE40.TXT", 0); // case-sensitive folder: both spellings
        QCOMPARE(index.childForUpdate(big, "FILE40.TXT", true), upper);
        QCOMPARE(index.childForUpdate(big, "file40.txt", true), ids[40]);

        for (int i = 0; i < 3000; i += 7) { // every name agrees with a plain walk
            const std::string name = "file" + std::to_string(i) + ".txt";
            QCOMPARE(index.childForUpdate(big, name, true), linear(big, name));
        }

        index.compact(); // renumbers: the tables go and come back on demand
        const EntryId bigNow = index.findPath(L"C:\\big");
        QCOMPARE(index.name(index.childForUpdate(bigNow, "file2999.txt", true)), std::string_view("file2999.txt"));
        QCOMPARE(index.pathForUpdate(L"C:\\big\\renamed.txt"), linear(bigNow, "renamed.txt"));
    }

    void siblingLists()
    {
        // Children taken out in any order (removed, or moved elsewhere) leave
        // the list intact: in small folders, which are walked, and in big
        // ones, whose table knows the child before each.
        for (const int count : {5, 300, 3000}) {
            FileIndex index;
            const EntryId root = index.addRoot("C:");
            const EntryId dir = index.add(root, "dir", EntryFlag::Directory);
            const EntryId other = index.add(root, "other", EntryFlag::Directory);
            std::vector<EntryId> ids;
            for (int i = 0; i < count; ++i)
                ids.push_back(index.add(dir, "f" + std::to_string(i), 0));
            std::set<EntryId> expected(ids.begin(), ids.end());
            const auto childrenOf = [&](EntryId parent) {
                std::set<EntryId> seen;
                for (EntryId c = index.entry(parent).firstChild; c != kNoEntry; c = index.entry(c).nextSibling) {
                    if (!seen.insert(c).second)
                        return std::set<EntryId> {kNoEntry}; // a loop
                }
                return seen;
            };
            std::vector<EntryId> order = ids;
            std::shuffle(order.begin(), order.end(), std::mt19937(count));
            std::size_t moved = 0;
            for (std::size_t k = 0; k < order.size(); ++k) {
                if (k % 3 == 0) {
                    QVERIFY(index.move(order[k], other, "m" + std::to_string(k)));
                    ++moved;
                } else {
                    index.remove(order[k]);
                }
                expected.erase(order[k]);
                if (k == order.size() / 2) {
                    for (int n = 0; n < 50; ++n)
                        expected.insert(index.add(dir, "new" + std::to_string(n), 0));
                }
                if (k % 97 == 0)
                    QVERIFY(childrenOf(dir) == expected);
            }
            QVERIFY(childrenOf(dir) == expected);
            QCOMPARE(childrenOf(other).size(), moved);
            for (const EntryId c : expected)
                QCOMPARE(index.childForUpdate(dir, index.name(c), true), c);
        }
    }

    void changeWatcherRoots()
    {
        QTemporaryDir dir;
        const std::wstring root = QDir::toNativeSeparators(dir.path()).toStdWString();
        const auto openExclusively = [&] { // fails while anyone else holds the folder open
            const win32::UniqueHandle h(::CreateFileW(root.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            return h.valid();
        };
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<FsChange> seen;
        ChangeWatcher watcher([&](std::vector<FsChange>&& changes) {
            std::lock_guard lock(mutex);
            seen.insert(seen.end(), changes.begin(), changes.end());
            changed.notify_all();
        });
        QVERIFY(watcher.roots().empty());

        watcher.setRoots({root});
        QCOMPARE(watcher.roots(), std::vector<std::wstring> {root});
        QVERIFY(!openExclusively());
        QFile file(dir.filePath(u"new.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        {
            std::unique_lock lock(mutex);
            QVERIFY(changed.wait_for(lock, std::chrono::seconds(5), [&] {
                return std::any_of(seen.begin(), seen.end(), [&](const FsChange& c) {
                    return c.kind == FsChange::Kind::Added && c.path == root + L"\\new.txt";
                });
            }));
        }

        // Dropped from the set: its handle is closed by the time setRoots() returns.
        watcher.setRoots({});
        QVERIFY(watcher.roots().empty());
        QVERIFY(openExclusively());
    }

    // GitHub #52 (design 7.2): a chosen folder is watched through its volume's
    // root, which must not keep a folder in it from being renamed (or moved
    // to the Recycle Bin). Watching the folder itself (the fallback when the
    // root cannot be opened) may keep the folders above it from that: what
    // Windows does then is told, not asserted.
    void watchedFoldersRename()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(u"a/b"_s));
        const std::wstring root = QDir::toNativeSeparators(dir.path()).toStdWString();
        const std::wstring a = root + L"\\a";
        const std::wstring renamed = root + L"\\a2";
        ChangeWatcher watcher([](std::vector<FsChange>&&) {});

        watcher.setRoots({root});
        QCOMPARE(watcher.roots().size(), std::size_t {1});
        QVERIFY2(::MoveFileW(a.c_str(), renamed.c_str()), qPrintable(u"error %1"_s.arg(::GetLastError())));
        QVERIFY(::MoveFileW(renamed.c_str(), a.c_str()));

        watcher.setRoots({a + L"\\b"});
        QCOMPARE(watcher.roots().size(), std::size_t {1});
        const bool moved = ::MoveFileW(a.c_str(), renamed.c_str());
        const DWORD error = moved ? 0 : ::GetLastError();
        qInfo("GitHub #52, design 7.2: renaming T\\a while T\\a\\b is watched: %s (error %lu)",
            moved ? "renamed" : "refused", error);
        watcher.setRoots({});
        if (moved)
            QVERIFY(::MoveFileW(renamed.c_str(), a.c_str()));
    }

    // At most 62 watched (one wait's limit), the first in the list; more
    // used to end the watcher's thread, and every watch with it.
    void changeWatcherCap()
    {
        QTemporaryDir dir;
        std::vector<std::wstring> folders;
        for (int i = 0; i < 70; ++i) {
            const QString name = u"f%1"_s.arg(i, 2, 10, QChar(u'0'));
            QVERIFY(QDir(dir.path()).mkdir(name));
            folders.push_back(QDir::toNativeSeparators(dir.filePath(name)).toStdWString());
        }
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<FsChange> seen;
        ChangeWatcher watcher([&](std::vector<FsChange>&& changes) {
            std::lock_guard lock(mutex);
            seen.insert(seen.end(), changes.begin(), changes.end());
            changed.notify_all();
        });
        watcher.setRoots(folders);
        const std::vector<std::wstring> watched = watcher.roots();
        QCOMPARE(watched.size(), ChangeWatcher::kMaxWatches);
        QVERIFY(std::equal(watched.begin(), watched.end(), folders.begin()));
        // Still watching: the first one, and the last one there was room for.
        for (const std::wstring& f : {folders.front(), folders[ChangeWatcher::kMaxWatches - 1]}) {
            QFile file(QString::fromStdWString(f + L"\\new.txt"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            std::unique_lock lock(mutex);
            QVERIFY(changed.wait_for(lock, std::chrono::seconds(5), [&] {
                return std::any_of(seen.begin(), seen.end(), [&](const FsChange& c) { return c.path == f + L"\\new.txt"; });
            }));
        }
        watcher.setRoots({});
        QVERIFY(watcher.roots().empty());
    }

    // GitHub #52: a root that cannot be opened has the folders named for it
    // watched in its place, and only then: once it opens, they are let go
    // (each holds its folder open, which keeps the folders above it from
    // being renamed).
    void changeWatcherFallback()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(u"a"_s) && QDir(dir.path()).mkpath(u"b"_s));
        const std::wstring t = QDir::toNativeSeparators(dir.path()).toStdWString();
        const std::wstring missing = t + L"\\missing"; // as a root this account may not list
        const std::wstring a = t + L"\\a";
        const std::wstring b = t + L"\\b";
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<FsChange> seen;
        ChangeWatcher watcher([&](std::vector<FsChange>&& changes) {
            std::lock_guard lock(mutex);
            seen.insert(seen.end(), changes.begin(), changes.end());
            changed.notify_all();
        });
        watcher.setRoots({missing}, {{missing, {a, b}}});
        QCOMPARE(watcher.roots(), (std::vector {a, b}));
        QFile file(QString::fromStdWString(a + L"\\x.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        {
            std::unique_lock lock(mutex);
            QVERIFY(changed.wait_for(lock, std::chrono::seconds(5), [&] {
                return std::any_of(seen.begin(), seen.end(), [&](const FsChange& c) {
                    return c.kind == FsChange::Kind::Added && c.path == a + L"\\x.txt";
                });
            }));
        }
        // The same again: nothing changes.
        watcher.setRoots({missing}, {{missing, {a, b}}});
        QCOMPARE(watcher.roots(), (std::vector {a, b}));
        // The root opens now: it alone.
        QVERIFY(QDir(dir.path()).mkdir(u"missing"_s));
        watcher.setRoots({missing}, {{missing, {a, b}}});
        QCOMPARE(watcher.roots(), std::vector {missing});
        // Folders named for a root not asked for are not watched.
        watcher.setRoots({}, {{missing, {a}}});
        QVERIFY(watcher.roots().empty());
    }

    // GitHub #52: 1 MB between two reads for a local volume's root, 64 KB
    // for anything else (over the network ReadDirectoryChangesW takes no more).
    void changeWatcherBuffers()
    {
        const std::wstring system = qEnvironmentVariable("SystemDrive", u"C:"_s).toStdWString();
        QCOMPARE(ChangeWatcher::bufferBytes(system), std::size_t {1024 * 1024});
        QCOMPARE(ChangeWatcher::bufferBytes(system + L"\\Windows"), std::size_t {64 * 1024});
        QCOMPARE(ChangeWatcher::bufferBytes(L"\\\\server\\share"), std::size_t {64 * 1024});
        ChangeWatcher watcher([](std::vector<FsChange>&&) {});
        watcher.setRoots({system}); // and the system takes it
        QCOMPARE(watcher.roots(), std::vector {system});
        watcher.setRoots({});
    }

    // GitHub #52 review: a 1 MB buffer the system took but whose reads then
    // fail (and not because events were lost) goes down to 64 KB; lost
    // events, a read that came back, or a small buffer leave it as it is.
    void changeWatcherShrinks()
    {
        constexpr std::size_t big = 1024 * 1024;
        constexpr std::size_t small = 64 * 1024;
        QCOMPARE(ChangeWatcher::bufferAfter(big, true, ERROR_SUCCESS), big);
        QCOMPARE(ChangeWatcher::bufferAfter(big, false, ERROR_NOTIFY_ENUM_DIR), big); // events lost
        QCOMPARE(ChangeWatcher::bufferAfter(big, false, ERROR_INVALID_PARAMETER), small);
        QCOMPARE(ChangeWatcher::bufferAfter(big, false, ERROR_NOT_ENOUGH_QUOTA), small);
        QCOMPARE(ChangeWatcher::bufferAfter(big, false, ERROR_INVALID_FUNCTION), small);
        QCOMPARE(ChangeWatcher::bufferAfter(small, false, ERROR_INVALID_PARAMETER), small);
        QCOMPARE(ChangeWatcher::bufferAfter(small, true, ERROR_SUCCESS), small);
    }

    // ---- GitHub #52: only the chosen folders ---------------------------------

    // With no way to the journals and folders chosen, only they are indexed,
    // with the folders on the way to them by name; nothing else of their
    // disk, and no other disk.
    void selectedDrivesRespectAnEmptySelection()
    {
        QVERIFY(volumeSelected(L"C:", std::nullopt));
        QVERIFY(!volumeSelected(L"C:", std::vector<std::wstring>{}));
        QVERIFY(volumeSelected(L"C:", std::vector<std::wstring>{L"c:"}));
        QVERIFY(!volumeSelected(L"D:", std::vector<std::wstring>{L"c:"}));
        QVERIFY(listLocalVolumes(true, {}, std::vector<std::wstring>{}).empty());
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        auto options = walkedOptions(std::vector{t.toStdWString()});
        options.selectedDrives = std::vector<std::wstring>{t.left(2).toStdWString()};
        IndexService service(store.path(), options);
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QVERIFY(indexedIn(service, t + u"\\docs\\a.txt"_s));
        options.selectedDrives = std::vector<std::wstring>{};
        service.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(service.volumeRoots().empty() && !indexedIn(service, t), 30'000);
        options.selectedDrives = std::vector<std::wstring>{t.left(2).toStdWString()};
        service.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\a.txt"_s) && allReady(service), 30'000);
    }

    void explicitDirectoryLinksInEveryAccessMode()
    {
        for (const auto access : {ntfs::VolumeAccess::Kind::None, ntfs::VolumeAccess::Kind::Direct, ntfs::VolumeAccess::Kind::Service}) {
            QTemporaryDir tree;
            QTemporaryDir store;
            QVERIFY(makeScopeTree(tree.path()));
            const QString t = QDir::toNativeSeparators(tree.path());
            const QString target = t + u"\\other"_s;
            const QString alias = t + u"\\docs\\工作链接"_s;
            QVERIFY2(makeJunction(alias, target), qPrintable(QString::number(::GetLastError())));
            QVERIFY(hasDirectoryLink(alias.toStdWString()));
            QVERIFY(hasDirectoryLink((alias + u"\\o.txt"_s).toStdWString()));
            QVERIFY(!hasDirectoryLink(target.toStdWString()));
            auto rules = branchRules(t);
            rules.excludedPaths.push_back(target.toStdWString()); // only reachable through the explicit alias
            auto options = walkedOptions(std::nullopt, rules);
            options.selectedDrives = std::vector<std::wstring>{t.left(2).toStdWString()};
            if (access != ntfs::VolumeAccess::Kind::None)
                options.volumeAccess = std::make_shared<ServiceDown>(access);
            options.extraFolders = {alias.toStdWString()};
            IndexService service(store.path(), options);
            service.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
            QVERIFY(!service.foldersOnly());
            QVERIFY(indexedIn(service, alias + u"\\o.txt"_s));
            QVERIFY(!indexedIn(service, target + u"\\o.txt"_s));
            QFile added(target + u"\\new.txt"_s);
            QVERIFY(added.open(QIODevice::WriteOnly));
            added.write("new");
            added.close();
            QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\new.txt"_s), 30'000);
            QVERIFY(QFile::rename(target + u"\\new.txt"_s, target + u"\\renamed.txt"_s));
            QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\renamed.txt"_s)
                    && !indexedIn(service, alias + u"\\new.txt"_s), 10'000);
            QVERIFY(QFile::remove(target + u"\\renamed.txt"_s));
            QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, alias + u"\\renamed.txt"_s), 10'000);
            options.extraFolders.clear();
            service.setOptions(options);
            QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, alias + u"\\o.txt"_s), 30'000);
            QVERIFY(indexedIn(service, t + u"\\docs\\a.txt"_s));
            options.extraFolders = {alias.toStdWString()};
            service.setOptions(options);
            QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\o.txt"_s) && allReady(service), 30'000);
            service.shutdown();
            // The target's real contents survive removing the alias from the index.
            QVERIFY(QFileInfo::exists(target + u"\\o.txt"_s));
        }
    }

    void explicitDirectoryLinksWithLiveMft()
    {
        if (!qEnvironmentVariableIsSet("WINSHUN_TEST_REAL_VOLUMES"))
            QSKIP("Opt in to reading the real MFT with WINSHUN_TEST_REAL_VOLUMES=1");
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring root = t.left(2).toStdWString();
        const QString pipe = qEnvironmentVariable("WINSHUN_TEST_LINK_PIPE");
        std::shared_ptr<ntfs::VolumeAccess> access;
        if (pipe.isEmpty()) {
            access = std::make_shared<ntfs::DirectVolumeAccess>();
        } else {
            auto throughService = std::make_shared<ntfs::ServiceVolumeAccess>(pipe.toStdWString());
            QVERIFY2(throughService->probe(), qPrintable(QString::fromStdWString(throughService->lastError())));
            access = std::move(throughService);
        }
        const auto volumes = listLocalVolumes(false, {}, std::vector<std::wstring>{root});
        QVERIFY(!volumes.empty() && volumes.front().ntfs);
        const auto journal = access->queryJournal(root, volumes.front().serial);
        if (!journal && pipe.isEmpty())
            QSKIP("Direct NTFS journal access requires an elevated test process");
        QVERIFY(journal);
        qInfo("Live %s access to %ls, journal %llu", pipe.isEmpty() ? "direct" : "service", root.c_str(), journal->id);
        // A fallback walk would otherwise make a broken MFT path look successful.
        QTest::failOnWarning(QRegularExpression(u"^WinShun: cannot read the MFT.*"_s));
        const QString target = t + u"\\other"_s;
        const QString alias = t + u"\\docs\\MFT链接"_s;
        QVERIFY(makeJunction(alias, target));
        auto rules = branchRules(t);
        rules.excludedPaths.push_back(target.toStdWString());
        auto options = walkedOptions(std::nullopt, rules);
        options.selectedDrives = std::vector<std::wstring>{root};
        options.volumeAccess = access;
        options.extraFolders = {alias.toStdWString()};
        IndexService service(store.path(), options);
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 180'000);
        QCOMPARE(service.volumeAccessKind(), access->kind());
        QCOMPARE(service.volumeRoots(), std::vector<std::wstring>{root});
        QVERIFY(indexedIn(service, alias + u"\\o.txt"_s));
        QVERIFY(!indexedIn(service, target + u"\\o.txt"_s));
        QFile added(target + u"\\mft-new.txt"_s);
        QVERIFY(added.open(QIODevice::WriteOnly));
        added.write("new");
        added.close();
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\mft-new.txt"_s), 10'000);
        QVERIFY(QFile::rename(target + u"\\mft-new.txt"_s, target + u"\\mft-renamed.txt"_s));
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\mft-renamed.txt"_s)
                && !indexedIn(service, alias + u"\\mft-new.txt"_s), 10'000);
        QVERIFY(QFile::remove(target + u"\\mft-renamed.txt"_s));
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, alias + u"\\mft-renamed.txt"_s), 10'000);
        options.extraFolders.clear();
        service.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, alias + u"\\o.txt"_s) && allReady(service), 30'000);
        QVERIFY(QFileInfo::exists(target + u"\\o.txt"_s));
        options.extraFolders = {alias.toStdWString()};
        service.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\o.txt"_s) && allReady(service), 30'000);
        options.selectedDrives = std::vector<std::wstring>{};
        service.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(service.volumeRoots().empty() && service.itemCount() == 0, 30'000);
        options.selectedDrives = std::vector<std::wstring>{root};
        service.setOptions(options);
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, alias + u"\\o.txt"_s) && allReady(service), 180'000);
        qInfo("Live %s: initial MFT, add/rename/delete, link removal/readd, drive deselection/reselection passed",
            pipe.isEmpty() ? "direct" : "service");
        service.shutdown();
        QVERIFY(QFileInfo::exists(target + u"\\o.txt"_s));
    }

    void chosenFoldersOnly()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        const std::wstring pics = (t + u"\\pics"_s).toStdWString();
        // As someone might write it: a small drive letter, a backslash at the end.
        const std::wstring picsTyped = (t.left(1).toLower() + t.mid(1) + u"\\PICS\\"_s).toStdWString();
        IndexService service(store.path(), walkedOptions(std::vector {docs, picsTyped}));
        QVERIFY(service.foldersOnly());
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QVERIFY(indexedIn(service, t + u"\\docs\\a.txt"_s));
        QVERIFY(indexedIn(service, t + u"\\docs\\sub\\b.txt"_s));
        QVERIFY(indexedIn(service, t + u"\\pics\\p.png"_s));
        QVERIFY(!indexedIn(service, t + u"\\other"_s));
        QCOMPARE(service.volumeRoots(), std::vector<std::wstring> {t.left(2).toStdWString()});
        {
            const auto index = service.index();
            const auto lock = index->readLock();
            QCOMPARE(index->roots().size(), std::size_t {1});
            // The root holds the first folder on the way, nothing else.
            std::size_t children = 0;
            for (EntryId c = index->entry(index->roots().front()).firstChild; c != kNoEntry; c = index->entry(c).nextSibling)
                ++children;
            QCOMPARE(children, std::size_t {1});
            // Spelled as on the disk, not as it was written.
            const EntryId p = index->findPath(pics);
            QVERIFY(p != kNoEntry);
            QCOMPARE(index->name(p), std::string_view("pics"));
        }
        const std::vector<IndexService::FolderStatus> statuses = service.folderStatuses();
        QCOMPARE(statuses.size(), std::size_t {2});
        QCOMPARE(statuses[0].path, docs);
        QCOMPARE(statuses[0].items, std::size_t {3}); // a.txt, sub, sub\b.txt
        QCOMPARE(statuses[0].failed, std::size_t {0});
        QVERIFY(statuses[0].lastVerified > 0);
        QVERIFY(statuses[0].watching); // through the volume's root
        QCOMPARE(statuses[1].items, std::size_t {1});
        service.shutdown();
        QVERIFY(QFileInfo::exists(indexfolder::scopePath(store.path())));
    }

    // Folders added are walked; those taken away go, with the folders on the
    // way that only they needed; one that takes in another stands for both;
    // with none chosen, nothing of the disk is left.
    void chosenFoldersChange()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        const std::wstring other = (t + u"\\other"_s).toStdWString();
        IndexService service(store.path(), walkedOptions(std::vector {docs}));
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QVERIFY(!indexedIn(service, t + u"\\other"_s));

        service.setScope(std::vector {docs, other});
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\other\\o.txt"_s) && allReady(service), 30'000);
        QCOMPARE(folderStates(service).size(), std::size_t {2});
        QVERIFY(indexedIn(service, t + u"\\docs\\sub\\b.txt"_s));
        QVERIFY(!indexedIn(service, t + u"\\pics"_s));

        service.setScope(std::vector {other});
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, t + u"\\docs"_s), 30'000);
        QVERIFY(indexedIn(service, t + u"\\other\\o.txt"_s));
        QCOMPARE(folderStates(service), std::vector {FolderState::Ready}); // its walk stands

        // The folder they are in: there by name only so far, walked now.
        service.setScope(std::vector {other, t.toStdWString()});
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\pics\\p.png"_s) && allReady(service), 30'000);
        QVERIFY(indexedIn(service, t + u"\\docs\\sub\\b.txt"_s));
        QVERIFY(indexedIn(service, t + u"\\other\\o.txt"_s));
        const std::vector<IndexService::FolderStatus> statuses = service.folderStatuses();
        QCOMPARE(statuses.size(), std::size_t {1});
        QCOMPARE(statuses.front().path, t.toStdWString());
        QCOMPARE(statuses.front().items, std::size_t {8});

        // None (the list emptied): nothing of the disk.
        service.setScope(std::vector<std::wstring> {});
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, t), 30'000);
        QVERIFY(service.folderStatuses().empty());
        QVERIFY(service.foldersOnly());
        QVERIFY(service.volumeRoots().empty());
        service.shutdown();
    }

    // Whole disks to chosen folders and back, in place: what is not chosen
    // goes (the chosen folders' entries stay, as current as they were), and
    // back to whole disks, the disk is walked whole again. With a way to the
    // journals (the index service, even one not answering for the moment),
    // disks are whole whatever folders are set: as before GitHub #52.
    void wholeDisksAndFolders()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        const CrawlRules rules = branchRules(t);
        IndexService service(store.path(), walkedOptions(std::nullopt, rules));
        QVERIFY(!service.foldersOnly());
        service.start();
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), IndexService::State::Ready, 60'000);
        QVERIFY(indexedIn(service, t + u"\\other\\o.txt"_s));
        QVERIFY(indexedIn(service, t + u"\\docs\\sub\\b.txt"_s));
        QVERIFY(service.folderStatuses().empty());

        service.setScope(std::vector {docs});
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, t + u"\\other"_s) && allReady(service), 30'000);
        QVERIFY(indexedIn(service, t + u"\\docs\\sub\\b.txt"_s));
        std::vector<IndexService::FolderStatus> statuses = service.folderStatuses();
        QCOMPARE(statuses.size(), std::size_t {1});
        QCOMPARE(statuses.front().items, std::size_t {3}); // counted: the index had them current
        QVERIFY(statuses.front().lastVerified > 0);

        // Made outside docs meanwhile: only the walk of the whole disk finds it.
        QFile made(t + u"\\other\\made.txt"_s);
        QVERIFY(made.open(QIODevice::WriteOnly));
        made.close();
        QTest::qWait(500);
        QVERIFY(!indexedIn(service, t + u"\\other\\made.txt"_s));
        service.setScope(std::nullopt);
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\other\\made.txt"_s), 60'000);
        QVERIFY(indexedIn(service, t + u"\\pics\\p.png"_s));
        QVERIFY(service.folderStatuses().empty());
        // A drive's root chosen: that drive whole.
        const std::wstring drive = t.left(2).toStdWString();
        service.setScope(std::vector {drive, docs});
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(service), std::vector {FolderState::WholeDrive}, 30'000);
        QTest::qWait(500);
        QVERIFY(indexedIn(service, t + u"\\pics\\p.png"_s));
        service.shutdown();

        // Folders set, and the index service there but not answering: whole disks.
        QTemporaryDir store2;
        IndexService::Options options = walkedOptions(std::vector {docs}, rules);
        options.volumeAccess = std::make_shared<ServiceDown>();
        IndexService served(store2.path(), options);
        QVERIFY(!served.foldersOnly());
        served.start();
        QTRY_COMPARE_WITH_TIMEOUT(served.state(), IndexService::State::Ready, 60'000);
        QVERIFY(indexedIn(served, t + u"\\other\\o.txt"_s));
        QVERIFY(indexedIn(served, t + u"\\pics\\p.png"_s));
        served.shutdown();
    }

    // scope.json goes with the snapshot: written after it, with its size and
    // time. One that does not (a Win顺 from before GitHub #52 saved over it)
    // tells nothing: every chosen folder is walked, and whatever else the
    // snapshot holds goes.
    void scopeFileNotMatching()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        const IndexService::Options options = walkedOptions(std::vector {docs});
        {
            IndexService first(store.path(), options);
            first.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(first), 30'000);
            first.shutdown();
        }
        QVERIFY(QFileInfo::exists(indexfolder::scopePath(store.path())));
        // Made while Win顺 is not running: only a walk of docs finds it.
        QFile later(t + u"\\docs\\later.txt"_s);
        QVERIFY(later.open(QIODevice::WriteOnly));
        later.close();
        {
            // It goes with the snapshot: docs as it was, not walked (no rescan at startup).
            IndexService second(store.path(), options);
            second.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(second), 30'000);
            QTest::qWait(1000);
            QVERIFY(!indexedIn(second, t + u"\\docs\\later.txt"_s));
            QVERIFY(second.folderStatuses().front().lastVerified > 0);
            second.shutdown();
        }
        // The snapshot saved again by another, with more in it.
        const QString path = indexfolder::snapshotPath(store.path());
        std::optional<snapshot::Contents> contents = snapshot::load(path);
        QVERIFY(contents && contents->rules);
        {
            const auto lock = contents->index->writeLock();
            const EntryId folder = contents->index->findPath(t.toStdWString());
            const EntryId inDocs = contents->index->findPath(docs);
            QVERIFY(folder != kNoEntry && inDocs != kNoEntry);
            contents->index->add(folder, "not-chosen", EntryFlag::Directory);
            contents->index->add(inDocs, "not-on-disk.txt", 0);
        }
        {
            const auto lock = contents->index->readLock();
            QVERIFY(snapshot::save(*contents->index, contents->volumes, contents->journals, *contents->rules, path));
        }
        IndexService third(store.path(), options);
        third.start();
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(third, t + u"\\docs\\later.txt"_s), 30'000);
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(third, t + u"\\docs\\not-on-disk.txt"_s) && allReady(third), 30'000);
        QVERIFY(!indexedIn(third, t + u"\\not-chosen"_s));
        third.shutdown();
    }

    // The volume's root is watched; what happens outside the chosen folders
    // is dropped, and what happens in them comes in at once. Half a rename
    // that leaves or enters one is a removal or an addition by itself.
    void changesOutsideChosenFolders()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        IndexService service(store.path(), walkedOptions(std::vector {(t + u"\\docs"_s).toStdWString()}));
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QDir base(tree.path());
        const auto touch = [&](const QString& rel) {
            QFile f(base.filePath(rel));
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        touch(u"other/outside.txt"_s);
        QVERIFY(base.mkpath(u"made"_s));
        touch(u"docs/inside.txt"_s); // after them: once it is in, they were seen too
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\inside.txt"_s), 10'000);
        QVERIFY(!indexedIn(service, t + u"\\other"_s));
        QVERIFY(!indexedIn(service, t + u"\\made"_s));

        // Moved in, with what is in it; moved out again.
        QVERIFY(base.rename(u"other"_s, u"docs/moved"_s));
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\moved\\o.txt"_s), 10'000);
        QVERIFY(indexedIn(service, t + u"\\docs\\moved\\outside.txt"_s));
        QVERIFY(base.rename(u"docs/moved"_s, u"elsewhere"_s));
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, t + u"\\docs\\moved"_s), 10'000);
        QVERIFY(!indexedIn(service, t + u"\\elsewhere"_s));
        // One moved out and another in at once: neither stands for the other.
        QVERIFY(base.mkpath(u"docs/leaving/inner"_s));
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\leaving\\inner"_s), 10'000);
        QVERIFY(base.rename(u"docs/leaving"_s, u"gone"_s));
        QVERIFY(base.rename(u"elsewhere"_s, u"docs/arrived"_s));
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\arrived\\o.txt"_s), 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, t + u"\\docs\\leaving"_s), 10'000);
        QVERIFY(!indexedIn(service, t + u"\\docs\\arrived\\inner"_s));
        service.shutdown();
    }

    // GitHub #52 review: a folder above a chosen one renamed in case only
    // (up -> UP): a move. Its entries stay, spelled as the disk has them now,
    // and nothing is walked again; not gone for a moment either, though
    // Windows first says the folder was removed (its 8.3 name goes).
    void aboveRenamedInCase()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QDir base(tree.path());
        QVERIFY(base.mkpath(u"up/docs/sub"_s));
        {
            QFile f(base.filePath(u"up/docs/sub/b.txt"_s));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        const QString t = QDir::toNativeSeparators(tree.path());
        IndexService service(store.path(), walkedOptions(std::vector {(t + u"\\up\\docs"_s).toStdWString()}));
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        const std::int64_t walked = service.folderStatuses().front().lastVerified;
        const std::wstring up = (t + u"\\up"_s).toStdWString();
        const auto spelled = [&] {
            const std::shared_ptr<FileIndex> index = service.index();
            const auto lock = index->readLock();
            const EntryId e = index->findPath(up);
            return e != kNoEntry ? std::string(index->name(e)) : std::string();
        };
        QCOMPARE(spelled(), std::string("up"));
        QVERIFY(::MoveFileW(up.c_str(), (t + u"\\UP"_s).toStdWString().c_str()));
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500); // past a walk 3 s later
        while (std::chrono::steady_clock::now() < until) {
            QVERIFY(indexedIn(service, t + u"\\up\\docs\\sub\\b.txt"_s)); // not gone, not for a moment
            QTest::qWait(25);
        }
        QCOMPARE(spelled(), std::string("UP"));
        QCOMPARE(folderStates(service), std::vector {FolderState::Ready});
        QCOMPARE(service.folderStatuses().front().lastVerified, walked);
        service.shutdown();
    }

    // The same, one batch at a time (the watcher gives them so): what is
    // applied, and which chosen folders are walked again because of it.
    void scopedChangeBatches()
    {
        using Kind = FsChange::Kind;
        const std::vector<std::wstring> chosen {L"C:\\Users\\me\\Documents", L"C:\\Work"};
        const std::vector<std::wstring> none;
        const auto chosenOn = [&](std::wstring_view root) -> const std::vector<std::wstring>& {
            return root == L"C:" ? chosen : none;
        };
        using Change = std::pair<Kind, std::wstring>;
        const auto applied = [](const ScopedChanges& s) {
            std::vector<Change> out;
            for (const FsChange& c : s.apply)
                out.emplace_back(c.kind, c.path);
            return out;
        };

        // In a chosen folder: kept; elsewhere on its disk: dropped; on a disk indexed whole: kept.
        ScopedChanges s = changesInScope({{Kind::Added, L"C:\\Users\\me\\Documents\\a.txt"},
                                             {Kind::Added, L"C:\\Users\\me\\Desktop\\b.txt"},
                                             {Kind::Removed, L"C:\\Windows\\Temp\\x.tmp"},
                                             {Kind::Added, L"C:\\Work2\\c.txt"}, {Kind::Added, L"D:\\d.txt"}},
            chosenOn);
        QCOMPARE(applied(s),
            (std::vector<Change> {{Kind::Added, L"C:\\Users\\me\\Documents\\a.txt"}, {Kind::Added, L"D:\\d.txt"}}));
        QVERIFY(s.owed.empty());
        QVERIFY(!s.wholeLost);

        // At or above a chosen folder: what takes it away is applied, and it is walked again.
        s = changesInScope({{Kind::Removed, L"C:\\Users\\me"}, {Kind::Added, L"C:\\Users\\me"},
                               {Kind::RenamedFrom, L"C:\\Work"}, {Kind::RenamedTo, L"C:\\Work.old"}},
            chosenOn);
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::Removed, L"C:\\Users\\me"}, {Kind::Removed, L"C:\\Work"}}));
        QCOMPARE(s.owed, chosen);
        QVERIFY(!s.lost); // nothing was lost: walked at once, never put off (LostEventsBackoff)

        // Renames: in a chosen folder, a move; out of one and into it in the
        // same batch, a removal and an addition (paired, the one left would
        // be moved to where the other came in).
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Work\\a"}, {Kind::RenamedTo, L"C:\\Work\\b"},
                               {Kind::RenamedFrom, L"C:\\Work\\leaving"}, {Kind::RenamedTo, L"C:\\Elsewhere\\gone"},
                               {Kind::RenamedFrom, L"C:\\Elsewhere\\x"}, {Kind::RenamedTo, L"C:\\Work\\arrived"}},
            chosenOn);
        QCOMPARE(applied(s),
            (std::vector<Change> {{Kind::RenamedFrom, L"C:\\Work\\a"}, {Kind::RenamedTo, L"C:\\Work\\b"},
                {Kind::Removed, L"C:\\Work\\leaving"}, {Kind::Added, L"C:\\Work\\arrived"}}));
        QVERIFY(s.owed.empty());

        // Events lost on the volume's root: its chosen folders are walked
        // again; on a folder watched by itself, that one; on a disk indexed
        // whole, that disk.
        s = changesInScope({{Kind::Overflow, L"C:"}}, chosenOn);
        QVERIFY(s.apply.empty());
        QCOMPARE(s.owed, chosen);
        QVERIFY(s.lost);
        QVERIFY(!s.wholeLost);
        s = changesInScope({{Kind::Overflow, L"C:\\Work"}}, chosenOn);
        QCOMPARE(s.owed, std::vector<std::wstring> {L"C:\\Work"});
        QVERIFY(s.lost);
        s = changesInScope({{Kind::Overflow, L"D:"}}, chosenOn);
        QVERIFY(s.owed.empty());
        QVERIFY(!s.lost);
        QVERIFY(s.wholeLost);

        // GitHub #52 review: a folder at or above chosen ones renamed, both
        // halves there. In case only: a move, its entries going along, and
        // nothing walked again (the folders are where they were). To above
        // other chosen ones: a move too, theirs walked again; only a move
        // (moveOnly), never walked in whole should it fail. Into a chosen
        // folder: what is there of it is the way to others, not all that is
        // in it: gone, and walked in whole where it came.
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Users\\ME"},
                               {Kind::RenamedFrom, L"C:\\Work"}, {Kind::RenamedTo, L"C:\\WORK"}},
            chosenOn);
        QCOMPARE(applied(s),
            (std::vector<Change> {{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Users\\ME"},
                {Kind::RenamedFrom, L"C:\\Work"}, {Kind::RenamedTo, L"C:\\WORK"}}));
        QVERIFY(!s.apply[0].moveOnly && s.apply[1].moveOnly && s.apply[3].moveOnly);
        QVERIFY(s.owed.empty());
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Work"}}, chosenOn);
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Work"}}));
        QVERIFY(s.apply[1].moveOnly);
        QCOMPARE(s.owed, chosen);
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Work\\me"}}, chosenOn);
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::Removed, L"C:\\Users\\me"}, {Kind::Added, L"C:\\Work\\me"}}));
        QVERIFY(!s.apply[1].moveOnly);
        QCOMPARE(s.owed, std::vector<std::wstring> {L"C:\\Users\\me\\Documents"});
        // ... and out of one to above another: gone from there, the other walked again.
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Work\\me"}, {Kind::RenamedTo, L"C:\\Users\\me"}}, chosenOn);
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::Removed, L"C:\\Work\\me"}}));
        QCOMPARE(s.owed, std::vector<std::wstring> {L"C:\\Users\\me\\Documents"});

        // Above chosen ones, said to be gone while it is there all the same
        // (Windows says so a batch before a rename in case only): nothing
        // goes, none is walked again; half a rename whose other half comes
        // later, too. Gone indeed, or half of a rename here: as before. In a
        // chosen folder: as it says.
        const auto there = [](const std::wstring& path) { return path == L"C:\\Users\\me"; };
        s = changesInScope({{Kind::Removed, L"C:\\Users\\me"}}, chosenOn, there);
        QVERIFY(s.apply.empty());
        QVERIFY(s.owed.empty());
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Users\\me"}}, chosenOn, there);
        QVERIFY(s.apply.empty());
        QVERIFY(s.owed.empty());
        s = changesInScope({{Kind::Removed, L"C:\\Work"}}, chosenOn, there);
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::Removed, L"C:\\Work"}}));
        QCOMPARE(s.owed, std::vector<std::wstring> {L"C:\\Work"});
        s = changesInScope({{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Users\\ME"}}, chosenOn, there);
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::RenamedFrom, L"C:\\Users\\me"}, {Kind::RenamedTo, L"C:\\Users\\ME"}}));
        QVERIFY(s.owed.empty());
        s = changesInScope({{Kind::Removed, L"C:\\Work\\sub"}}, chosenOn, [](const std::wstring&) { return true; });
        QCOMPARE(applied(s), (std::vector<Change> {{Kind::Removed, L"C:\\Work\\sub"}}));
        QVERIFY(s.owed.empty());
    }

    // GitHub #52 review: a moved folder's new name that only ends the move
    // (moveOnly) brings nothing in when the move cannot be made: not walked
    // in whole, as a folder above chosen ones would be (all of a profile).
    void moveOnlyChange()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(u"there/sub"_s));
        const std::wstring t = QDir::toNativeSeparators(dir.path()).toStdWString();
        const auto build = [&](FileIndex& index) {
            auto lock = index.writeLock();
            EntryId e = index.addRoot(QString::fromStdWString(t.substr(0, 2)).toStdString());
            for (const QString& part : QString::fromStdWString(t.substr(3)).split(u'\\'))
                e = index.add(e, part.toStdString(), EntryFlag::Directory);
        };
        const Crawler crawler(CrawlRules {});
        for (const bool moveOnly : {true, false}) {
            FileIndex index;
            build(index);
            FsChange to {FsChange::Kind::RenamedTo, t + L"\\there"};
            to.moveOnly = moveOnly;
            // Moved from a folder the index does not have: the move cannot be made.
            applyChanges(index, {{FsChange::Kind::RenamedFrom, t + L"\\gone"}, to}, crawler, {});
            const auto lock = index.readLock();
            QCOMPARE(index.findPath(t + L"\\there") != kNoEntry, !moveOnly);
            QCOMPARE(index.findPath(t + L"\\there\\sub") != kNoEntry, !moveOnly);
        }
    }

    // A chosen folder deleted: its entries go, and it is "not found"; made
    // again, it is walked. On a disk being ejected (suspended), nothing is
    // looked at and the entries stay.
    void chosenFolderGone()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        const std::wstring pics = (t + u"\\pics"_s).toStdWString();
        const std::wstring drive = t.left(2).toStdWString();
        IndexService service(store.path(), walkedOptions(std::vector {docs, pics}));
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);

        service.suspendVolume(drive);
        QVERIFY(!service.folderStatuses().front().watching);
        QVERIFY(QDir(t + u"\\pics"_s).removeRecursively());
        service.verifyFolder(pics); // not while the disk is away
        QTest::qWait(2500); // the pass of the other volumes, which leaves this one be
        QVERIFY(indexedIn(service, t + u"\\pics\\p.png"_s));
        QCOMPARE(folderStates(service), (std::vector {FolderState::Ready, FolderState::Ready}));
        service.resumeVolume(drive); // back: walked
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(service), (std::vector {FolderState::Ready, FolderState::NotFound}), 30'000);
        QVERIFY(!indexedIn(service, t + u"\\pics"_s));
        QVERIFY(indexedIn(service, t + u"\\docs\\a.txt"_s));
        QTRY_VERIFY_WITH_TIMEOUT(service.folderStatuses().front().watching, 10'000);

        // Deleted while watched: the entries go at once, the state a moment later.
        QVERIFY(QDir(t + u"\\docs"_s).removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(service, t + u"\\docs"_s), 10'000);
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(service), (std::vector {FolderState::NotFound, FolderState::NotFound}), 30'000);
        // Asked to be walked, still not there: still not found.
        service.verifyFolder(docs);
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(service), (std::vector {FolderState::NotFound, FolderState::NotFound}), 30'000);

        // Made again: walked (the change above them asks for it).
        QVERIFY(makeScopeTree(tree.path()));
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\sub\\b.txt"_s) && indexedIn(service, t + u"\\pics\\p.png"_s), 30'000);
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QVERIFY(!indexedIn(service, t + u"\\other"_s));
        service.shutdown();
    }

    // When a chosen folder is walked again: no sooner than the interval after
    // its last walk, nor than 60 times as long as that took; then once the
    // user is away, or two hours later anyway.
    void folderVerifySchedule()
    {
        using namespace std::chrono_literals;
        const std::int64_t walked = 1'800'000'000;
        const auto at = [&](std::chrono::hours h, std::chrono::minutes m = {}) {
            return walked + std::chrono::duration_cast<std::chrono::seconds>(h + m).count();
        };
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, at(1h)), VerifyNow::No);
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, at(23h, 59min)), VerifyNow::No);
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, at(24h)), VerifyNow::IfAway);
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, at(25h, 59min)), VerifyNow::IfAway);
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, at(26h)), VerifyNow::Yes);
        // Walked for 30 minutes: not again for 30 hours.
        QCOMPARE(folderVerifyDue(walked, 30min, 24h, at(29h)), VerifyNow::No);
        QCOMPARE(folderVerifyDue(walked, 30min, 24h, at(30h)), VerifyNow::IfAway);
        QCOMPARE(folderVerifyDue(walked, 30min, 1h, at(31h)), VerifyNow::IfAway);
        QCOMPARE(folderVerifyDue(walked, 30min, 1h, at(32h)), VerifyNow::Yes);
        // Every hour, a quick one.
        QCOMPARE(folderVerifyDue(walked, 1s, 1h, at(0h, 59min)), VerifyNow::No);
        QCOMPARE(folderVerifyDue(walked, 1s, 1h, at(1h)), VerifyNow::IfAway);
        // Never walked: now. Only at startup (0): never.
        QCOMPARE(folderVerifyDue(0, 0ms, 24h, walked), VerifyNow::Yes);
        QCOMPARE(folderVerifyDue(walked, 2s, 0h, at(100h)), VerifyNow::No);
        QCOMPARE(folderVerifyDue(0, 0ms, 0h, walked), VerifyNow::No);
        // The clock put back (the last walk "in the future"): from now on, when away.
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, walked - 7200), VerifyNow::IfAway);
        QCOMPARE(folderVerifyDue(walked, 2s, 24h, walked - 60), VerifyNow::No);
    }

    // Which chosen folders the regular check walks again: those walked
    // before and due; whether the user is away is asked once, and only when
    // it matters.
    void foldersDueNow()
    {
        using namespace std::chrono_literals;
        using State = IndexService::FolderStatus::State;
        const std::int64_t now = 1'800'000'000;
        const std::int64_t old = now - 30 * 3600; // due, and two hours past it
        const auto status = [](const wchar_t* path, State state, std::int64_t verified) {
            IndexService::FolderStatus s;
            s.path = path;
            s.state = state;
            s.lastVerified = verified;
            s.took = 1s;
            return s;
        };
        const std::vector<IndexService::FolderStatus> statuses {
            status(L"C:\\old", State::Ready, old),
            status(L"C:\\due", State::Ready, now - 25 * 3600), // due an hour ago: once the user is away
            status(L"C:\\new", State::Ready, now - 3600),
            status(L"C:\\gone", State::NotFound, old),
            status(L"C:\\locked", State::Unreadable, old),
            status(L"C:\\waiting", State::Waiting, 0), // owed a walk already
            status(L"C:\\walking", State::Verifying, old),
            status(L"C:\\left-out", State::Excluded, old),
            status(L"E:\\stick", State::Offline, old),
            status(L"D:", State::WholeDrive, old),
        };
        int asked = 0;
        const auto present = [&] {
            ++asked;
            return false;
        };
        const auto away = [&] {
            ++asked;
            return true;
        };
        QCOMPARE(foldersDue(statuses, 24h, now, present), (std::vector<std::wstring> {L"C:\\old", L"C:\\gone", L"C:\\locked"}));
        QCOMPARE(asked, 1);
        QCOMPARE(foldersDue(statuses, 24h, now, away),
            (std::vector<std::wstring> {L"C:\\old", L"C:\\due", L"C:\\gone", L"C:\\locked"}));
        QVERIFY(foldersDue(statuses, 0h, now, away).empty()); // only at startup
        asked = 0;
        QVERIFY(foldersDue({statuses[2]}, 24h, now, away).empty());
        QCOMPARE(asked, 0);
    }

    // Lost events of chosen folders: walked again 3 s later; when that keeps
    // happening, 30 s, then 5 minutes; 3 s again after 10 calm minutes.
    void lostEventsBackoff()
    {
        using namespace std::chrono_literals;
        LostEventsBackoff backoff;
        const auto t = std::chrono::steady_clock::time_point {} + 1h;
        QVERIFY(backoff.next(t) == 3s);
        QVERIFY(backoff.next(t + 10s) == 30s);
        QVERIFY(backoff.next(t + 50s) == 5min);
        QVERIFY(backoff.next(t + 6min) == 5min);
        QVERIFY(backoff.next(t + 16min) == 3s); // ten minutes after the last time
        QVERIFY(backoff.next(t + 17min) == 30s);
    }

    // GitHub #52 review: a pass that ends clears its flag before it looks at
    // what is owed. A folder asked to be walked at that very moment (立即核对)
    // is seen by the pass, or whoever asked sees no pass running and starts
    // one; never neither.
    void passOverOrder()
    {
        std::atomic<bool> running = true;
        std::vector<std::wstring> owed;
        bool launched = false;
        // Asked for right after the pass looked: the worst moment.
        const bool walk = passOver(running, [&] {
            const bool any = !owed.empty();
            owed.push_back(L"C:\\Work"); // verifyFolder()
            launched = !running.load(); // walkOwedFolders(): no pass running, one is started
            return any;
        });
        QVERIFY(walk || launched);
        QVERIFY(!running.load());
        // Owed before: the end of the pass takes them up.
        running = true;
        QVERIFY(passOver(running, [] { return true; }));
        QVERIFY(!running.load());
    }

    // A chosen folder whose regular check is due is walked again by itself:
    // what changed while Win顺 was not running comes in then, not at startup
    // (no startup rescan here), and not when only startup checks are wanted.
    void scheduledFolderCheck()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        {
            IndexService service(store.path(), walkedOptions(std::vector {docs}));
            service.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
            service.shutdown();
        }
        // Its last walk 30 hours ago, as scope.json has it; a file came since.
        QFile scopeFile(indexfolder::scopePath(store.path()));
        QVERIFY(scopeFile.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(scopeFile.readAll()).object();
        scopeFile.close();
        QJsonArray folders = root.value(u"folders"_s).toArray();
        QCOMPARE(folders.size(), 1);
        QJsonObject folder = folders[0].toObject();
        const qint64 walked = folder.value(u"verified"_s).toInteger();
        QVERIFY(walked > 0);
        folder[u"verified"_s] = walked - 30 * 3600;
        folders[0] = folder;
        root[u"folders"_s] = folders;
        QVERIFY(scopeFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(scopeFile.write(QJsonDocument(root).toJson()) > 0);
        scopeFile.close();
        QFile later(t + u"\\docs\\later.txt"_s);
        QVERIFY(later.open(QIODevice::WriteOnly));
        later.close();

        IndexService::Options options = walkedOptions(std::vector {docs});
        options.folderVerifyCheckMs = 200;
        {
            options.folderVerifyEvery = std::chrono::hours(0); // only at startup: not now
            IndexService service(store.path(), options);
            service.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
            QTest::qWait(1500);
            QVERIFY(!indexedIn(service, t + u"\\docs\\later.txt"_s));
            QVERIFY(service.folderStatuses().front().lastVerified < walked);
            service.shutdown();
        }
        options.folderVerifyEvery = std::chrono::hours(24);
        IndexService service(store.path(), options);
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(service, t + u"\\docs\\later.txt"_s), 30'000);
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service) && service.folderStatuses().front().lastVerified >= walked, 10'000);
        QVERIFY(!indexedIn(service, t + u"\\other"_s));
        service.shutdown();
    }

    // GitHub #52 review: folders chosen while the index service is on its
    // way (the index waits for it, accessWaitMs), with a disk followed
    // through its journal so far. Nothing of a disk without a chosen folder
    // goes while it waits; the service there, the disks are whole and stay
    // so; not there in time, only the chosen folders are left.
    void chosenFoldersAwaitService()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::optional<VolumeInfo> volume = volumeOfPath(t);
        QVERIFY(volume);
        if (!volume->ntfs)
            QSKIP("the temporary folder is not on an NTFS volume");
        const std::wstring away = absentDrive() + L"\\Work"; // the folder chosen, on no disk here
        QVERIFY(away.size() > 2);
        const CrawlRules rules = branchRules(t);
        // As the index service left it: the volume followed through its journal.
        const auto followed = [&] {
            return resaveSnapshot(store.path(), [](snapshot::Contents& contents) {
                for (JournalPosition& j : contents.journals)
                    j = {0x5eed, 1};
            });
        };
        {
            IndexService first(store.path(), walkedOptions(std::nullopt, rules));
            first.start();
            QTRY_COMPARE_WITH_TIMEOUT(first.state(), IndexService::State::Ready, 60'000);
            first.shutdown();
        }
        QVERIFY(followed());

        IndexService::Options options = walkedOptions(std::vector {away}, rules);
        options.accessWaitMs = 60'000;
        options.rescanDelayMs = 200;
        {
            IndexService service(store.path(), options);
            QVERIFY(service.foldersOnly());
            service.start();
            QTRY_COMPARE_WITH_TIMEOUT(service.state(), IndexService::State::Ready, 10'000);
            QTest::qWait(500);
            QVERIFY(indexedIn(service, t + u"\\other\\o.txt"_s)); // waiting: nothing went
            service.setVolumeAccess(std::make_shared<ServiceDown>());
            QVERIFY(!service.foldersOnly());
            QTest::qWait(1500); // walked whole (its journal cannot be read now), never emptied
            QTRY_VERIFY_WITH_TIMEOUT(!service.isRefreshing(), 30'000);
            QVERIFY(indexedIn(service, t + u"\\other\\o.txt"_s));
            QCOMPARE(service.volumeRoots(), std::vector {volume->root});
            service.shutdown();
        }

        // Not there in time: only the chosen folder, whose disk is not here.
        QVERIFY(followed());
        options.accessWaitMs = 1500;
        IndexService late(store.path(), options);
        late.start();
        QTRY_VERIFY_WITH_TIMEOUT(!indexedIn(late, t), 30'000);
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(late), std::vector {FolderState::Offline}, 10'000);
        QVERIFY(late.volumeRoots().empty());
        late.shutdown();
    }

    // GitHub #52 review: a chosen folder written otherwise than the disk has
    // it, in letters beyond ASCII (доки for ДОКИ): the index, whose names are
    // the disk's, finds it all the same; walked once and ready, not put off
    // every few seconds. One it cannot find even so (an 8.3 short name) is
    // told unreadable at once, and not looked for again and again.
    void chosenFolderSpelledOtherwise()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QDir base(tree.path());
        QVERIFY(base.mkpath(u"ДОКИ/внутри"_s) && base.mkpath(u"longfoldername/sub"_s));
        for (const QString& file : {u"ДОКИ/a.txt"_s, u"longfoldername/sub/b.txt"_s}) {
            QFile f(base.filePath(file));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        const QString t = QDir::toNativeSeparators(tree.path());
        IndexService service(store.path(), walkedOptions(std::vector {(t + u"\\доки"_s).toStdWString()}));
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QVERIFY(indexedIn(service, t + u"\\ДОКИ\\a.txt"_s));
        QVERIFY(indexedIn(service, t + u"\\ДОКИ\\внутри"_s));
        const std::int64_t walked = service.folderStatuses().front().lastVerified;
        QTest::qWait(4500); // past the 3 s after which one not found was walked again
        QCOMPARE(folderStates(service), std::vector {FolderState::Ready});
        QCOMPARE(service.folderStatuses().front().lastVerified, walked);

        // An 8.3 name, where the volume keeps them: the index has the long one.
        const std::wstring full = (t + u"\\longfoldername"_s).toStdWString();
        wchar_t shortPath[MAX_PATH] {};
        const DWORD n = ::GetShortPathNameW(full.c_str(), shortPath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH || win32::equalsIgnoreCase(std::wstring_view(shortPath, n), full)) {
            service.shutdown();
            QSKIP("no 8.3 names on this volume");
        }
        service.setScope(std::vector {std::wstring(shortPath, n)});
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(service), std::vector {FolderState::Unreadable}, 10'000);
        const std::int64_t told = service.folderStatuses().front().lastVerified;
        QTest::qWait(4500);
        QCOMPARE(folderStates(service), std::vector {FolderState::Unreadable});
        QCOMPARE(service.folderStatuses().front().lastVerified, told);
        service.shutdown();
    }

    // GitHub #52 review: a chosen folder whose disk is not here is offline,
    // whatever scope.json says of how it stood when it last was.
    void offlineOverScopeFile()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const std::wstring docs = (t + u"\\docs"_s).toStdWString();
        const std::wstring away = absentDrive() + L"\\Work";
        QVERIFY(away.size() > 2);
        {
            IndexService first(store.path(), walkedOptions(std::vector {docs}));
            first.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(first), 30'000);
            first.shutdown();
        }
        // Ready when it was last here, as scope.json has it (still with the snapshot).
        QFile scopeFile(indexfolder::scopePath(store.path()));
        QVERIFY(scopeFile.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(scopeFile.readAll()).object();
        scopeFile.close();
        QJsonArray folders = root.value(u"folders"_s).toArray();
        QCOMPARE(folders.size(), 1);
        QJsonObject was = folders[0].toObject();
        QCOMPARE(was.value(u"state"_s).toString(), u"ready"_s);
        was[u"path"_s] = QString::fromStdWString(away);
        folders.append(was);
        root[u"folders"_s] = folders;
        QVERIFY(scopeFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(scopeFile.write(QJsonDocument(root).toJson()) > 0);
        scopeFile.close();

        IndexService second(store.path(), walkedOptions(std::vector {docs, away}));
        second.start();
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(second), (std::vector {FolderState::Ready, FolderState::Offline}), 10'000);
        QTest::qWait(500);
        QCOMPARE(folderStates(second), (std::vector {FolderState::Ready, FolderState::Offline}));
        second.shutdown();
    }

    // GitHub #52 review: a chosen folder on a drive left out whole ("C:" in
    // 不搜索的文件夹) says so, not that its disk is away.
    void chosenFolderOnExcludedDrive()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        CrawlRules rules;
        rules.excludedPaths.push_back(t.left(2).toStdWString());
        IndexService service(store.path(), walkedOptions(std::vector {(t + u"\\docs"_s).toStdWString()}, rules));
        service.start();
        QTRY_COMPARE_WITH_TIMEOUT(folderStates(service), std::vector {FolderState::Excluded}, 10'000);
        QVERIFY(!indexedIn(service, t + u"\\docs"_s));
        QVERIFY(service.volumeRoots().empty());
        service.shutdown();
    }

    // GitHub #52 review: a snapshot whose scope.json does not go with it
    // (saved again by an older Win顺, or the file could not be written) may
    // hold a disk's chosen folders only. With whole disks now, the disks
    // walked are walked whole a while after startup, even with no rescan
    // asked for at startup.
    void staleScopeFileWholeDisks()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        QVERIFY(makeScopeTree(tree.path()));
        const QString t = QDir::toNativeSeparators(tree.path());
        const CrawlRules rules = branchRules(t);
        {
            IndexService first(store.path(), walkedOptions(std::vector {(t + u"\\docs"_s).toStdWString()}, rules));
            first.start();
            QTRY_VERIFY_WITH_TIMEOUT(allReady(first), 30'000);
            QVERIFY(!indexedIn(first, t + u"\\other"_s));
            first.shutdown();
        }
        QVERIFY(QFileInfo::exists(indexfolder::scopePath(store.path())));
        QVERIFY(resaveSnapshot(store.path()));
        IndexService::Options options = walkedOptions(std::nullopt, rules); // rescanOnStartup off
        options.rescanDelayMs = 200;
        IndexService second(store.path(), options);
        second.start();
        QTRY_VERIFY_WITH_TIMEOUT(indexedIn(second, t + u"\\other\\o.txt"_s), 30'000);
        QVERIFY(indexedIn(second, t + u"\\pics\\p.png"_s));
        second.shutdown();
    }

    // scope.json is an index file: counted, moved and deleted with the others,
    // and no reason to refuse a folder as the index's.
    void scopeFileIsAnIndexFile()
    {
        QTemporaryDir dir;
        const auto write = [&](const QString& name, int bytes) {
            QFile f(QDir(dir.path()).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(QByteArray(bytes, 'x')), qint64 {bytes});
        };
        write(u"index.bin"_s, 100);
        write(u"scope.json"_s, 10);
        write(u"scope.json.a1B2c3"_s, 5); // one being saved when Win顺 stopped
        QCOMPARE(indexfolder::scopePath(dir.path()), QDir(dir.path()).filePath(u"scope.json"_s));
        QVERIFY(indexfolder::onlyIndexFiles(dir.path()));
        QCOMPARE(indexfolder::size(dir.path()), std::uint64_t {110});
        QTemporaryDir to;
        QVERIFY(!indexfolder::copy(dir.path(), to.path()));
        QCOMPARE(QFileInfo(indexfolder::scopePath(to.path())).size(), qint64 {10});
        indexfolder::remove(dir.path());
        QVERIFY(!QFileInfo::exists(indexfolder::scopePath(dir.path())));
        write(u"notes.txt"_s, 1);
        QVERIFY(!indexfolder::onlyIndexFiles(dir.path()));
    }

    void removeExcludedRules()
    {
        // New exclusions are applied to an index in place.
        FileIndex index;
        const EntryId c = index.addRoot("C:");
        const EntryId a = index.add(c, "a", EntryFlag::Directory);
        const EntryId modules = index.add(a, "node_modules", EntryFlag::Directory);
        index.add(modules, "x.js", 0);
        const EntryId svn = index.add(a, ".svn", EntryFlag::Directory);
        const EntryId pristine = index.add(svn, "pristine", EntryFlag::Directory);
        index.add(pristine, "p", 0);
        const EntryId otherPristine = index.add(a, "pristine", EntryFlag::Directory); // not inside .svn
        const EntryId skip = index.add(c, "Skip", EntryFlag::Directory);
        index.add(skip, "s.txt", 0);
        const EntryId keep = index.add(c, "keep.txt", 0);
        const EntryId upper = index.add(c, "NODE_MODULES", EntryFlag::Directory); // names ignore case
        const EntryId file = index.add(a, "node_modules", 0); // a file of that name is not a folder

        const CrawlRules rules {{L"C:\\skip"}, {L"node_modules", L".svn\\pristine"}, {}, {}};
        QCOMPARE(removeExcluded(index, rules), std::size_t {7});
        for (const EntryId gone : {modules, pristine, skip, upper})
            QVERIFY(index.entry(gone).isDeleted());
        for (const EntryId kept : {a, svn, otherPristine, keep, file})
            QVERIFY(!index.entry(kept).isDeleted());
        QCOMPARE(removeExcluded(index, rules), std::size_t {0}); // again: nothing left to do
    }

    void excludedFolders()
    {
        // "!folder\" leaves out what is inside such a folder, not every name.
        const NameMatcher folder(ws::parseQuery(u"index !node_modules\\"_s));
        QCOMPARE(folder.matchPath(u"C:\\app\\node_modules\\lib\\index.js"_s, false), -1);
        QVERIFY(folder.matchPath(u"C:\\app\\src\\index.js"_s, false) >= 0);
        QVERIFY(folder.matchName("index") >= 0); // apps have no folders

        // With a name too: only those names inside such a folder.
        const NameMatcher logs(ws::parseQuery(u"!tmp\\*.log"_s));
        QCOMPARE(logs.matchPath(u"C:\\tmp\\a.log"_s, false), -1);
        QVERIFY(logs.matchPath(u"C:\\tmp\\a.txt"_s, false) >= 0);
        QVERIFY(logs.matchPath(u"C:\\docs\\a.log"_s, false) >= 0);

        // The same on the index.
        SampleTree t;
        const NameMatcher underUsers(ws::parseQuery(u"!users\\"_s));
        QCOMPARE(underUsers.match(t.index, t.index.entry(t.report)), -1);
        QVERIFY(underUsers.match(t.index, t.index.entry(t.windows)) >= 0);
        QVERIFY(underUsers.match(t.index, t.index.entry(t.users)) >= 0); // the folder itself stays
    }

    void pinyinBasics()
    {
        const auto readings = [](char32_t c) {
            std::vector<std::string> out;
            for (const auto r : pinyin::readings(c))
                out.emplace_back(r);
            return out;
        };
        QCOMPARE(readings(U'报'), (std::vector<std::string> {"bao"}));
        QCOMPARE(readings(U'行').front(), std::string("xing")); // most common first
        QVERIFY(std::ranges::count(readings(U'行'), std::string("hang")) == 1);
        QCOMPARE(readings(U'绿').front(), std::string("lv"));
        QVERIFY(readings(U'a').empty());
        QVERIFY(readings(U'レ').empty());

        QVERIFY(pinyin::hasHan("年度报告.docx"));
        QVERIFY(!pinyin::hasHan("report.docx"));
        QVERIFY(!pinyin::hasHan("レポート.txt")); // Japanese kana are not Chinese characters
        QVERIFY(pinyin::isPinyinTerm("bg"));
        QVERIFY(pinyin::isPinyinTerm("v2bg"));
        QVERIFY(!pinyin::isPinyinTerm("b"));
        QVERIFY(!pinyin::isPinyinTerm("2024"));
        QVERIFY(!pinyin::isPinyinTerm("a.b"));
        QVERIFY(!pinyin::isPinyinTerm(std::string(64, 'a')));
    }

    void pinyinFind()
    {
        const auto find = [](std::string_view term, std::u16string_view text) -> std::pair<int, int> {
            const auto span = pinyin::Matcher(term).findUtf16(text);
            return span ? std::pair {static_cast<int>(span->start), static_cast<int>(span->length)} : std::pair {-1, 0};
        };
        using P = std::pair<int, int>;
        // Initials, whole syllables, prefixes, and mixes of them.
        for (const char* term : {"bg", "baogao", "baog", "bgao", "bga"})
            QCOMPARE(find(term, u"报告"), P(0, 2));
        QCOMPARE(find("bg", u"年度报告.docx"), P(2, 2));
        QCOMPARE(find("ndbg", u"年度报告.docx"), P(0, 4));
        QCOMPARE(find("bg", u"Report报告"), P(6, 2));
        QCOMPARE(find("v2bg", u"V2报告"), P(0, 4)); // ASCII matches itself, case folded
        QCOMPARE(find("yhk", u"银行卡"), P(0, 3)); // 行 read as hang
        QCOMPARE(find("yinhang", u"银行卡"), P(0, 2));
        QCOMPARE(find("cq", u"重庆"), P(0, 2)); // 重 read as chong
        QCOMPARE(find("lvse", u"绿色"), P(0, 2));
        QCOMPARE(find("luse", u"绿色"), P(0, 2)); // ü typed as u
        // Each character needs its own letters, in order, with nothing between.
        QCOMPARE(find("bx", u"报告"), P(-1, 0));
        QCOMPARE(find("baoo", u"报告"), P(-1, 0));
        QCOMPARE(find("bgd", u"报告.docx"), P(-1, 0));
        QCOMPARE(find("ab", u"报告"), P(-1, 0));
        QCOMPARE(find("bg", u"报 告"), P(-1, 0));
        // Spans in UTF-8 bytes for index names.
        const auto span = pinyin::Matcher("bg").findUtf8("年度报告.docx");
        QVERIFY(span);
        QCOMPARE(span->start, std::size_t {6});
        QCOMPARE(span->length, std::size_t {6});
    }

    void pinyinSearch()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId project = index.add(root, "项目资料", EntryFlag::Directory);
        const EntryId plan = index.add(project, "方案.doc", 0);
        const EntryId annual = index.add(root, "年度报告.docx", 0);
        const EntryId report = index.add(root, "报告.txt", 0);
        const EntryId latin = index.add(root, "bg.txt", 0);
        const EntryId other = index.add(root, "其他.txt", 0);
        QVERIFY(index.entry(annual).flags & EntryFlag::Han);
        QVERIFY(!(index.entry(latin).flags & EntryFlag::Han));
        index.setFlags(annual, EntryFlag::Hidden); // the index keeps its own flag
        QVERIFY(index.entry(annual).flags & EntryFlag::Han);
        const EntryId renamed = index.add(root, "notes.txt", 0);
        QVERIFY(index.move(renamed, root, "笔记.txt")); // renamed to Chinese
        QVERIFY(index.entry(renamed).flags & EntryFlag::Han);

        const NameMatcher bg(ws::parseQuery(u"bg"_s));
        const int literal = bg.match(index, index.entry(latin));
        const int whole = bg.match(index, index.entry(report));
        const int part = bg.match(index, index.entry(annual));
        QVERIFY(whole > 0);
        QVERIFY(part > 0);
        QVERIFY(literal > whole); // spelled out ranks first
        QVERIFY(whole > part); // the whole name before part of one
        QCOMPARE(bg.match(index, index.entry(other)), -1);
        // A less common reading still matches, below the most common one:
        // 行 is mostly xing, also hang.
        const EntryId bank = index.add(root, "银行.txt", 0);
        const int common = NameMatcher(ws::parseQuery(u"yx"_s)).match(index, index.entry(bank));
        const int lessCommon = NameMatcher(ws::parseQuery(u"yh"_s)).match(index, index.entry(bank));
        QVERIFY(lessCommon > 0);
        QVERIFY(common > lessCommon);
        QVERIFY(NameMatcher(ws::parseQuery(u"bj"_s)).match(index, index.entry(renamed)) > 0);

        // Folders in path terms, and history paths.
        QVERIFY(NameMatcher(ws::parseQuery(u"xmzl\\fa"_s)).match(index, index.entry(plan)) > 0);
        QCOMPARE(NameMatcher(ws::parseQuery(u"xx\\fa"_s)).match(index, index.entry(plan)), -1);
        QVERIFY(NameMatcher(ws::parseQuery(u"fa"_s)).matchPath(u"C:\\项目资料\\方案.doc"_s, false) > 0);
        // Exclusions stay literal.
        QVERIFY(NameMatcher(ws::parseQuery(u"txt !bg"_s)).match(index, index.entry(report)) > 0);
    }

    void indexAddFindRemoveMove()
    {
        SampleTree t;
        QCOMPARE(t.index.liveCount(), std::size_t {7});
        QCOMPARE(t.index.findPath(L"c:\\users\\ME\\documents\\report.DOCX"), t.report);
        QCOMPARE(t.index.findPath(L"C:\\"), t.root);
        QCOMPARE(t.index.findPath(L"C:\\nope"), kNoEntry);
        QCOMPARE(t.index.extLength(t.index.entry(t.report)), std::size_t {4});

        QVERIFY(t.index.move(t.report, t.me, "Report-final.docx"));
        QCOMPARE(t.index.path(t.report), u"C:\\Users\\me\\Report-final.docx"_s);
        QCOMPARE(t.index.findChild(t.docs, "report.docx"), kNoEntry);
        QVERIFY(!t.index.move(t.users, t.docs, "Users")); // into its own subtree

        QCOMPARE(t.index.remove(t.users), std::size_t {5});
        QCOMPARE(t.index.liveCount(), std::size_t {2});
        QCOMPARE(t.index.findPath(L"C:\\Users"), kNoEntry);
        QVERIFY(t.index.entry(t.notes).isDeleted());
    }

    void indexPaths()
    {
        SampleTree t;
        QCOMPARE(t.index.path(t.root), u"C:\\"_s);
        QCOMPARE(t.index.path(t.notes), u"C:\\Users\\me\\Documents\\notes.txt"_s);
        QCOMPARE(t.index.depth(t.notes), 4);

        // Deeper than the fast path's fixed buffer.
        EntryId cur = t.root;
        for (int i = 0; i < 80; ++i)
            cur = t.index.add(cur, "d", EntryFlag::Directory);
        QCOMPARE(t.index.path(cur).count(u'\\'), 80);
    }

    // While the index is saved (a long read), a writer waits at its gate, not
    // on the lock, where it would hold up the searches' read locks too.
    void longReadLetsReadersPastWriters()
    {
        using namespace std::chrono_literals;
        FileIndex index;
        std::atomic<bool> wrote = false;
        std::atomic<bool> read = false;
        std::jthread writer;
        std::jthread search;
        std::optional<FileIndex::LongReadLock> save = index.longReadLock(); // let go before the threads are joined
        writer = std::jthread([&] {
            const auto lock = index.writeLock();
            wrote = true;
        });
        std::this_thread::sleep_for(100ms); // waiting by now
        QVERIFY(!wrote);
        search = std::jthread([&] {
            const auto lock = index.readLock();
            read = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(read, 2000);
        QVERIFY(!wrote);
        save.reset();
        writer.join();
        QVERIFY(wrote);
    }

    // With nothing typed the recent items are listed, each looked up on disk:
    // the local ones answer at once, and a share whose server is gone (seconds
    // to say so) holds up the first search for the one wait only, the next
    // not at all, and never shows up.
    void recentItemsDoNotWaitForSlowDrives()
    {
        QTemporaryDir dir;
        const QString here = QDir::toNativeSeparators(dir.filePath(u"here.txt"_s));
        QFile file(here);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QString gone = QDir::toNativeSeparators(dir.filePath(u"gone.txt"_s));
        const QString remote = u"\\\\10.255.255.1\\share\\far.txt"_s; // never answers

        // The share stays silent without the network: one really not there
        // takes 10-20 s, but for half a minute after any program's lookup
        // there timed out Windows says "not found" at once, and the slow way
        // went untried. Its thread never gets to it either, as a busy
        // computer may run a new thread late (the search after the first
        // then waited the whole wait again).
        qputenv("WINSHUN_SILENT_SHARES", "1"); // read as the engine is made
        SearchEngine engine(nullptr);
        qunsetenv("WINSHUN_SILENT_SHARES");
        // Each search's rows queued to this thread, not a QSignalSpy: its
        // wait() counts from where the spy is when it begins, so a search
        // done before then (a busy computer) would be waited for 5 s in vain.
        quint64 asked = 0;
        std::optional<std::pair<QStringList, qint64>> rows; // and how long the search took
        QObject receiver;
        QObject::connect(&engine, &SearchEngine::resultsReady, &receiver,
            [&](quint64 id, const SearchResults& results, qint64, qint64 elapsedUs) {
                if (id != asked)
                    return;
                QStringList paths;
                for (const SearchResult& r : results)
                    paths.append(r.path);
                rows = {paths, elapsedUs};
            },
            Qt::QueuedConnection);
        const auto listed = [&](const QStringList& history, qint64& elapsedUs) {
            rows.reset();
            SearchEngine::Request request;
            request.history = history;
            asked = engine.submit(std::move(request));
            if (!QTest::qWaitFor([&] { return rows.has_value(); }, 5000))
                return QStringList {u"(no results)"_s};
            elapsedUs = rows->second;
            return rows->first;
        };

        qint64 us = 0;
        // The drive's thread running first: a new thread may start too late
        // for the first look's wait on a busy computer (up to 0.1 s beside 32
        // busy threads), which is up to Windows.
        QTRY_COMPARE(listed({here}, us), QStringList {here});
        // "At once" is under the wait (100 ms), which a search that sat it out
        // takes at least: the rest is room for a busy computer putting a
        // thread off (once 42 ms, beside 64 busy threads).
        QCOMPARE(listed({here, gone}, us), QStringList {here});
        qInfo("local only: %lld us", us);
        QVERIFY2(us < 90'000, qPrintable(QString::number(us)));

        QCOMPARE(listed({remote, here, gone}, us), QStringList {here});
        qInfo("with the share, first time: %lld us", us);
        QVERIFY2(us >= 90'000, qPrintable(QString::number(us))); // waited for: it might have answered
        QVERIFY2(us < 400'000, qPrintable(QString::number(us))); // the one wait (100 ms), room for a busy computer

        QCOMPARE(listed({remote, here, gone}, us), QStringList {here});
        qInfo("with the share, again: %lld us", us);
        QVERIFY2(us < 90'000, qPrintable(QString::number(us))); // slow today: not waited for
    }

    // GitHub #115: a content scan stops at its own pace (the file in hand: a
    // hard disk spinning up, an antivirus holding it), and the name search
    // typed meanwhile has its rows at once all the same, not after it.
    void namesDoNotWaitForContentScans()
    {
        QTemporaryDir tree;
        QTemporaryDir store;
        const QString docs = QDir::toNativeSeparators(tree.filePath(u"docs"_s));
        QVERIFY(QDir().mkpath(docs));
        for (const QString& name : {u"alpha.txt"_s, u"beta.txt"_s}) {
            QFile file(docs + u'\\' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("a needle in a haystack");
        }
        IndexService service(store.path(), walkedOptions(std::vector {docs.toStdWString()}));
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(allReady(service), 30'000);
        QVERIFY(indexedIn(service, docs + u"\\beta.txt"_s));

        qputenv("WINSHUN_SLOW_READS", "3000"); // read as the engine is made: each file takes 3 s more
        auto engine = std::make_unique<SearchEngine>(&service);
        qunsetenv("WINSHUN_SLOW_READS");
        quint64 asked = 0;
        std::optional<QStringList> rows;
        QObject receiver;
        QObject::connect(engine.get(), &SearchEngine::resultsReady, &receiver,
            [&](quint64 id, const SearchResults& results, qint64, qint64) {
                if (id != asked)
                    return;
                QStringList paths;
                for (const SearchResult& r : results)
                    paths.append(r.path);
                rows = paths;
            },
            Qt::QueuedConnection);

        SearchEngine::Request content;
        content.text = u"needle"_s;
        content.scope = Scope::Content;
        content.contentExtensions = {u"txt"_s};
        content.contentFolders = {docs};
        content.contentThreads = 1;
        asked = engine->submit(std::move(content));
        QTRY_VERIFY(rows.has_value()); // the (empty) start of the scan
        QTest::qWait(300); // in its first file by now

        rows.reset();
        SearchEngine::Request names;
        names.text = u"beta"_s;
        const auto began = std::chrono::steady_clock::now();
        asked = engine->submit(std::move(names));
        QTRY_VERIFY_WITH_TIMEOUT(rows.has_value(), 5000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        qInfo("names beside a scan in a slow file: %lld ms", static_cast<long long>(ms));
        QVERIFY2(ms < 1500, qPrintable(QString::number(ms))); // not once the file is done
        QVERIFY(rows->contains(docs + u"\\beta.txt"_s));

        // And the scan was in its file all the while: it stops once that is
        // read (else the names had nothing to wait for).
        const auto quitting = std::chrono::steady_clock::now();
        engine.reset();
        const auto stopping = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - quitting).count();
        QVERIFY2(stopping > 1000, qPrintable(QString::number(stopping)));
    }

    void snapshotRoundTrip()
    {
        SampleTree t;
        t.index.remove(t.notes);
        t.index.setFolderRecord(t.root, ntfs::kRootRecord, t.root);
        t.index.setFolderRecord(t.root, 100, t.docs);
        t.index.setFolderRecord(t.root, 101, t.windows);
        const EntryId gone = t.index.add(t.users, "gone", EntryFlag::Directory);
        t.index.setFolderRecord(t.root, 102, gone);
        t.index.remove(gone);
        // Up and down from one entry to the next (zigzag), and the largest.
        t.index.setModified(t.report, 1'700'000'123);
        t.index.setCreated(t.report, 1'600'000'456);
        t.index.setModified(t.windows, 0xFFFF'FFFF);
        t.index.setModified(t.docs, 5);
        t.index.setSize(t.report, packSize(123'456));
        const std::vector<VolumeInfo> volumes {{L"C:", 0x1234, true}};
        const std::vector<JournalPosition> journals {{0xABCDEF, 123456789}};
        QTemporaryDir dir;
        const QString file = dir.filePath(u"index.bin"_s);
        const CrawlRules rules {{L"C:\\skip"}, {L"node_modules", L".svn\\pristine"}, {L"C:\\Windows"}, {L"AppData"}};
        QVERIFY(snapshot::save(t.index, volumes, journals, rules, file));

        const auto contents = snapshot::load(file);
        QVERIFY(contents);
        const auto& loaded = contents->index;
        QVERIFY(loaded);
        QCOMPARE(loaded->liveCount(), std::size_t {6});
        const EntryId report = loaded->findPath(L"C:\\Users\\me\\Documents\\Report.docx");
        QVERIFY(report != kNoEntry);
        QCOMPARE(loaded->path(report), u"C:\\Users\\me\\Documents\\Report.docx"_s);
        QVERIFY(contents->times);
        QCOMPARE(loaded->modified(report), FileTime {1'700'000'123});
        QVERIFY(contents->created);
        QCOMPARE(loaded->created(report), FileTime {1'600'000'456});
        QCOMPARE(loaded->modified(loaded->findPath(L"C:\\Windows")), FileTime {0xFFFF'FFFF});
        QCOMPARE(loaded->modified(loaded->findPath(L"C:\\Users\\me\\Documents")), FileTime {5});
        QCOMPARE(loaded->modified(loaded->findPath(L"C:\\Users")), FileTime {0});
        QCOMPARE(loaded->size(report), packSize(123'456)); // version 8: sizes too
        QCOMPARE(loaded->size(loaded->findPath(L"C:\\Users\\me\\Documents")), kUnknownSize);
        QCOMPARE(loaded->size(loaded->findPath(L"C:\\Windows")), kUnknownSize);
        QVERIFY(loaded->entry(loaded->findPath(L"C:\\Windows")).flags & EntryFlag::LowPriority);
        QCOMPARE(loaded->findPath(L"C:\\Users\\me\\Documents\\notes.txt"), kNoEntry);

        QVERIFY(contents->journals == journals);
        QCOMPARE(contents->volumes.size(), std::size_t {1});
        QVERIFY(contents->volumes[0].root == L"C:" && contents->volumes[0].serial == 0x1234);
        QVERIFY(contents->rules && *contents->rules == rules);
        QVERIFY(contents->attachment.empty());
        const EntryId root = loaded->roots().front();
        QCOMPARE(loaded->folderByRecord(root, ntfs::kRootRecord), root);
        QCOMPARE(loaded->folderByRecord(root, 100), loaded->findPath(L"C:\\Users\\me\\Documents"));
        QCOMPARE(loaded->folderByRecord(root, 101), loaded->findPath(L"C:\\Windows"));
        QCOMPARE(loaded->folderRecords(root)->size(), std::size_t {3}); // the removed folder is not kept

        // Data attached to entries: written with the ids the entries get in the file.
        const QString attachedFile = dir.filePath(u"attached.bin"_s);
        QVERIFY(snapshot::save(t.index, volumes, journals, rules, attachedFile, [&](const std::vector<EntryId>& newIds) {
            const EntryId id = newIds[t.report];
            std::vector<char> data(sizeof id);
            std::memcpy(data.data(), &id, sizeof id);
            return data;
        }));
        const auto attached = snapshot::load(attachedFile);
        QVERIFY(attached && attached->attachment.size() == sizeof(EntryId));
        EntryId reportId = kNoEntry;
        std::memcpy(&reportId, attached->attachment.data(), sizeof reportId);
        QCOMPARE(attached->index->path(reportId), u"C:\\Users\\me\\Documents\\Report.docx"_s);

        QVERIFY(!snapshot::load(dir.filePath(u"missing.bin"_s)));
        QFile truncated(file);
        QVERIFY(truncated.resize(truncated.size() - 3));
        QVERIFY(!snapshot::load(file)); // the end is gone

        // Packed in many blocks: entries and an attachment that straddle them.
        FileIndex big;
        const EntryId folder = big.add(big.addRoot("E:"), "folder", EntryFlag::Directory);
        for (int i = 0; i < 100000; ++i)
            big.add(folder, "file number " + std::to_string(i) + " with a fairly long name.txt", 0);
        std::vector<char> blob(3'000'000);
        for (std::size_t i = 0; i < blob.size(); ++i)
            blob[i] = static_cast<char>(i * 7 + i / 1000);
        const QString bigFile = dir.filePath(u"big.bin"_s);
        QVERIFY(snapshot::save(big, {}, {}, {}, bigFile, [&](const std::vector<EntryId>&) { return blob; }));
        QFile packed(bigFile);
        QVERIFY(packed.open(QIODevice::ReadOnly) && packed.read(8) == QByteArray("QFINDEXZ", 8));
        QVERIFY(packed.size() < 5'000'000); // about 8 MB unpacked
        packed.close();
        const auto bigLoaded = snapshot::load(bigFile);
        QVERIFY(bigLoaded && bigLoaded->attachment == blob);
        QCOMPARE(bigLoaded->index->liveCount(), std::size_t {100002});
        QVERIFY(bigLoaded->index->findPath(L"E:\\folder\\file number 99999 with a fairly long name.txt") != kNoEntry);
        QVERIFY(packed.open(QIODevice::ReadWrite));
        packed.seek(packed.size() / 2);
        packed.write("garbage!", 8); // in the middle of a block
        packed.close();
        QVERIFY(!snapshot::load(bigFile));

        // A file from before snapshots were packed.
        QByteArray plain("QFINDEX", 8);
        const auto put = [&](auto v) { plain.append(reinterpret_cast<const char*>(&v), sizeof v); };
        put(std::uint32_t {4}); // version
        put(std::uint32_t {0}); // volumes
        for (int i = 0; i < 4; ++i)
            put(std::uint32_t {0}); // rules
        put(std::uint32_t {2}); // entries
        put(kNoEntry);
        put(static_cast<std::uint8_t>(EntryFlag::Root | EntryFlag::Directory));
        put(std::uint16_t {2});
        plain.append("D:");
        put(EntryId {0});
        put(std::uint8_t {0});
        put(std::uint16_t {5});
        plain.append("a.txt");
        put(std::uint32_t {0}); // folder record tables
        put(std::uint64_t {0}); // attachment
        put(std::uint32_t {0x21444E45}); // "END!"
        QFile old(dir.filePath(u"old.bin"_s));
        QVERIFY(old.open(QIODevice::WriteOnly) && old.write(plain) == plain.size());
        old.close();
        const auto oldLoaded = snapshot::load(old.fileName());
        QVERIFY(oldLoaded && oldLoaded->index->findPath(L"D:\\a.txt") != kNoEntry);

        // Version 5: names once each, entries record by record (Win顺 0.5).
        QByteArray five("QFINDEX", 8);
        const auto put5 = [&](auto v) { five.append(reinterpret_cast<const char*>(&v), sizeof v); };
        put5(std::uint32_t {5}); // version
        put5(std::uint32_t {0}); // volumes
        for (int i = 0; i < 4; ++i)
            put5(std::uint32_t {0}); // rules
        put5(std::uint32_t {2}); // names
        put5(std::uint16_t {2});
        five.append("D:");
        put5(std::uint16_t {5});
        five.append("a.txt");
        put5(std::uint32_t {3}); // entries: back, flags, name
        put5(std::uint8_t {0});
        put5(static_cast<std::uint8_t>(EntryFlag::Root | EntryFlag::Directory));
        put5(std::uint8_t {0});
        put5(std::uint8_t {1});
        put5(std::uint8_t {0});
        put5(std::uint8_t {1});
        put5(std::uint8_t {2}); // another a.txt, in the root too
        put5(std::uint8_t {0});
        put5(std::uint8_t {1});
        put5(std::uint32_t {1}); // folder record tables
        put5(EntryId {0});
        put5(std::uint32_t {1});
        put5(std::uint32_t {ntfs::kRootRecord});
        put5(EntryId {0});
        put5(std::uint64_t {0}); // attachment
        put5(std::uint32_t {0x21444E45}); // "END!"
        QFile v5(dir.filePath(u"v5.bin"_s));
        QVERIFY(v5.open(QIODevice::WriteOnly) && v5.write(five) == five.size());
        v5.close();
        const auto v5Loaded = snapshot::load(v5.fileName());
        QVERIFY(v5Loaded && v5Loaded->index->liveCount() == 3);
        QVERIFY(v5Loaded->index->findPath(L"D:\\a.txt") != kNoEntry);
        QCOMPARE(v5Loaded->index->folderByRecord(0, ntfs::kRootRecord), EntryId {0});
    }

    void folderRecords()
    {
        // The table against a reference map, with many erases (backward shifts).
        RecordTable table;
        std::unordered_map<std::uint32_t, EntryId> reference;
        std::mt19937 rng(7);
        for (int i = 0; i < 200000; ++i) {
            const auto record = static_cast<std::uint32_t>(rng() % 5000 + 1);
            if (rng() % 3 == 0) {
                table.erase(record);
                reference.erase(record);
            } else {
                table.set(record, static_cast<EntryId>(i));
                reference[record] = static_cast<EntryId>(i);
            }
        }
        QCOMPARE(table.size(), reference.size());
        for (std::uint32_t r = 1; r <= 5000; ++r) {
            const auto it = reference.find(r);
            QCOMPARE(table.find(r), it == reference.end() ? kNoEntry : it->second);
        }

        // Lookups skip removed folders; compaction renumbers; a removed root drops its table.
        SampleTree t;
        FileIndex& index = t.index;
        const EntryId d = index.addRoot("D:");
        const EntryId data = index.add(d, "data", EntryFlag::Directory);
        index.setFolderRecord(t.root, 30, t.docs);
        index.setFolderRecord(t.root, 31, t.me);
        index.setFolderRecord(t.root, 32, t.report); // not a folder: never returned
        index.setFolderRecord(d, 30, data);
        QCOMPARE(index.folderByRecord(t.root, 30), t.docs);
        QCOMPARE(index.folderByRecord(d, 30), data);
        QCOMPARE(index.folderByRecord(t.root, 32), kNoEntry);
        index.remove(t.notes);
        index.remove(t.docs);
        QCOMPARE(index.folderByRecord(t.root, 30), kNoEntry);
        QVERIFY(index.compact() > 0);
        const EntryId me = index.findPath(L"C:\\Users\\me");
        QCOMPARE(index.folderByRecord(t.root, 31), me);
        const EntryId newD = index.roots().back();
        QCOMPARE(index.folderByRecord(newD, 30), index.findPath(L"D:\\data"));
        QCOMPARE(index.folderRecords(t.root)->size(), std::size_t {1}); // 30 and 32 went with docs
        index.forgetFolderRecord(t.root, 31);
        QCOMPARE(index.folderByRecord(t.root, 31), kNoEntry);
        index.remove(newD);
        QVERIFY(!index.folderRecords(newD));
    }

    void mftRecord()
    {
        RecordBuilder b(0x0003); // in use, folder
        b.standardInformation(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
        b.fileName(0x0002'0000'0000'04D2ull, u"LONGNA~1", 2); // the DOS alias: skipped
        b.fileName(0x0002'0000'0000'04D2ull, u"Long name 中文", 1);
        const std::u16string longName(150, u'x'); // runs across the first 512-byte stride
        b.fileName(0x0001'0000'0000'0309ull, longName, 0);
        auto bytes = b.finish();

        ntfs::FileRecord record;
        auto copy = bytes;
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(record.inUse);
        QVERIFY(record.directory);
        QCOMPARE(record.baseRecord, std::uint32_t {0});
        QCOMPARE(record.attributes, std::uint32_t {FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE});
        QCOMPARE(record.names.size(), std::size_t {2});
        QCOMPARE(ntfs::recordOf(record.names[0].parent), std::uint32_t {1234});
        QCOMPARE(std::u16string(record.names[0].name), std::u16string(u"Long name 中文"));
        QCOMPARE(ntfs::recordOf(record.names[1].parent), std::uint32_t {777});
        QCOMPARE(std::u16string(record.names[1].name), longName);

        copy = bytes;
        copy[510] ^= std::byte {1}; // torn write: the check value no longer matches
        QVERIFY(!ntfs::parseFileRecord(copy.data(), copy.size(), record));
        copy = bytes;
        copy[0] = std::byte {'B'}; // "BAAD"
        QVERIFY(!ntfs::parseFileRecord(copy.data(), copy.size(), record));

        RecordBuilder unused(0x0000);
        unused.fileName(5, u"deleted.txt", 1);
        copy = unused.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(!record.inUse);
        QVERIFY(record.names.empty());

        RecordBuilder extension(0x0001, 0x0003'0000'0000'002Aull);
        extension.fileName(5, u"link.txt", 0);
        copy = extension.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QCOMPARE(record.baseRecord, std::uint32_t {42});
        QCOMPARE(record.names.size(), std::size_t {1});
        QVERIFY(!record.sizeKnown); // no $DATA here

        // The file's size: a resident $DATA's value, a non-resident one's
        // real size (from the piece at VCN 0 only); a named stream is not it.
        RecordBuilder small(0x0001);
        small.fileName(5, u"small.txt", 1);
        small.resident(0x80, std::vector<std::byte>(37));
        copy = small.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(record.sizeKnown);
        QCOMPARE(record.size, std::uint64_t {37});
        RecordBuilder large(0x0001);
        large.fileName(5, u"large.bin", 1);
        large.nonResidentData({std::byte {0x11}, std::byte {0x01}, std::byte {0x02}, std::byte {0}}, 0, 4096, 3000, 3000, 0);
        copy = large.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(record.sizeKnown);
        QCOMPARE(record.size, std::uint64_t {3000});
        RecordBuilder later(0x0001, 0x0003'0000'0000'002Aull); // a later piece of a fragmented file: says nothing
        later.nonResident(0x80, {std::byte {0x11}, std::byte {0x01}, std::byte {0x02}, std::byte {0}}, 9, 40960, 5);
        copy = later.finish();
        QVERIFY(ntfs::parseFileRecord(copy.data(), copy.size(), record));
        QVERIFY(!record.sizeKnown);
    }

    void mftRunList()
    {
        const auto bytes = [](std::initializer_list<int> v) {
            std::vector<std::byte> out;
            for (const int b : v)
                out.push_back(static_cast<std::byte>(b));
            return out;
        };
        // 0x10 clusters at 0x4000, then 8 clusters 0x1000 clusters back (negative delta).
        const auto runs = ntfs::decodeRunList(bytes({0x31, 0x10, 0x00, 0x40, 0x00, 0x21, 0x08, 0x00, 0xF0, 0x00}), 4096);
        QVERIFY(runs);
        QCOMPARE(runs->size(), std::size_t {2});
        QCOMPARE((*runs)[0].offset, std::uint64_t {0x4000} * 4096);
        QCOMPARE((*runs)[0].length, std::uint64_t {0x10} * 4096);
        QCOMPARE((*runs)[1].offset, std::uint64_t {0x3000} * 4096);
        QCOMPARE((*runs)[1].length, std::uint64_t {8} * 4096);
        QVERIFY(!ntfs::decodeRunList(bytes({0x31, 0x10, 0x00, 0x40, 0x00}), 4096)); // no end marker
        QVERIFY(!ntfs::decodeRunList(bytes({0x01, 0x10, 0x00}), 4096)); // sparse
        QVERIFY(!ntfs::decodeRunList(bytes({0x31, 0x10, 0x00}), 4096)); // cut short
    }

    void mftLayouts()
    {
        QVERIFY(ntfs::supported({512, 512, 1024})); // converted from FAT32
        QVERIFY(ntfs::supported({512, 2048, 4096})); // "format /L" with small clusters
        QVERIFY(ntfs::supported({4096, 4096, 4096})); // a 4K-sector disk
        QVERIFY(ntfs::supported({512, 2u << 20, 1024})); // the largest NTFS clusters
        QVERIFY(!ntfs::supported({4096, 2048, 4096})); // a cluster smaller than a sector
        QVERIFY(!ntfs::supported({512, 4096, 1000}));
        QVERIFY(!ntfs::supported({512, 4096, 256}));
        QVERIFY(!ntfs::supported({512, 0, 1024}));

        // Reads stay within extents, take whole clusters, and stop past the bytes asked for.
        const std::vector<ntfs::Extent> extents {{8192, 3 * 512}, {40960, 20 * 512}, {1024, 2 * 512}};
        const auto plan = ntfs::planReads(extents, 3 * 512 + 9 * 512 + 100, 512, 4 * 512);
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> want {
            {8192, 1536}, {40960, 2048}, {43008, 2048}, {45056, 1024}};
        QCOMPARE(plan.size(), want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            QCOMPARE(plan[i].offset, want[i].first);
            QCOMPARE(plan[i].length, want[i].second);
        }
        QCOMPARE(ntfs::planReads(extents, 100 * 512, 512, 4 * 512).size(), std::size_t {7}); // all, and short
        for (std::size_t i = 0, at = 0; i < plan.size(); at += plan[i].length, ++i)
            QCOMPARE(plan[i].at, std::uint64_t {at});

        // A stretch left out across two extents: the reads go on after it, where it ends.
        const std::vector<ntfs::Extent> two {{0x10000, 16384}, {0x40000, 16384}};
        const std::vector<ntfs::Stretch> skip {{8192, 16384}};
        const auto around = ntfs::planReads(two, 32768, 4096, 65536, skip);
        QCOMPARE(around.size(), std::size_t {2});
        QCOMPARE(around[0].offset, std::uint64_t {0x10000});
        QCOMPARE(around[0].length, std::uint64_t {8192});
        QCOMPARE(around[0].at, std::uint64_t {0});
        QCOMPARE(around[1].offset, std::uint64_t {0x40000 + 8192});
        QCOMPARE(around[1].length, std::uint64_t {8192});
        QCOMPARE(around[1].at, std::uint64_t {24576});

        // Free stretches: whole granules (here 8 records, one byte of the bitmap) of free records.
        const auto bits = [](std::initializer_list<int> v) {
            std::vector<std::byte> out;
            for (const int b : v)
                out.push_back(static_cast<std::byte>(b));
            return out;
        };
        const auto bitmap = bits({0xFF, 0, 0, 0x01, 0, 0, 0, 0x80});
        const auto stretches = [&](std::uint64_t minBytes, std::uint64_t bytes) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
            for (const ntfs::Stretch& s : ntfs::freeStretches(bitmap, 1024, 8192, minBytes, bytes))
                out.emplace_back(s.at, s.length);
            return out;
        };
        using Stretches = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
        QCOMPARE(stretches(8192, 65536), (Stretches {{8192, 16384}, {32768, 24576}}));
        QCOMPARE(stretches(24576, 65536), (Stretches {{32768, 24576}})); // the shorter one is read
        QCOMPARE(stretches(8192, 131072), (Stretches {{8192, 16384}, {32768, 24576}})); // past the bitmap: in use
        QCOMPARE(stretches(8192, 40960), (Stretches {{8192, 16384}, {32768, 8192}})); // only the bytes asked for
        QVERIFY(ntfs::freeStretches(bitmap, 1024, 4096, 4096, 65536).empty()); // a granule must be whole bytes
    }

    // An MFT split into extents of odd lengths, out of order on the volume:
    // with clusters smaller than records, records run across extents and
    // across the reader's blocks. Each comes back whole, in order. With its
    // bitmap, the free records from 1.5 to 3.5 MB into it (garbage here) are
    // left out where they fill a whole megabyte, from 2 to 3 MB.
    void mftReader_data()
    {
        QTest::addColumn<int>("clusterSize");
        QTest::addColumn<int>("recordSize");
        QTest::addColumn<bool>("fromRecordZero"); // the extents read from the MFT's own record
        QTest::addColumn<bool>("withBitmap");
        QTest::addColumn<int>("bitmapHolder"); // the record with the bitmap, named in record 0's attribute list; 0: record 0
        QTest::addColumn<int>("readsInFlight");
        QTest::newRow("512-byte clusters") << 512 << 1024 << false << false << 0 << 2;
        QTest::newRow("512-byte clusters, record 0") << 512 << 1024 << true << false << 0 << 2;
        QTest::newRow("512-byte clusters, 4 KB records") << 512 << 4096 << true << false << 0 << 3;
        QTest::newRow("2 KB clusters, 4 KB records") << 2048 << 4096 << false << false << 0 << 2;
        QTest::newRow("4 KB clusters") << 4096 << 1024 << false << false << 0 << 4;
        QTest::newRow("64 KB clusters") << 65536 << 4096 << true << false << 0 << 2;
        QTest::newRow("512-byte clusters, bitmap") << 512 << 1024 << false << true << 0 << 2;
        QTest::newRow("512-byte clusters, 4 KB records, bitmap") << 512 << 4096 << true << true << 0 << 4;
        QTest::newRow("2 KB clusters, 4 KB records, bitmap") << 2048 << 4096 << true << true << 0 << 3;
        QTest::newRow("64 KB clusters, bitmap") << 65536 << 4096 << false << true << 0 << 2;
        // Record 2 runs across the first two extents with 512-byte clusters.
        QTest::newRow("bitmap in another record") << 512 << 1024 << false << true << 2 << 2;
        QTest::newRow("bitmap in another record, 4 KB") << 4096 << 4096 << true << true << 9 << 2;
        // Its run list in two parts: the first cluster in record 0, the rest in record 2.
        QTest::newRow("bitmap in two parts") << 512 << 1024 << false << true << -2 << 2;
    }
    void mftReader()
    {
        QFETCH(int, clusterSize);
        QFETCH(int, recordSize);
        QFETCH(bool, fromRecordZero);
        QFETCH(bool, withBitmap);
        QFETCH(int, bitmapHolder);
        QFETCH(int, readsInFlight);
        const bool twoParts = bitmapHolder < 0; // the first in record 0, the second in record -bitmapHolder
        const auto holder = static_cast<std::uint32_t>(std::abs(bitmapHolder));
        const auto cluster = static_cast<std::uint64_t>(clusterSize);
        const auto size = static_cast<std::size_t>(recordSize);
        const auto isFree = [&](std::uint64_t record) {
            return withBitmap && record * size >= (3u << 19) && record * size < (7u << 19);
        };
        const auto leftOut = [&](std::uint64_t record) {
            return withBitmap && record * size >= (2u << 20) && record * size < (3u << 20);
        };

        // Extents in clusters. The first holds record 0 whole, as on a real
        // volume; the fourth is longer than a read (4 MB).
        std::vector<std::uint64_t> clusters {std::max<std::uint64_t>(3, 2 * size / cluster + 1), 1, 5,
            (9u << 19) / cluster + 1, 7, 2, 9, 4};
        std::uint64_t total = 0;
        for (const std::uint64_t c : clusters)
            total += c * cluster;
        const std::uint32_t count = static_cast<std::uint32_t>((total - 2 * cluster) / size); // the rest is allocated, unused
        // On the volume: in another order, with a cluster of garbage before each.
        const std::size_t order[] {0, 4, 2, 6, 1, 5, 3, 7};
        std::vector<ntfs::Extent> extents(clusters.size());
        std::uint64_t at = 16 * cluster;
        for (const std::size_t k : order) {
            at += cluster;
            extents[k] = {at, clusters[k] * cluster};
            at += extents[k].length;
        }
        // The bitmap after them: a bit per record, set when it is in use.
        std::vector<std::byte> bitmap((count + 63) / 64 * 8);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (i % 7 != 3 && !isFree(i))
                bitmap[i / 8] |= std::byte {1} << (i % 8);
        }
        const ntfs::Extent bitmapExtent {at + cluster, (bitmap.size() + cluster - 1) / cluster * cluster};
        std::vector<std::byte> image(static_cast<std::size_t>(bitmapExtent.offset + bitmapExtent.length), std::byte {0xCC});
        std::memcpy(image.data() + bitmapExtent.offset, bitmap.data(), bitmap.size());

        // The records, laid along the extents.
        const auto file = [](std::uint32_t i) { return u"f" + QString::number(i).toStdU16String(); };
        std::size_t extent = 0;
        std::uint64_t inExtent = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const bool holds = withBitmap && i == holder; // the $BITMAP attribute
            RecordBuilder b(i % 7 == 3 && !holds ? 0x0000 : 0x0001, holds && i != 0 ? 0x0001'0000'0000'0000ull : 0, size);
            const std::uint64_t lastVcn = bitmapExtent.length / cluster - 1;
            if (i == 0) {
                b.fileName(0x0001'0000'0000'0005ull, u"$MFT", 3);
                if (withBitmap && holder != 0) {
                    // The attribute list: an entry for each part of the $BITMAP attribute.
                    std::vector<std::byte> list;
                    const auto entry = [&](std::uint64_t vcn, std::uint32_t number) {
                        std::vector<std::byte> e(0x20);
                        put<std::uint32_t>(e, 0, 0xB0);
                        put<std::uint16_t>(e, 4, 0x20);
                        e[7] = std::byte {0x1A};
                        put<std::uint64_t>(e, 8, vcn);
                        put<std::uint64_t>(e, 0x10, 0x0001'0000'0000'0000ull | number);
                        list.insert(list.end(), e.begin(), e.end());
                    };
                    if (twoParts)
                        entry(0, 0);
                    entry(twoParts ? 1 : 0, holder);
                    b.resident(0x20, list);
                }
                b.nonResident(0x80, runList(extents, cluster), total / cluster - 1);
                if (withBitmap && twoParts) // its first cluster
                    b.nonResident(0xB0, runList({{bitmapExtent.offset, cluster}}, cluster), 0, bitmap.size());
            }
            if (holds && twoParts) {
                b.nonResident(0xB0, runList({{bitmapExtent.offset + cluster, bitmapExtent.length - cluster}}, cluster),
                    lastVcn, 0, 1);
            } else if (holds) {
                b.nonResident(0xB0, runList({bitmapExtent}, cluster), lastVcn, bitmap.size());
            } else if (i != 0) {
                b.fileName(0x0001'0000'0000'0005ull, file(i), 1);
            }
            const std::vector<std::byte> record = isFree(i) ? std::vector<std::byte>(size, std::byte {0xCC}) : b.finish();
            for (std::size_t done = 0; done < size;) {
                const std::size_t n = std::min<std::size_t>(size - done, extents[extent].length - inExtent);
                std::memcpy(image.data() + extents[extent].offset + inExtent, record.data() + done, n);
                done += n;
                inExtent += n;
                if (inExtent == extents[extent].length) {
                    ++extent;
                    inExtent = 0;
                }
            }
        }

        const ntfs::Geometry geometry {512, static_cast<std::uint32_t>(clusterSize), static_cast<std::uint32_t>(recordSize)};
        const std::uint64_t validBytes = std::uint64_t {count} * size;
        ntfs::MftReader reader(image, geometry, fromRecordZero ? std::vector<ntfs::Extent>() : extents,
            extents[0].offset, validBytes, {readsInFlight, true});
        QVERIFY2(reader.valid(), qPrintable(QString::fromStdWString(reader.error())));
        QCOMPARE(reader.extentsFromRecordZero(), fromRecordZero);
        QCOMPARE(reader.recordCount(), std::uint64_t {count});
        std::uint32_t next = 0;
        ntfs::FileRecord record;
        while (reader.next()) {
            if (reader.firstRecord() != next) { // only over the megabyte left out
                QCOMPARE(std::uint64_t {next} * size, std::uint64_t {2u << 20});
                QCOMPARE(std::uint64_t {reader.firstRecord()} * size, std::uint64_t {3u << 20});
                next = reader.firstRecord();
            }
            for (std::size_t i = 0; i < reader.blockRecords(); ++i, ++next) {
                QVERIFY(!leftOut(next));
                if (isFree(next)) {
                    QVERIFY(!reader.parse(i, record)); // garbage, read all the same
                    continue;
                }
                QVERIFY2(reader.parse(i, record), qPrintable(u"record %1"_s.arg(next)));
                if (withBitmap && holder != 0 && next == holder) {
                    QVERIFY(record.inUse && record.names.empty()); // holds the bitmap, nothing else
                    continue;
                }
                QCOMPARE(record.inUse, next % 7 != 3);
                if (record.inUse) {
                    QCOMPARE(record.names.size(), std::size_t {1});
                    QCOMPARE(std::u16string(record.names[0].name), next == 0 ? std::u16string(u"$MFT") : file(next));
                }
            }
        }
        QVERIFY2(reader.valid(), qPrintable(QString::fromStdWString(reader.error())));
        QCOMPARE(next, count);
        QCOMPARE(reader.bytesDone(), validBytes);
        QCOMPARE(reader.bytesSkipped(), withBitmap ? std::uint64_t {1u << 20} : std::uint64_t {0});
        QCOMPARE(reader.bitmapRecord(), withBitmap && !twoParts ? holder : 0u);

        // A read that comes back short (past the end of the volume) is an error, not the end.
        image.resize(static_cast<std::size_t>(extents[3].offset + cluster));
        ntfs::MftReader cut(image, geometry, extents, extents[0].offset, validBytes);
        while (cut.next()) {
        }
        QVERIFY(!cut.valid());
    }

    // What the bulk path reads out of a file's record (ntfs::parseFileInfo,
    // ntfs::decodeDataRuns): the $DATA's shape and sizes, the flags that keep
    // a file from being read that way, the ids, the names.
    void fileInfo()
    {
        // $STANDARD_INFORMATION as Windows writes it (0x48 bytes): the times,
        // the attributes at 0x20, the security id at 0x34.
        const auto standardInformation = [](std::uint32_t attributes, std::uint32_t securityId) {
            std::vector<std::byte> value(0x48);
            put<std::int64_t>(value, 0x08, 0x01DC'0000'1111'2222ll); // modified
            put<std::int64_t>(value, 0x10, 0x01DC'0000'3333'4444ll); // MFT changed
            put(value, 0x20, attributes);
            put(value, 0x34, securityId);
            return value;
        };
        const auto fixedUp = [](RecordBuilder& b) {
            std::vector<std::byte> record = b.finish();
            ntfs::FileRecord parsed;
            if (!ntfs::parseFileRecord(record.data(), record.size(), parsed)) // fixups applied in place
                record.clear();
            return record;
        };
        ntfs::FileInfo info;

        // A small file: resident data, a Win32 name and its DOS alias (two
        // $FILE_NAMEs, one name), a security id.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_ARCHIVE, 0x123));
            b.fileName(0x0001'0000'0000'0005ull, u"readme.txt", 1);
            b.fileName(0x0001'0000'0000'0005ull, u"README~1.TXT", 2);
            const std::string text = "hello, world";
            std::vector<std::byte> data(text.size());
            std::memcpy(data.data(), text.data(), text.size());
            b.resident(0x80, data);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(info.inUse && !info.directory);
            QCOMPARE(info.sequence, std::uint16_t {1});
            QCOMPARE(info.names, std::uint16_t {1});
            QCOMPARE(info.standardInfoBytes, 0x48u);
            QCOMPARE(info.securityId, 0x123u);
            QCOMPARE(info.attributes, static_cast<std::uint32_t>(FILE_ATTRIBUTE_ARCHIVE));
            QCOMPARE(info.modified, 0x01DC'0000'1111'2222ll);
            QCOMPARE(info.mftChanged, 0x01DC'0000'3333'4444ll);
            QCOMPARE(info.data, ntfs::FileInfo::Data::Resident);
            QCOMPARE(info.size, std::uint64_t {text.size()});
            QCOMPARE(std::string(reinterpret_cast<const char*>(info.resident.data()), info.resident.size()), text);
            QVERIFY(!info.hasAttributeList && !info.hasReparse && !info.hasSecurityDescriptor);
        }
        // Non-resident: two runs, a valid data length short of the size.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_ARCHIVE, 0x124));
            b.fileName(0x0001'0000'0000'0005ull, u"big.log", 3);
            b.nonResidentData(runList({{40 * 4096, 3 * 4096}, {100 * 4096, 2 * 4096}}, 4096), 4, 5 * 4096, 18000, 9000, 0);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QCOMPARE(info.data, ntfs::FileInfo::Data::NonResident);
            QCOMPARE(info.size, std::uint64_t {18000});
            QCOMPARE(info.initialized, std::uint64_t {9000});
            QCOMPARE(info.allocated, std::uint64_t {5 * 4096});
            QCOMPARE(info.lowestVcn, std::uint64_t {0});
            QCOMPARE(info.highestVcn, std::uint64_t {4});
            QCOMPARE(info.dataFlags, std::uint16_t {0});
            const auto runs = ntfs::decodeDataRuns(info.runList);
            QVERIFY(runs);
            QCOMPARE(runs->size(), std::size_t {2});
            QCOMPARE((*runs)[0].lcn, std::int64_t {40});
            QCOMPARE((*runs)[0].clusters, std::uint64_t {3});
            QCOMPARE((*runs)[1].lcn, std::int64_t {100});
            QCOMPARE((*runs)[1].clusters, std::uint64_t {2});
        }
        // Compressed, encrypted, sparse: the flags; a reparse point and an
        // attribute list; two unnamed $DATAs.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_REPARSE_POINT, 0x125));
            b.resident(0x20, std::vector<std::byte>(0x20)); // $ATTRIBUTE_LIST
            b.fileName(0x0001'0000'0000'0005ull, u"packed.bin", 1);
            b.nonResidentData(runList({{40 * 4096, 16 * 4096}}, 4096), 15, 16 * 4096, 60000, 60000,
                ntfs::kDataCompressed | ntfs::kDataSparse | ntfs::kDataEncrypted);
            std::vector<std::byte> reparse(0x10);
            put<std::uint32_t>(reparse, 0, 0x8000'0017u); // IO_REPARSE_TAG_WOF
            b.resident(0xC0, reparse);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(info.hasAttributeList);
            QVERIFY(info.hasReparse);
            QCOMPARE(info.reparseTag, 0x8000'0017u);
            QVERIFY(info.dataFlags & ntfs::kDataCompressed);
            QVERIFY(info.dataFlags & ntfs::kDataSparse);
            QVERIFY(info.dataFlags & ntfs::kDataEncrypted);
            QVERIFY(info.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        }
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, standardInformation(FILE_ATTRIBUTE_ARCHIVE, 7));
            b.resident(0x80, std::vector<std::byte>(3));
            b.resident(0x80, std::vector<std::byte>(5));
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QCOMPARE(info.data, ntfs::FileInfo::Data::Several);
        }
        // Old volumes: a $SECURITY_DESCRIPTOR of the file's own, a short $STANDARD_INFORMATION.
        {
            RecordBuilder b(0x0001);
            b.resident(0x10, std::vector<std::byte>(0x30));
            b.resident(0x50, std::vector<std::byte>(0x40));
            b.resident(0x80, std::vector<std::byte>(1));
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(info.hasSecurityDescriptor);
            QCOMPARE(info.standardInfoBytes, 0x30u);
            QCOMPARE(info.securityId, 0u);
        }
        // A free record parses as not in use; garbage does not parse.
        {
            RecordBuilder b(0x0000);
            const std::vector<std::byte> record = fixedUp(b);
            QVERIFY(!record.empty() && ntfs::parseFileInfo(record, info));
            QVERIFY(!info.inUse);
            std::vector<std::byte> garbage(1024, std::byte {0xCC});
            QVERIFY(!ntfs::parseFileInfo(garbage, info));
        }

        // Run lists with a sparse run (no offset): kept as a run without
        // clusters; the next run's offset counts from the one before it.
        const std::vector<std::byte> withHole {std::byte {0x11}, std::byte {0x04}, std::byte {0x10}, // 4 clusters at 0x10
            std::byte {0x01}, std::byte {0x03}, // 3 clusters, sparse
            std::byte {0x11}, std::byte {0x02}, std::byte {0x05}, // 2 clusters at 0x15
            std::byte {0x00}};
        const auto runs = ntfs::decodeDataRuns(withHole);
        QVERIFY(runs);
        QCOMPARE(runs->size(), std::size_t {3});
        QCOMPARE((*runs)[0].lcn, std::int64_t {0x10});
        QCOMPARE((*runs)[1].lcn, std::int64_t {-1});
        QCOMPARE((*runs)[1].clusters, std::uint64_t {3});
        QCOMPARE((*runs)[2].lcn, std::int64_t {0x15});
        QVERIFY(!ntfs::decodeRunList(withHole, 4096)); // the MFT's own decoder refuses sparse runs
        const std::vector<std::byte> cut {std::byte {0x21}, std::byte {0x04}}; // ends inside a run
        QVERIFY(!ntfs::decodeDataRuns(cut));
        const std::vector<std::byte> noEnd {std::byte {0x11}, std::byte {0x04}, std::byte {0x10}};
        QVERIFY(!ntfs::decodeDataRuns(noEnd));
    }

    // The grams the bulk path takes from a file's bytes are those readFile()
    // takes from the file: the encoding from the first chunk, the chunks the same.
    void collectTextMatchesReadFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::mt19937_64 rng(20261014);
        const auto check = [&](const char* name, const QByteArray& bytes) {
            const QString path = QDir::toNativeSeparators(dir.filePath(QString::fromLatin1(name))); // as the index has paths
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(bytes) == bytes.size());
            file.close();
            const ContentIndexer::FileText fromFile = ContentIndexer::readFile(path.toStdWString(), 64ll << 20, {});
            const std::optional<std::uint32_t> stamp = ContentIndexer::stampOf(path.toStdWString());
            QVERIFY(stamp);
            const ContentIndexer::FileText fromBytes
                = ContentIndexer::collectText({bytes.constData(), static_cast<std::size_t>(bytes.size())}, *stamp);
            QCOMPARE(fromBytes.outcome, fromFile.outcome);
            QCOMPARE(fromBytes.stamp, fromFile.stamp);
            QVERIFY2(fromBytes.keys == fromFile.keys, name);
            QVERIFY(fromBytes.outcome == ContentIndexer::Outcome::Empty || !fromBytes.keys.empty());
        };
        check("ascii.txt", QByteArray("The quick brown fox jumps over the lazy dog.\nfor (int i = 0; i < n; ++i)\n"));
        check("utf8.md", QString(u"季度报告：第三季度收入增长 12%，详见附录。\nUnicode café naïve 日本語テキスト\n"_s).toUtf8());
        QByteArray utf16;
        utf16.append("\xFF\xFE", 2);
        const QString wide = u"合同编号 2026-10 甲方乙方 WinShun"_s;
        utf16.append(reinterpret_cast<const char*>(wide.utf16()), wide.size() * 2);
        check("utf16.txt", utf16);
        // Longer than a chunk (ContentScanner::kChunkBytes): several feeds,
        // words running across the chunk boundary.
        QByteArray big;
        const char* words[] = {"alpha", "beta", "gamma", "delta", "报告", "合同", "epsilon"};
        while (big.size() < static_cast<qsizetype>(ContentScanner::kChunkBytes) * 2 + 777) {
            big.append(words[rng() % std::size(words)]);
            big.append(rng() % 7 == 0 ? '\n' : ' ');
        }
        check("big.txt", big);
        check("empty.txt", QByteArray());
        QByteArray binary(3000, '\0');
        for (char& c : binary)
            c = static_cast<char>(rng());
        check("binary.dat", binary);
    }

    void usnRecords()
    {
        std::vector<std::byte> buffer(8);
        const std::int64_t next = 0x12345678;
        std::memcpy(buffer.data(), &next, 8);
        appendUsn(buffer, 0x0005'0000'0000'1000ull, 0x0001'0000'0000'0005ull, 4096, USN_REASON_FILE_CREATE,
            FILE_ATTRIBUTE_ARCHIVE, u"新建文本.txt");
        appendUsn(buffer, 0x77, 0x5, 4200, USN_REASON_FILE_DELETE, 0, u"v3 record", 3); // skipped
        appendUsn(buffer, 0x0001'0000'0000'2000ull, 0x5, 4300, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE,
            FILE_ATTRIBUTE_DIRECTORY, u"dir");
        std::vector<ntfs::UsnRecord> records;
        QCOMPARE(ntfs::parseUsnRecords(buffer, records), next);
        QCOMPARE(records.size(), std::size_t {2});
        QCOMPARE(ntfs::recordOf(records[0].file), std::uint32_t {0x1000});
        QCOMPARE(ntfs::recordOf(records[0].parent), ntfs::kRootRecord);
        QCOMPARE(records[0].usn, std::int64_t {4096});
        QCOMPARE(records[0].reason, std::uint32_t {USN_REASON_FILE_CREATE});
        QCOMPARE(std::u16string(records[0].name), std::u16string(u"新建文本.txt"));
        QCOMPARE(records[1].attributes, std::uint32_t {FILE_ATTRIBUTE_DIRECTORY});
        QCOMPARE(std::u16string(records[1].name), std::u16string(u"dir"));

        buffer.resize(buffer.size() - 4); // the last record cut short
        records.clear();
        QCOMPARE(ntfs::parseUsnRecords(buffer, records), std::int64_t {-1});
    }

    // The index service's wire format (VolumeProtocol.h): each payload comes
    // back as it went; one cut short, or with a name running past its end,
    // or a root that is no drive, is refused; a frame too long is refused
    // by its header alone; bytes a later version adds at the end are left alone.
    void volumeProtocol()
    {
        using namespace volproto;
        const auto payloadOf = [](std::span<const std::byte> frame) { return frame.subspan(Writer::kHeaderBytes); };
        const auto header = [](std::span<const std::byte> frame, Type& type, std::uint32_t& length) {
            return parseHeader(frame.first<8>(), type, length);
        };
        Type type {};
        std::uint32_t length = 0;

        Hello hello;
        hello.minor = 7;
        hello.version[0] = 1;
        hello.version[1] = 2;
        hello.version[2] = 3;
        hello.pid = 4242;
        Writer w;
        encode(w, hello);
        std::span<const std::byte> frame = w.finish(Type::Hello);
        QVERIFY(header(frame, type, length));
        QCOMPARE(type, Type::Hello);
        QCOMPARE(std::size_t {length}, frame.size() - Writer::kHeaderBytes);
        Hello back;
        QVERIFY(decode(payloadOf(frame), back));
        QCOMPARE(back.major, kMajor);
        QCOMPARE(back.minor, std::uint16_t {7});
        QCOMPARE(back.version[2], std::uint16_t {3});
        QCOMPARE(back.pid, std::uint32_t {4242});
        QVERIFY(!decode(payloadOf(frame).first(payloadOf(frame).size() - 1), back)); // cut short
        std::vector<std::byte> longer(payloadOf(frame).begin(), payloadOf(frame).end());
        longer.push_back(std::byte {0x99});
        QVERIFY(decode(longer, back)); // a later minor's field: ignored

        w.reset();
        encode(w, OpenMft {L"d:", 0xCAFEBABE, true});
        frame = w.finish(Type::OpenMft);
        OpenMft open;
        QVERIFY(header(frame, type, length) && type == Type::OpenMft);
        QVERIFY(decode(payloadOf(frame), open));
        QCOMPARE(open.root, std::wstring(L"d:"));
        QCOMPARE(open.serial, std::uint32_t {0xCAFEBABE});
        QVERIFY(open.lowPriority);
        for (const wchar_t* bad : {L"D:\\", L"DD", L"", L"\\\\?\\D:", L"1:"}) {
            w.reset();
            encode(w, OpenMft {bad, 0, false});
            QVERIFY2(!decode(payloadOf(w.finish(Type::OpenMft)), open), qPrintable(QString::fromWCharArray(bad)));
        }
        // A name whose length runs past the payload.
        w.reset();
        w.u16(2);
        w.u16(u'D');
        QVERIFY(!decode(payloadOf(w.finish(Type::OpenMft)), open));
        // A frame longer than any read: refused by the header.
        w.reset();
        w.u32(0);
        frame = w.finish(Type::OpenMft);
        std::vector<std::byte> tooLong(frame.begin(), frame.end());
        const std::uint32_t huge = kMaxPayloadBytes + 1;
        std::memcpy(tooLong.data(), &huge, 4);
        QVERIFY(!header(tooLong, type, length));

        w.reset();
        encode(w, MftInfo {{4096, 65536, 4096}, 123456789, 123456789ull * 4096});
        MftInfo info;
        QVERIFY(decode(payloadOf(w.finish(Type::MftInfo)), info));
        QCOMPARE(info.geometry.sectorSize, 4096u);
        QCOMPARE(info.geometry.clusterSize, 65536u);
        QCOMPARE(info.geometry.recordSize, 4096u);
        QCOMPARE(info.recordCount, std::uint64_t {123456789});
        QCOMPARE(info.validBytes, 123456789ull * 4096);

        w.reset();
        encode(w, MftDone {1, L"reading the MFT failed, error 1117"});
        MftDone done;
        QVERIFY(decode(payloadOf(w.finish(Type::MftDone)), done));
        QCOMPARE(done.status, 1u);
        QCOMPARE(done.error, std::wstring(L"reading the MFT failed, error 1117"));

        w.reset();
        encode(w, QueryJournal {L"E:", 77});
        QueryJournal query;
        QVERIFY(decode(payloadOf(w.finish(Type::QueryJournal)), query));
        QCOMPARE(query.root, std::wstring(L"E:"));
        QCOMPARE(query.serial, 77u);

        w.reset();
        encode(w, ntfs::JournalInfo {0x0123456789ABCDEFull, -5, 1ll << 40});
        ntfs::JournalInfo journal;
        QVERIFY(decode(payloadOf(w.finish(Type::JournalInfo)), journal));
        QCOMPARE(journal.id, 0x0123456789ABCDEFull);
        QCOMPARE(journal.firstUsn, std::int64_t {-5});
        QCOMPARE(journal.nextUsn, std::int64_t {1} << 40);

        w.reset();
        encode(w, FollowJournal {L"C:", 1, 2});
        FollowJournal follow;
        QVERIFY(decode(payloadOf(w.finish(Type::FollowJournal)), follow));
        QCOMPARE(follow.root, std::wstring(L"C:"));
        QCOMPARE(follow.serial, 1u);
        QCOMPARE(follow.journalId, std::uint64_t {2});

        w.reset();
        encode(w, Read {-1, true});
        Read read;
        QVERIFY(decode(payloadOf(w.finish(Type::Read)), read));
        QCOMPARE(read.usn, std::int64_t {-1});
        QVERIFY(read.wait);

        w.reset();
        encode(w, JournalLost {1181});
        JournalLost lost;
        QVERIFY(decode(payloadOf(w.finish(Type::JournalLost)), lost));
        QCOMPARE(lost.error, 1181u);

        w.reset();
        encode(w, CloseVolume {L"F:"});
        CloseVolume close;
        QVERIFY(decode(payloadOf(w.finish(Type::CloseVolume)), close));
        QCOMPARE(close.root, std::wstring(L"F:"));

        w.reset();
        encode(w, Closed {3});
        Closed closed;
        QVERIFY(decode(payloadOf(w.finish(Type::Closed)), closed));
        QCOMPARE(closed.closed, 3u);

        w.reset();
        encode(w, Error {5, L"拒绝访问"});
        Error error;
        QVERIFY(decode(payloadOf(w.finish(Type::Error)), error));
        QCOMPARE(error.code, 5u);
        QCOMPARE(error.message, std::wstring(L"拒绝访问"));

        w.reset();
        encode(w, HelloRefused {static_cast<std::uint32_t>(Refusal::TooManyConnections)});
        HelloRefused refused;
        QVERIFY(decode(payloadOf(w.finish(Type::HelloRefused)), refused));
        QCOMPARE(refused.reason, 2u);

        w.reset();
        HelloOk ok;
        ok.capabilities = 0x80000001;
        encode(w, ok);
        HelloOk okBack;
        QVERIFY(decode(payloadOf(w.finish(Type::HelloOk)), okBack));
        QCOMPARE(okBack.capabilities, 0x80000001u);
        QVERIFY(decode({}, okBack) == false);
    }

    // An MFT block as the service streams it: records parsed from the
    // builder's bytes, written, read back the same, record by record; a
    // free record, one that is no record, an extension record. Then the
    // block mangled every way a bad service could: refused whole.
    void volumeProtocolBlock()
    {
        using namespace volproto;
        RecordBuilder b(0x0003); // in use, folder
        b.standardInformation(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);
        b.fileName(0x0002'0000'0000'04D2ull, u"Long name 中文", 1);
        const std::u16string longName(255, u'x');
        b.fileName(0x0001'0000'0000'0309ull, longName, 0);
        // Each record's bytes stay: the parsed names point into them.
        std::vector<std::byte> folderBytes = b.finish();
        ntfs::FileRecord folder;
        QVERIFY(ntfs::parseFileRecord(folderBytes.data(), folderBytes.size(), folder));
        folder.created = 0x01DC'0000'1234'5678ll;
        RecordBuilder unused(0x0000);
        unused.fileName(5, u"deleted.txt", 1);
        std::vector<std::byte> freeBytes = unused.finish();
        ntfs::FileRecord free;
        QVERIFY(ntfs::parseFileRecord(freeBytes.data(), freeBytes.size(), free));
        RecordBuilder extension(0x0001, 0x0003'0000'0000'002Aull);
        extension.fileName(5, u"link.txt", 0);
        std::vector<std::byte> extBytes = extension.finish();
        ntfs::FileRecord ext;
        QVERIFY(ntfs::parseFileRecord(extBytes.data(), extBytes.size(), ext));

        RecordBuilder fileBuilder(0x0001);
        fileBuilder.fileName(5, u"data.bin", 1);
        fileBuilder.resident(0x80, std::vector<std::byte>(300));
        std::vector<std::byte> fileBytes = fileBuilder.finish();
        ntfs::FileRecord file;
        QVERIFY(ntfs::parseFileRecord(fileBytes.data(), fileBytes.size(), file));
        QVERIFY(file.sizeKnown && file.size == 300);

        BlockWriter bw;
        bw.begin(1000);
        bw.add(folder, true);
        bw.add(free, true);
        bw.add(folder, false); // garbage on the volume
        bw.add(ext, true);
        bw.add(file, true);
        QCOMPARE(bw.count(), 5u);
        const std::span<const std::byte> frame = bw.finish(12345);
        Type type {};
        std::uint32_t length = 0;
        QVERIFY(parseHeader(frame.first<8>(), type, length));
        QCOMPARE(type, Type::MftBlock);
        const std::vector<std::byte> payload(frame.begin() + Writer::kHeaderBytes, frame.end());
        QCOMPARE(std::size_t {length}, payload.size());

        BlockReader br;
        QVERIFY(br.load(std::vector<std::byte>(payload)));
        QCOMPARE(br.firstRecord(), 1000u);
        QCOMPARE(br.count(), 5u);
        QCOMPARE(br.bytesDone(), std::uint64_t {12345});
        ntfs::FileRecord out;
        QVERIFY(br.record(0, out));
        QVERIFY(out.inUse && out.directory && !out.hasAttributeList);
        QCOMPARE(out.baseRecord, 0u);
        QCOMPARE(out.attributes, std::uint32_t {FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE});
        QCOMPARE(out.modified, folder.modified);
        QCOMPARE(out.created, folder.created);
        QCOMPARE(out.names.size(), std::size_t {2});
        QCOMPARE(out.names[0].parent, 0x0002'0000'0000'04D2ull);
        QCOMPARE(std::u16string(out.names[0].name), std::u16string(u"Long name 中文"));
        QCOMPARE(std::u16string(out.names[1].name), longName);
        QVERIFY(br.record(1, out));
        QVERIFY(!out.inUse && out.names.empty());
        QCOMPARE(out.created, std::int64_t {0});
        QVERIFY(!br.record(2, out));
        QVERIFY(br.record(3, out));
        QVERIFY(out.inUse);
        QCOMPARE(out.baseRecord, 42u);
        QCOMPARE(out.names.size(), std::size_t {1});
        QCOMPARE(std::u16string(out.names[0].name), std::u16string(u"link.txt"));
        QVERIFY(!out.sizeKnown);
        QVERIFY(br.record(4, out));
        QVERIFY(out.sizeKnown);
        QCOMPARE(out.size, std::uint64_t {300});
        QVERIFY(!br.record(5, out)); // past the end

        // The other end at minor 1: no sizes in the records, either way.
        BlockWriter old;
        old.begin(1000, false);
        old.add(file, true);
        const std::span<const std::byte> oldFrame = old.finish(1);
        const std::vector<std::byte> oldPayload(oldFrame.begin() + Writer::kHeaderBytes, oldFrame.end());
        BlockReader oldReader;
        QVERIFY(!oldReader.load(std::vector<std::byte>(oldPayload), true)); // told wrongly: cut short
        QVERIFY(oldReader.load(std::vector<std::byte>(oldPayload), false));
        QVERIFY(oldReader.record(0, out));
        QVERIFY(out.inUse && !out.sizeKnown && out.size == 0);
        QCOMPARE(std::u16string(out.names[0].name), std::u16string(u"data.bin"));

        // Minor 2 keeps file sizes, without the new creation-time column.
        BlockWriter versionTwo;
        versionTwo.begin(1000, true, false);
        versionTwo.add(folder, true);
        const auto versionTwoFrame = versionTwo.finish(1);
        std::vector<std::byte> versionTwoPayload(versionTwoFrame.begin() + Writer::kHeaderBytes, versionTwoFrame.end());
        BlockReader versionTwoReader;
        QVERIFY(versionTwoReader.load(std::move(versionTwoPayload), true, false));
        QVERIFY(versionTwoReader.record(0, out));
        QCOMPARE(out.created, std::int64_t {0});
        QCOMPARE(out.modified, folder.modified);

        // Mangled: each refused whole, and the reader keeps nothing.
        const auto refused = [&](std::vector<std::byte> bad, const char* what) {
            BlockReader r;
            QVERIFY2(!r.load(std::move(bad)), what);
            QCOMPARE(r.count(), 0u);
        };
        std::vector<std::byte> bad = payload;
        put<std::uint32_t>(bad, 4, 6); // one record more than there is
        refused(bad, "count past the end");
        bad = payload;
        bad.pop_back(); // the last name cut short
        refused(bad, "cut short");
        bad = payload;
        bad.push_back(std::byte {0}); // a byte after the last record
        refused(bad, "trailing bytes");
        bad = payload;
        bad[16] = std::byte {3}; // a record kind that does not exist
        refused(bad, "unknown kind");
        bad = payload;
        bad[16 + 1 + 4 + 4 + 8 + 1] = std::byte {5}; // the folder says five names, has two
        refused(bad, "names past the end");
        bad = payload;
        put<std::uint16_t>(bad, 16 + 1 + 4 + 4 + 8 + 1 + 1 + 8, 0); // an empty name
        refused(bad, "empty name");
        refused({}, "nothing at all");
        bad.assign(16, std::byte {0}); // an empty block is fine
        BlockReader empty;
        QVERIFY(empty.load(std::move(bad)));
        QCOMPARE(empty.count(), 0u);
    }

    void usnApply()
    {
        // A synthetic volume: the root's name is a real folder, for the steps
        // that look at the disk (hard links, folders moved in from outside).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::wstring rootPath = QDir::toNativeSeparators(tmp.path()).toStdWString();
        const std::string rootName = wtf8::fromUtf16(wtf8::view(rootPath));
        CrawlRules rules;
        rules.excludedNames.push_back(L"skip");
        const Crawler crawler(rules);
        FileIndex index;
        const EntryId root = index.addRoot(rootName);
        const EntryId docs = index.add(root, "docs", EntryFlag::Directory);
        const EntryId dst = index.add(root, "dst", EntryFlag::Directory);
        const EntryId a = index.add(docs, "a.txt", 0);
        index.add(docs, "x", 0);
        const EntryId upperX = index.add(docs, "X", 0);
        index.setFolderRecord(root, ntfs::kRootRecord, root);
        index.setFolderRecord(root, 100, docs);
        index.setFolderRecord(root, 200, dst);

        constexpr DWORD kDir = FILE_ATTRIBUTE_DIRECTORY;
        const auto ref = [](std::uint32_t record) { return std::uint64_t {record} | (std::uint64_t {1} << 48); };
        std::int64_t usn = 0;
        const auto rec = [&](std::uint32_t file, std::uint32_t parent, std::uint32_t reason, DWORD attributes,
                             std::u16string_view name) {
            return ntfs::UsnRecord {ref(file), ref(parent), usn += 100, reason, attributes, name};
        };
        // (findPath starts from a drive letter; this root is a whole folder path.)
        const auto childOf = [&](EntryId parent, std::string_view name) { return index.findChild(parent, name); };

        const std::vector<ntfs::UsnRecord> batch {
            rec(1000, 100, USN_REASON_FILE_CREATE, 0, u"new.txt"),
            rec(1000, 100, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND, 0, u"new.txt"), // a repeat
            rec(1000, 100, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND | USN_REASON_CLOSE, 0, u"new.txt"),
            rec(300, 100, USN_REASON_FILE_CREATE, kDir, u"sub"),
            rec(1001, 300, USN_REASON_FILE_CREATE | USN_REASON_CLOSE, 0, u"inner.txt"),
            rec(301, 100, USN_REASON_FILE_CREATE, kDir, u"skip"), // excluded by name
            rec(1002, 301, USN_REASON_FILE_CREATE, 0, u"ignored.txt"), // inside it
            rec(1003, 100, USN_REASON_RENAME_OLD_NAME, 0, u"a.txt"), // a.txt -> dst\b.txt
            rec(1003, 200, USN_REASON_RENAME_NEW_NAME, 0, u"b.txt"),
            rec(1003, 200, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, 0, u"b.txt"),
            rec(300, 100, USN_REASON_RENAME_OLD_NAME, kDir, u"sub"), // sub -> dst\Sub2, with its file
            rec(300, 200, USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME, kDir, u"Sub2"),
            rec(1004, 100, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, 0, u"X"), // only "X", not "x"
            rec(1000, 100, USN_REASON_BASIC_INFO_CHANGE, FILE_ATTRIBUTE_HIDDEN, u"new.txt"),
            // 0x40000 on disk: extended attributes (code integrity), not a cloud file.
            rec(1007, 100, USN_REASON_FILE_CREATE | USN_REASON_CLOSE, 0x40000 | FILE_ATTRIBUTE_ARCHIVE, u"signed.dll"),
        };
        std::vector<EntryId> written;
        UsnApplier applier(index, crawler, rootName, [&](std::span<const EntryId> ids) {
            written.insert(written.end(), ids.begin(), ids.end());
        });
        applier.apply(batch, {});
        QVERIFY(!written.empty());
        QVERIFY(std::all_of(written.begin(), written.end(), [&](EntryId id) { return index.name(id) == "new.txt"; }));
        written.clear();
        applier.apply(std::vector {rec(1007, 100, USN_REASON_DATA_OVERWRITE | USN_REASON_CLOSE, 0, u"signed.dll"),
                          rec(1008, 100, USN_REASON_DATA_EXTEND, 0, u"unknown.txt")}, {});
        QCOMPARE(written.size(), std::size_t {1}); // not the file the index does not have
        QCOMPARE(index.name(written[0]), std::string_view("signed.dll"));
        const EntryId sub = index.folderByRecord(root, 300);
        QVERIFY(sub != kNoEntry);
        QCOMPARE(index.path(sub), QString::fromStdWString(rootPath + L"\\dst\\Sub2"));
        QVERIFY(childOf(sub, "inner.txt") != kNoEntry);
        QCOMPARE(childOf(docs, "skip"), kNoEntry);
        QCOMPARE(index.folderByRecord(root, 301), kNoEntry);
        QCOMPARE(childOf(docs, "a.txt"), kNoEntry);
        QCOMPARE(childOf(dst, "b.txt"), a); // moved, same entry
        QVERIFY(index.entry(upperX).isDeleted());
        QVERIFY(childOf(docs, "x") != kNoEntry);
        const EntryId created = childOf(docs, "new.txt");
        QVERIFY(created != kNoEntry);
        QVERIFY(index.entry(created).flags & EntryFlag::Hidden);
        const EntryId dll = childOf(docs, "signed.dll");
        QVERIFY(dll != kNoEntry);
        QVERIFY(!(index.entry(dll).flags & EntryFlag::Offline));
        QCOMPARE(applier.safePosition(usn + 1), usn + 1);

        const std::vector<ntfs::UsnRecord> second {
            rec(1001, 300, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, 0, u"inner.txt"),
            rec(300, 200, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, kDir, u"Sub2"),
            rec(1000, 100, USN_REASON_RENAME_OLD_NAME, 0, u"new.txt"), // into a folder outside the index
            rec(1000, 9999, USN_REASON_RENAME_NEW_NAME, 0, u"new.txt"),
            rec(1005, 200, USN_REASON_RENAME_OLD_NAME, 0, u"b.txt"), // its new name comes later
        };
        applier.apply(second, {});
        QCOMPARE(index.folderByRecord(root, 300), kNoEntry);
        QVERIFY(index.entry(sub).isDeleted());
        QCOMPARE(childOf(docs, "new.txt"), kNoEntry);
        QCOMPARE(applier.safePosition(usn + 1), usn); // replay from the pending rename

        // Replaying everything (as after a restart) changes nothing.
        const std::size_t live = index.liveCount();
        UsnApplier replay(index, crawler, rootName);
        replay.apply(batch, {});
        replay.apply(second, {});
        QCOMPARE(index.liveCount(), live);
        QCOMPARE(childOf(docs, "new.txt"), kNoEntry);
        QCOMPARE(childOf(dst, "b.txt"), a);
        QCOMPARE(index.folderByRecord(root, 300), kNoEntry);

        // Hard links: the disk tells whether the named link was added or removed.
        QVERIFY(QDir(tmp.path()).mkpath(u"dst"_s));
        QFile link(tmp.filePath(u"dst/link.txt"_s));
        QVERIFY(link.open(QIODevice::WriteOnly));
        link.close();
        applier.apply(std::vector {rec(1006, 200, USN_REASON_HARD_LINK_CHANGE | USN_REASON_CLOSE, 0, u"link.txt")}, {});
        QVERIFY(childOf(dst, "link.txt") != kNoEntry);
        QVERIFY(link.remove());
        applier.apply(std::vector {rec(1006, 200, USN_REASON_HARD_LINK_CHANGE | USN_REASON_CLOSE, 0, u"link.txt")}, {});
        QCOMPARE(childOf(dst, "link.txt"), kNoEntry);

        // A folder moved in from outside the index is walked, and its subfolders registered.
        QVERIFY(QDir(tmp.path()).mkpath(u"docs/moved/deeper"_s));
        QFile deep(tmp.filePath(u"docs/moved/deeper/deep.txt"_s));
        QVERIFY(deep.open(QIODevice::WriteOnly));
        deep.close();
        applier.apply(std::vector {rec(500, 100, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, kDir, u"moved")}, {});
        const EntryId moved = index.folderByRecord(root, 500);
        QVERIFY(moved != kNoEntry);
        const EntryId deeper = childOf(moved, "deeper");
        QVERIFY(deeper != kNoEntry);
        QVERIFY(childOf(deeper, "deep.txt") != kNoEntry);
        QCOMPARE(index.folderByRecord(root, recordOfPath(tmp.filePath(u"docs/moved/deeper"_s))), deeper);
    }

    void interning()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        index.setInterning(true);
        const EntryId a = index.add(root, "index.js", 0);
        const EntryId dir = index.add(root, "lib", EntryFlag::Directory);
        const EntryId b = index.add(dir, "index.js", 0);
        const EntryId c = index.add(dir, "INDEX.js", 0); // different bytes: not shared
        QCOMPARE(index.entry(a).nameOffset, index.entry(b).nameOffset);
        QVERIFY(index.entry(a).nameOffset != index.entry(c).nameOffset);
        for (int i = 0; i < 200000; ++i) // forces the table to grow
            index.add(dir, "n" + std::to_string(i % 70000), 0);
        index.setInterning(false);
        const EntryId d = index.add(dir, "index.js", 0); // interning off: stored again
        QVERIFY(index.entry(d).nameOffset != index.entry(a).nameOffset);
        QCOMPARE(index.name(d), std::string_view("index.js"));
        QCOMPARE(index.path(b), u"C:\\lib\\index.js"_s);
    }

    void nameStorage()
    {
        // Names of every length (one- and two-byte lengths), with and without
        // extensions and Chinese characters, over many chunks: each read back
        // as it was, and each once in a pass over the stored names.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "目录.d", EntryFlag::Directory);
        QVERIFY(index.entry(dir).flags & EntryFlag::Han);
        QCOMPARE(index.extLength(index.entry(dir)), std::size_t {0}); // a folder's
        std::vector<std::pair<EntryId, std::string>> added;
        for (int i = 0; i < 40000; ++i) {
            std::string name = "n" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 300), 'x');
            if (i % 3 == 0)
                name += ".txt";
            if (i % 7 == 0)
                name += "报告";
            added.push_back({index.add(dir, name, 0), name});
        }
        const EntryId cut = index.add(dir, std::string(FileIndex::kMaxName + 10, 'y'), 0);
        QCOMPARE(index.name(cut), std::string_view(std::string(FileIndex::kMaxName, 'y')));
        const EntryId longExtension = index.add(dir, "a." + std::string(200, 'e'), 0);
        QCOMPARE(index.extLength(index.entry(longExtension)), std::size_t {0}); // 128 bytes or more: none
        for (const auto& [id, name] : added) {
            QCOMPARE(index.name(id), std::string_view(name));
            QCOMPARE(index.extLength(index.entry(id)), std::size_t {extensionLength(name, false)});
            QCOMPARE((index.entry(id).flags & EntryFlag::Han) != 0, pinyin::hasHan(name));
        }

        QVERIFY(index.nameChunkCount() > 10);
        std::set<std::uint32_t> offsets;
        bool same = true;
        for (std::size_t c = 0; c < index.nameChunkCount(); ++c) {
            index.forEachName(c, [&](std::uint32_t offset, std::string_view name) {
                same = same && index.nameAt(offset) == name;
                offsets.insert(offset);
            });
        }
        QVERIFY(same);
        QCOMPARE(offsets.size(), added.size() + 4); // and the root, the folder, the two above
        for (std::size_t i = 0; i < index.slotCount(); ++i)
            QVERIFY(offsets.contains(index.entry(static_cast<EntryId>(i)).nameOffset));

        // After a compaction, the same names.
        for (std::size_t i = 0; i < added.size(); i += 2)
            index.remove(added[i].first);
        index.compact();
        std::size_t live = 0;
        for (std::size_t i = 0; i < index.slotCount(); ++i) {
            const std::string_view name = index.name(static_cast<EntryId>(i));
            live += name.starts_with('n');
        }
        QCOMPARE(live, added.size() / 2);
        QCOMPARE(index.name(index.findPath(L"C:\\目录.d\\n1x")), std::string_view("n1x"));
    }

    void compact()
    {
        SampleTree t;
        FileIndex& index = t.index;
        index.setInterning(true);
        const EntryId lib = index.add(t.me, "lib", EntryFlag::Directory);
        const EntryId js1 = index.add(lib, "index.js", 0);
        const EntryId js2 = index.add(t.docs, "index.js", 0);
        index.setInterning(false);
        QCOMPARE(index.entry(js1).nameOffset, index.entry(js2).nameOffset);
        QCOMPARE(index.compact(), std::size_t {0}); // nothing removed yet

        index.remove(t.notes);
        index.remove(lib); // with its child
        QVERIFY(index.move(t.report, t.users, "Report-final.docx")); // leaves the old name behind
        index.setModified(t.report, 1'700'000'000);
        index.setModified(js2, 1'600'000'000);
        QCOMPARE(index.slotCount(), std::size_t {10});
        QCOMPARE(index.liveCount(), std::size_t {7});

        // Ids kept elsewhere are renumbered alongside, in the same order.
        const std::uint64_t generation = index.generation();
        std::vector<std::pair<EntryId, QString>> kept;
        for (EntryId id = 0; id < index.slotCount(); ++id)
            kept.emplace_back(id, index.entry(id).isDeleted() ? QString() : index.path(id));
        {
            const FileIndex::IdPin pin = index.pinIds();
            QVERIFY(index.idsPinned());
        }
        QVERIFY(!index.idsPinned());
        QCOMPARE(index.compact(false, [&](const FileIndex::Renumber& renumber) {
            for (auto& [id, path] : kept)
                id = renumber(id);
        }), std::size_t {3});
        QCOMPARE(index.generation(), generation + 1);
        for (const auto& [id, path] : kept)
            QCOMPARE(id == kNoEntry ? QString() : index.path(id), path);
        QCOMPARE(index.slotCount(), std::size_t {7});
        QCOMPARE(index.liveCount(), std::size_t {7});
        for (std::size_t i = 0; i < index.slotCount(); ++i)
            QVERIFY(!index.entry(static_cast<EntryId>(i)).isDeleted());
        QCOMPARE(index.roots().size(), std::size_t {1});
        QCOMPARE(index.path(index.roots().front()), u"C:\\"_s);

        const EntryId report = index.findPath(L"C:\\Users\\Report-final.docx");
        QVERIFY(report != kNoEntry);
        QCOMPARE(index.extLength(index.entry(report)), std::size_t {4});
        QCOMPARE(index.modified(report), FileTime {1'700'000'000}); // the times move with their entries
        const EntryId js = index.findPath(L"C:\\Users\\me\\Documents\\index.js");
        QVERIFY(js != kNoEntry);
        QCOMPARE(index.modified(js), FileTime {1'600'000'000});
        QCOMPARE(index.modified(index.add(t.users, "new.txt", 0)), FileTime {0}); // a slot used again starts with none
        QCOMPARE(index.path(js), u"C:\\Users\\me\\Documents\\index.js"_s);
        QVERIFY(index.entry(index.findPath(L"C:\\Windows")).flags & EntryFlag::LowPriority);
        QCOMPARE(index.findPath(L"C:\\Users\\me\\lib"), kNoEntry);
        QCOMPARE(index.findPath(L"C:\\Users\\me\\Documents\\notes.txt"), kNoEntry);

        // The sibling lists still work both ways: remove the middle child, add new ones.
        const EntryId users = index.findPath(L"C:\\Users");
        const EntryId me = index.findPath(L"C:\\Users\\me");
        QCOMPARE(index.remove(me), std::size_t {3});
        QCOMPARE(index.findChild(users, "Report-final.docx"), report);
        const EntryId added = index.add(users, "new.txt", 0);
        QCOMPARE(index.path(added), u"C:\\Users\\new.txt"_s);
        QCOMPARE(index.compact(), std::size_t {3});
        QCOMPARE(index.path(index.findPath(L"C:\\Users\\new.txt")), u"C:\\Users\\new.txt"_s);
        QCOMPARE(index.path(index.findPath(L"C:\\Users\\Report-final.docx")), u"C:\\Users\\Report-final.docx"_s);

        // Large enough to span several chunks and grow the name map.
        FileIndex big;
        const EntryId root = big.addRoot("D:");
        std::vector<EntryId> files;
        big.setInterning(true);
        for (int i = 0; i < 200000; ++i)
            files.push_back(big.add(root, "f" + std::to_string(i % 5000) + ".txt", 0));
        big.setInterning(false);
        for (std::size_t i = 0; i < files.size(); i += 3)
            big.remove(files[i]);
        const std::size_t live = big.liveCount();
        QCOMPARE(big.compact(), files.size() + 1 - live);
        QCOMPARE(big.slotCount(), live);
        std::set<std::uint32_t> offsets; // shared names stay shared: 5000 file names + "D:"
        for (std::size_t i = 0; i < live; ++i)
            offsets.insert(big.entry(static_cast<EntryId>(i)).nameOffset);
        QCOMPARE(offsets.size(), std::size_t {5001});
        std::size_t children = 0;
        for (EntryId c = big.entry(big.roots().front()).firstChild; c != kNoEntry; c = big.entry(c).nextSibling) {
            QCOMPARE(big.entry(c).parent, big.roots().front());
            ++children;
        }
        QCOMPARE(children, live - 1);
        QCOMPARE(big.name(static_cast<EntryId>(live - 1)), std::string_view("f4999.txt"));
    }

    void syncWithDisk()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir base(tmp.path());
        const auto touch = [&](const QString& rel) {
            QFile f(base.filePath(rel));
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        QVERIFY(base.mkpath(u"docs/sub"_s));
        QVERIFY(base.mkpath(u"node_modules/pkg"_s));
        QVERIFY(base.mkpath(u"skip/inner"_s));
        QVERIFY(base.mkpath(u".tool"_s));
        touch(u"docs/a.txt"_s);
        touch(u"docs/sub/b.txt"_s);
        touch(u"node_modules/pkg/index.js"_s);
        touch(u"skip/inner/c.txt"_s);

        const std::wstring rootPath = QDir::toNativeSeparators(tmp.path()).toStdWString();
        CrawlRules rules;
        rules.excludedPaths.push_back(rootPath + L"\\skip");
        rules.lowPriorityNames.push_back(L"node_modules");
        const Crawler crawler(rules);

        FileIndex index;
        const EntryId root = index.addRoot("X:");
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 4, false, {}));
        // docs, sub, a, b, node_modules, pkg, index.js, .tool  (+ root)
        QCOMPARE(index.liveCount(), std::size_t {9});
        QCOMPARE(index.findPath(L"X:\\skip"), kNoEntry);
        const EntryId js = index.findPath(L"X:\\node_modules\\pkg\\index.js");
        QVERIFY(js != kNoEntry);
        QVERIFY(index.entry(js).flags & EntryFlag::LowPriority); // inherited
        QVERIFY(index.entry(index.findPath(L"X:\\.tool")).flags & EntryFlag::LowPriority);

        // Change the disk, then sync again in place.
        QVERIFY(QFile::remove(base.filePath(u"docs/a.txt"_s)));
        QVERIFY(QDir(base.filePath(u"docs/sub"_s)).removeRecursively());
        touch(u"docs/new.txt"_s);
        QVERIFY(base.rename(u"node_modules"_s, u"Node_Modules2"_s));
        const EntryId docs = index.findPath(L"X:\\docs");
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, true, {}));
        QCOMPARE(index.findPath(L"X:\\docs"), docs); // unchanged folders keep their entry
        QCOMPARE(index.findPath(L"X:\\docs\\a.txt"), kNoEntry);
        QCOMPARE(index.findPath(L"X:\\docs\\sub"), kNoEntry);
        QVERIFY(index.findPath(L"X:\\docs\\new.txt") != kNoEntry);
        QVERIFY(index.findPath(L"X:\\Node_Modules2\\pkg\\index.js") != kNoEntry);
        QVERIFY(!(index.entry(index.findPath(L"X:\\Node_Modules2")).flags & EntryFlag::LowPriority));
        QCOMPARE(index.liveCount(), std::size_t {7});

        // A case-only rename keeps the entry; a stale entry that differs only
        // in case from one on disk is dropped instead of piling up.
        const EntryId fresh = index.findPath(L"X:\\docs\\new.txt");
        QVERIFY(base.rename(u"docs/new.txt"_s, u"docs/NEW.txt"_s));
        touch(u"docs/x.txt"_s);
        index.add(docs, "x.txt", 0);
        index.add(docs, "X.TXT", 0);
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, true, {}));
        QCOMPARE(index.findPath(L"X:\\docs\\NEW.txt"), fresh);
        QCOMPARE(index.name(fresh), std::string_view("NEW.txt"));
        std::vector<std::string> names;
        for (EntryId c = index.entry(docs).firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
            names.emplace_back(index.name(c));
        std::sort(names.begin(), names.end());
        QCOMPARE(names, (std::vector<std::string> {"NEW.txt", "x.txt"}));
    }

    void uncPaths()
    {
        QCOMPARE(unc::normalize(L"\\\\Host\\Share\\"), std::wstring(L"\\\\Host\\Share"));
        QCOMPARE(unc::normalize(L"//nas/docs/a//b/"), std::wstring(L"\\\\nas\\docs\\a\\b"));
        QCOMPARE(unc::normalize(L"\\\\192.168.1.10\\资料"), std::wstring(L"\\\\192.168.1.10\\资料"));
        QVERIFY(unc::normalize(L"\\\\host").empty()); // a server, no share
        QVERIFY(unc::normalize(L"\\\\host\\").empty());
        QVERIFY(unc::normalize(L"\\\\?\\UNC\\host\\share").empty());
        QVERIFY(unc::normalize(L"C:\\x").empty());
        QVERIFY(unc::normalize(L"\\\\host\\sh*re").empty());
        QVERIFY(unc::isUnc(L"\\\\nas\\docs") && !unc::isUnc(L"\\\\.\\C:") && !unc::isUnc(L"C:\\"));
        QCOMPARE(unc::host(L"\\\\nas\\docs\\a"), std::wstring_view(L"nas"));
        QCOMPARE(unc::host(L"\\\\192.168.1.10"), std::wstring_view(L"192.168.1.10")); // signing in to a server
        QVERIFY(unc::shareRoot(L"\\\\192.168.1.10").empty());
        QCOMPARE(unc::shareRoot(L"\\\\nas\\docs\\a\\b"), std::wstring_view(L"\\\\nas\\docs"));
        QVERIFY(unc::contains(L"\\\\nas\\docs", L"\\\\NAS\\Docs\\a"));
        QVERIFY(unc::contains(L"\\\\nas\\docs", L"\\\\nas\\docs"));
        QVERIFY(!unc::contains(L"\\\\nas\\docs", L"\\\\nas\\docs2\\a"));
    }

    // Shares are roots named by their path, next to the drives; a path finds
    // the share it is on by the longest name that starts it.
    void indexUncRoots()
    {
        FileIndex index;
        const EntryId c = index.addRoot("C:");
        const EntryId share = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs");
        const EntryId share2 = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs2");
        const EntryId deeper = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs\\team");
        QCOMPARE(ShareIndexer::ensureRoot(index, L"\\\\NAS\\Docs"), share); // made once
        QVERIFY(index.entry(share).flags & EntryFlag::Remote);
        const EntryId a = index.add(share, "a", EntryFlag::Directory | EntryFlag::Remote);
        const EntryId file = index.add(a, "plan.xlsx", EntryFlag::Remote);
        const EntryId other = index.add(share2, "a", EntryFlag::Directory | EntryFlag::Remote);
        const EntryId team = index.add(deeper, "x.txt", EntryFlag::Remote);
        index.add(c, "a", EntryFlag::Directory);

        QCOMPARE(index.path(share), u"\\\\nas\\docs\\"_s); // a root alone ends in '\', as "C:\" does
        QCOMPARE(index.path(file), u"\\\\nas\\docs\\a\\plan.xlsx"_s);
        QCOMPARE(index.findPath(L"\\\\NAS\\docs\\A\\plan.xlsx"), file);
        QCOMPARE(index.findPath(L"\\\\nas\\docs"), share);
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\"), share);
        QCOMPARE(index.findPath(L"\\\\nas\\docs2\\a"), other); // not docs + "2\a"
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\team\\x.txt"), team); // the longest
        QCOMPARE(index.findPath(L"\\\\nas\\other\\a"), kNoEntry);
        QCOMPARE(index.findPath(L"\\\\nas"), kNoEntry);
        QVERIFY(index.findPath(L"C:\\a") != kNoEntry);

        // A share no longer set is dropped from an index loaded from disk.
        QCOMPARE(ShareIndexer::dropUnknownRoots(index, {{L"\\\\nas\\docs", {}}}), std::size_t {4}); // 2 roots, 2 items
        QCOMPARE(index.findPath(L"\\\\nas\\docs2\\a"), kNoEntry);
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\team\\x.txt"), kNoEntry);
        QCOMPARE(index.findPath(L"\\\\nas\\docs\\a\\plan.xlsx"), file);
        QVERIFY(index.findPath(L"C:\\a") != kNoEntry);
    }

    // Excluded names count from the share on: a server called like an
    // excluded folder is no reason to leave its shares out.
    void uncExcludedPath()
    {
        const Crawler crawler(CrawlRules {{L"\\\\nas\\docs\\private"}, {L"nas", L"#recycle", L"docs"}, {}, {}});
        QVERIFY(!crawler.isExcludedPath(L"\\\\nas\\docs\\a\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"\\\\nas\\docs\\#recycle\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"\\\\nas\\docs\\a\\docs\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"\\\\nas\\docs\\private\\b.txt"));
    }

    // A whole drive excluded (GitHub #17): "X:\" in the settings is "X:" here.
    void driveExcludedPath()
    {
        const Crawler crawler(CrawlRules {{L"X:", L"C:\\skip"}, {}, {}, {}});
        QVERIFY(crawler.isExcludedDir(L"X:", L"X:"));
        QVERIFY(crawler.isExcludedPath(L"X:"));
        QVERIFY(crawler.isExcludedPath(L"x:\\a\\b.txt"));
        QVERIFY(crawler.isExcludedPath(L"C:\\skip\\b.txt"));
        QVERIFY(!crawler.isExcludedPath(L"C:\\a\\b.txt"));
        QVERIFY(!crawler.isExcludedPath(L"Y:\\a"));
    }

    // A sync that defers removals keeps what is gone until told; a folder
    // that cannot be read keeps what it had.
    void syncDefersRemovals()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir base(tmp.path());
        const auto touch = [&](const QString& rel) {
            QFile f(base.filePath(rel));
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        QVERIFY(base.mkpath(u"docs/sub"_s));
        QVERIFY(base.mkpath(u"locked"_s));
        touch(u"docs/a.txt"_s);
        touch(u"docs/sub/b.txt"_s);
        touch(u"locked/c.txt"_s);
        const std::wstring rootPath = QDir::toNativeSeparators(tmp.path()).toStdWString();
        const Crawler crawler(CrawlRules {});
        FileIndex index;
        const EntryId root = index.addRoot("X:");
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, false, {}));
        QCOMPARE(index.liveCount(), std::size_t {7});

        QVERIFY(QFile::remove(base.filePath(u"docs/a.txt"_s)));
        QVERIFY(QDir(base.filePath(u"docs/sub"_s)).removeRecursively());
        QVERIFY(QFile::remove(base.filePath(u"locked/c.txt"_s)));
        touch(u"docs/new.txt"_s);
        // "locked" cannot be read this time: as if access was denied.
        const Crawler::Lister list = [&](const Crawler::Root& dir, DirListing& out) {
            if (dir.path.ends_with(L"\\locked"))
                return false;
            return crawler.listDirectory(dir, out);
        };
        std::vector<EntryId> gone;
        Crawler::SyncStats stats;
        QVERIFY(crawler.sync(index, {{root, rootPath, 0}}, 2, false, {}, list, &gone, &stats));
        QVERIFY(index.findPath(L"X:\\docs\\new.txt") != kNoEntry); // added at once
        QVERIFY(index.findPath(L"X:\\docs\\a.txt") != kNoEntry); // removed only when told
        QVERIFY(index.findPath(L"X:\\locked\\c.txt") != kNoEntry);
        QCOMPARE(gone.size(), std::size_t {2}); // a.txt and sub (with b.txt in it)
        QCOMPARE(stats.failed, std::size_t {1});
        QCOMPARE(stats.listed, std::size_t {2}); // the root and docs
        for (const EntryId id : gone)
            index.remove(id);
        QCOMPARE(index.findPath(L"X:\\docs\\a.txt"), kNoEntry);
        QCOMPARE(index.findPath(L"X:\\docs\\sub\\b.txt"), kNoEntry);
        QVERIFY(index.findPath(L"X:\\locked\\c.txt") != kNoEntry);
    }

    void shareProblems()
    {
        using P = share::Problem;
        QCOMPARE(share::classify(ERROR_BAD_NETPATH, true), P::Offline);
        QCOMPARE(share::classify(ERROR_NETNAME_DELETED, false), P::Dropped);
        QCOMPARE(share::classify(ERROR_BAD_NET_NAME, true), P::NotFound);
        QCOMPARE(share::classify(ERROR_ACCESS_DENIED, true), P::NeedsSignIn);
        QCOMPARE(share::classify(ERROR_ACCESS_DENIED, false), P::Denied); // one folder kept from us
        QCOMPARE(share::classify(ERROR_LOGON_FAILURE, false), P::NeedsSignIn);
        QCOMPARE(share::classify(1272, true), P::GuestBlocked); // ERROR_SMB_GUEST_LOGON_BLOCKED
        QVERIFY(share::affectsShare(P::Offline) && share::affectsShare(P::NeedsSignIn));
        QVERIFY(!share::affectsShare(P::Denied));
    }

    // The content search never opens a file on a network share.
    void remoteNotInContent()
    {
        FileIndex index;
        const EntryId share = ShareIndexer::ensureRoot(index, L"\\\\nas\\docs");
        const EntryId remote = index.add(share, "notes.txt", EntryFlag::Remote);
        const EntryId local = index.add(index.addRoot("C:"), "notes.txt", 0);
        ContentFilter filter;
        filter.extensions = ExtensionFilter({u"txt"_s});
        QVERIFY(filter.accepts(index, index.entry(local)));
        QVERIFY(!filter.accepts(index, index.entry(remote)));
    }

    // Shares stay in the snapshot, as roots with their items and when they
    // were last read in full.
    void snapshotKeepsShares()
    {
        FileIndex index;
        const EntryId c = index.addRoot("C:");
        index.add(c, "local.txt", 0);
        const EntryId share = ShareIndexer::ensureRoot(index, L"\\\\192.168.1.10\\资料");
        index.setModified(share, 1'790'000'000);
        const EntryId folder = index.add(share, "2026", EntryFlag::Directory | EntryFlag::Remote);
        index.add(folder, "预算.xlsx", EntryFlag::Remote);
        QTemporaryDir dir;
        const QString file = dir.filePath(u"index.bin"_s);
        QVERIFY(snapshot::save(index, {{L"C:", 1, true}}, {{1, 1}}, CrawlRules {}, file));
        const auto contents = snapshot::load(file);
        QVERIFY(contents && contents->index);
        const FileIndex& loaded = *contents->index;
        const EntryId budget = loaded.findPath(L"\\\\192.168.1.10\\资料\\2026\\预算.xlsx");
        QVERIFY(budget != kNoEntry);
        QVERIFY(loaded.entry(budget).flags & EntryFlag::Remote);
        QCOMPARE(loaded.modified(loaded.findPath(L"\\\\192.168.1.10\\资料")), FileTime {1'790'000'000});
        QCOMPARE(loaded.liveCount(), std::size_t {5});
    }

    // The whole of it over a real share: this computer's own Users share,
    // where there is one. Read, then verified, a few files gone (believed), then
    // most of them (not believed, until accepted).
    void shareIndexerLoopback()
    {
        using namespace std::chrono_literals;
        QTemporaryDir tmp; // under %TEMP%: C:\Users\<me>\AppData\Local\Temp
        QVERIFY(tmp.isValid());
        const QString local = QDir::toNativeSeparators(tmp.path());
        const QString users = QDir::toNativeSeparators(QDir(qEnvironmentVariable("SystemDrive") + u"/Users"_s).path());
        if (!local.startsWith(users + u'\\', Qt::CaseInsensitive))
            QSKIP("the temporary folder is not under C:\\Users");
        const std::wstring path = L"\\\\localhost\\Users" + local.mid(users.size()).toStdWString();
        if (!QFileInfo(QString::fromStdWString(path)).isDir())
            QSKIP("no Users share on this computer");
        // Asked as Windows asks (SMB 1 framing offering SMB 2), Windows answers in SMB 2.
        QVERIFY(share::probe(L"localhost", 1500ms, true) == share::Answer::Smb2);
        QDir base(tmp.path());
        QVERIFY(base.mkpath(u"many"_s));
        for (int i = 0; i < 1500; ++i) {
            QFile f(base.filePath(u"many/f%1.txt"_s.arg(i)));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        for (const QString& name : {u"a.txt"_s, u"b.txt"_s, u"._a.txt"_s, u".DS_Store"_s}) {
            QFile f(base.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        auto index = std::make_shared<FileIndex>();
        ShareIndexer::Options o;
        o.shares = {{path, {}}};
        o.excludedFiles = {L"._*", L".DS_Store"};
        o.verifyEvery = 0min;
        o.verifyEveryUnwatched = 0min;
        o.watch = false; // what goes is found by the next pass alone, as on a server that does not tell
        ShareIndexer shares({[&] { return index; }, [] { return true; }}, o, {});
        const auto status = [&] {
            const auto all = shares.statuses();
            return all.empty() ? ShareIndexer::Status {} : all.front();
        };
        const auto settled = [&](ShareIndexer::State state) {
            for (int i = 0; i < 300; ++i) { // 30 s
                const auto s = status();
                if (s.state == state)
                    return true;
                std::this_thread::sleep_for(100ms);
            }
            return false;
        };
        shares.start();
        QVERIFY(settled(ShareIndexer::State::Online));
        QCOMPARE(status().items, std::size_t {1503}); // many, its 1500, a.txt, b.txt: no ._a.txt, .DS_Store
        {
            const auto lock = index->readLock();
            const EntryId a = index->findPath(path + L"\\a.txt");
            QVERIFY(a != kNoEntry);
            QVERIFY(index->entry(a).flags & EntryFlag::Remote);
            QCOMPARE(index->findPath(path + L"\\._a.txt"), kNoEntry);
        }

        QVERIFY(QFile::remove(base.filePath(u"b.txt"_s)));
        for (int i = 0; i < 1400; ++i)
            QVERIFY(QFile::remove(base.filePath(u"many/f%1.txt"_s.arg(i))));
        shares.verifyNow(path);
        QVERIFY(settled(ShareIndexer::State::Suspect));
        QCOMPARE(status().missing, std::size_t {1401});
        QCOMPARE(index->liveCount(), std::size_t {1504}); // nothing removed yet
        shares.acceptMissing(path);
        QVERIFY(settled(ShareIndexer::State::Online));
        QCOMPARE(status().items, std::size_t {102});
        QCOMPARE(index->liveCount(), std::size_t {103});
        shares.stop();
    }

    // GitHub #116: a server gone away is probed again after 1, 2, 4, then
    // every 5 minutes, and Windows itself is asked every quarter of an hour
    // (the probe can be wrong). Three days of it, at once.
    void shareRetry()
    {
        using namespace std::chrono_literals;
        using ms = std::chrono::milliseconds;
        share::Retry retry;
        QCOMPARE(retry.away(), ms(1min));
        QCOMPARE(retry.away(), ms(2min));
        QCOMPARE(retry.away(), ms(4min));
        QCOMPARE(retry.away(), ms(5min));
        QCOMPARE(retry.away(), ms(5min));
        retry.answered(); // the probe answers again, reading fails all the same: soon again
        QCOMPARE(retry.away(), ms(1min));

        share::Retry r;
        const auto start = share::Retry::Clock::time_point {} + 1000h;
        auto t = start;
        QVERIFY(r.windowsDue(t, false)); // not since it was read: before calling it offline
        r.askedWindows(t);
        QVERIFY(!r.windowsDue(t + 1min, false));
        QVERIFY(r.windowsDue(t + 1min, true)); // 立即核对: at once, whenever
        auto lastWindows = t;
        ms longestProbe {0};
        ms longestWindows {0};
        int windows = 1;
        while (t - start < 72h) {
            const ms wait = r.away();
            longestProbe = std::max(longestProbe, wait);
            t += wait;
            if (r.windowsDue(t, false)) {
                longestWindows = std::max(longestWindows, std::chrono::duration_cast<ms>(t - lastWindows));
                r.askedWindows(t);
                lastWindows = t;
                ++windows;
            }
        }
        // A server back at any time is seen within 5 minutes by the probe,
        // within 20 by Windows where the probe cannot see it.
        QCOMPARE(longestProbe, ms(5min));
        QVERIFY(longestWindows >= ms(15min) && longestWindows <= ms(20min));
        QVERIFY(windows >= 72 * 3 && windows <= 72 * 4 + 1);
        r.read();
        QVERIFY(r.windowsDue(t, false));
        QCOMPARE(r.away(), ms(1min));

        // A wait cut short (woken: the network changed, the user asked) does
        // not make the next one longer.
        share::Retry woken;
        QCOMPARE(woken.away(true), ms(1min)); // nothing waited yet
        QCOMPARE(woken.away(), ms(2min));
        QCOMPARE(woken.away(true), ms(2min));
        QCOMPARE(woken.away(true), ms(2min));
        QCOMPARE(woken.away(), ms(4min));
        woken.answered();
        QCOMPARE(woken.away(true), ms(1min));
        QCOMPARE(share::RetryTimes {}.shown, ms(1s)); // 立即核对 seen to check, however quick the answer

        share::Retry quick({.first = 50ms, .last = 200ms, .windows = 1s, .answer = 300ms, .problem = 2s});
        QCOMPARE(quick.away(), ms(50));
        QCOMPARE(quick.away(), ms(100));
        QCOMPARE(quick.away(), ms(200));
        QCOMPARE(quick.away(), ms(200));
    }

    // GitHub #116, the share thread with the network faked (the probe, and
    // Windows opening the share); what it reads is this computer's own Users
    // share. Away from the start: offline, tried again by itself. 立即核对
    // while away: shown at once, answered, Windows asked once for two quick
    // clicks, a quick answer still shown as checking a moment; why, where
    // only a server tells. Windows getting there late, or where the probe says no: read,
    // and it stays online. Away and back: online again by itself. Stopping
    // while Windows still tries returns at once, the try let go of.
    void shareIndexerRetries()
    {
        using namespace std::chrono_literals;
        using State = ShareIndexer::State;
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString local = QDir::toNativeSeparators(tmp.path());
        const QString users = QDir::toNativeSeparators(QDir(qEnvironmentVariable("SystemDrive") + u"/Users"_s).path());
        if (!local.startsWith(users + u'\\', Qt::CaseInsensitive))
            QSKIP("the temporary folder is not under C:\\Users");
        const std::wstring path = L"\\\\localhost\\Users" + local.mid(users.size()).toStdWString();
        if (!QFileInfo(QString::fromStdWString(path)).isDir())
            QSKIP("no Users share on this computer");
        for (const QString& name : {u"a.txt"_s, u"b.txt"_s, u"c.txt"_s}) {
            QFile f(QDir(tmp.path()).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        auto index = std::make_shared<FileIndex>();
        // The network faked; a try of Windows' that stop() lets go of
        // holds it still.
        struct Network {
            std::atomic<bool> probe {false};
            std::atomic<int> opens {0};
            std::atomic<int> opened {0}; // tries that have answered
            std::atomic<unsigned long> openError {ERROR_BAD_NETPATH};
            std::atomic<int> openMs {0};
        };
        const auto network = std::make_shared<Network>();
        std::atomic<bool>& probe = network->probe;
        std::atomic<int>& opens = network->opens;
        std::atomic<unsigned long>& openError = network->openError;
        std::atomic<int>& openMs = network->openMs;
        ShareIndexer::Source source {[&] { return index; }, [] { return true; },
            [network](const std::wstring&, bool) { return network->probe.load(); },
            [network](const std::wstring&) {
                ++network->opens;
                std::this_thread::sleep_for(std::chrono::milliseconds(network->openMs.load()));
                ++network->opened;
                return network->openError.load();
            }};
        ShareIndexer::Options o;
        o.shares = {{path, {}}};
        o.verifyEvery = 0min;
        o.verifyEveryUnwatched = 0min;
        o.watch = false;
        o.retry = {.first = 100ms, .last = 400ms, .windows = 1s, .answer = 300ms, .problem = 2s, .shown = 200ms};
        std::atomic<int> problems {0}; // times it was found in Problem by `changed`
        ShareIndexer* self = nullptr;
        ShareIndexer::Callbacks callbacks;
        callbacks.changed = [&] {
            if (self && !self->statuses().empty() && self->statuses().front().state == State::Problem)
                ++problems;
        };
        ShareIndexer shares(source, o, callbacks);
        self = &shares;
        const auto status = [&] {
            const auto all = shares.statuses();
            return all.empty() ? ShareIndexer::Status {} : all.front();
        };
        const auto until = [&](const std::function<bool(const ShareIndexer::Status&)>& done, std::chrono::milliseconds limit) {
            const auto end = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < end) {
                if (done(status()))
                    return true;
                std::this_thread::sleep_for(10ms);
            }
            return done(status());
        };
        const auto offline = [](const ShareIndexer::Status& s) { return s.state == State::Problem && !s.checking; };

        // Away from the start: Windows asked before it is called offline, and
        // again every second (`windows`) while the probe goes on saying no.
        shares.start();
        QVERIFY(until(offline, 3s));
        QCOMPARE(status().problem, share::Problem::Offline);
        QVERIFY(status().lastAttempt != 0);
        QCOMPARE(opens.load(), 1);
        QVERIFY(shares.offline(path + L"\\a.txt"));
        QVERIFY(until([&](const auto&) { return opens.load() >= 2; }, 3s));

        // 立即核对 while away, clicked twice: shown at once, Windows asked once.
        openMs = 150;
        const int before = opens.load();
        shares.verifyNow(path);
        QVERIFY(status().checking);
        shares.verifyNow(path);
        QVERIFY(until(offline, 2s));
        QCOMPARE(opens.load(), before + 1);
        QCOMPARE(status().problem, share::Problem::Offline);

        // Answered at once (Windows says no straight away): still shown as
        // checking for `shown`, so that the click is seen to do something.
        openMs = 0;
        const auto clicked = std::chrono::steady_clock::now();
        shares.verifyNow(path);
        QVERIFY(until(offline, 2s));
        const auto checkingFor = std::chrono::steady_clock::now() - clicked;
        QVERIFY(checkingFor >= 200ms);
        QVERIFY(checkingFor < 1s);

        // What Windows says counts where only a server that answered says it
        // (signing in); a code of its own for a NAS starting up leaves the
        // share offline, tried again in minutes rather than half an hour.
        openError = ERROR_NOT_READY; // share::Problem::Other
        shares.verifyNow(path);
        QVERIFY(until(offline, 2s));
        QCOMPARE(status().problem, share::Problem::Offline);
        QCOMPARE(status().error, static_cast<unsigned long>(ERROR_NOT_READY));
        openError = ERROR_LOGON_FAILURE;
        shares.verifyNow(path);
        QVERIFY(until(offline, 2s));
        QCOMPARE(status().problem, share::Problem::NeedsSignIn);
        openError = ERROR_BAD_NETPATH;

        // Windows gets there, after the share thread stopped waiting for it
        // (`answer`): that wakes the share, and it is read.
        openMs = 1200;
        openError = 0;
        const auto asked = std::chrono::steady_clock::now();
        shares.verifyNow(path);
        QVERIFY(until([](const ShareIndexer::Status& s) { return !s.checking; }, 2s));
        QVERIFY(std::chrono::steady_clock::now() - asked < 900ms); // answered after `answer`, not Windows' time
        QVERIFY(until([](const ShareIndexer::Status& s) { return s.state == State::Online; }, 5s));
        QCOMPARE(status().items, std::size_t {3});
        QVERIFY(!shares.offline(path + L"\\a.txt"));

        // The probe still says no, Windows gets there: it stays online.
        openMs = 0;
        problems = 0;
        for (int i = 0; i < 2; ++i) {
            shares.verifyNow(path);
            QVERIFY(until([](const ShareIndexer::Status& s) { return s.state == State::Online && !s.checking; }, 5s));
        }
        QCOMPARE(problems.load(), 0);

        // Away for real, then back: online again by itself, within `last`
        // (and the time a pass takes).
        openError = ERROR_BAD_NETPATH;
        shares.verifyNow(path);
        QVERIFY(until(offline, 2s));
        QVERIFY(shares.offline(path));
        probe = true;
        const auto back = std::chrono::steady_clock::now();
        QVERIFY(until([](const ShareIndexer::Status& s) { return s.state == State::Online; }, 5s));
        qInfo("online again %lld ms after the server came back",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - back).count()));

        // Away again, Windows slow to give up: stopping does not wait for it
        // (quitting or restarting Win顺 would hang a minute on a server gone
        // away); the try let go of ends by itself and leaves the share be.
        probe = false;
        openMs = 1500;
        const int tried = opens.load();
        shares.verifyNow(path);
        QVERIFY(until(offline, 2s));
        QVERIFY(opens.load() > tried);
        QVERIFY(network->opened.load() < opens.load()); // Windows still trying
        const auto stopping = std::chrono::steady_clock::now();
        shares.stop();
        const auto stopTook = std::chrono::steady_clock::now() - stopping;
        QVERIFY2(stopTook < 500ms,
            qPrintable(QString::number(std::chrono::duration_cast<std::chrono::milliseconds>(stopTook).count())));
        self = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(network->opened.load() == network->opens.load(), 5000);
    }

    // GitHub #116: the probe said no where Windows got there.
    void shareProbeRace()
    {
        using namespace std::chrono_literals;
        using A = share::Answer;
        using Then = FakeSmb::Then;
        const auto timed = [](const std::function<A()>& ask) {
            const auto from = std::chrono::steady_clock::now();
            const A answer = ask();
            return std::pair {answer,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from)};
        };
        const auto ms = [](std::chrono::milliseconds d) { return QByteArray::number(static_cast<qint64>(d.count())) + " ms"; };
        {
            FakeSmb server(Then::Smb2, Then::Smb2);
            QVERIFY(server.port() != 0);
            const auto [answer, took] = timed([&] { return share::probeAt({L"127.0.0.1"}, server.port(), 1500ms); });
            QCOMPARE(answer, A::Smb2);
            QVERIFY2(took < 500ms, ms(took).constData());
        }
        {
            // A first address that never answers (one the NAS no longer has
            // after a restart, an IPv6 one it does not listen on; behind a
            // TUN proxy, any): before, asked one after the other, it took all
            // the time and the next was never asked. Here one that takes the
            // connection and says nothing, the server on a second address.
            std::unique_ptr<FakeSmb> quiet;
            std::unique_ptr<FakeSmb> server;
            for (int i = 0; i < 5 && !(server && server->port() != 0); ++i) {
                server.reset();
                quiet = std::make_unique<FakeSmb>(Then::Quiet, Then::Quiet);
                server = std::make_unique<FakeSmb>(Then::Smb2, Then::Smb2, 0x7F000002, quiet->port()); // 127.0.0.2
            }
            QVERIFY(quiet->port() != 0 && server->port() == quiet->port());
            const auto [answer, took] = timed([&] { return share::probeAt({L"127.0.0.1", L"127.0.0.2"}, server->port(), 3000ms); });
            QCOMPARE(answer, A::Smb2);
            QVERIFY2(took < 300ms, ms(took).constData()); // not after the quiet one's silence (400 ms)
        }
        {
            // Quiet to SMB 1 framing, answering SMB 2: asked in SMB 2 after a
            // moment's silence, not only once hung up on.
            FakeSmb server(Then::Quiet, Then::Smb2);
            const auto [answer, took] = timed([&] { return share::probeAt({L"127.0.0.1"}, server.port(), 3000ms); });
            QCOMPARE(answer, A::Smb2);
            QVERIFY2(took >= 400ms && took < 1500ms, ms(took).constData());
            QCOMPARE(server.asked(true), 1);
        }
        {
            FakeSmb server(Then::HangUp, Then::Smb2); // hangs up on SMB 1 framing: SMB 2 at once
            const auto [answer, took] = timed([&] { return share::probeAt({L"127.0.0.1"}, server.port(), 3000ms); });
            QCOMPARE(answer, A::Smb2);
            QVERIFY2(took < 400ms, ms(took).constData());
        }
        {
            FakeSmb server(Then::Smb1, Then::HangUp); // an old one
            QCOMPARE(share::probeAt({L"127.0.0.1"}, server.port(), 1500ms), A::Smb1Only);
        }
        {
            // Takes the connection and says nothing, in either framing (a
            // proxy in TUN mode with no server behind it): not a server.
            FakeSmb server(Then::Quiet, Then::Quiet);
            const auto [answer, took] = timed([&] { return share::probeAt({L"127.0.0.1"}, server.port(), 600ms); });
            QCOMPARE(answer, A::None);
            QVERIFY2(took >= 550ms && took < 1200ms, ms(took).constData());
            QCOMPARE(server.asked(false), 1);
            QCOMPARE(server.asked(true), 1);
        }
        std::uint16_t closed = 0;
        {
            const FakeSmb gone(Then::Smb2, Then::Smb2);
            closed = gone.port();
        }
        QCOMPARE(share::probeAt({L"127.0.0.1"}, closed, 1500ms), A::None); // nothing listening
    }

    // GitHub #116: what an action on a file on a server finds, the probe and
    // Windows asked at once, Windows having the last word.
    void shareReach()
    {
        using namespace std::chrono_literals;
        using R = share::Reach;
        using Clock = std::chrono::steady_clock;
        const auto shown = [](Clock::duration d) {
            return QByteArray::number(static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()))
                + " ms";
        };
        // Steps answering after `ms`; they own what they hold, as their
        // threads can outlive reach().
        const auto probe = [](bool yes, int ms = 0) {
            return [yes, ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return yes;
            };
        };
        const auto windows = [](unsigned long error, int ms = 0, std::shared_ptr<std::atomic<bool>> done = {}) {
            return [error, ms, done] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                if (done)
                    *done = true;
                return error;
            };
        };
        const auto steps = [](std::function<bool()> p, std::function<unsigned long()> w) {
            share::ReachSteps s;
            s.probe = std::move(p);
            s.windows = std::move(w);
            return s;
        };
        // Answers that come at once are taken at once: none of these waits
        // for the limit.
        auto from = Clock::now();
        // The probe says no, Windows gets there: it opens.
        QCOMPARE(share::reach(steps(probe(false), windows(0, 50)), 2s), R::There);
        // Neither: 连不上.
        QCOMPARE(share::reach(steps(probe(false), windows(ERROR_BAD_NETPATH, 20)), 2s), R::Unreachable);
        // Windows has the last word, the probe's yes notwithstanding.
        QCOMPARE(share::reach(steps(probe(true), windows(ERROR_FILE_NOT_FOUND)), 2s), R::Missing);
        QCOMPARE(share::reach(steps(probe(true), windows(ERROR_BAD_NETPATH, 20)), 2s), R::Unreachable);
        // Not there: the file, its folder.
        QCOMPARE(share::reach(steps(probe(true), windows(ERROR_PATH_NOT_FOUND)), 2s), R::Missing);
        // The share not there now (a NAS starting up, a folder still locked):
        // not the file gone, nothing is forgotten for it.
        QCOMPARE(share::reach(steps(probe(false), windows(ERROR_BAD_NET_NAME)), 2s), R::ShareAway);
        // The server answered, not plainly (kept from this user, signing in):
        // the action goes on and its own error says why; never "not there".
        QCOMPARE(share::reach(steps(probe(true), windows(ERROR_ACCESS_DENIED)), 2s), R::There);
        QCOMPARE(share::reach(steps(probe(false), windows(ERROR_LOGON_FAILURE)), 2s), R::There);
        // The service Windows needs for it is off (WebClient, for WebDAV).
        QCOMPARE(share::reach(steps(probe(true), windows(ERROR_SERVICE_DISABLED)), 2s), R::NoService);
        // No Windows step: the probe decides.
        QCOMPARE(share::reach(steps(probe(true), {}), 2s), R::There);
        QCOMPARE(share::reach(steps(probe(false), {}), 2s), R::Unreachable);
        auto took = Clock::now() - from;
        QVERIFY2(took < 1s, shown(took).constData());
        // What Windows' answers mean, by themselves.
        QCOMPARE(share::reachOf(0), R::There);
        QCOMPARE(share::reachOf(ERROR_FILE_NOT_FOUND), R::Missing);
        QCOMPARE(share::reachOf(ERROR_PATH_NOT_FOUND), R::Missing);
        QCOMPARE(share::reachOf(ERROR_BAD_NET_NAME), R::ShareAway);
        QCOMPARE(share::reachOf(ERROR_SERVICE_DISABLED), R::NoService);
        QCOMPARE(share::reachOf(ERROR_ACCESS_DENIED), R::There);
        QCOMPARE(share::reachOf(ERROR_BAD_NETPATH), R::Unreachable);
        QCOMPARE(share::reachOf(ERROR_SEM_TIMEOUT), R::Unreachable);
        QCOMPARE(share::reachOf(ERROR_NETNAME_DELETED), R::Unreachable);
        QCOMPARE(share::reachOf(ERROR_TIMEOUT), R::Unreachable);
        QCOMPARE(share::reachOf(ERROR_NO_NET_OR_BAD_PATH), R::Unreachable);

        // Windows quick, the probe slow: Windows' word at once.
        from = Clock::now();
        QCOMPARE(share::reach(steps(probe(false, 1500), windows(0, 20)), 300ms), R::There);
        took = Clock::now() - from;
        QVERIFY2(took < 200ms, shown(took).constData());

        // Windows slow to give up, the probe saying no: the limit holds, and
        // what Windows says late is not waited for (its thread ends by itself).
        auto late = std::make_shared<std::atomic<bool>>(false);
        from = Clock::now();
        QCOMPARE(share::reach(steps(probe(false), windows(0, 1500, late)), 300ms), R::Unreachable);
        took = Clock::now() - from;
        QVERIFY2(took >= 290ms && took < 500ms, shown(took).constData());
        QVERIFY(!*late);
        QTRY_VERIFY_WITH_TIMEOUT(late->load(), 5000);
        // The probe not back by the limit either (a server gone away, the
        // probe's own limit longer): Unreachable at the limit, not twice it.
        late = std::make_shared<std::atomic<bool>>(false);
        from = Clock::now();
        QCOMPARE(share::reach(steps(probe(true, 1000), windows(0, 1500, late)), 300ms), R::Unreachable);
        took = Clock::now() - from;
        QVERIFY2(took >= 290ms && took < 500ms, shown(took).constData());
        QTRY_VERIFY_WITH_TIMEOUT(late->load(), 5000);
        // The probe said yes in time, Windows nothing yet: the server is
        // there, Windows on its way. Waited for twice the limit, then a
        // verdict all the same...
        late = std::make_shared<std::atomic<bool>>(false);
        from = Clock::now();
        QCOMPARE(share::reach(steps(probe(true, 100), windows(0, 1500, late)), 300ms), R::Unreachable);
        took = Clock::now() - from;
        QVERIFY2(took >= 590ms && took < 800ms, shown(took).constData());
        QTRY_VERIFY_WITH_TIMEOUT(late->load(), 5000);
        // ... and what Windows says in that time is taken.
        from = Clock::now();
        QCOMPARE(share::reach(steps(probe(true), windows(ERROR_FILE_NOT_FOUND, 450)), 300ms), R::Missing);
        took = Clock::now() - from;
        QVERIFY2(took >= 440ms && took < 650ms, shown(took).constData());
    }

    // GitHub #116 review: an action on a WebDAV file asks Windows too (its
    // WebClient), as on SMB. The HTTP probe lets through a server that
    // refuses this user (401, https not signed in to), and that is not the
    // file gone. With WebClient disabled Windows is not asked: it opens no
    // WebDAV folder then. GitHub #114: stopped, WebClient is started first
    // (until then Windows says of any WebDAV folder that it is not there),
    // and no file is forgotten for what Windows says without it.
    void shareActionSteps()
    {
        using namespace std::chrono_literals;
        using R = share::Reach;
        using Clock = std::chrono::steady_clock;
        const auto shown = [](Clock::duration d) {
            return QByteArray::number(static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()))
                + " ms";
        };
        const std::wstring dav = L"\\\\127.0.0.1@8714\\DavWWWRoot\\docs\\a.txt";
        const std::wstring smb = L"\\\\nas\\share\\a.txt";
        struct Asked {
            std::atomic<int> windows {0};
            std::atomic<int> starts {0};
            std::mutex mutex;
            std::wstring path; // what Windows was last asked about
            QByteArray order; // 's' for each start of WebClient, 'w' for each time Windows was asked
        };
        const auto asked = std::make_shared<Asked>(); // held by the steps, whose threads can outlive reach()
        // WebClient running, or stopped and started (or not) in `startMs`;
        // `runningAfter`: still running once Windows answered.
        struct Client {
            bool running = true;
            bool starts = true;
            int startMs = 0;
            bool runningAfter = true;
        };
        const auto with = [asked](bool probeYes, unsigned long error, bool clientDisabled, int windowsMs = 0,
                              Client client = {}) {
            share::ReachProbes p;
            p.probe = [probeYes](const std::wstring&, std::chrono::milliseconds) { return probeYes; };
            p.windows = [asked, error, windowsMs](const std::wstring& path) {
                ++asked->windows;
                {
                    std::lock_guard lock(asked->mutex);
                    asked->path = path;
                    asked->order += 'w';
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(windowsMs));
                return error;
            };
            p.webClientDisabled = [clientDisabled] { return clientDisabled; };
            p.webClientRunning = [client] { return client.runningAfter; };
            p.startWebClient = [asked, client](std::chrono::milliseconds) {
                if (client.running)
                    return true; // as webdav::startClient: nothing to start
                ++asked->starts;
                {
                    std::lock_guard lock(asked->mutex);
                    asked->order += 's';
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(client.startMs));
                return client.starts;
            };
            return p;
        };
        const auto order = [asked] { // and anew
            std::lock_guard lock(asked->mutex);
            return std::exchange(asked->order, {});
        };
        const auto reach = [](const std::wstring& path, std::chrono::milliseconds wait, share::ReachProbes p) {
            return share::reach(share::actionSteps(path, wait, std::move(p)), wait);
        };
        // The HTTP probe let it through; Windows says: refused (signing in)
        // goes on, its own error saying why; not there; not got to (a
        // certificate refused). And where the probe said no, it got there.
        QCOMPARE(reach(dav, 2s, with(true, ERROR_ACCESS_DENIED, false)), R::There);
        QCOMPARE(reach(dav, 2s, with(true, ERROR_LOGON_FAILURE, false)), R::There);
        QCOMPARE(reach(dav, 2s, with(true, ERROR_FILE_NOT_FOUND, false)), R::Missing);
        QCOMPARE(reach(dav, 2s, with(true, ERROR_BAD_NETPATH, false)), R::Unreachable);
        QCOMPARE(reach(dav, 2s, with(false, 0, false)), R::There);
        QCOMPARE(asked->windows.load(), 5);
        {
            std::lock_guard lock(asked->mutex);
            QVERIFY(asked->path == dav);
        }
        // WebClient disabled: Windows not asked, NoService at once.
        auto from = Clock::now();
        QCOMPARE(reach(dav, 2s, with(true, 0, true)), R::NoService);
        auto took = Clock::now() - from;
        QVERIFY2(took < 200ms, shown(took).constData());
        QCOMPARE(asked->windows.load(), 5);
        // Nothing to do with SMB.
        QCOMPARE(reach(smb, 2s, with(false, ERROR_FILE_NOT_FOUND, true)), R::Missing);
        QCOMPARE(asked->windows.load(), 6);
        // WebClient slow (its first access can take half a minute): the limit holds.
        from = Clock::now();
        QCOMPARE(reach(dav, 300ms, with(false, 0, false, 1500)), R::Unreachable);
        took = Clock::now() - from;
        QVERIFY2(took >= 290ms && took < 500ms, shown(took).constData());
        QCOMPARE(asked->starts.load(), 0); // running all along: nothing started
        order();

        // WebClient stopped: started, then Windows asked, which has the last word.
        QCOMPARE(reach(dav, 2s, with(true, 0, false, 0, {.running = false})), R::There);
        QCOMPARE(order(), QByteArray("sw"));
        QCOMPARE(reach(dav, 2s, with(true, ERROR_FILE_NOT_FOUND, false, 0, {.running = false})), R::Missing);
        QCOMPARE(order(), QByteArray("sw"));
        // Its start taking a while, within the time Windows is waited for.
        from = Clock::now();
        QCOMPARE(reach(dav, 2s, with(false, 0, false, 0, {.running = false, .startMs = 400})), R::There);
        took = Clock::now() - from;
        QVERIFY2(took >= 390ms && took < 1500ms, shown(took).constData());
        QCOMPARE(order(), QByteArray("sw"));
        // Not started (nothing starts it, or not in time): Windows not
        // asked, the file not forgotten.
        QCOMPARE(reach(dav, 2s, with(true, ERROR_PATH_NOT_FOUND, false, 0, {.running = false, .starts = false})),
            R::NoService);
        QCOMPARE(order(), QByteArray("s"));
        // Stopped meanwhile: what Windows said is of that, not of the file;
        // while it runs, a folder not there is just that.
        QCOMPARE(reach(dav, 2s, with(true, ERROR_PATH_NOT_FOUND, false, 0, {.runningAfter = false})), R::NoService);
        QCOMPARE(reach(dav, 2s, with(true, ERROR_BAD_NETPATH, false, 0, {.runningAfter = false})), R::NoService);
        QCOMPARE(reach(dav, 2s, with(true, ERROR_PATH_NOT_FOUND, false)), R::Missing);
        QCOMPARE(order(), QByteArray("www"));
        // Disabled: not started. SMB: no WebClient.
        QCOMPARE(reach(dav, 2s, with(true, 0, true, 0, {.running = false})), R::NoService);
        QCOMPARE(reach(smb, 2s, with(true, ERROR_PATH_NOT_FOUND, false, 0, {.running = false, .runningAfter = false})),
            R::Missing);
        QCOMPARE(order(), QByteArray("w"));
        QCOMPARE(asked->starts.load(), 4);
        QCOMPARE(share::reachOf(ERROR_SERVICE_NOT_ACTIVE), R::NoService);
    }

    // GitHub #116 review: the files of one action, each server asked at
    // once, under one time limit for all (servers gone away one after the
    // other added up); a server's first file with the probe and Windows, its
    // others then with Windows alone, none once it did not answer.
    void shareCheckFiles()
    {
        using namespace std::chrono_literals;
        using R = share::Reach;
        using Clock = std::chrono::steady_clock;
        const auto shown = [](Clock::duration d) {
            return QByteArray::number(static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()))
                + " ms";
        };
        // Files "server\\name": servers answering after 300 ms ("a", "b",
        // "c"), one past any limit here ("slow"), one gone away ("dead", said
        // after 100 ms); "gone" files are not there.
        struct Log {
            std::mutex mutex;
            std::vector<std::wstring> asked; // "first a\\1", "next a\\2"
        };
        const auto log = std::make_shared<Log>(); // held by the steps, whose threads can outlive checkFiles()
        const auto answer = [log](const std::wstring& file, bool first) {
            {
                std::lock_guard lock(log->mutex);
                log->asked.push_back((first ? L"first " : L"next ") + file);
            }
            const std::wstring server = file.substr(0, file.find(L'\\'));
            if (server == L"dead") {
                std::this_thread::sleep_for(100ms);
                return R::Unreachable;
            }
            std::this_thread::sleep_for(server == L"slow" ? 2000ms : 300ms);
            return file.find(L"gone") != std::wstring::npos ? R::Missing : R::There;
        };
        share::FileSteps steps;
        steps.server = [](const std::wstring& file) { return file.substr(0, file.find(L'\\')); };
        steps.first = [answer](const std::wstring& file) { return answer(file, true); };
        steps.next = [answer](const std::wstring& file) { return answer(file, false); };
        const auto taken = [log] {
            std::lock_guard lock(log->mutex);
            return std::exchange(log->asked, {});
        };
        const auto has = [](const std::vector<std::wstring>& asked, const std::wstring& what) {
            return std::find(asked.begin(), asked.end(), what) != asked.end();
        };

        // Three servers at once: the time of one (one after the other, 900 ms).
        auto from = Clock::now();
        auto found = share::checkFiles({L"a\\1", L"b\\1", L"c\\1"}, 2s, steps);
        auto took = Clock::now() - from;
        QCOMPARE(found.size(), std::size_t {3});
        QVERIFY(found[L"a\\1"] == R::There && found[L"b\\1"] == R::There && found[L"c\\1"] == R::There);
        QVERIFY2(took >= 290ms && took < 550ms, shown(took).constData());
        taken();

        // A server's files one after the other, the first with the probe; a
        // server past the limit is Unreachable at the limit, the others kept.
        from = Clock::now();
        found = share::checkFiles({L"a\\1", L"slow\\1", L"a\\gone"}, 800ms, steps);
        took = Clock::now() - from;
        QCOMPARE(found.size(), std::size_t {3});
        QVERIFY(found[L"a\\1"] == R::There);
        QVERIFY(found[L"a\\gone"] == R::Missing);
        QVERIFY(found[L"slow\\1"] == R::Unreachable);
        QVERIFY2(took >= 790ms && took < 1100ms, shown(took).constData());
        auto asked = taken();
        QVERIFY(has(asked, L"first a\\1"));
        QVERIFY(has(asked, L"next a\\gone"));
        QVERIFY(has(asked, L"first slow\\1"));

        // A server gone away: its other files are not asked about, nor waited for.
        from = Clock::now();
        found = share::checkFiles({L"dead\\1", L"dead\\2", L"a\\1", L"dead\\3"}, 2s, steps);
        took = Clock::now() - from;
        QVERIFY(found[L"dead\\1"] == R::Unreachable && found[L"dead\\2"] == R::Unreachable
            && found[L"dead\\3"] == R::Unreachable);
        QVERIFY(found[L"a\\1"] == R::There);
        QVERIFY2(took >= 290ms && took < 550ms, shown(took).constData());
        asked = taken();
        QVERIFY(has(asked, L"first dead\\1"));
        QVERIFY(std::none_of(asked.begin(), asked.end(), [](const std::wstring& s) { return s.starts_with(L"next dead"); }));
    }

    // GitHub #116 review: Windows asked one at a time for a server (each ask
    // can take a minute on one gone away). A failure that began before a
    // caller asked says nothing of now: it asks afresh, once for all who
    // waited with it. A server's own answer (the file not there) is about
    // that file alone. Other servers are not held up.
    void shareOneAtATime()
    {
        using namespace std::chrono_literals;
        using Clock = std::chrono::steady_clock;
        const auto shown = [](Clock::duration d) {
            return QByteArray::number(static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()))
                + " ms";
        };
        struct Count {
            std::atomic<int> asks {0};
            std::atomic<int> now {0}; // under way
            std::atomic<int> most {0}; // under way at once, at most
        };
        const auto ask = [](const std::shared_ptr<Count>& count, unsigned long error, int ms) {
            return [count, error, ms] {
                ++count->asks;
                const int n = ++count->now;
                for (int most = count->most.load(); n > most && !count->most.compare_exchange_weak(most, n);) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                --count->now;
                return error;
            };
        };
        const auto soon = [](const std::function<bool()>& done) {
            for (const auto end = Clock::now() + 2s; !done() && Clock::now() < end;)
                std::this_thread::sleep_for(2ms);
            return done();
        };
        constexpr auto kNoPath = static_cast<unsigned long>(ERROR_BAD_NETPATH);
        share::OneAtATime asking;

        // One under way (400 ms), three more asking meanwhile: not its
        // failure (begun before they asked), one ask of theirs, shared.
        const auto nas = std::make_shared<Count>();
        std::atomic<unsigned long> firstSaid {0};
        std::jthread first([&] { firstSaid = asking.ask(L"nas", ask(nas, kNoPath, 400)); });
        QVERIFY(soon([&] { return nas->asks.load() == 1; }));
        const auto from = Clock::now();
        std::atomic<unsigned long> said[3] {};
        std::vector<std::jthread> more; // joined, should a check fail on the way
        for (int i = 0; i < 3; ++i)
            more.emplace_back([&, i] { said[i] = asking.ask(L"NAS", ask(nas, kNoPath, 300)); });
        // Meanwhile another server is not held up.
        const auto other = std::make_shared<Count>();
        const auto otherFrom = Clock::now();
        QCOMPARE(asking.ask(L"other", ask(other, 0, 50)), 0UL);
        const auto otherTook = Clock::now() - otherFrom;
        QVERIFY2(otherTook < 250ms, shown(otherTook).constData());
        first.join();
        for (std::jthread& t : more)
            t.join();
        const auto took = Clock::now() - from;
        QCOMPARE(firstSaid.load(), kNoPath);
        for (const std::atomic<unsigned long>& s : said)
            QCOMPARE(s.load(), kNoPath);
        QCOMPARE(nas->asks.load(), 2); // the first, and one afresh for the three
        QCOMPARE(nas->most.load(), 1); // never two at once
        QVERIFY2(took >= 550ms, shown(took).constData()); // the first's failure taken: about 400 ms
        // Asked again after: afresh.
        QCOMPARE(asking.ask(L"nas", ask(nas, 0, 0)), 0UL);
        QCOMPARE(nas->asks.load(), 3);

        // A server's own answer is about its file: one waiting meanwhile asks about its own.
        const auto files = std::make_shared<Count>();
        std::atomic<unsigned long> gone {0};
        std::jthread missing([&] { gone = asking.ask(L"nas", ask(files, ERROR_FILE_NOT_FOUND, 300)); });
        QVERIFY(soon([&] { return files->asks.load() == 1; }));
        QCOMPARE(asking.ask(L"nas", ask(files, 0, 20)), 0UL);
        missing.join();
        QCOMPARE(gone.load(), static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
        QCOMPARE(files->asks.load(), 2);
        QCOMPARE(files->most.load(), 1);
    }

    void shareServerAnswered()
    {
        // The server talking: there, not there, signing in, kept from us, its IPC$.
        const unsigned long answered[] {0, ERROR_FILE_NOT_FOUND, ERROR_BAD_NET_NAME, ERROR_ACCESS_DENIED,
            ERROR_LOGON_FAILURE, ERROR_NOT_SUPPORTED};
        for (const unsigned long e : answered)
            QVERIFY2(share::serverAnswered(e), qPrintable(QString::number(e)));
        // The network failing on the way, or Windows giving up by its own clock.
        const unsigned long silent[] {ERROR_BAD_NETPATH, ERROR_NETNAME_DELETED, ERROR_SEM_TIMEOUT, ERROR_TIMEOUT,
            ERROR_NO_NET_OR_BAD_PATH, ERROR_HOST_UNREACHABLE};
        for (const unsigned long e : silent)
            QVERIFY2(!share::serverAnswered(e), qPrintable(QString::number(e)));
    }

    void shareWindowsMemory()
    {
        using namespace std::chrono_literals;
        share::WindowsMemory memory;
        const auto asked = std::make_shared<std::atomic<int>>(0);
        const auto ask = [asked](unsigned long error, int ms = 0) {
            return [asked, error, ms] {
                ++*asked;
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return error;
            };
        };
        // Windows never got there: the probe decides.
        QVERIFY(!memory.quick(L"nas", true, 1s, ask(0)));
        memory.said(L"nas", ERROR_BAD_NETPATH);
        QVERIFY(!memory.quick(L"nas", true, 1s, ask(0)));
        QCOMPARE(asked->load(), 0);
        // It got there (openRoot, askWindows): a yes of the last seconds
        // stands; afresh, Windows is asked.
        memory.said(L"NAS", 0);
        QVERIFY(memory.quick(L"nas", false, 1s, ask(0)) == std::optional(true));
        QCOMPARE(asked->load(), 0);
        QVERIFY(memory.quick(L"nas", true, 1s, ask(0)) == std::optional(true));
        QCOMPARE(asked->load(), 1);
        // Asked afresh, it does not get there: no, and the probe decides again.
        QVERIFY(memory.quick(L"nas", true, 1s, ask(ERROR_BAD_NETPATH)) == std::optional(false));
        QVERIFY(!memory.quick(L"nas", true, 1s, ask(0)));
        // Slow: the wait holds, no answer in time is a no; until Windows gets there after all.
        memory.said(L"nas", 0);
        const auto from = std::chrono::steady_clock::now();
        QVERIFY(memory.quick(L"nas", true, 200ms, ask(0, 800)) == std::optional(false));
        QVERIFY(std::chrono::steady_clock::now() - from < 700ms);
        QVERIFY(!memory.quick(L"nas", false, 200ms, ask(0)));
        QTRY_VERIFY_WITH_TIMEOUT(memory.quick(L"nas", false, 200ms, ask(0)) == std::optional(true), 5000);

        // Quick checks at once (keys pressed again) ask Windows once, its
        // answer for all of them.
        share::WindowsMemory together;
        together.said(L"nas", 0);
        const int before = asked->load();
        std::atomic<int> yes {0};
        {
            std::vector<std::jthread> checks;
            for (int i = 0; i < 4; ++i) {
                checks.emplace_back([&] {
                    if (together.quick(L"nas", true, 2s, ask(0, 500)) == std::optional(true))
                        ++yes;
                });
            }
        }
        QCOMPARE(yes.load(), 4);
        QCOMPARE(asked->load(), before + 1);
    }

    void shareHearsay()
    {
        using namespace std::chrono_literals;
        share::Hearsay heard;
        QVERIFY(!heard.last(L"nas", 1s));
        heard.probeSaid(L"nas", false);
        QVERIFY(heard.last(L"NAS", 1s) == std::optional(false));
        // Windows got there: a no from the probe after it does not undo
        // that while it is recent (the probe can be wrong there).
        heard.windowsSaid(L"nas", true);
        heard.probeSaid(L"nas", false);
        QVERIFY(heard.last(L"nas", 1s) == std::optional(true));
        heard.windowsSaid(L"nas", false); // Windows not getting there does
        QVERIFY(heard.last(L"nas", 1s) == std::optional(false));
        heard.probeSaid(L"nas", true);
        QVERIFY(heard.last(L"nas", 1s) == std::optional(true));
        heard.probeSaid(L"old", false); // lately only
        heard.windowsSaid(L"long", true);
        std::this_thread::sleep_for(80ms);
        QVERIFY(!heard.last(L"old", 50ms));
        QVERIFY(heard.last(L"old", 5s) == std::optional(false));
        // Windows' yes can be taken for longer than the rest (a drag does),
        // the probe's no since notwithstanding; Windows' own no undoes it.
        QVERIFY(!heard.last(L"long", 50ms));
        QVERIFY(heard.last(L"long", 50ms, 5s) == std::optional(true));
        heard.probeSaid(L"long", false);
        QVERIFY(heard.last(L"long", 50ms) == std::optional(false));
        QVERIFY(heard.last(L"long", 50ms, 5s) == std::optional(true));
        heard.windowsSaid(L"long", false);
        QVERIFY(heard.last(L"long", 50ms, 5s) == std::optional(false));

        // By path (share::lastHeard): a local one is there, WSL answers for
        // itself; a server just probed is known, WebDAV on its port apart.
        QVERIFY(share::lastHeard(L"C:\\Windows", 1s) == std::optional(true));
        QVERIFY(!share::lastHeard(L"\\\\wsl.localhost\\Ubuntu\\home", 1s));
        QCOMPARE(share::probe(L"192.0.2.1", 300ms, true), share::Answer::None); // TEST-NET-1: nothing there
        QVERIFY(share::lastHeard(L"\\\\192.0.2.1\\share\\a.txt", 10s) == std::optional(false));
        QVERIFY(!share::lastHeard(L"\\\\192.0.2.1\\DavWWWRoot\\a.txt", 10s));
    }

    void nameSearchTopK()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "data", EntryFlag::Directory);
        for (int i = 0; i < 70000; ++i)
            index.add(dir, "file_" + std::to_string(i) + ".txt", 0);
        index.add(dir, "file_1", 0); // exact name: must win

        const NameMatcher matcher(ws::parseQuery(u"file_1"_s));
        WorkerPool pool(4);
        const auto out = searchNames(index, matcher, {.limit = 10}, pool, {});
        QVERIFY(!out.cancelled);
        QCOMPARE(out.hits.size(), std::size_t {10});
        QCOMPARE(index.name(out.hits.front().id), std::string_view("file_1"));
        // file_1, file_10..19, ..., file_10000..19999 (.txt), plus the exact name
        QCOMPARE(out.totalMatches, std::size_t {1 + 10 + 100 + 1000 + 10000 + 1});
        for (std::size_t i = 1; i < out.hits.size(); ++i)
            QVERIFY(out.hits[i - 1].score >= out.hits[i].score);

        const auto cancelled = searchNames(index, matcher, {.limit = 10}, pool, [] { return true; });
        QVERIFY(cancelled.cancelled);
    }

    void nameSearchTestsNamesFirst()
    {
        // A search rules names out first (NameMatcher::nameMayMatch), once per
        // stored name: it must find what matching every entry finds, for every
        // kind of query.
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const char* folders[] = {"src", "app", "报告", "node_modules", "proj", "tmp", "Data 2025"};
        const char* files[] = {"readme.md", "Report.pdf", "报告2025.docx", "index.js", "a.c", "abc.txt", "我的文档.txt",
            "data.json", "x", "TEST.PDF", "draft report.doc", "app.log", "src", "合同.xlsx", "README"};
        std::vector<EntryId> level {root};
        for (int depth = 0; depth < 3; ++depth) {
            std::vector<EntryId> next;
            for (const EntryId parent : level) {
                for (const char* folder : folders)
                    next.push_back(index.add(parent, folder, EntryFlag::Directory));
                for (const char* file : files)
                    index.add(parent, file, 0);
            }
            level = std::move(next);
        }
        WorkerPool pool(4);
        for (const QString& q : {u"report"_s, u"bg"_s, u"baogao"_s, u"报告"_s, u"src app"_s, u"report !pdf"_s,
                 u"!node_modules\\ readme"_s, u"a?c*"_s, u"*.pdf"_s, u"ext:pdf"_s, u"ext:docx 报告"_s, u"proj\\readme"_s,
                 u"2025"_s, u"README"_s, u"!*.log app"_s, u"wd"_s, u"x"_s, u"src\\"_s, u"\"draft report\""_s,
                 u"tmp\\ data"_s, u"ext:txt,md"_s, u"!src\\ src"_s}) {
            const NameMatcher matcher(ws::parseQuery(q));
            std::size_t matches = 0;
            for (std::size_t i = 0; i < index.slotCount(); ++i) {
                const Entry& e = index.entry(static_cast<EntryId>(i));
                if ((e.flags & EntryFlag::Root) || matcher.match(index, e) < 0)
                    continue;
                ++matches;
                QVERIFY2(matcher.nameMayMatch(index.name(e), index.nameExtLength(e.nameOffset),
                             index.nameHasHan(e.nameOffset)),
                    qPrintable(q));
            }
            const auto out = searchNames(index, matcher, {.limit = 1000}, pool, {});
            QVERIFY2(out.totalMatches == matches, qPrintable(q));
            QVERIFY2(matches > 0 || q == u"!src\\ src"_s, qPrintable(q));
        }
    }

    void nameSearchKindsAndTimes()
    {
        constexpr FileTime T = 1'700'000'000; // 2023: before 1980 is no time (knownTime)
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "work", EntryFlag::Directory);
        // Many files that match better than the folders: a list of the best
        // 20 of all would hold no folder.
        for (int i = 0; i < 50; ++i)
            index.setModified(index.add(dir, "plan" + std::to_string(i) + ".txt", 0), T + 1000 + i);
        const EntryId folderA = index.add(dir, "old plans", EntryFlag::Directory);
        const EntryId folderB = index.add(dir, "my plans", EntryFlag::Directory);
        index.setModified(folderA, T + 10);
        index.setModified(folderB, T + 2000);
        const NameMatcher matcher(ws::parseQuery(u"plan"_s));
        WorkerPool pool(4);
        const auto isDir = [&](const NameHit& h) { return index.entry(h.id).isDir(); };

        const auto all = searchNames(index, matcher, {.limit = 20}, pool, {});
        QCOMPARE(all.hits.size(), std::size_t {20});
        QVERIFY(std::none_of(all.hits.begin(), all.hits.end(), isDir));

        const auto apart = searchNames(index, matcher, {.limit = 20, .kindsApart = true}, pool, {});
        QCOMPARE(apart.hits.size(), std::size_t {22}); // 20 files, and the 2 folders
        QCOMPARE(std::count_if(apart.hits.begin(), apart.hits.end(), isDir), std::ptrdiff_t {2});
        QCOMPARE(apart.totalMatches, std::size_t {52});

        const auto folders = searchNames(index, matcher, {.limit = 20, .foldersOnly = true}, pool, {});
        QCOMPARE(folders.hits.size(), std::size_t {2});
        QVERIFY(std::all_of(folders.hits.begin(), folders.hits.end(), isDir));
        QCOMPARE(folders.totalMatches, std::size_t {2});

        // Newest first, whatever the match.
        const auto byTime = searchNames(index, matcher, {.limit = 5, .rankBy = RankBy::Newest}, pool, {});
        QCOMPARE(byTime.hits.size(), std::size_t {5});
        QCOMPARE(byTime.hits[0].id, folderB);
        QCOMPARE(index.name(byTime.hits[1].id), std::string_view("plan49.txt"));
        for (std::size_t i = 1; i < byTime.hits.size(); ++i)
            QVERIFY(byTime.hits[i - 1].modified > byTime.hits[i].modified);

        // Oldest first; a time not known goes last, not first.
        // 1970-01-01 (unpacked from an archive that knew no time) and the
        // DOS epoch are not known either: last too, and said to be 0.
        const EntryId undated = index.add(dir, "plan undated.txt", 0); // modified 0
        const EntryId epoch = index.add(dir, "plan epoch.txt", 0);
        const EntryId dos = index.add(dir, "plan dos.txt", 0);
        index.setModified(epoch, 1);
        index.setModified(dos, 315'532'800 - 8 * 3600); // 1980-01-01 00:00 in UTC+8
        const auto oldest = searchNames(index, matcher, {.limit = 60, .rankBy = RankBy::Oldest}, pool, {});
        QCOMPARE(oldest.hits.size(), std::size_t {55});
        QCOMPARE(oldest.hits[0].id, folderA);
        QCOMPARE(index.name(oldest.hits[1].id), std::string_view("plan0.txt"));
        const std::vector<EntryId> lastThree {oldest.hits[52].id, oldest.hits[53].id, oldest.hits[54].id};
        for (const EntryId id : {undated, epoch, dos})
            QVERIFY(std::ranges::find(lastThree, id) != lastThree.end());
        QVERIFY(std::all_of(oldest.hits.begin() + 52, oldest.hits.end(), [](const NameHit& h) { return h.modified == 0; }));
        for (std::size_t i = 2; i < 52; ++i)
            QVERIFY(oldest.hits[i - 1].modified < oldest.hits[i].modified);
        // Kept to a few, the oldest still: the heap must not let a 0 in.
        const auto oldestFew = searchNames(index, matcher, {.limit = 3, .rankBy = RankBy::Oldest}, pool, {});
        QCOMPARE(oldestFew.hits.size(), std::size_t {3});
        QCOMPARE(oldestFew.hits[0].id, folderA);
        QVERIFY(std::none_of(oldestFew.hits.begin(), oldestFew.hits.end(), [](const NameHit& h) { return h.modified == 0; }));
        const auto newestAll = searchNames(index, matcher, {.limit = 60, .rankBy = RankBy::Newest}, pool, {});
        QVERIFY(std::all_of(newestAll.hits.end() - 3, newestAll.hits.end(), [](const NameHit& h) { return h.modified == 0; }));
        QCOMPARE(knownTime(315'619'200), FileTime {315'619'200});
        QCOMPARE(knownTime(315'532'800), FileTime {0});

        // Equal matches: the one written last first.
        const EntryId a = index.add(root, "same1.txt", 0);
        const EntryId b = index.add(root, "same2.txt", 0);
        index.setModified(a, T + 100);
        index.setModified(b, T + 300);
        const auto ties = searchNames(index, NameMatcher(ws::parseQuery(u"same"_s)), {.limit = 5}, pool, {});
        QCOMPARE(ties.hits.size(), std::size_t {2});
        QCOMPARE(ties.hits[0].score, ties.hits[1].score);
        QCOMPARE(ties.hits[0].id, b);

        // Content matches: the better first, or by time with a time not
        // known (1970 too) last both ways, the better match on a tie.
        const auto row = [](std::uint32_t modified, int relevance) {
            SearchResult r;
            r.modified = modified;
            r.relevance = relevance;
            return r;
        };
        const SearchResult older = row(T + 100, 9), newer = row(T + 200, 1), undatedRow = row(1, 50);
        const SearchResult twin = row(T + 200, 5);
        QVERIFY(rankedBefore(older, newer, RankBy::Match));
        QVERIFY(rankedBefore(newer, older, RankBy::Newest));
        QVERIFY(rankedBefore(older, newer, RankBy::Oldest));
        QVERIFY(rankedBefore(older, undatedRow, RankBy::Oldest));
        QVERIFY(rankedBefore(older, undatedRow, RankBy::Newest));
        QVERIFY(!rankedBefore(undatedRow, older, RankBy::Oldest));
        QVERIFY(rankedBefore(twin, newer, RankBy::Newest));
        QVERIFY(!rankedBefore(newer, twin, RankBy::Oldest));

        // Dated far ahead (an installer's 2076): a clock gone wrong, ordered
        // as not known, last both ways; a day ahead is still trusted.
        const FileTime until = trustedUntil();
        const SearchResult ahead = row(0xC000'0000u, 50); // 2072
        QVERIFY(rankedBefore(newer, ahead, RankBy::Newest));
        QVERIFY(rankedBefore(older, ahead, RankBy::Oldest));
        QCOMPARE(sortTime(0xC000'0000u, until), FileTime {0});
        QCOMPARE(sortTime(until - 3600, until), until - 3600);
        QCOMPARE(sortTime(1, until), FileTime {0});

        // The panel's newest first: the one dated ahead last, its own date
        // still known to the index.
        const EntryId future = index.add(root, "same-ahead.txt", 0);
        index.setModified(future, 0xC000'0000u);
        const auto newestFirst = searchNames(index, NameMatcher(ws::parseQuery(u"same"_s)),
            {.limit = 5, .order = {SortKey::Modified, true}, .trustedUntil = until}, pool, {});
        QVERIFY(!newestFirst.hits.empty());
        QVERIFY(newestFirst.hits.front().id != future);
        QCOMPARE(newestFirst.hits.back().id, future);
        QCOMPARE(newestFirst.hits.back().modified, FileTime {0});
        QCOMPARE(index.modified(future), 0xC000'0000u);
    }

    // 优先显示的文件类型 (PreferredTypes): any case, without the dot; a
    // name's extension as the index has it (after its last dot).
    void preferredTypesTable()
    {
        const PreferredTypes none;
        QVERIFY(none.empty());
        QVERIFY(!none.has("exe"));
        QVERIFY(!none.hasName("curl.exe"));

        const PreferredTypes types({"exe", "PS1", "", "averyveryverylongone", "exe"});
        QVERIFY(!types.empty());
        QVERIFY(types.has("exe"));
        QVERIFY(types.has("EXE"));
        QVERIFY(types.has("ps1"));
        QVERIFY(!types.has("ex"));
        QVERIFY(!types.has("exe2"));
        QVERIFY(!types.has(""));
        QVERIFY(!types.has("averyveryverylongone")); // longer than any extension kept
        QVERIFY(types.hasName("curl.EXE"));
        QVERIFY(types.hasName("node-v22.tar.exe"));
        QVERIFY(!types.hasName("curl.exe.bak"));
        QVERIFY(!types.hasName("exe"));
        QVERIFY(!types.hasName("curl."));

        // At most 32 kept.
        std::vector<std::string> many;
        for (int i = 0; i < 40; ++i)
            many.push_back("x" + std::to_string(i));
        const PreferredTypes full(many);
        QVERIFY(full.has("x31"));
        QVERIFY(!full.has("x32"));
    }

    // The scores the types listed first get (forum #104, #116), in the
    // search over the index: comparable matches go their way, much better
    // ones stay ahead, and a program named just what was typed loses nothing
    // for being in a program folder. Only by match.
    void nameSearchPreferredTypes()
    {
        FileIndex index;
        const EntryId f = index.addRoot("F:");
        const EntryId test = index.add(f, "wsexetest", EntryFlag::Directory);
        const EntryId folder = index.add(test, "curl", EntryFlag::Directory);
        const EntryId header = index.add(test, "curl.h", 0);
        const EntryId exe = index.add(test, "curl.exe", 0);
        index.add(test, "curl-config", 0);
        index.add(test, "libcurl.dll", 0);
        const EntryId notes = index.add(test, "curl tools.txt", 0); // at the start, as:
        const EntryId updater = index.add(test, "curl-updater.exe", 0);
        const EntryId c = index.addRoot("C:");
        const auto aside = static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::LowPriority);
        const EntryId windows = index.add(c, "Windows", aside);
        const EntryId system32 = index.add(windows, "System32", aside);
        const EntryId systemCurl = index.add(system32, "curl.exe", EntryFlag::LowPriority);
        const EntryId helper = index.add(system32, "curl-helper.exe", EntryFlag::LowPriority);
        WorkerPool pool(4);
        const NameMatcher matcher(ws::parseQuery(u"curl"_s));
        const PreferredTypes programs({"exe", "bat", "cmd", "ps1", "msi"});

        const auto scores = [&](const NameSearchOptions& options) {
            std::unordered_map<EntryId, int> out;
            for (const NameHit& hit : searchNames(index, matcher, options, pool, {}).hits)
                out[hit.id] = hit.score;
            return out;
        };
        auto plain = scores({.limit = 20, .kindsApart = true});
        QCOMPARE(plain.size(), std::size_t {9});
        // As before: the folder, the header, then the program.
        QVERIFY(plain[folder] > plain[header]);
        QVERIFY(plain[header] > plain[exe]);
        QVERIFY(plain[notes] > plain[updater]);
        QVERIFY(plain[header] > plain[systemCurl]);

        auto preferred = scores({.limit = 20, .kindsApart = true, .preferred = programs});
        QCOMPARE(preferred[exe], plain[exe] + PreferredTypes::kBonus);
        QVERIFY(preferred[exe] > preferred[header]);
        QVERIFY(preferred[updater] > preferred[notes]); // comparable: the program first
        // Much better matches stay ahead: the folder named so, the header named so.
        QVERIFY(preferred[folder] > preferred[updater]);
        QVERIFY(preferred[header] > preferred[updater]);
        // C:\Windows\System32\curl.exe is the curl asked for, in a program folder or not...
        QVERIFY(preferred[systemCurl] > preferred[header]);
        QCOMPARE(preferred[systemCurl], plain[systemCurl] + PreferredTypes::kBonus + 35);
        // ... but the rest of a program folder stays below the user's own.
        QVERIFY(preferred[helper] < preferred[notes]);
        QCOMPARE(preferred[helper], plain[helper] + PreferredTypes::kBonus);
        QCOMPARE(preferred[folder], plain[folder]);

        // By time, or by a column, the scores are as they were (ties only).
        QVERIFY(scores({.limit = 20, .rankBy = RankBy::Newest, .kindsApart = true, .preferred = programs})
            == scores({.limit = 20, .rankBy = RankBy::Newest, .kindsApart = true}));
        QVERIFY(scores({.limit = 20, .order = {SortKey::Name, false}, .preferred = programs})
            == scores({.limit = 20, .order = {SortKey::Name, false}}));
        // Empty: nothing listed first.
        QVERIFY(scores({.limit = 20, .kindsApart = true, .preferred = PreferredTypes()}) == plain);
        // Another list: the header's type.
        auto headers = scores({.limit = 20, .kindsApart = true, .preferred = PreferredTypes({"H"})});
        QVERIFY(headers[header] > headers[exe]);
        QCOMPARE(headers[exe], plain[exe]);
    }

    // The same through the engine (arrange): among the names typed just so,
    // the types listed first come first, in 全部 and 文件, folders first or
    // not; a folder named so still goes before a program that only begins
    // so; by time, nothing changes.
    void preferredTypesFirst()
    {
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const EntryId f = index->addRoot("F:");
            const EntryId test = index->add(f, "wsexetest", EntryFlag::Directory);
            index->add(test, "curl", EntryFlag::Directory);
            for (const char* name : {"curl.h", "curl.exe", "curl-config", "libcurl.dll", "tools-helper.exe"})
                index->add(test, name, 0);
            index->add(test, "tools", EntryFlag::Directory);
            const EntryId c = index->addRoot("C:");
            const auto aside = static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::LowPriority);
            const EntryId system32 = index->add(index->add(c, "Windows", aside), "System32", aside);
            index->add(system32, "curl.exe", EntryFlag::LowPriority);
            FileTime t = 1'700'000'000;
            for (std::size_t i = 0; i < index->slotCount(); ++i)
                index->setModified(static_cast<EntryId>(i), t += 100); // each a little later
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](const QString& text, Scope scope, const QStringList& preferred,
                                KindOrder kindOrder = KindOrder::FilesFirst, RankBy rankBy = RankBy::Match) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            request.scope = scope;
            request.preferredTypes = preferred;
            request.kindOrder = kindOrder;
            request.rankBy = rankBy;
            engine.submit(std::move(request));
            QStringList paths;
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                paths.append(r.path);
            return paths;
        };
        const QStringList programs {u"exe"_s, u"bat"_s, u"cmd"_s, u"ps1"_s, u"msi"_s};
        const QString exe = u"F:\\wsexetest\\curl.exe"_s;
        const QString systemExe = u"C:\\Windows\\System32\\curl.exe"_s;
        const QString folder = u"F:\\wsexetest\\curl"_s;
        const QString header = u"F:\\wsexetest\\curl.h"_s;

        // Forum #116: "curl" lists curl.exe first, not the folder curl or curl.h.
        for (const Scope scope : {Scope::All, Scope::Files}) {
            const QStringList found = search(u"curl"_s, scope, programs);
            QCOMPARE(found.mid(0, 4), (QStringList {exe, systemExe, folder, header}));
            QCOMPARE(found.size(), 6);
        }
        QCOMPARE(search(u"curl"_s, Scope::All, programs, KindOrder::FoldersFirst).mid(0, 4),
            (QStringList {exe, systemExe, folder, header}));
        // Empty: as before.
        QCOMPARE(search(u"curl"_s, Scope::All, {}).mid(0, 4), (QStringList {folder, header, exe, systemExe}));
        // Another list.
        QCOMPARE(search(u"curl"_s, Scope::All, {u"h"_s}).first(), header);
        // The folder named "tools" before the program that only begins so.
        QCOMPARE(search(u"tools"_s, Scope::All, programs),
            (QStringList {u"F:\\wsexetest\\tools"_s, u"F:\\wsexetest\\tools-helper.exe"_s}));
        // Newest first: the list does not care.
        QCOMPARE(search(u"curl"_s, Scope::Files, programs, KindOrder::FilesFirst, RankBy::Newest),
            search(u"curl"_s, Scope::Files, {}, KindOrder::FilesFirst, RankBy::Newest));
    }

    // Settings' skipped file types: files of them are left out of names (and
    // the start list), whatever the case; a folder that is named so is not a
    // file; asked for by extension (ext: typed, a type chosen), they show.
    void skippedFileTypes()
    {
        const QString root = u"F:\\wsskiptest\\"_s;
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const EntryId f = index->addRoot("F:");
            const EntryId test = index->add(f, "wsskiptest", EntryFlag::Directory);
            for (const char* name : {"report.docx", "report.bak", "report.WBK", "report.Bak.txt", "report-old.bak", "report"})
                index->add(test, name, name == std::string("report-old.bak") ? EntryFlag::Directory : 0);
            FileTime t = 1'700'000'000;
            for (std::size_t i = 0; i < index->slotCount(); ++i)
                index->setModified(static_cast<EntryId>(i), t += 100);
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](const QString& text, Scope scope, const QStringList& skipped, const QStringList& types = {}) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            request.scope = scope;
            request.excludedTypes = skipped;
            request.extensions = types;
            engine.submit(std::move(request));
            QStringList names;
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                names.append(r.path.mid(root.size()));
            names.sort();
            return names;
        };
        const QStringList all {u"report"_s, u"report-old.bak"_s, u"report.Bak.txt"_s, u"report.WBK"_s, u"report.bak"_s, u"report.docx"_s};
        QCOMPARE(search(u"report"_s, Scope::Files, {}), all);
        // Not skipped by default; upper case in the setting does not matter.
        for (const Scope scope : {Scope::All, Scope::Files}) {
            QCOMPARE(search(u"report"_s, scope, {u"BAK"_s, u"wbk"_s}),
                (QStringList {u"report"_s, u"report-old.bak"_s, u"report.Bak.txt"_s, u"report.docx"_s})); // the folder stays
        }
        // A folder is no file of the type.
        QVERIFY(search(u"report"_s, Scope::Folders, {u"bak"_s}).contains(u"report-old.bak"_s));
        // Asked for: ext: typed, or a type chosen with it among.
        QCOMPARE(search(u"report ext:bak"_s, Scope::Files, {u"bak"_s}), (QStringList {u"report.bak"_s}));
        QCOMPARE(search(u"report"_s, Scope::Files, {u"bak"_s, u"wbk"_s}, {u"bak"_s, u"docx"_s}),
            (QStringList {u"report.bak"_s, u"report.docx"_s}));
        // Another type chosen: a skipped one stays out, though the type names it only as one of its own.
        QCOMPARE(search(u"report"_s, Scope::Files, {u"bak"_s}, {u"docx"_s}), (QStringList {u"report.docx"_s}));
        // Nothing typed, the files written last.
        {
            spy.clear();
            SearchEngine::Request request;
            request.startList = StartList::Modified;
            request.excludedTypes = {u"bak"_s, u"wbk"_s, u"docx"_s};
            engine.submit(std::move(request));
            QVERIFY(spy.wait(5000));
            QStringList names;
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                names.append(r.path.mid(root.size()));
            QVERIFY(!names.contains(u"report.docx"_s) && !names.contains(u"report.bak"_s) && !names.contains(u"report.WBK"_s));
            QVERIFY(names.contains(u"report.Bak.txt"_s));
        }
        // The matcher alone: files only, as the index has them.
        FileIndex index;
        const EntryId top = index.addRoot("C:");
        const EntryId file = index.add(top, "a.BAK", 0);
        const EntryId folder = index.add(top, "b.bak", EntryFlag::Directory);
        const EntryId other = index.add(top, "c.txt", 0);
        ws::ParsedQuery query = ws::parseQuery(u""_s);
        query.excludedExtensions = {"bak"};
        const NameMatcher matcher(query);
        QCOMPARE(matcher.match(index, index.entry(file)), -1);
        QVERIFY(matcher.match(index, index.entry(folder)) >= 0);
        QVERIFY(matcher.match(index, index.entry(other)) >= 0);
    }

    // GitHub #132: Settings' skipped file names (wildcards, any case) leave
    // files so named out of names and the start list; a folder so named is
    // not a file; a word typed that is such a name shows them.
    void skippedFileNames()
    {
        const QString root = u"F:\\wsskipnames\\"_s;
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const EntryId f = index->addRoot("F:");
            const EntryId test = index->add(f, "wsskipnames", EntryFlag::Directory);
            for (const char* name : {"~$report.docx", "report.docx", "report~$.txt", "~$report", "Thumbs.db", "draft-report.txt"})
                index->add(test, name, name == std::string("~$report") ? EntryFlag::Directory : 0);
            FileTime t = 1'700'000'000;
            for (std::size_t i = 0; i < index->slotCount(); ++i)
                index->setModified(static_cast<EntryId>(i), t += 100);
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto run = [&](SearchEngine::Request request) {
            spy.clear();
            engine.submit(std::move(request));
            QStringList names;
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                names.append(r.path.mid(root.size()));
            names.sort();
            return names;
        };
        const auto search = [&](const QString& text, const QStringList& skipped, Scope scope = Scope::All) {
            SearchEngine::Request request;
            request.text = text;
            request.scope = scope;
            request.excludedFileNames = skipped;
            return run(std::move(request));
        };
        QCOMPARE(search(u"report"_s, {}),
            (QStringList {u"draft-report.txt"_s, u"report.docx"_s, u"report~$.txt"_s, u"~$report"_s, u"~$report.docx"_s}));
        // The file goes, the folder so named stays; "~$" elsewhere in a name is no "~$*".
        QCOMPARE(search(u"report"_s, {u"~$*"_s}),
            (QStringList {u"draft-report.txt"_s, u"report.docx"_s, u"report~$.txt"_s, u"~$report"_s}));
        // A whole name, any case; and "?" one character.
        QCOMPARE(search(u"thumbs"_s, {u"THUMBS.DB"_s}), QStringList());
        QCOMPARE(search(u"report"_s, {u"report.doc?"_s}),
            (QStringList {u"draft-report.txt"_s, u"report~$.txt"_s, u"~$report"_s, u"~$report.docx"_s}));
        // Asked for: a word typed that is such a name, as it is or with wildcards.
        QVERIFY(search(u"~$report"_s, {u"~$*"_s}).contains(u"~$report.docx"_s));
        QVERIFY(search(u"~$*.docx"_s, {u"~$*"_s}).contains(u"~$report.docx"_s));
        QVERIFY(search(u"docx \"~$\""_s, {u"~$*"_s}).contains(u"~$report.docx"_s));
        QVERIFY(search(u"^~$rep"_s, {u"~$*"_s}).contains(u"~$report.docx"_s));
        // Not asked for: a word that is not one (*.docx), or one ruled out.
        QCOMPARE(search(u"*.docx"_s, {u"~$*"_s}), (QStringList {u"report.docx"_s}));
        QCOMPARE(search(u"report !~$x"_s, {u"~$*"_s}),
            (QStringList {u"draft-report.txt"_s, u"report.docx"_s, u"report~$.txt"_s, u"~$report"_s}));
        // Nothing typed, the files written last.
        {
            SearchEngine::Request request;
            request.startList = StartList::Modified;
            request.excludedFileNames = {u"draft-*"_s};
            const QStringList names = run(std::move(request));
            QVERIFY(names.contains(u"report.docx"_s));
            QVERIFY(!names.contains(u"draft-report.txt"_s));
        }
        // The matcher alone: files only, as the index has them.
        FileIndex index;
        const EntryId top = index.addRoot("C:");
        const EntryId file = index.add(top, "~$A.DOCX", 0);
        const EntryId folder = index.add(top, "~$b", EntryFlag::Directory);
        const EntryId other = index.add(top, "c~$.txt", 0);
        ws::ParsedQuery query = ws::parseQuery(u""_s);
        query.excludedFileNames = {"~$*.docx"};
        const NameMatcher matcher(query);
        QCOMPARE(matcher.match(index, index.entry(file)), -1);
        QVERIFY(matcher.match(index, index.entry(folder)) >= 0);
        QVERIFY(matcher.match(index, index.entry(other)) >= 0);
    }

    // GitHub #49: "search here" on F1 (the box holds "…\F1\ ") also showed
    // what F11 holds: the folders of a typed path match by name, and "f1" is
    // in "f11".
    void typedFolderDoesNotTakeSimilarNames()
    {
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const EntryId root = index->addRoot("F:");
            const EntryId top = index->add(root, "wsno49", EntryFlag::Directory);
            const EntryId f1 = index->add(top, "F1", EntryFlag::Directory);
            const EntryId f11 = index->add(top, "F11", EntryFlag::Directory);
            index->add(f1, "report.txt", 0);
            index->add(index->add(f1, "sub", EntryFlag::Directory), "report-deep.txt", 0);
            index->add(f11, "report.txt", 0);
            index->add(index->add(f11, "sub", EntryFlag::Directory), "report-deep.txt", 0);
            index->add(top, "report-top.txt", 0);
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](const QString& text) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            engine.submit(std::move(request));
            QStringList paths;
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                paths.append(r.path);
            paths.sort();
            return paths;
        };
        const QStringList inF1 {u"F:\\wsno49\\F1\\report.txt"_s, u"F:\\wsno49\\F1\\sub\\report-deep.txt"_s};
        // As Explorer's menu types it, and with the other slash.
        for (const QString& folder : {u"F:\\wsno49\\F1\\ "_s, u"F:/wsno49/F1/ "_s}) {
            const QStringList found = search(folder + u"report"_s);
            QCOMPARE(found, inF1);
        }
        // A folder the index does not have holds nothing of its own, whatever
        // folders have its name in theirs.
        QCOMPARE(search(u"F:\\wsno49\\F\\ report"_s), QStringList());
        // Nothing typed after it: the folder's own, not F11's.
        const QStringList bare = search(u"F:\\wsno49\\F1\\ "_s);
        QVERIFY2(std::ranges::none_of(bare, [](const QString& p) { return p.contains(u"F11"); }), qPrintable(bare.join(u'|')));
    }

    // GitHub #44: the folder typed lists what is in it from disk, and those
    // rows went above everything whatever order was chosen. Now, other than
    // best match, they are sorted with the rest, "folders first" or not.
    void typedFolderRowsFollowTheSort()
    {
        QTemporaryDir dir;
        const QString top = QDir::toNativeSeparators(dir.path());
        const QDateTime base = QDateTime::fromSecsSinceEpoch(1'792'584'000, QTimeZone::UTC);
        const auto make = [&](const QString& name, bool isDir, int minutes, int bytes) {
            const QString path = top + u'\\' + name;
            if (isDir) {
                if (!QDir().mkpath(path))
                    return QString();
                return path; // a folder's time is set below, once its files are in
            }
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return QString();
            file.write(QByteArray(bytes, 'x'));
            file.setFileTime(base.addSecs(minutes * 60), QFileDevice::FileModificationTime);
            return path;
        };
        QVERIFY(!make(u"zqdir-old"_s, true, 0, 0).isEmpty());
        QVERIFY(!make(u"zqdir-new"_s, true, 0, 0).isEmpty());
        QVERIFY(!make(u"zqfile-mid.txt"_s, false, 20, 300).isEmpty());
        QVERIFY(!make(u"zqfile-top.bin"_s, false, 40, 100).isEmpty());
        QVERIFY(!make(u"zqfile-low.md"_s, false, 5, 200).isEmpty());
        // Folder times through the Win32 call the files use.
        const auto setDirTime = [&](const QString& path, int minutes) {
            const HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return false;
            ULARGE_INTEGER t;
            t.QuadPart = (static_cast<quint64>(base.addSecs(minutes * 60).toSecsSinceEpoch()) + 11644473600ULL) * 10'000'000ULL;
            const FILETIME ft {t.LowPart, t.HighPart};
            const bool ok = SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
            return ok;
        };
        QVERIFY(setDirTime(top + u"\\zqdir-old"_s, 10));
        QVERIFY(setDirTime(top + u"\\zqdir-new"_s, 30));

        QTemporaryDir store;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(store.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            EntryId folder = index->addRoot(top.left(2).toStdString());
            for (const QString& name : top.mid(3).split(u'\\', Qt::SkipEmptyParts))
                folder = index->add(folder, name.toStdString(), EntryFlag::Directory);
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](RankBy rankBy, SortOrder order, KindOrder kinds, int limit = 100) {
            spy.clear();
            SearchEngine::Request request;
            request.text = top + u"\\ zq"_s;
            request.rankBy = rankBy;
            request.order = order;
            request.kindOrder = kinds;
            request.limit = limit;
            engine.submit(std::move(request));
            QStringList names;
            if (!spy.wait(5000))
                return QStringList {u"(no results)"_s};
            for (const SearchResult& r : spy.at(0).at(1).value<SearchResults>())
                names.append(r.name);
            return names;
        };
        using L = QStringList;
        // Newest first: by time, the two kinds apart as asked.
        QCOMPARE(search(RankBy::Newest, {}, KindOrder::FilesFirst),
            (L {u"zqfile-top.bin"_s, u"zqfile-mid.txt"_s, u"zqfile-low.md"_s, u"zqdir-new"_s, u"zqdir-old"_s}));
        QCOMPARE(search(RankBy::Newest, {}, KindOrder::FoldersFirst),
            (L {u"zqdir-new"_s, u"zqdir-old"_s, u"zqfile-top.bin"_s, u"zqfile-mid.txt"_s, u"zqfile-low.md"_s}));
        QCOMPARE(search(RankBy::Oldest, {}, KindOrder::FoldersFirst),
            (L {u"zqdir-old"_s, u"zqdir-new"_s, u"zqfile-low.md"_s, u"zqfile-mid.txt"_s, u"zqfile-top.bin"_s}));
        // The launcher's own sorts (#44): name, type, size.
        QCOMPARE(search(RankBy::Match, {SortKey::Name, true}, KindOrder::FoldersFirst),
            (L {u"zqdir-old"_s, u"zqdir-new"_s, u"zqfile-top.bin"_s, u"zqfile-mid.txt"_s, u"zqfile-low.md"_s}));
        QCOMPARE(search(RankBy::Match, {SortKey::Type, false}, KindOrder::FilesFirst).mid(0, 3),
            (L {u"zqfile-top.bin"_s, u"zqfile-low.md"_s, u"zqfile-mid.txt"_s}));
        QCOMPARE(search(RankBy::Match, {SortKey::Size, true}, KindOrder::FilesFirst).mid(0, 3),
            (L {u"zqfile-mid.txt"_s, u"zqfile-low.md"_s, u"zqfile-top.bin"_s}));
        // A longer list only adds to the end of a shorter one (the next page).
        QCOMPARE(search(RankBy::Newest, {}, KindOrder::FilesFirst, 3),
            (L {u"zqfile-top.bin"_s, u"zqfile-mid.txt"_s, u"zqfile-low.md"_s}));
    }

    // GitHub #49 as users meet it: F1 in the index, but what was opened
    // lately in F11 came up first, the recent items being matched by the
    // names of their folders ("f1" in "F11") and not by where they lie. On
    // disk: a recent item is looked up there (one the index lacks too).
    void typedFolderLeavesOutRecentLookAlikes()
    {
        QTemporaryDir dir;
        const QString top = QDir::toNativeSeparators(dir.path());
        QVERIFY(QDir(dir.path()).mkpath(u"F1/sub"_s) && QDir(dir.path()).mkpath(u"F11/sub"_s));
        const auto write = [&](const QString& relative) {
            QFile file(top + u'\\' + relative);
            return file.open(QIODevice::WriteOnly) ? QDir::toNativeSeparators(file.fileName()) : QString();
        };
        const QString inF1 = write(u"F1\\report.txt"_s);
        const QString newInF1 = write(u"F1\\sub\\new.txt"_s); // not in the index yet
        const QString inF11 = write(u"F11\\report.txt"_s);
        const QString deepInF11 = write(u"F11\\sub\\report-deep.txt"_s);
        QVERIFY(!inF1.isEmpty() && !newInF1.isEmpty() && !inF11.isEmpty() && !deepInF11.isEmpty());

        QTemporaryDir store;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(store.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            EntryId folder = index->addRoot(top.left(2).toStdString());
            for (const QString& name : top.mid(3).split(u'\\', Qt::SkipEmptyParts))
                folder = index->add(folder, name.toStdString(), EntryFlag::Directory);
            const EntryId f1 = index->add(folder, "F1", EntryFlag::Directory);
            const EntryId f11 = index->add(folder, "F11", EntryFlag::Directory);
            index->add(f1, "report.txt", 0);
            index->add(f1, "sub", EntryFlag::Directory);
            index->add(f11, "report.txt", 0);
            index->add(index->add(f11, "sub", EntryFlag::Directory), "report-deep.txt", 0);
            QVERIFY(index->findPath((top + u"\\F1"_s).toStdWString()) == f1);
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](const QString& text) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            request.history = {inF11, deepInF11, newInF1};
            engine.submit(std::move(request));
            if (!spy.wait(5000))
                return SearchResults {};
            return spy.at(0).at(1).value<SearchResults>();
        };
        // As Explorer's menu types it, and with a word after it.
        for (const QString& text : {top + u"\\F1\\ "_s, top + u"\\F1\\ report"_s}) {
            const SearchResults rows = search(text);
            QStringList paths;
            for (const SearchResult& r : rows)
                paths.append(r.path);
            QVERIFY2(paths.contains(inF1), qPrintable(paths.join(u'|')));
            QVERIFY2(std::ranges::none_of(paths, [](const QString& p) { return p.contains(u"\\F11"_s); }),
                qPrintable(paths.join(u'|')));
        }
        // What was opened in F1 lately still comes with it, from the disk.
        const SearchResults rows = search(top + u"\\F1\\ "_s);
        const auto it = std::ranges::find(rows, newInF1, &SearchResult::path);
        QVERIFY(it != rows.end());
        QVERIFY(it->recent);
    }

    // GitHub #110: typed in a folder's window, what is in that folder (at any
    // depth) comes first, then what is so named anywhere else, marked, as
    // Listary lists it; or that folder alone. One search of the index: each
    // part ranked as a list of its own, as the order asks.
    void folderTypedInListsItsOwnFirst()
    {
        constexpr FileTime T = 1'700'000'000;
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        {
            const auto index = service.index();
            const auto lock = index->writeLock();
            const auto add = [&](EntryId folder, const char* name, FileTime when, std::uint8_t flags = 0) {
                const EntryId id = index->add(folder, name, flags);
                index->setModified(id, when);
                return id;
            };
            const EntryId top = index->add(index->addRoot("F:"), "ws110", EntryFlag::Directory);
            const EntryId work = add(top, "work", T, EntryFlag::Directory);
            add(work, "report-a.txt", T + 100);
            add(work, "notes.md", T + 50);
            add(add(work, "sub", T, EntryFlag::Directory), "report-deep.txt", T + 300);
            add(work, "report-plan", T + 60, EntryFlag::Directory);
            const EntryId other = add(top, "other", T, EntryFlag::Directory);
            add(other, "report.txt", T + 500); // named just so: first of its part, not above the folder's
            add(other, "report-b.txt", T + 400);
            add(other, "deep-report.txt", T + 200);
            add(other, "readme.md", T + 600);
            add(top, "report-top.txt", T + 700);
            add(add(top, "worker", T, EntryFlag::Directory), "report-w.txt", T + 800); // "work" in its name: not in it
        }
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        struct Found {
            QStringList names; // "~" before a row found elsewhere
            qint64 total = -1;
            SearchResults rows;
        };
        const auto search = [&](const QString& text, const QString& folder, bool only = false, RankBy rankBy = RankBy::Match,
                                const QStringList& history = {}, int limit = 100) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            request.currentFolder = folder;
            request.currentFolderOnly = only;
            request.rankBy = rankBy;
            request.history = history;
            request.limit = limit;
            engine.submit(std::move(request));
            Found found;
            if (!spy.wait(5000)) {
                found.names = {u"(no results)"_s};
                return found;
            }
            found.rows = spy.at(0).at(1).value<SearchResults>();
            found.total = spy.at(0).at(2).toLongLong();
            for (const SearchResult& r : found.rows)
                found.names.append(r.elsewhere ? u'~' + r.name : r.name);
            return found;
        };
        const auto sorted = [](QStringList names) {
            names.sort();
            return names;
        };
        // One run of the folder's rows, then one of the others.
        const auto grouped = [](const Found& f) {
            const auto split = std::ranges::find_if(f.rows, &SearchResult::elsewhere);
            return std::all_of(split, f.rows.cend(), [](const SearchResult& r) { return r.elsewhere; });
        };
        using L = QStringList;
        const QString work = u"F:\\ws110\\work"_s;
        const L inside {u"report-a.txt"_s, u"report-deep.txt"_s, u"report-plan"_s};
        const L elsewhere {u"~deep-report.txt"_s, u"~report-b.txt"_s, u"~report-top.txt"_s, u"~report-w.txt"_s, u"~report.txt"_s};

        Found first = search(u"report"_s, work);
        QVERIFY2(grouped(first), qPrintable(first.names.join(u'|')));
        QCOMPARE(sorted(first.names.mid(0, 3)), inside);
        QCOMPARE(sorted(first.names.mid(3)), elsewhere);
        QCOMPARE(first.names.at(3), u"~report.txt"_s); // named just so, first after the folder's rows
        QVERIFY(first.rows.at(3).exact);
        QCOMPARE(first.total, qint64 {8});
        // As the folder is written in the box: with its backslash, or the other slash.
        QCOMPARE(search(u"report"_s, work + u"\\"_s).names, first.names);
        QCOMPARE(search(u"report"_s, u"F:/ws110/work"_s).names, first.names);

        // Only the folder: its rows alone, and the count of them.
        const Found only = search(u"report"_s, work, true);
        QCOMPARE(sorted(only.names), inside);
        QCOMPARE(only.total, qint64 {3});
        // The same as a path typed (the box edited, "search here"): that folder alone, as ever (#49).
        QCOMPARE(sorted(search(work + u"\\ report"_s, {}).names), inside);

        // An order chosen: the folder first all the same, each part in it.
        QCOMPARE(search(u"report"_s, work, false, RankBy::Newest).names,
            (L {u"report-deep.txt"_s, u"report-a.txt"_s, u"report-plan"_s, u"~report-w.txt"_s, u"~report-top.txt"_s,
                u"~report.txt"_s, u"~report-b.txt"_s, u"~deep-report.txt"_s}));
        QCOMPARE(search(u"report"_s, work, false, RankBy::Oldest).names.mid(0, 4),
            (L {u"report-a.txt"_s, u"report-deep.txt"_s, u"report-plan"_s, u"~deep-report.txt"_s}));

        // Opened lately: first of its own part, never above the folder's rows.
        const Found recent = search(u"report"_s, work, false, RankBy::Match,
            {u"F:\\ws110\\other\\deep-report.txt"_s, u"F:\\ws110\\work\\sub\\report-deep.txt"_s});
        QVERIFY2(grouped(recent), qPrintable(recent.names.join(u'|')));
        QCOMPARE(recent.names.at(0), u"report-deep.txt"_s);
        QVERIFY(recent.rows.at(0).recent);
        QCOMPARE(recent.names.at(3), u"~deep-report.txt"_s);
        QVERIFY(recent.rows.at(3).recent);
        QCOMPARE(recent.names.at(4), u"~report.txt"_s);

        // All the words, and a kind of file, after the folder as anywhere.
        QCOMPARE(search(u"report deep"_s, work).names, (L {u"report-deep.txt"_s, u"~deep-report.txt"_s}));
        QCOMPARE(search(u"*.md"_s, work).names, (L {u"notes.md"_s, u"~readme.md"_s}));
        QCOMPARE(search(u"*.md"_s, work, true).names, L {u"notes.md"_s});

        // A page more adds to the end: the folder's rows, then the rest.
        QCOMPARE(search(u"report"_s, work, false, RankBy::Match, {}, 3).names, first.names.mid(0, 3));
        const Found page = search(u"report"_s, work, false, RankBy::Match, {}, 5);
        QCOMPARE(page.names.mid(0, 3), first.names.mid(0, 3));
        QCOMPARE(page.rows.size(), qsizetype {5});
        QVERIFY2(grouped(page) && page.rows.at(3).elsewhere, qPrintable(page.names.join(u'|')));

        // A folder the index does not have: everything is elsewhere (alone: nothing);
        // a drive: everything is in it.
        const Found missing = search(u"report"_s, u"F:\\ws110\\gone"_s);
        QCOMPARE(sorted(missing.names), sorted(L {u"~report-a.txt"_s, u"~report-deep.txt"_s, u"~report-plan"_s} + elsewhere));
        QCOMPARE(search(u"report"_s, u"F:\\ws110\\gone"_s, true).names, L {});
        const Found drive = search(u"report"_s, u"F:\\"_s);
        QCOMPARE(drive.rows.size(), qsizetype {8});
        QVERIFY(std::ranges::none_of(drive.rows, &SearchResult::elsewhere));
    }

    void newestFiles()
    {
        constexpr FileTime T = 1'700'000'000;
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId docs = index.add(root, "Docs", EntryFlag::Directory);
        const EntryId build = index.add(root, "build", EntryFlag::Directory);
        index.setModified(docs, T + 950);
        index.setModified(build, T + 100);
        const EntryId appData = index.add(root, "AppData", EntryFlag::Directory | EntryFlag::LowPriority);
        const auto file = [&](EntryId folder, const char* name, FileTime when, std::uint8_t flags = 0) {
            const EntryId id = index.add(folder, name, flags);
            index.setModified(id, when);
            return id;
        };
        const EntryId report = file(docs, "report.docx", T + 900);
        const EntryId plan = file(docs, "plan.xlsx", T + 800);
        file(docs, "notes.txt", T + 700); // a third from Docs: over its share
        const EntryId exe = file(build, "app.exe", T + 850);
        file(build, "main.obj", T + 2000); // compiler output
        file(build, "app.pdb", T + 2000);
        file(build, ".ninja_log", T + 2000); // a dot-name
        file(docs, "~$report.docx", T + 2000); // Office's lock file
        file(docs, "movie.mp4.crdownload", T + 2000); // a download under way
        file(docs, "secret.txt", T + 2000, EntryFlag::Hidden);
        file(appData, "cache.dat", T + 2000, EntryFlag::LowPriority);
        file(docs, "camera.jpg", T + 5000); // dated in the future
        file(docs, "undated.txt", 0);
        file(docs, "from-a-zip.txt", 1); // 1970-01-01: no time
        // A build folder (CMakeCache.txt in it): all it holds, however new.
        const EntryId outDir = index.add(root, "out", EntryFlag::Directory);
        file(outDir, "CMakeCache.txt", T + 1500);
        file(index.add(outDir, "bin", EntryFlag::Directory), "app.exe", T + 3000);
        file(outDir, "compile_commands.json", T + 3000);
        file(index.add(root, "CMakeFiles", EntryFlag::Directory), "x.json", T + 3000);
        const EntryId folder = index.add(docs, "Project", EntryFlag::Directory);
        index.setModified(folder, T + 3000);
        WorkerPool pool(4);

        const auto ids = [](const NewestOutput& out) {
            std::vector<EntryId> ids;
            for (const NameHit& hit : out.hits)
                ids.push_back(hit.id);
            return ids;
        };
        const NewestOutput out = ws::newestFiles(index, {.limit = 5, .notAfter = T + 4000}, pool, {});
        QVERIFY(!out.cancelled);
        QCOMPARE(ids(out), (std::vector<EntryId> {report, exe, plan}));

        // The folders of those files, each once, by their newest file (not
        // by their own time: Project's is newer, but nothing in it is).
        const NewestOutput folders = ws::newestFiles(index, {.limit = 5, .folders = true, .notAfter = T + 4000}, pool, {});
        QCOMPARE(ids(folders), (std::vector<EntryId> {docs, build}));
        QCOMPARE(folders.hits[0].modified, FileTime {T + 900});

        // An archive unpacked with new times, many folders deep: three from
        // it, the rest from elsewhere.
        const EntryId pack = index.add(root, "pack", EntryFlag::Directory);
        for (int a = 0; a < 6; ++a) {
            const EntryId chapter = index.add(pack, "part" + std::to_string(a), EntryFlag::Directory);
            for (int f = 0; f < 4; ++f)
                file(chapter, ("pic" + std::to_string(f) + ".jpg").c_str(), T + 3500);
        }
        const NewestOutput mixed = ws::newestFiles(index, {.limit = 5, .notAfter = T + 4000}, pool, {});
        QCOMPARE(mixed.hits.size(), std::size_t {5});
        QCOMPARE(std::count_if(mixed.hits.begin(), mixed.hits.end(), [&](const NameHit& h) { return h.modified == T + 3500; }),
            std::ptrdiff_t {3});
        QCOMPARE(mixed.hits[3].id, report);

        QVERIFY(ws::isWorkFile("Report.TMP", false));
        QVERIFY(ws::isWorkFile("~WRL0001.tmp", false));
        QVERIFY(!ws::isWorkFile("report.docx", false));
        QVERIFY(!ws::isWorkFile("Makefile", false));
        QVERIFY(!ws::isWorkFile("logs.v2", true)); // a folder: its name only
        QVERIFY(ws::isWorkFile("28090b2f866c9be9746cf4e6457c181b", false));
        QVERIFY(ws::isWorkFile("f76b2dcdd318e482d5b110bfbcb42f18_t.jpg", false));
        QVERIFY(!ws::isWorkFile("20261010123456789.jpg", false)); // digits alone
        QVERIFY(!ws::isWorkFile("deadbeef.txt", false)); // short
    }

    void relativePath()
    {
        FileIndex index;
        const EntryId root = index.addRoot("F:");
        const EntryId tools = index.add(root, "tools", EntryFlag::Directory);
        const EntryId out = index.add(index.add(tools, "work", EntryFlag::Directory), "out", EntryFlag::Directory);
        const EntryId covers = index.add(out, "covers", EntryFlag::Directory);
        index.add(covers, "og.png", 0);
        const EntryId coversFile = index.add(out, "Covers", 0); // a file of the name too
        index.add(index.add(root, "old-tools", EntryFlag::Directory), "work", EntryFlag::Directory);
        const EntryId other = index.add(index.add(index.add(root, "tools", EntryFlag::Directory), "out",
            EntryFlag::Directory), "covers", EntryFlag::Directory); // tools\out\covers, no work between
        WorkerPool pool(4);
        const auto find = [&](std::vector<std::string> names, bool foldersOnly) {
            auto ids = findRelativePath(index, names, foldersOnly, 10, pool, {});
            std::ranges::sort(ids);
            return ids;
        };

        QCOMPARE(find({"tools", "work", "out", "covers"}, true), std::vector<EntryId> {covers});
        QCOMPARE(find({"TOOLS", "Work", "out", "covers"}, false), (std::vector<EntryId> {covers, coversFile}));
        QCOMPARE(find({"work", "out", "covers"}, true), std::vector<EntryId> {covers}); // from anywhere along the way
        QCOMPARE(find({"out", "covers"}, true), (std::vector<EntryId> {covers, other}));
        QCOMPARE(find({"tools", "out", "covers"}, true), std::vector<EntryId> {other}); // one inside the other, no gaps
        QCOMPARE(find({"tools", "work"}, true), std::vector<EntryId> {index.entry(out).parent}); // not old-tools\work
        QVERIFY(find({"tools", "cov"}, false).empty()); // still being typed
        QVERIFY(findRelativePath(index, {"out", "covers"}, true, 10, pool, [] { return true; }).empty());
        QCOMPARE(findRelativePath(index, {"out", "covers"}, true, 1, pool, {}).size(), std::size_t {1});
    }

    void exactName()
    {
        const NameMatcher readme(ws::parseQuery(u"README"_s));
        QVERIFY(readme.isExactName("readme", true));
        QVERIFY(readme.isExactName("README.md", false));
        QVERIFY(!readme.isExactName("README.md", true)); // a folder's name has no extension
        QVERIFY(!readme.isExactName("readme-old.md", false));
        QVERIFY(!NameMatcher(ws::parseQuery(u"read me"_s)).isExactName("read me", true)); // two terms
        QVERIFY(!NameMatcher(ws::parseQuery(u"read*"_s)).isExactName("readme", true));
    }

    void appSearch()
    {
        const auto app = [](const QString& name, const QString& id, const QString& target,
                             AppKind kind = AppKind::Desktop) {
            AppInfo a;
            a.name = name;
            a.id = id;
            a.target = target;
            a.kind = kind;
            a.prepare();
            return a;
        };
        const AppList apps {
            app(u"Visual Studio Code"_s, u"Microsoft.VisualStudioCode"_s, uR"(C:\VS Code\Code.exe)"_s),
            app(u"Word"_s, u"Microsoft.Office.WINWORD.EXE.15"_s, uR"(C:\Office\WINWORD.EXE)"_s),
            app(u"计算器"_s, u"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s, uR"(C:\Program Files\WindowsApps\Calc)"_s,
                AppKind::Store),
            app(u"Telegram"_s, u"Telegram.TelegramDesktop"_s, uR"(C:\Telegram\Telegram.exe)"_s),
            app(u"Uninstall Telegram"_s, uR"(C:\Telegram\unins000.exe)"_s, uR"(C:\Telegram\unins000.exe)"_s),
            app(u"Telegram FAQ"_s, uR"(C:\Telegram\faq.html)"_s, uR"(C:\Telegram\faq.html)"_s),
            // English first in the user's languages: Windows names it in English.
            app(u"Notepad"_s, u"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"_s, uR"(C:\Program Files\WindowsApps\Notepad)"_s,
                AppKind::Store),
            app(u"PowerShell"_s, u"Microsoft.PowerShell"_s, uR"(C:\PS\pwsh.exe)"_s),
        };
        const auto names = [&](const QString& text, const QStringList& history = {}) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const AppHit& hit : searchApps(apps, query, matcher, history))
                out.append(apps[hit.index].name);
            return out;
        };
        // The name a row shows: the one it was found by.
        const auto shown = [&](const QString& text) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const AppHit& hit : searchApps(apps, query, matcher, {}))
                out.append(hit.otherName >= 0 ? apps[hit.index].otherNames[hit.otherName] : apps[hit.index].name);
            return out;
        };

        QCOMPARE(apps[0].initials, std::string("vsc"));
        QCOMPARE(apps[1].program, std::string("WINWORD"));
        QVERIFY(apps[2].program.empty()); // packaged: no program file of its own
        QVERIFY(!apps[3].auxiliary);
        QVERIFY(apps[4].auxiliary); // uninstaller
        QVERIFY(apps[5].auxiliary); // a document

        QCOMPARE(names(u"jsq"_s), QStringList {u"计算器"_s}); // pinyin initials
        QCOMPARE(names(u"jisuan"_s), QStringList {u"计算器"_s});
        QCOMPARE(names(u"vsc"_s), QStringList {u"Visual Studio Code"_s}); // initials of the words
        QCOMPARE(names(u"winword"_s), QStringList {u"Word"_s}); // the program's file name
        QCOMPARE(names(u"telegram"_s), (QStringList {u"Telegram"_s, u"Telegram FAQ"_s, u"Uninstall Telegram"_s}));
        QCOMPARE(names(u"telegram !faq"_s), (QStringList {u"Telegram"_s, u"Uninstall Telegram"_s}));
        // Windows' own apps by their names in the other language too.
        QCOMPARE(apps[2].otherNames, QStringList {u"Calculator"_s});
        QCOMPARE(apps[6].otherNames, QStringList {u"记事本"_s});
        QVERIFY(apps[0].otherNames.isEmpty());
        QCOMPARE(shown(u"记事本"_s), QStringList {u"记事本"_s});
        QCOMPARE(shown(u"jsb"_s), QStringList {u"记事本"_s}); // and its pinyin
        QCOMPARE(shown(u"note"_s), QStringList {u"Notepad"_s});
        QCOMPARE(shown(u"calc"_s), QStringList {u"Calculator"_s});
        QCOMPARE(shown(u"计算器"_s), QStringList {u"计算器"_s});
        // Word-prefix span: pieces are prefixes of consecutive words.
        const QStringList vscode {u"Visual Studio Code"_s};
        QCOMPARE(names(u"vscode"_s), vscode); // v|s|code
        QCOMPARE(names(u"vs code"_s), vscode);
        QCOMPARE(names(u"studiocode"_s), vscode);
        QCOMPARE(names(u"vstudio"_s), vscode);
        QCOMPARE(names(u"visualstudio"_s), vscode);
        QVERIFY(names(u"vcode"_s).isEmpty()); // skips Studio
        QVERIFY(names(u"vscodx"_s).isEmpty());
        QVERIFY(names(u"vscode ext:exe"_s).isEmpty());
        QCOMPARE(names(u"pshell"_s), QStringList {u"PowerShell"_s}); // camelCase: p|shell
        QCOMPARE(names(u"powershell"_s), QStringList {u"PowerShell"_s});
        QVERIFY(names(u"pshel1"_s).isEmpty());
        QVERIFY(names(u"ext:exe"_s).isEmpty()); // file syntax: never an app
        QVERIFY(names(uR"(vs\code)"_s).isEmpty());
        QVERIFY(names(QString()).isEmpty());

        // Recently opened apps rank higher.
        const QStringList history {uR"(D:\notes.txt)"_s, apps[4].launchPath()};
        QCOMPARE(names(u"telegram"_s, history),
            (QStringList {u"Telegram"_s, u"Uninstall Telegram"_s, u"Telegram FAQ"_s}));

        QCOMPARE(apps[2].launchPath(), u"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s);
        QCOMPARE(appIdOf(apps[2].launchPath()), apps[2].id);
        QVERIFY(isAppLaunchPath(u"SHELL:appsfolder\\x"_s));
        QVERIFY(appIdOf(uR"(C:\Telegram\Telegram.exe)"_s).isEmpty());
    }

    void appAliases()
    {
        const auto app = [](const QString& name, const QString& target, const QString& arguments = {}) {
            AppInfo a;
            a.name = name;
            a.id = target;
            a.target = target;
            a.arguments = arguments;
            a.prepare(); // with the list Win顺 ships (apps.txt)
            return a;
        };
        const auto hits = [](const AppList& list, const QString& text) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            return searchApps(list, query, matcher, {});
        };
        const auto first = [&](const AppList& list, const QString& text) {
            const std::vector<AppHit> found = hits(list, text);
            return found.empty() ? AppHit {list.size(), -1000, -1, false} : found.front();
        };
        const auto names = [&](const AppList& list, const QString& text) {
            QStringList out;
            for (const AppHit& hit : hits(list, text))
                out.append(list[hit.index].name);
            return out;
        };
        // The name a row shows: the one it was found by.
        const auto shown = [&](const AppList& list, const QString& text) {
            QStringList out;
            for (const AppHit& hit : hits(list, text))
                out.append(hit.otherName >= 0 ? list[hit.index].otherNames[hit.otherName] : list[hit.index].name);
            return out;
        };
        const auto scores = [&](const AppList& list, const QString& text) {
            QStringList out;
            for (const AppHit& hit : hits(list, text)) {
                out.append(
                    u"%1 %2%3"_s.arg(list[hit.index].name).arg(hit.score).arg(hit.exact ? u" exact"_s : QString()));
            }
            return out;
        };
        QVERIFY(!AppAliases::builtIn().entries().empty());

        // WeChat by its program, whatever its shortcut is named: a portable
        // one's is 启动微信. WXWork has "wx" in it, but is another program.
        const AppList wechat {
            app(u"微信输入法"_s, uR"(C:\Program Files\Tencent\WeType\3.0.0.34\wetype_update.exe)"_s),
            app(u"企业微信"_s, uR"(C:\Program Files (x86)\WXWork\WXWork.exe)"_s),
            app(u"启动微信"_s, uR"(D:\微信绿色版\WeChat\WeChat.exe)"_s),
        };
        QCOMPARE(wechat[2].aliases, (std::vector<std::string> {"微信", "WeChat"}));
        QCOMPARE(wechat[1].aliases, std::vector<std::string> {"WeCom"});
        const QStringList all {u"启动微信"_s, u"微信输入法"_s, u"企业微信"_s};
        QCOMPARE(names(wechat, u"wx"_s), all); // the alias's pinyin
        QCOMPARE(names(wechat, u"weixin"_s), all);
        QCOMPARE(names(wechat, u"微信"_s), all);
        QCOMPARE(names(wechat, u"wechat"_s), QStringList {u"启动微信"_s});
        QCOMPARE(shown(wechat, u"wechat"_s), QStringList {u"启动微信"_s}); // an alias is not shown
        QVERIFY(first(wechat, u"微信"_s).exact); // typed in full, as a name would be
        QVERIFY(first(wechat, u"wechat"_s).exact);
        QVERIFY(!first(wechat, u"wx"_s).exact);
        // WeChat 4 is Weixin.exe, its shortcut 微信.
        const AppList weixin {app(u"微信"_s, uR"(C:\Program Files\Tencent\Weixin\Weixin.exe)"_s)};
        QCOMPARE(weixin[0].aliases, std::vector<std::string> {"WeChat"}); // not its own name again
        QVERIFY(first(weixin, u"wechat"_s).exact);

        // Abbreviations, typed in full only.
        const AppList adobe {
            app(u"Adobe Illustrator 2024"_s,
                uR"(C:\Program Files\Adobe\Adobe Illustrator 2024\Support Files\Contents\Windows\Illustrator.exe)"_s),
            app(u"Adobe Photoshop 2024"_s, uR"(C:\Program Files\Adobe\Adobe Photoshop 2024\Photoshop.exe)"_s),
            app(u"步骤记录器"_s, uR"(C:\Windows\system32\psr.exe)"_s),
            app(u"PowerShell 7 (x64)"_s, uR"(C:\Program Files\PowerShell\7\pwsh.exe)"_s, u"-WorkingDirectory ~"_s),
            app(u"Windows PowerShell"_s, uR"(C:\Windows\system32\WindowsPowerShell\v1.0\powershell.exe)"_s),
            app(u"PowerPoint"_s, uR"(C:\Program Files\Microsoft Office\root\Office16\POWERPNT.EXE)"_s),
            app(u"WPS Office"_s, uR"(C:\Users\me\AppData\Local\Kingsoft\WPS Office\ksolaunch.exe)"_s),
        };
        QCOMPARE(names(adobe, u"ps"_s).value(0), u"Adobe Photoshop 2024"_s); // above psr.exe (40)
        QCOMPARE(first(adobe, u"ps"_s).score, 112); // below a name typed in full, above a program's
        QVERIFY(first(adobe, u"ps"_s).exact);
        QCOMPARE(names(adobe, u"PS"_s).value(0), u"Adobe Photoshop 2024"_s);
        QCOMPARE(names(adobe, u"ai"_s).value(0), u"Adobe Illustrator 2024"_s);
        QCOMPARE(names(adobe, u"ppt"_s), QStringList {u"PowerPoint"_s});
        // Above the initials of Windows PowerShell (45).
        QCOMPARE(names(adobe, u"wps"_s), (QStringList {u"WPS Office"_s, u"Windows PowerShell"_s}));
        // "s" and "a" find Photoshop and Illustrator by their names alone (Adobe,
        // Photoshop), exactly as they would without the list.
        AppList plain = adobe;
        const AppAliases none;
        for (AppInfo& a : plain)
            a.prepare(none);
        QVERIFY(plain[1].abbreviations.empty());
        for (const QString& text : {u"s"_s, u"a"_s, u"p"_s, u"i"_s, u"pss"_s, u"ppts"_s})
            QCOMPARE(scores(adobe, text), scores(plain, text));
        QVERIFY(first(plain, u"ps"_s).score < 112);
        // A shell started with options is still that shell.
        QVERIFY(!adobe[3].hosted);
        QCOMPARE(adobe[3].program, std::string("pwsh"));
        QCOMPARE(names(adobe, u"pwsh"_s), QStringList {u"PowerShell 7 (x64)"_s});
        QCOMPARE(first(adobe, u"pwsh"_s).score, 109);

        // A shortcut that runs something else through a host program is not
        // that program: "cmd" is the Command Prompt, not a VS developer prompt.
        const AppList hosts {
            app(u"Developer Command Prompt for VS 2022"_s, uR"(C:\Windows\system32\cmd.exe)"_s,
                uR"(/k "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat")"_s),
            app(u"x64 Native Tools Command Prompt for VS 2022"_s, uR"(C:\Windows\system32\cmd.exe)"_s,
                uR"(/k "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")"_s),
            app(u"命令提示符"_s, uR"(C:\Windows\system32\cmd.exe)"_s),
            app(u"Hyper-V 管理器"_s, uR"(C:\Windows\System32\mmc.exe)"_s, uR"("C:\Windows\System32\virtmgmt.msc")"_s),
            app(u"Windows 工具"_s, uR"(C:\Windows\explorer.exe)"_s,
                u"shell:::{D20EA4E1-3957-11d2-A40B-0C5020524153}"_s),
            app(u"资源管理器"_s, uR"(C:\Windows\explorer.exe)"_s),
            app(u"Python 3.13 (64-bit)"_s, uR"(C:\Python313\python.exe)"_s),
            app(u"Python 3.13 Module Docs (64-bit)"_s, uR"(C:\Python313\python.exe)"_s, u"-m pydoc -b"_s),
            app(u"IDLE (Python 3.13 64-bit)"_s, uR"(C:\Python313\pythonw.exe)"_s,
                uR"("C:\Python313\Lib\idlelib\idle.pyw")"_s),
        };
        for (const std::size_t i : std::initializer_list<std::size_t> {0, 1, 3, 4, 7, 8}) {
            QVERIFY2(hosts[i].hosted, qPrintable(hosts[i].name));
            QVERIFY(hosts[i].program.empty());
        }
        for (const std::size_t i : std::initializer_list<std::size_t> {2, 5, 6})
            QVERIFY2(!hosts[i].hosted, qPrintable(hosts[i].name));
        QCOMPARE(names(hosts, u"cmd"_s), QStringList {u"命令提示符"_s});
        QCOMPARE(first(hosts, u"cmd"_s).score, 109);
        QCOMPARE(names(hosts, u"developer command"_s).value(0), u"Developer Command Prompt for VS 2022"_s); // its name
        QCOMPARE(names(hosts, u"native tools"_s).value(0), u"x64 Native Tools Command Prompt for VS 2022"_s);
        QVERIFY(names(hosts, u"mmc"_s).isEmpty());
        QCOMPARE(names(hosts, u"explorer"_s), QStringList {u"资源管理器"_s}); // no arguments: the program itself
        QCOMPARE(names(hosts, u"python"_s),
            (QStringList {
                u"Python 3.13 (64-bit)"_s, u"Python 3.13 Module Docs (64-bit)"_s, u"IDLE (Python 3.13 64-bit)"_s}));

        // Shell objects, no program file: by their ids, and the commands that open them.
        const auto shellObject = [](const QString& name, const QString& id) {
            AppInfo a;
            a.name = name;
            a.id = id;
            a.prepare();
            return a;
        };
        const AppList shell {shellObject(u"文件资源管理器"_s, u"Microsoft.Windows.Explorer"_s),
            shellObject(u"控制面板"_s, u"Microsoft.Windows.ControlPanel"_s)};
        QCOMPARE(shell[0].otherNames, QStringList {u"File Explorer"_s});
        QCOMPARE(names(shell, u"explorer"_s), QStringList {u"文件资源管理器"_s});
        QVERIFY(first(shell, u"explorer"_s).exact);
        QCOMPARE(shown(shell, u"control panel"_s), QStringList {u"Control Panel"_s});
        QCOMPARE(names(shell, u"control"_s).value(0), u"控制面板"_s);

        // Chromium browsers mostly run a chrome.exe: Chrome's is the one in Google\Chrome.
        const AppList browsers {
            app(u"Helium"_s, uR"(C:\Program Files\imput\Helium\Application\chrome.exe)"_s),
            app(u"Google Chrome"_s, uR"(C:\Users\me\AppData\Local\Google\Chrome\Application\chrome.exe)"_s),
        };
        QVERIFY(browsers[0].aliases.empty());
        QCOMPARE(browsers[1].aliases, (std::vector<std::string> {"Chrome", "谷歌浏览器", "谷歌"}));
        QCOMPARE(names(browsers, u"chrome"_s), (QStringList {u"Google Chrome"_s, u"Helium"_s}));
        QCOMPARE(names(browsers, u"谷歌"_s), QStringList {u"Google Chrome"_s});

        // The list's format.
        const AppAliases table = AppAliases::parse(u"# a comment\n"
                                                   u"[图像]\n"
                                                   u"program = foo.exe\n"
                                                   u"short = ps; P; x1；中文\n"
                                                   u"\n"
                                                   u"[东西]\n"
                                                   u"package = Vendor.Thing_abc123\n"
                                                   u"names = Thing | 东西\n"
                                                   u"[My Tool]\n"
                                                   u"title = My Tool\n"
                                                   u"aliases = 工具箱\n"
                                                   u"[Runner]\n"
                                                   u"program = Vendor/Bin/run\n"
                                                   u"aliases = runner\n"
                                                   u"[nothing to find it by]\n"
                                                   u"aliases = lost\n"
                                                   u"[again]\n"
                                                   u"PROGRAM = FOO\n"
                                                   u"aliases = second\n"_s);
        QCOMPARE(table.entries().size(), std::size_t {5}); // not the block nothing finds
        const AppAliases::Entry* foo = table.find({}, u"Foo"_s, {}, {});
        QVERIFY(foo);
        QCOMPARE(foo->label, u"图像"_s); // the first block to claim it
        QVERIFY(!table.find({}, {}, {}, u"lost"_s));
        const auto prepared = [&](const QString& name, const QString& target, const QString& id = {}) {
            AppInfo a;
            a.name = name;
            a.id = id.isEmpty() ? target : id;
            a.target = target;
            a.kind = id.isEmpty() ? AppKind::Desktop : AppKind::Store;
            a.prepare(table);
            return a;
        };
        const AppList mine {
            prepared(u"图像"_s, uR"(C:\Foo\foo.exe)"_s),
            prepared(u"Vendor Thing"_s, uR"(C:\Program Files\WindowsApps\Vendor.Thing_1.0.0.0_x64__abc123)"_s,
                u"Vendor.Thing_abc123!App"_s),
            prepared(u"My Tool"_s, uR"(C:\Tools\tool.exe)"_s),
            prepared(u"Run"_s, uR"(C:\Program Files\Vendor\Bin\run.exe)"_s),
            prepared(u"Other Run"_s, uR"(C:\Other\run.exe)"_s),
        };
        QCOMPARE(mine[0].abbreviations, (std::vector<std::string> {"ps", "x1"})); // two or more letters and digits
        QVERIFY(mine[0].aliases.empty()); // not [again]'s
        QCOMPARE(mine[1].otherNames, (QStringList {u"Thing"_s, u"东西"_s})); // by its package
        QCOMPARE(mine[2].aliases, std::vector<std::string> {"工具箱"}); // by its name
        QCOMPARE(mine[3].aliases, std::vector<std::string> {"runner"}); // by the end of its path
        QVERIFY(mine[4].aliases.empty()); // another run.exe
        QCOMPARE(names(mine, u"ps"_s), QStringList {u"图像"_s});
        QCOMPARE(names(mine, u"PS"_s), QStringList {u"图像"_s});
        QCOMPARE(first(mine, u"x1"_s).score, 112);
        QVERIFY(names(mine, u"p"_s).isEmpty()); // only typed in full
        QVERIFY(names(mine, u"s"_s).isEmpty());
        QVERIFY(names(mine, u"pss"_s).isEmpty());
        QCOMPARE(shown(mine, u"东西"_s), QStringList {u"东西"_s}); // a name of its own: shown when found by it
        QCOMPARE(shown(mine, u"工具箱"_s), QStringList {u"My Tool"_s}); // an alias: never shown
        QCOMPARE(shown(mine, u"gjx"_s), QStringList {u"My Tool"_s});
        QVERIFY(first(mine, u"工具箱"_s).exact);
    }

    void historyCounts()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(u"history.txt"_s);
        {
            // Kept before counts were: each opened once, at an unknown time.
            QFile old(file);
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("C:\\a.txt\nD:\\b.txt\n");
        }
        History history(file, 3);
        history.load();
        QCOMPARE(history.items(), (QStringList {uR"(C:\a.txt)"_s, uR"(D:\b.txt)"_s}));
        QCOMPARE(history.use(uR"(c:\A.TXT)"_s).count, 1); // any case
        QCOMPARE(history.use(uR"(c:\A.TXT)"_s).last, 0);

        history.record(uR"(D:\b.txt)"_s, 1000);
        history.record(uR"(D:\b.txt)"_s, 2000);
        history.record(uR"(E:\c.txt)"_s, 3000);
        history.record(uR"(F:\d.txt)"_s, 4000); // over capacity: the oldest goes
        QCOMPARE(history.items(), (QStringList {uR"(F:\d.txt)"_s, uR"(E:\c.txt)"_s, uR"(D:\b.txt)"_s}));
        QCOMPARE(history.use(uR"(D:\b.txt)"_s).count, 3);
        QCOMPARE(history.use(uR"(C:\a.txt)"_s).count, 0);

        History again(file, 3);
        again.load();
        QCOMPARE(again.items(), history.items());
        QCOMPARE(again.use(uR"(D:\b.txt)"_s).count, 3);
        QCOMPARE(again.use(uR"(D:\b.txt)"_s).last, 2000);
        again.remove(uR"(d:\B.txt)"_s);
        QCOMPARE(again.use(uR"(D:\b.txt)"_s).count, 0);
    }

    void historyFormats()
    {
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        const QString file = dir.filePath(u"history.txt"_s);
        const auto write = [&](const QByteArray& text) {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text);
        };
        // Bare paths, a line of 3 fields, a line with counts of the new kind.
        write("C:\\bare.txt\n"
              "D:\\three.txt\t4\t0\n"
              "E:\\new.txt\t2\t" + QByteArray::number(now - day) + "\t1.5\tbg\t1.25\t" + QByteArray::number(now - day)
              + "\tbaogao\t0.5\t" + QByteArray::number(now - 2 * day) + "\n");
        History history(file);
        history.load();
        QCOMPARE(history.items(), (QStringList {u"C:\\bare.txt"_s, u"D:\\three.txt"_s, u"E:\\new.txt"_s}));
        QCOMPARE(history.use(u"C:\\bare.txt"_s).count, 1);
        QCOMPARE(history.use(u"D:\\three.txt"_s).count, 4);
        QCOMPARE(history.use(u"E:\\new.txt"_s).count, 2);
        QCOMPARE(history.use(u"E:\\new.txt"_s).last, now - day);
        // Only E has a text typed: "bg" lists it first, the others as they were.
        QCOMPARE(history.ranked(u"bg"_s, now).first(), u"E:\\new.txt"_s);
        // A path with no time: weight as many as it was opened, half-life old.
        // D (4 opens, a half-life old: 2) outranks the bare one (1, a half-life old: 0.5).
        QCOMPARE(history.ranked(u"zzz"_s, now), (QStringList {u"D:\\three.txt"_s, u"E:\\new.txt"_s, u"C:\\bare.txt"_s}));
    }

    void historyRoundTrip()
    {
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        const QString file = dir.filePath(u"history.txt"_s);
        History history(file);
        history.record(u"C:\\a.txt"_s, u"Report"_s, now - 2 * day);
        history.record(u"C:\\a.txt"_s, u"report "_s, now - day);
        history.record(u"D:\\b.txt"_s, u"bg"_s, now);
        history.record(u"D:\\b.txt"_s, QString(), now); // not typed: no text kept
        History again(file);
        again.load();
        QCOMPARE(again.items(), history.items());
        QCOMPARE(again.use(u"C:\\a.txt"_s).count, 2);
        QCOMPARE(again.use(u"D:\\b.txt"_s).count, 2);
        for (const QString& typed : {u"report"_s, u"bg"_s, u"nothing"_s, u"rep"_s, u"reports"_s})
            QCOMPARE(again.ranked(typed, now + day), history.ranked(typed, now + day));
        QCOMPARE(again.ranked(u"report"_s, now).first(), u"C:\\a.txt"_s); // the text came back
        QCOMPARE(again.ranked(u"bg"_s, now).first(), u"D:\\b.txt"_s);

        // The first three fields are what older builds read.
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> fields = f.readLine().trimmed().split('\t');
        QVERIFY(fields.size() > 3);
        QCOMPARE(fields[1].toInt(), 2);
    }

    void historyNormalize()
    {
        QCOMPARE(History::normalize(u"  Report \t 2025\r\n"_s), u"report 2025"_s);
        QCOMPARE(History::normalize(u"report size:>1mb"_s), u"report"_s); // a filter is not the text
        QCOMPARE(History::normalize(u"dm:7d REPORT ext:pdf,docx"_s), u"report"_s);
        QCOMPARE(History::normalize(u"report abc/"_s), u"report"_s); // nor the folder to look in
        QCOMPARE(History::normalize(u"report"_s), History::normalize(u"report size:>1mb"_s));
        QCOMPARE(History::normalize(u"BG"_s), u"bg"_s); // pinyin initials as typed
        QCOMPARE(History::normalize(u"size:>1mb"_s), QString()); // nothing left
        QCOMPARE(History::normalize(QString(100, u'a')).size(), 64);
    }

    void historyRanking()
    {
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        History history(dir.filePath(u"history.txt"_s));
        history.record(u"E:\\other.txt"_s, u"zzz"_s, now - 4 * day);
        history.record(u"D:\\longer.txt"_s, u"report final"_s, now - 3 * day); // the text starts with "rep"
        history.record(u"C:\\shorter.txt"_s, u"re"_s, now - 2 * day); // "rep" starts with it
        history.record(u"B:\\exact.txt"_s, u"rep"_s, now - 2 * day);
        history.record(u"A:\\plain.txt"_s, QString(), now - day); // never found by typing
        history.record(u"F:\\latest.txt"_s, QString(), now);
        QCOMPARE(history.ranked(u"rep"_s, now),
            (QStringList {u"B:\\exact.txt"_s, u"D:\\longer.txt"_s, u"C:\\shorter.txt"_s, u"F:\\latest.txt"_s,
                u"A:\\plain.txt"_s, u"E:\\other.txt"_s}));
        // The rest by the path's own weight: the more recent opening first.
        // An empty text (or only filters): items() as they are.
        QCOMPARE(history.ranked(QString(), now), history.items());
        QCOMPARE(history.ranked(u"  size:>1mb "_s, now), history.items());
        QCOMPARE(history.items().first(), u"F:\\latest.txt"_s);
        // Case and filters do not tell texts apart.
        QCOMPARE(history.ranked(u" REP dm:7d"_s, now), history.ranked(u"rep"_s, now));
    }

    void historyFrecency()
    {
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        {
            // Often over a month, and once yesterday, for the same text.
            History history(dir.filePath(u"a.txt"_s));
            for (int i = 0; i < 10; ++i)
                history.record(u"C:\\often.txt"_s, u"x"_s, now - (30 - 3 * i) * day);
            history.record(u"D:\\once.txt"_s, u"x"_s, now - day);
            history.record(u"D:\\once.txt"_s, u"y"_s, now - day); // and for its own text
            QCOMPARE(history.items().first(), u"D:\\once.txt"_s); // newer
            QCOMPARE(history.ranked(u"x"_s, now).first(), u"C:\\often.txt"_s);
            QCOMPARE(history.ranked(u"y"_s, now).first(), u"D:\\once.txt"_s);
        }
        {
            // Decay: 20 opens long ago, against 4 lately.
            History history(dir.filePath(u"b.txt"_s));
            for (int i = 0; i < 20; ++i)
                history.record(u"C:\\stale.txt"_s, QString(), now - 200 * day);
            for (int i = 0; i < 4; ++i)
                history.record(u"D:\\lately.txt"_s, QString(), now - day);
            QCOMPARE(history.ranked(u"q"_s, now - 200 * day).first(), u"C:\\stale.txt"_s); // then, it was ahead
            QCOMPARE(history.ranked(u"q"_s, now).first(), u"D:\\lately.txt"_s); // now it has decayed
            // Opened again, the weight is the decayed one plus one (not the count).
            history.record(u"C:\\stale.txt"_s, QString(), now);
            QCOMPARE(history.ranked(u"q"_s, now).first(), u"D:\\lately.txt"_s);
        }
    }

    void historyTextCap()
    {
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        const QString file = dir.filePath(u"history.txt"_s);
        History history(file);
        // t1 is typed 3 times; the others once each, older ones first.
        for (int i = 0; i < 3; ++i)
            history.record(u"C:\\a.txt"_s, u"t1"_s, now - 100 * day + i);
        for (int i = 2; i <= 8; ++i)
            history.record(u"C:\\a.txt"_s, u"t%1"_s.arg(i), now - (20 - i) * day);
        // A ninth: the weakest (as of now) goes, which is t1 (long ago), not the new one.
        history.record(u"C:\\a.txt"_s, u"t9"_s, now);
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> fields = f.readAll().trimmed().split('\t');
        QCOMPARE((fields.size() - 4) / 3, 8); // 8 texts after path, count, last, weight
        QVERIFY(!fields.contains("t1"));
        QVERIFY(fields.contains("t9"));
        QVERIFY(fields.contains("t8"));
    }

    void historyForget()
    {
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        const QString file = dir.filePath(u"history.txt"_s);
        History history(file);
        history.record(u"C:\\a.txt"_s, u"abc"_s, now);
        history.record(u"D:\\b.txt"_s, u"abc"_s, now - day);
        history.remove(u"c:\\A.TXT"_s);
        QCOMPARE(history.items(), QStringList {u"D:\\b.txt"_s});
        // Opened again, nothing of the old texts is left.
        history.record(u"C:\\a.txt"_s, u"other"_s, now);
        QCOMPARE(history.ranked(u"abc"_s, now).first(), u"D:\\b.txt"_s);
        history.remove(u"C:\\a.txt"_s);
        history.clear();
        QVERIFY(history.items().isEmpty());
        History again(file);
        again.load();
        QVERIFY(again.items().isEmpty());
        history.record(u"C:\\a.txt"_s, QString(), now);
        history.clear();
        history.record(u"C:\\a.txt"_s, QString(), now);
        QCOMPARE(history.ranked(u"abc"_s, now), QStringList {u"C:\\a.txt"_s});
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("abc"));
    }

    void frequentAppRanking()
    {
        const auto app = [](const QString& name, const QString& id, const QString& target,
                             AppKind kind = AppKind::Desktop) {
            AppInfo a;
            a.name = name;
            a.id = id;
            a.target = target;
            a.kind = kind;
            a.prepare();
            return a;
        };
        const AppList apps {
            app(u"Code"_s, u"Microsoft.VisualStudioCode"_s, uR"(C:\VS Code\Code.exe)"_s),
            app(u"Word"_s, u"Microsoft.Office.WINWORD.EXE.15"_s, uR"(C:\Office\WINWORD.EXE)"_s),
            app(u"计算器"_s, u"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s, uR"(C:\WindowsApps\Calc)"_s,
                AppKind::Store),
            app(u"Uninstall Code"_s, uR"(C:\VS Code\unins000.exe)"_s, uR"(C:\VS Code\unins000.exe)"_s),
            app(u"Win顺"_s, uR"(C:\WinShun\WinShun.exe)"_s, uR"(C:\WinShun\WinShun.exe)"_s),
            app(u"Notes"_s, uR"(C:\Notes\Notes.exe)"_s, uR"(C:\Notes\Notes.exe)"_s),
        };
        constexpr qint64 day = 86'400'000;
        const qint64 now = 1000 * day;
        QTemporaryDir dir;
        History history(dir.filePath(u"history.txt"_s));
        const auto names = [&](const std::vector<AppUse>& windows, std::size_t limit = 6) {
            QStringList out;
            for (const std::size_t i : frequentApps(apps, history, windows, now, limit))
                out.append(apps[i].name);
            return out;
        };
        QVERIFY(names({}).isEmpty());

        // Opened from Win顺: often beats lately, within the same week.
        history.record(apps[0].launchPath(), now - 2 * day);
        history.record(apps[0].launchPath(), now - day);
        history.record(apps[2].launchPath(), now - 1000);
        history.record(uR"(D:\report.docx)"_s, now); // not an app
        history.record(apps[3].launchPath(), now); // an uninstaller
        history.record(apps[4].launchPath(), now); // Win顺 itself
        QCOMPARE(names({}), (QStringList {u"Code"_s, u"计算器"_s}));

        // Then what Windows saw started lately: by id, or by the program an
        // AppUserModelID app starts; not what it saw long ago, nor twice.
        const std::vector<AppUse> windows {
            {uR"(C:\Office\WINWORD.EXE)"_s, 1, now - 3 * day}, // Word's id is an AppUserModelID
            {uR"(C:\Notes\Notes.exe)"_s, 5, now - 200 * day}, // long ago
            {u"Microsoft.VisualStudioCode"_s, 9, now - day}, // already in
            {uR"(C:\Unknown\x.exe)"_s, 9, now - day}, // not an app
        };
        QCOMPARE(names(windows), (QStringList {u"Code"_s, u"计算器"_s, u"Word"_s}));
        QCOMPARE(names(windows, 1), QStringList {u"Code"_s});
    }

    void settingsIndexParsing()
    {
        // As Windows ships them: a newer file (pages as <Node>), a Control
        // Panel task, and an older file's entry (<PageID>, <PolicyIds>).
        const QByteArray xml = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><PCSettings>"
                               "<SearchableContent IncludeWithFeature=\"X\">"
                               "<Filename>AAA_SettingsPageNetworkEthernet</Filename>"
                               "<ApplicationInformation><FontFamily>Segoe Fluent Icons</FontFamily><Glyph>x</Glyph>"
                               "</ApplicationInformation>"
                               "<SettingIdentity><SettingPaths>"
                               "<Path><Node Type=\"Page\">SettingsPageNetworkEthernet</Node><Node Type=\"Group\">G</Node></Path>"
                               "<Path><Node Type=\"Page\">SettingsPageOther</Node></Path>"
                               "</SettingPaths></SettingIdentity>"
                               "<SettingInformation><Description>@{windows?ms-resource://a/b}</Description>"
                               "<HighKeywords>@{windows?ms-resource://a/c}</HighKeywords></SettingInformation>"
                               "</SearchableContent>"
                               "<SearchableContent><Filename>Classic_{9EF86966}</Filename>"
                               "<ApplicationInformation><DeepLink>Microsoft.WindowsFirewall\\PageConfigureApps</DeepLink>"
                               "<Icon>%SystemRoot%\\System32\\netcenter.dll,-1</Icon></ApplicationInformation>"
                               "<SettingIdentity><PageID>{8E908FC9}</PageID><HostID>{12B1697E}</HostID>"
                               "<Condition>shcond://v1#IsServer;1</Condition></SettingIdentity>"
                               "<SettingInformation><Description>@shell32.dll,-24389</Description>"
                               "<Keywords>@@shell32.dll,-25174@shell32.dll,-25173</Keywords></SettingInformation>"
                               "</SearchableContent>"
                               "<SearchableContent><Filename>AAA_SettingsPageAudio</Filename>"
                               "<SettingIdentity><SettingPaths><Path><PageID>SettingsPageAudio</PageID>"
                               "<PolicyIds>apps-volume;sound;sound-devices</PolicyIds></Path></SettingPaths></SettingIdentity>"
                               "<SettingInformation><Description>Sound</Description></SettingInformation>"
                               "</SearchableContent></PCSettings>";
        const std::vector<SettingsIndexEntry> entries = parseSettingsIndex(xml);
        QCOMPARE(entries.size(), std::size_t {3});
        QCOMPARE(entries[0].key, u"AAA_SettingsPageNetworkEthernet"_s);
        QCOMPARE(entries[0].page, u"SettingsPageNetworkEthernet"_s); // the first path's page
        QVERIFY(entries[0].deepLink.isEmpty());
        QCOMPARE(entries[0].description, u"@{windows?ms-resource://a/b}"_s);
        QCOMPARE(entries[0].keywords, QStringList {u"@{windows?ms-resource://a/c}"_s});
        QCOMPARE(entries[1].deepLink, uR"(Microsoft.WindowsFirewall\PageConfigureApps)"_s);
        QCOMPARE(entries[1].icon, uR"(%SystemRoot%\System32\netcenter.dll,-1)"_s);
        QCOMPARE(entries[1].host, u"{12B1697E}"_s);
        QCOMPARE(entries[1].condition, u"shcond://v1#IsServer;1"_s);
        QCOMPARE(entries[1].keywords, (QStringList {u"@shell32.dll,-25174"_s, u"@shell32.dll,-25173"_s}));
        QCOMPARE(entries[2].page, u"SettingsPageAudio"_s);
        QCOMPARE(entries[2].policyIds, u"apps-volume;sound;sound-devices"_s);
        QCOMPARE(entries[2].description, u"Sound"_s);

        // Which of a page's ms-settings: names opens the page itself.
        QCOMPARE(choosePageUri(u"SettingsPageAudio"_s, u"apps-volume;audio-outputdevices;sound;sound-devices"_s), u"sound"_s);
        QCOMPARE(choosePageUri(u"SettingsPageInstalledApps"_s, u"appsfeatures;appsfeatures-app;installed-apps"_s),
            u"installed-apps"_s); // named by the page
        QCOMPARE(choosePageUri(u"SettingsPageNetworkAirplaneMode"_s, u"network-airplanemode;proximity"_s),
            u"network-airplanemode"_s);
        QCOMPARE(choosePageUri(u"SettingsPageNetworkManageAdapterOptions"_s,
                     u"network-advancedsettings;network-advancedsharing"_s),
            u"network-advancedsettings"_s); // nothing to go by: the first
        QCOMPARE(choosePageUri(u"SettingsPageX"_s, u"display"_s), u"display"_s);
        QVERIFY(choosePageUri(u"SettingsPageX"_s, QString()).isEmpty());
    }

    void placeCommands()
    {
        const QString windir = QDir::toNativeSeparators(qEnvironmentVariable("WINDIR"));
        QCOMPARE(commandForDeepLink(u"Microsoft.DeviceManager"_s),
            uR"(%windir%\system32\control.exe /name Microsoft.DeviceManager)"_s);
        QCOMPARE(commandForDeepLink(uR"(Microsoft.WindowsFirewall\PageConfigureApps)"_s),
            uR"(%windir%\system32\control.exe /name Microsoft.WindowsFirewall /page PageConfigureApps)"_s);
        QCOMPARE(commandForDeepLink(uR"(%windir%\system32\dccw.exe)"_s), uR"(%windir%\system32\dccw.exe)"_s);
        QCOMPARE(commandForDeepLink(u"shell:::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"_s),
            u"shell:::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"_s);
        QVERIFY(commandForDeepLink(u"AccountProtection"_s).isEmpty()); // Windows Security's own
        QVERIFY(commandForDeepLink(QString()).isEmpty());

        using Split = std::pair<QString, QString>;
        QCOMPARE(splitCommand(uR"(%windir%\system32\control.exe /name Microsoft.DeviceManager)"_s),
            (Split {windir + uR"(\system32\control.exe)"_s, u"/name Microsoft.DeviceManager"_s}));
        QCOMPARE(splitCommand(u"ms-settings:display"_s), (Split {u"ms-settings:display"_s, QString()}));
        QCOMPARE(splitCommand(uR"(shell:::{26EE0668}\3\::{7007ACC7})"_s), (Split {uR"(shell:::{26EE0668}\3\::{7007ACC7})"_s, QString()}));
        QCOMPARE(splitCommand(u"windowsdefender://threat"_s), (Split {u"windowsdefender://threat"_s, QString()}));
        QCOMPARE(splitCommand(u"\"C:\\Program Files\\x.exe\" -a b"_s), (Split {uR"(C:\Program Files\x.exe)"_s, u"-a b"_s}));
        QCOMPARE(splitCommand(u"mmsys.cpl"_s), (Split {u"mmsys.cpl"_s, QString()}));
        QCOMPARE(splitCommand(u"%windir%"_s), (Split {windir, QString()}));

        QCOMPARE(describeCommand(uR"(%windir%\system32\mmc.exe %windir%\system32\diskmgmt.msc)"_s), u"diskmgmt.msc"_s);
        QCOMPARE(describeCommand(uR"(%windir%\system32\control.exe mmsys.cpl)"_s), u"mmsys.cpl"_s);
        QCOMPARE(describeCommand(u"mmsys.cpl"_s), u"mmsys.cpl"_s);
        QCOMPARE(describeCommand(u"shell:startup"_s), u"shell:startup"_s);
        QCOMPARE(describeCommand(u"%windir%"_s), windir); // a folder: where it is
        QCOMPARE(describeCommand(uR"(%windir%\system32\rundll32.exe sysdm.cpl,EditEnvironmentVariables)"_s),
            u"rundll32.exe sysdm.cpl,EditEnvironmentVariables"_s);

        QCOMPARE(placePath(u"AAA_SettingsPageAudio"_s), u"winshun-place:AAA_SettingsPageAudio"_s);
        QVERIFY(isPlacePath(placePath(u"x"_s)));
        QCOMPARE(placeKeyOf(placePath(u"winshun/disk-management"_s)), u"winshun/disk-management"_s);
        QVERIFY(placeKeyOf(uR"(C:\x.txt)"_s).isEmpty());
        QVERIFY(!isPlacePath(u"shell:AppsFolder\\x"_s));
    }

    void placeExtras()
    {
        const auto place = [](const QString& name, const QString& command, bool page = false) {
            PlaceInfo p;
            p.name = name;
            p.key = name;
            p.command = command;
            p.kind = AppKind::Setting;
            p.page = page;
            return p;
        };
        PlaceList places {
            place(u"视差背景"_s, u"ms-settings:personalization-background"_s),
            place(u"背景图像设置"_s, u"ms-settings:personalization-background"_s, true),
            place(u"检查防火墙状态"_s, u"FW"_s),
            place(u"Windows Defender 防火墙"_s, u"fw"_s),
        };
        const QString text = u"# comment\n"
                             u"[ms-settings:personalization-background]\n"
                             u"keywords = 壁纸; 桌面背景；换壁纸 ;\n"
                             u"\n"
                             u"[fw]\n"
                             u"keywords = 防火墙\n"
                             u"[ms-settings:nowhere]\n"
                             u"keywords = x\n"
                             u"[disk-management]\n"
                             u"name = 磁盘管理 | Disk Management\n"
                             u"open = diskmgmt.msc\n"
                             u"keywords = 分区; diskmgmt\n"
                             u"[virus]\n"
                             u"name = 病毒和威胁防护 | Virus & threat protection\n"
                             u"open = windowsdefender://threat\n"
                             u"kind = security\n"
                             u"icon = app:Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI\n"_s;
        const QStringList unmatched = applyPlaceExtras(places, text, true);
        QCOMPARE(unmatched, QStringList {u"ms-settings:nowhere"_s});
        QVERIFY(places[0].ownKeywords.isEmpty()); // a task on the page: the page itself gets them
        QCOMPARE(places[1].ownKeywords, (QStringList {u"壁纸"_s, u"桌面背景"_s, u"换壁纸"_s}));
        QCOMPARE(places[2].ownKeywords, QStringList {u"防火墙"_s}); // no page among them: each one, any case
        QCOMPARE(places[3].ownKeywords, QStringList {u"防火墙"_s});
        QCOMPARE(places.size(), std::size_t {6});
        QCOMPARE(places[4].name, u"磁盘管理"_s);
        QCOMPARE(places[4].key, u"winshun/disk-management"_s);
        QCOMPARE(places[4].command, u"diskmgmt.msc"_s);
        QVERIFY(places[4].kind == AppKind::Tool);
        QCOMPARE(places[4].ownKeywords, (QStringList {u"分区"_s, u"diskmgmt"_s}));
        QVERIFY(places[5].kind == AppKind::Security);
        QCOMPARE(places[5].icon, u"app:Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI"_s);

        PlaceList english;
        applyPlaceExtras(english, text, false);
        QCOMPARE(english[0].name, u"Disk Management"_s);
    }

    void placeSearch()
    {
        const auto place = [](const QString& name, const QString& command, const QStringList& keywords,
                               const QStringList& own = {}) {
            PlaceInfo p;
            p.name = name;
            p.key = u"k/"_s + name;
            p.command = command;
            p.keywords = keywords;
            p.ownKeywords = own;
            p.prepare();
            return p;
        };
        const PlaceList places {
            place(u"查看网络连接"_s, u"shell:::{7007ACC7}"_s, {u"适配器"_s, u"卡"_s, u"网络"_s, u"connections"_s},
                {u"网卡"_s, u"ncpa.cpl"_s, u"ip地址"_s}),
            place(u"设备管理器"_s, u"control.exe /name Microsoft.DeviceManager"_s, {u"适配器"_s, u"驱动程序"_s}),
            place(u"管理网络适配器设置"_s, u"ms-settings:network-advancedsettings"_s, {u"网络"_s}),
            place(u"网络重置"_s, u"MS-SETTINGS:network-advancedsettings"_s, {u"重置"_s}),
            place(u"节电模式设置"_s, u"ms-settings:batterysaver"_s, {u"battery saver settings"_s}),
            place(u"更改显示器的分辨率"_s, u"ms-settings:display"_s, {u"显示器"_s}),
            place(u"保护历史记录"_s, u"windowsdefender://history"_s, {}, {u"隔离区"_s}),
            place(u"凭据管理器"_s, u"control.exe /name Microsoft.CredentialManager"_s, {}),
            place(u"当我看向别处时，自动调暗屏幕"_s, u"ms-settings:batterysaver-dim"_s, {}),
        };
        const auto names = [&](const QString& text, const QStringList& history = {}) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const PlaceHit& hit : searchPlaces(places, query, matcher, history))
                out.append(places[hit.index].name);
            return out;
        };

        QCOMPARE(names(u"网卡"_s).value(0), u"查看网络连接"_s); // an own keyword
        QCOMPARE(names(u"NCPA.cpl"_s), QStringList {u"查看网络连接"_s});
        // Pinyin of a keyword, above a name the initials merely touch (我看)...
        QCOMPARE(names(u"wk"_s), (QStringList {u"查看网络连接"_s, u"当我看向别处时，自动调暗屏幕"_s}));
        // ... but below one they fill half of: 管理器, not 隔离区.
        QCOMPARE(names(u"glq"_s), (QStringList {u"设备管理器"_s, u"凭据管理器"_s, u"保护历史记录"_s}));
        QCOMPARE(names(u"geliqu"_s).value(0), u"保护历史记录"_s); // spelled out, it is meant
        QCOMPARE(names(u"glq"_s, {places[6].path()}).value(0), u"保护历史记录"_s); // opened recently
        QCOMPARE(names(u"ip 地址"_s), QStringList {u"查看网络连接"_s}); // each word in a keyword
        // A whole keyword ranks above a name that only contains the word.
        QCOMPARE(names(u"适配器"_s), (QStringList {u"设备管理器"_s, u"查看网络连接"_s, u"管理网络适配器设置"_s}));
        QCOMPARE(names(u"saver"_s), QStringList {u"节电模式设置"_s}); // a word inside a keyword
        QVERIFY(names(u"aver"_s).isEmpty()); // ... not the middle of one
        QVERIFY(names(u"卡"_s).isEmpty()); // one character: the name must have it
        QCOMPARE(names(u"驱动"_s), QStringList {u"设备管理器"_s}); // two Chinese characters: anywhere in a keyword
        QCOMPARE(names(u"网络 适配器"_s).value(0), u"查看网络连接"_s); // one word in the name, one in a keyword
        // Both tasks open the same page: one row, the better one.
        QCOMPARE(names(u"重置"_s), QStringList {u"网络重置"_s});
        QCOMPARE(names(u"网络"_s).count(u"管理网络适配器设置"_s) + names(u"网络"_s).count(u"网络重置"_s), 1);
        QVERIFY(!names(u"网卡 !查看"_s).contains(u"查看网络连接"_s));
        QVERIFY(names(u"ext:txt"_s).isEmpty()); // file syntax
        QVERIFY(names(uR"(网络\适配器)"_s).isEmpty());
        QVERIFY(names(QString()).isEmpty());
        QCOMPARE(names(u"分辨率"_s), QStringList {u"更改显示器的分辨率"_s});

        // Recently opened places rank higher.
        QCOMPARE(names(u"适配器"_s, {uR"(D:\a.txt)"_s, places[0].path()}).value(0), u"查看网络连接"_s);
    }

    void placeKeywordWordStart()
    {
        // A word counts in a keyword where a word of it starts: "word" is not
        // in "password". (For 管理网络密码 Windows lists "pass-word" and "word"
        // itself as well, so there "word" finds it all the same.)
        const auto found = [](const QStringList& keywords, const QString& text) {
            PlaceInfo p;
            p.name = u"管理网络密码"_s;
            p.key = u"k/credentials"_s;
            p.command = u"control.exe /name Microsoft.CredentialManager"_s;
            p.keywords = keywords;
            p.prepare();
            const PlaceList places {p};
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            return !searchPlaces(places, query, matcher, {}).empty();
        };
        const QStringList password {u"password"_s, u"passwords"_s};
        QVERIFY(!found(password, u"word"_s));
        QVERIFY(!found(password, u"words"_s));
        QVERIFY(found(password, u"pass"_s));
        QVERIFY(found(password, u"password"_s));
        QVERIFY(found({u"pass-word"_s}, u"word"_s));
    }

    void switchExtras()
    {
        const QString text = u"[switch-test]\n"
                             u"name = 深色模式 | Dark mode\n"
                             u"switch = theme\n"
                             u"open = ms-settings:colors\n"
                             u"kind = settings\n"
                             u"keywords = 深浅色; dark mode\n"
                             u"on = 深色; dark\n"
                             u"off = 浅色; light\n"
                             u"[plain]\n"
                             u"name = 运行 | Run\n"
                             u"open = shell:::{2559a1f3}\n"_s;
        PlaceList places;
        QVERIFY(applyPlaceExtras(places, text, true).isEmpty());
        QCOMPARE(places.size(), std::size_t {2});
        QCOMPARE(places[0].name, u"深色模式"_s);
        QCOMPARE(places[0].switchId, u"theme"_s);
        QCOMPARE(places[0].ownKeywords, (QStringList {u"深浅色"_s, u"dark mode"_s}));
        QCOMPARE(places[0].onWords, (QStringList {u"深色"_s, u"dark"_s}));
        QCOMPARE(places[0].offWords, (QStringList {u"浅色"_s, u"light"_s}));
        QVERIFY(places[0].kind == AppKind::Setting);
        QVERIFY(places[1].switchId.isEmpty() && places[1].onWords.isEmpty() && places[1].offWords.isEmpty());
        places[0].prepare();
        QCOMPARE(places[0].ownFolded.size(), std::size_t {6}); // the state words find the switch too

        PlaceList english;
        applyPlaceExtras(english, text, false);
        QCOMPARE(english[0].name, u"Dark mode"_s);
        QCOMPARE(english[0].switchId, u"theme"_s);
    }

    void switchPlacesShipped()
    {
        const PlaceList places = switchPlaces();
        QHash<QString, const PlaceInfo*> byId;
        for (const PlaceInfo& p : places) {
            if (!p.switchId.isEmpty())
                byId.insert(p.switchId, &p);
        }
        const QStringList ids {u"theme"_s, u"hidden-files"_s, u"file-extensions"_s, u"bluetooth"_s, u"wlan"_s,
            u"volume"_s, u"keep-awake"_s};
        QCOMPARE(byId.size(), qsizetype {7});
        for (const QString& id : ids)
            QVERIFY2(byId.contains(id), qPrintable(id));
        QCOMPARE(byId[u"bluetooth"_s]->key, u"winshun/switch-bluetooth"_s);
        QCOMPARE(byId[u"bluetooth"_s]->command, u"ms-settings:bluetooth"_s);
        QCOMPARE(byId[u"theme"_s]->key, u"winshun/switch-dark-mode"_s);
        QCOMPARE(byId[u"keep-awake"_s]->key, u"winshun/switch-keep-awake"_s);
        QVERIFY(std::all_of(byId.cbegin(), byId.cend(), [](const PlaceInfo* p) { return p->kind == AppKind::Setting; }));

        // No word names two switches: "关××" has to know which one.
        QHash<QString, QString> owner;
        for (const QString& id : ids) {
            const PlaceInfo& p = *byId[id];
            for (const QString& word : QStringList {p.name} + p.ownKeywords + p.onWords + p.offWords) {
                QVERIFY2(owner.value(word.toLower(), id) == id, qPrintable(word));
                owner.insert(word.toLower(), id);
            }
        }

        // What the switches took from the pages: the rest stays there.
        const auto ownOf = [&](const QString& name) {
            const auto it = std::find_if(places.begin(), places.end(), [&](const PlaceInfo& p) { return p.name == name; });
            return it == places.end() ? QStringList {u"(none)"_s} : it->ownKeywords;
        };
        QCOMPARE(ownOf(u"蓝牙和其他设备"_s), (QStringList {u"配对"_s, u"蓝牙耳机"_s, u"添加设备"_s}));
        QCOMPARE(ownOf(u"颜色"_s), (QStringList {u"主题色"_s, u"强调色"_s, u"透明效果"_s}));
        QCOMPARE(ownOf(u"文件夹选项"_s), QStringList {u"文件夹选项"_s});
        QCOMPARE(ownOf(u"Wi-Fi"_s), (QStringList {u"已知网络"_s, u"忘记网络"_s}));
        QVERIFY(!ownOf(u"声音"_s).contains(u"音量"_s));
    }

    void switchCommandSyntax()
    {
        const PlaceList places = switchPlaces();
        using K = SwitchIntent::Kind;
        struct Case {
            QStringView text;
            QStringView id; // empty: not a command, searched as typed
            K kind = K::Toggle;
            int value = 0;
            bool onTop = true; // false: a state word alone, an ordinary row
        };
        const std::vector<Case> cases {
            // The switch alone: an ordinary row.
            {u"蓝牙", {}}, {u"bluetooth", {}}, {u"bt", {}}, {u"ly", {}}, {u"音量", {}}, {u"深色模式", {}},
            {u"dark mode", {}}, {u"深浅色", {}}, {u"隐藏文件", {}}, {u"hidden files", {}}, {u"扩展名", {}},
            {u"后缀名", {}}, {u"file extensions", {}}, {u"保持唤醒", {}}, {u"唤醒", {}}, {u"不睡眠", {}},
            {u"防止睡眠", {}}, {u"awake", {}}, {u"caffeine", {}}, {u"Bluetooth", {}}, {u"WiFi", {}},
            // Verb and switch, either way round.
            {u"开蓝牙", u"bluetooth", K::On}, {u"打开蓝牙", u"bluetooth", K::On}, {u"开启蓝牙", u"bluetooth", K::On},
            {u"启用蓝牙", u"bluetooth", K::On}, {u"蓝牙开", u"bluetooth", K::On}, {u"蓝牙 开", u"bluetooth", K::On},
            {u"bluetooth on", u"bluetooth", K::On}, {u"turn on bluetooth", u"bluetooth", K::On},
            {u"enable bt", u"bluetooth", K::On}, {u"BLUETOOTH ON", u"bluetooth", K::On},
            {u"关蓝牙", u"bluetooth", K::Off}, {u"关闭蓝牙", u"bluetooth", K::Off}, {u"关掉蓝牙", u"bluetooth", K::Off},
            {u"关上蓝牙", u"bluetooth", K::Off}, {u"蓝牙关", u"bluetooth", K::Off}, {u"bluetooth off", u"bluetooth", K::Off},
            {u"disable bt", u"bluetooth", K::Off}, {u"bt off", u"bluetooth", K::Off},
            {u"停用蓝牙", u"bluetooth", K::Off}, {u"禁用蓝牙", u"bluetooth", K::Off},
            {u"切换蓝牙", u"bluetooth", K::Toggle}, {u"蓝牙开关", u"bluetooth", K::Toggle},
            {u"toggle bluetooth", u"bluetooth", K::Toggle},
            {u"wifi off", u"wlan", K::Off}, {u"关 WiFi", u"wlan", K::Off}, {u"关闭无线网", u"wlan", K::Off},
            {u"关wlan", u"wlan", K::Off}, {u"turn off wifi", u"wlan", K::Off}, {u"开 wi-fi", u"wlan", K::On},
            // Full-width letters and digits are half-width.
            {u"关闭ＷＬＡＮ", u"wlan", K::Off}, {u"音量　２０", u"volume", K::SetLevel, 20},
            // The settings page.
            {u"蓝牙设置", u"bluetooth", K::OpenSettings}, {u"深色模式设置", u"theme", K::OpenSettings},
            {u"隐藏文件设置", u"hidden-files", K::OpenSettings}, {u"bluetooth settings", u"bluetooth", K::OpenSettings},
            {u"wifi 选项", u"wlan", K::OpenSettings},
            // A state word: alone it only tells Enter what to do.
            {u"深色", u"theme", K::On, 0, false}, {u"暗色", u"theme", K::On, 0, false},
            {u"dark", u"theme", K::On, 0, false}, {u"浅色", u"theme", K::Off, 0, false},
            {u"亮色", u"theme", K::Off, 0, false}, {u"light", u"theme", K::Off, 0, false},
            {u"静音", u"volume", K::Off, 0, false}, {u"mute", u"volume", K::Off, 0, false},
            {u"取消静音", u"volume", K::On, 0, false}, {u"解除静音", u"volume", K::On, 0, false},
            {u"unmute", u"volume", K::On, 0, false},
            {u"结束保持唤醒", u"keep-awake", K::Off, 0, false}, {u"停止保持唤醒", u"keep-awake", K::Off, 0, false},
            {u"stop keeping awake", u"keep-awake", K::Off, 0, false},
            {u"显示隐藏文件", u"hidden-files", K::On, 0, false}, {u"show hidden files", u"hidden-files", K::On, 0, false},
            {u"不显示隐藏文件", u"hidden-files", K::Off, 0, false}, {u"hide hidden files", u"hidden-files", K::Off, 0, false},
            {u"显示扩展名", u"file-extensions", K::On, 0, false}, {u"隐藏扩展名", u"file-extensions", K::Off, 0, false},
            {u"不显示后缀名", u"file-extensions", K::Off, 0, false},
            // A verb with a state word.
            {u"关闭深色", u"theme", K::Off}, {u"关深色模式", u"theme", K::Off}, {u"打开深色模式", u"theme", K::On},
            {u"dark mode on", u"theme", K::On}, {u"开浅色", u"theme", K::Off}, {u"关闭静音", u"volume", K::On},
            // Volume.
            {u"音量 20", u"volume", K::SetLevel, 20}, {u"音量20", u"volume", K::SetLevel, 20},
            {u"音量 20%", u"volume", K::SetLevel, 20}, {u"vol 20", u"volume", K::SetLevel, 20},
            {u"volume 20", u"volume", K::SetLevel, 20}, {u"音量 0", u"volume", K::SetLevel, 0},
            {u"音量 150", u"volume", K::SetLevel, 150}, {u"音量 +10", u"volume", K::StepLevel, 10},
            {u"音量 -5", u"volume", K::StepLevel, -5}, {u"音量+10", u"volume", K::StepLevel, 10},
            {u"音量 12345", {}}, {u"音量 abc", {}}, {u"蓝牙 20", {}},
            // Keep awake.
            {u"保持唤醒 1小时", u"keep-awake", K::AwakeFor, 60}, {u"保持唤醒 一小时", u"keep-awake", K::AwakeFor, 60},
            {u"唤醒 60分钟", u"keep-awake", K::AwakeFor, 60}, {u"awake 1h", u"keep-awake", K::AwakeFor, 60},
            {u"keep awake 90m", u"keep-awake", K::AwakeFor, 90}, {u"保持唤醒 半小时", u"keep-awake", K::AwakeFor, 30},
            {u"保持唤醒 一个半小时", u"keep-awake", K::AwakeFor, 90}, {u"保持唤醒 1.5小时", u"keep-awake", K::AwakeFor, 90},
            {u"保持唤醒 两小时", u"keep-awake", K::AwakeFor, 120}, {u"awake 2 hours", u"keep-awake", K::AwakeFor, 120},
            {u"保持唤醒 到 18:00", u"keep-awake", K::AwakeUntil, 18 * 60},
            {u"保持唤醒到18点", u"keep-awake", K::AwakeUntil, 18 * 60},
            {u"保持唤醒 到18：30", u"keep-awake", K::AwakeUntil, 18 * 60 + 30},
            {u"保持唤醒到9点半", u"keep-awake", K::AwakeUntil, 9 * 60 + 30},
            {u"awake until 18:00", u"keep-awake", K::AwakeUntil, 18 * 60},
            {u"关闭保持唤醒", u"keep-awake", K::Off}, {u"取消唤醒", u"keep-awake", K::Off},
            {u"awake off", u"keep-awake", K::Off}, {u"开启保持唤醒", u"keep-awake", K::On},
            {u"保持唤醒 100小时", {}}, {u"保持唤醒 0分钟", {}}, {u"保持唤醒 到 25:00", {}}, {u"保持唤醒 1", {}},
            {u"音量 1小时", {}}, // a duration is for keeping awake only
            // Not about a switch: searched as typed.
            {u"夜间模式", {}}, {u"关于", {}}, {u"关机", {}}, {u"开始菜单", {}}, {u"打开方式", {}},
            {u"关闭防火墙", {}}, {u"关闭实时保护", {}}, {u"开热点", {}}, {u"关闭更新", {}}, {u"启用网卡", {}},
            {u"禁用网卡", {}}, {u"开蓝牙耳机", {}}, {u"关蓝牙和wifi", {}}, {u"蓝牙 设置 打开", {}}, {u"online", {}},
            {u"daemon", {}}, {u"on", {}}, {u"关", {}}, {u"", {}}, {u"   ", {}}, {u"ext:txt", {}},
        };
        for (const Case& c : cases) {
            const auto command = parseSwitchCommand(c.text, places);
            const QString what = u"\""_s + c.text.toString() + u"\""_s;
            if (c.id.isEmpty()) {
                QVERIFY2(!command, qPrintable(what + u" should not be a command"_s));
                continue;
            }
            QVERIFY2(command.has_value(), qPrintable(what + u" should be a command"_s));
            QVERIFY2(places[command->place].switchId == c.id, qPrintable(what + u" is the switch " + places[command->place].switchId));
            QVERIFY2(command->intent.kind == c.kind, qPrintable(what + u" has intent " + QString::number(int(command->intent.kind))));
            QVERIFY2(command->intent.value == c.value, qPrintable(what + u" has value " + QString::number(command->intent.value)));
            QVERIFY2(command->onTop == c.onTop, qPrintable(what + u" onTop " + QString::number(command->onTop)));
        }
        // No switch in the list: nothing is one.
        QVERIFY(!parseSwitchCommand(u"关蓝牙", PlaceList {}));
    }

    void switchPlaceSearch()
    {
        const PlaceList places = switchPlaces();
        const auto hits = [&](const QString& text, const QStringList& history = {}) {
            const ParsedQuery query = ws::parseQuery(text);
            const NameMatcher matcher(query);
            return searchPlaces(places, query, matcher, history);
        };
        const auto names = [&](const QString& text, const QStringList& history = {}) {
            QStringList out;
            for (const PlaceHit& hit : hits(text, history))
                out.append(places[hit.index].name);
            return out;
        };

        // The switch is the one row of its concept: the page with the same command gives way.
        QCOMPARE(names(u"蓝牙"_s), QStringList {u"蓝牙"_s});
        QCOMPARE(names(u"bluetooth"_s), QStringList {u"蓝牙"_s});
        QCOMPARE(names(u"深色"_s), QStringList {u"深色模式"_s});
        QCOMPARE(names(u"深色模式"_s), QStringList {u"深色模式"_s});
        QCOMPARE(names(u"音量"_s), QStringList {u"音量"_s});
        QCOMPARE(names(u"静音"_s), QStringList {u"音量"_s});
        QCOMPARE(names(u"wifi"_s), QStringList {u"WLAN"_s});
        QCOMPARE(names(u"无线网络"_s), QStringList {u"WLAN"_s});
        QCOMPARE(names(u"唤醒"_s), QStringList {u"保持唤醒"_s});
        QCOMPARE(names(u"隐藏文件"_s), QStringList {u"显示隐藏的文件"_s});
        QCOMPARE(names(u"扩展名"_s), QStringList {u"显示文件扩展名"_s});
        // What the pages keep is still theirs.
        QCOMPARE(names(u"蓝牙耳机"_s), QStringList {u"蓝牙和其他设备"_s});
        QCOMPARE(names(u"扬声器"_s).value(0), u"声音"_s); // (the Control Panel's sound dialog too)
        QVERIFY(!names(u"扬声器"_s).contains(u"音量"_s));
        QCOMPARE(names(u"文件夹选项"_s), QStringList {u"文件夹选项"_s});
        QCOMPARE(names(u"强调色"_s), QStringList {u"颜色"_s});
        QCOMPARE(names(u"夜间模式"_s), QStringList {u"夜灯"_s}); // Windows' name for the night light, not dark mode
        // Two switches with one command are two rows.
        const QStringList both = names(u"显示"_s);
        QVERIFY(both.contains(u"显示隐藏的文件"_s) && both.contains(u"显示文件扩展名"_s));
        // A page the query names better than it names the switch stays, above it.
        {
            PlaceInfo page;
            page.name = u"abc"_s;
            page.key = u"k/page"_s;
            page.command = u"x:one"_s;
            PlaceInfo sw;
            sw.name = u"开关"_s;
            sw.key = u"k/switch"_s;
            sw.command = u"X:ONE"_s;
            sw.switchId = u"one"_s;
            sw.ownKeywords = {u"abc"_s};
            PlaceInfo other = sw;
            other.name = u"另一个"_s;
            other.key = u"k/other"_s;
            other.command = u"x:other"_s;
            other.ownKeywords = {u"abc"_s};
            PlaceList mixed {sw, page, other};
            for (PlaceInfo& p : mixed)
                p.prepare();
            const ParsedQuery query = ws::parseQuery(u"abc"_s);
            const NameMatcher matcher(query);
            QStringList found;
            for (const PlaceHit& hit : searchPlaces(mixed, query, matcher, {}))
                found.append(mixed[hit.index].name);
            // The page (a name spelled out) above the switch; "another" has the same switch id: one row.
            QCOMPARE(found, (QStringList {u"abc"_s, u"开关"_s}));
        }

        // How sure a hit is: the initials of a Chinese keyword are a guess.
        const auto weak = [&](const QString& text) {
            const std::vector<PlaceHit> found = hits(text);
            return found.empty() ? std::optional<bool>() : std::optional<bool>(found.front().weak);
        };
        QCOMPARE(weak(u"ly"_s), std::optional<bool>(true));
        QCOMPARE(weak(u"lanya"_s), std::optional<bool>(false)); // spelled out
        QCOMPARE(weak(u"蓝牙"_s), std::optional<bool>(false));
        QCOMPARE(weak(u"bt"_s), std::optional<bool>(false));
        QCOMPARE(weak(u"blue"_s), std::optional<bool>(false));
        QCOMPARE(weak(u"dark"_s), std::optional<bool>(false));
        QCOMPARE(names(u"ly"_s).value(0), u"蓝牙"_s);
    }

    void switchRowsInSearch()
    {
        QTemporaryDir dir;
        IndexService::Options options;
        options.content.enabled = false;
        IndexService service(dir.path() + u"/index"_s, options);
        AppCatalog apps;
        SystemCatalog catalog;
        const auto setApps = [&](const QStringList& names) {
            auto list = std::make_shared<AppList>();
            for (const QString& name : names) {
                AppInfo a;
                a.name = name;
                a.id = uR"(C:\Apps\)"_s + name + u".exe"_s;
                a.target = a.id;
                a.prepare();
                list->push_back(std::move(a));
            }
            apps.setApps(list);
        };
        setApps({});
        const auto places = std::make_shared<const PlaceList>(switchPlaces());
        catalog.setPlaces(places);
        const QString bluetoothPage = (*places)[0].path();
        const auto keepAwake = std::find_if(places->begin(), places->end(), [](const PlaceInfo& p) { return p.switchId == u"keep-awake"_s; });
        QVERIFY(keepAwake != places->end());

        SearchEngine engine(&service, &apps, &catalog);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        const auto search = [&](const QString& text, Scope scope = Scope::All, const QStringList& history = {},
                                const QStringList& pinned = {}) {
            spy.clear();
            SearchEngine::Request request;
            request.text = text;
            request.scope = scope;
            request.history = history;
            request.pinnedPlaces = pinned;
            engine.submit(std::move(request));
            if (!spy.wait(5000))
                return SearchResults {};
            return spy.at(0).at(1).value<SearchResults>();
        };
        const auto switches = [](const SearchResults& rows) {
            QStringList ids;
            for (const SearchResult& r : rows) {
                if (!r.switchId.isEmpty())
                    ids.append(r.switchId);
            }
            return ids;
        };
        const auto indexOfSwitch = [](const SearchResults& rows, const QString& id) {
            const auto it = std::find_if(rows.cbegin(), rows.cend(), [&](const SearchResult& r) { return r.switchId == id; });
            return it == rows.cend() ? qsizetype {-1} : it - rows.cbegin();
        };

        // A command: the switch with what was asked, first; the page and the plain row give way.
        SearchResults rows = search(u"关蓝牙"_s);
        QVERIFY(!rows.isEmpty());
        QCOMPARE(rows[0].switchId, u"bluetooth"_s);
        QVERIFY(rows[0].intent == (SwitchIntent {SwitchIntent::Kind::Off, 0}));
        QVERIFY(rows[0].isPlace());
        QCOMPARE(rows[0].target, u"ms-settings:bluetooth"_s);
        QVERIFY(!rows[0].statusRow);
        QCOMPARE(switches(rows), QStringList {u"bluetooth"_s});
        QVERIFY(std::none_of(rows.cbegin(), rows.cend(), [&](const SearchResult& r) { return r.path == bluetoothPage; }));

        rows = search(u"音量 30"_s);
        QCOMPARE(rows.value(0).switchId, u"volume"_s);
        QVERIFY(rows[0].intent == (SwitchIntent {SwitchIntent::Kind::SetLevel, 30}));
        rows = search(u"蓝牙设置"_s);
        QCOMPARE(rows.value(0).switchId, u"bluetooth"_s);
        QVERIFY(rows[0].intent.kind == SwitchIntent::Kind::OpenSettings);
        QCOMPARE(switches(rows), QStringList {u"bluetooth"_s});

        // The switch alone: the ordinary row, Enter toggles; the page is not listed.
        rows = search(u"蓝牙"_s);
        QCOMPARE(rows.value(0).switchId, u"bluetooth"_s);
        QVERIFY(rows[0].intent == SwitchIntent {});
        QCOMPARE(rows.size(), qsizetype {1});
        // A state word alone is an ordinary row that knows what Enter does.
        rows = search(u"深色"_s);
        QCOMPARE(rows.value(0).switchId, u"theme"_s);
        QVERIFY(rows[0].intent == (SwitchIntent {SwitchIntent::Kind::On, 0}));
        rows = search(u"静音"_s);
        QCOMPARE(rows.value(0).switchId, u"volume"_s);
        QVERIFY(rows[0].intent == (SwitchIntent {SwitchIntent::Kind::Off, 0}));
        // Not about a switch.
        QVERIFY(switches(search(u"夜间模式"_s)).isEmpty());
        QCOMPARE(search(u"夜间模式"_s).value(0).name, u"夜灯"_s);

        // A state word does not push an app named so down; "dark" ...
        setApps({u"dark"_s});
        rows = search(u"dark"_s);
        QCOMPARE(rows.value(0).name, u"dark"_s);
        QVERIFY(rows[0].app == AppKind::Desktop);
        QVERIFY(indexOfSwitch(rows, u"theme"_s) > 0);
        // ... and a guess does not push an app found as well down either.
        setApps({u"蓝宇"_s});
        rows = search(u"ly"_s);
        QCOMPARE(rows.value(0).name, u"蓝宇"_s);
        QVERIFY(indexOfSwitch(rows, u"bluetooth"_s) > 0);
        // With no such app the guess is first.
        setApps({});
        rows = search(u"ly"_s);
        QCOMPARE(rows.value(0).switchId, u"bluetooth"_s);
        // A name spelled out is not a guess.
        setApps({u"蓝牙助手"_s});
        QCOMPARE(search(u"蓝牙"_s).value(0).switchId, u"bluetooth"_s);
        setApps({});

        // Files, folders and contents: no switch rows.
        for (const Scope scope : {Scope::Files, Scope::Folders}) {
            for (const QString& text : {u"关蓝牙"_s, u"蓝牙"_s, u"静音"_s})
                QVERIFY(switches(search(text, scope)).isEmpty());
        }

        // The start list: never a switch, even one in the history.
        const QString switchPath = keepAwake->path();
        rows = search(QString(), Scope::All, {switchPath, bluetoothPage});
        QCOMPARE(rows.size(), qsizetype {1});
        QCOMPARE(rows[0].path, bluetoothPage);
        QVERIFY(switches(rows).isEmpty());
        // ... but one asked to be kept in view, first, marked.
        rows = search(QString(), Scope::All, {switchPath, bluetoothPage}, {switchPath});
        QCOMPARE(rows.size(), qsizetype {2});
        QCOMPARE(rows[0].switchId, u"keep-awake"_s);
        QVERIFY(rows[0].statusRow);
        QVERIFY(!rows[1].statusRow);
        QCOMPARE(rows[1].path, bluetoothPage);
        rows = search(QString(), Scope::All, {}, {switchPath, switchPath, bluetoothPage}); // a switch only, once
        QCOMPARE(rows.size(), qsizetype {1});
        QVERIFY(search(QString(), Scope::Files, {}, {switchPath}).isEmpty());
        QVERIFY(switches(search(u"蓝牙"_s, Scope::All, {}, {switchPath})).size() == 1); // typing: no pinned row
        QVERIFY(!search(u"蓝牙"_s, Scope::All, {}, {switchPath}).first().statusRow);
    }

    void appLogoChoice()
    {
        // As ChatGPT's package ships them.
        QStringList files {u"Square44x44Logo.png"_s, u"Square44x44Logo.scale-200.png"_s};
        for (const int size : {16, 20, 24, 30, 32, 36, 40, 44, 48, 60, 64, 72, 80, 96, 256}) {
            for (const QString& form : {u"unplated"_s, u"lightunplated"_s})
                files.append(u"Square44x44Logo.targetsize-%1_altform-%2.png"_s.arg(size).arg(form));
        }
        const QString logo = u"Square44x44Logo.png"_s;
        const auto pick = [&](const QStringList& names, int pixels, bool dark) {
            const auto choice = chooseLogoFile(logo, names, pixels, dark);
            return choice ? choice->name : QString();
        };
        // The size drawn for exactly these pixels, in the theme's variant.
        QCOMPARE(pick(files, 48, true), u"Square44x44Logo.targetsize-48_altform-unplated.png"_s);
        QCOMPARE(pick(files, 48, false), u"Square44x44Logo.targetsize-48_altform-lightunplated.png"_s);
        // Else the next bigger one, scaled down, never a smaller one scaled up.
        QCOMPARE(pick(files, 42, true), u"Square44x44Logo.targetsize-44_altform-unplated.png"_s);
        QCOMPARE(pick(files, 300, true), u"Square44x44Logo.scale-200.png"_s); // bigger than any target size
        QVERIFY(!chooseLogoFile(logo, files, 48, true)->plated);

        // Older apps: logos made for a plate, by display scale; the biggest.
        const QStringList plated {u"Square44x44Logo.scale-100.png"_s, u"Square44x44Logo.scale-200.png"_s,
            u"Square44x44Logo.scale-150.png"_s, u"Square44x44Logo.scale-100_contrast-black.png"_s,
            u"StoreLogo.png"_s};
        QCOMPARE(pick(plated, 48, true), u"Square44x44Logo.scale-200.png"_s);
        QVERIFY(chooseLogoFile(logo, plated, 48, true)->plated);

        // Only an unplated variant: used in either theme.
        QCOMPARE(pick({u"Square44x44Logo.targetsize-32_altform-unplated.png"_s}, 32, false),
            u"Square44x44Logo.targetsize-32_altform-unplated.png"_s);
        // Files for another language than the one resolved are left out.
        const QStringList languages {u"Square44x44Logo.targetsize-48_altform-unplated_lang-en-us.png"_s,
            u"Square44x44Logo.targetsize-48_altform-unplated_lang-zh-cn.png"_s};
        QCOMPARE(chooseLogoFile(logo, languages, 48, true, u"Square44x44Logo.scale-100_lang-zh-cn.png"_s)->name,
            u"Square44x44Logo.targetsize-48_altform-unplated_lang-zh-cn.png"_s);
        QVERIFY(!chooseLogoFile(logo, {u"Other.png"_s, u"Square44x44Logo.targetsize-48.jpg"_s}, 48, true));
        // One choice across folders: a resource package holds only its scale's files.
        QCOMPARE(pick({u"C:/app_split.scale-150/Assets/Square44x44Logo.scale-150.png"_s,
                          u"C:/app/Assets/Square44x44Logo.targetsize-48_altform-unplated.png"_s},
                     48, true),
            u"C:/app/Assets/Square44x44Logo.targetsize-48_altform-unplated.png"_s);
    }

    void installedApps() // this machine's real list
    {
        const AppList apps = loadInstalledApps({});
        QVERIFY(!apps.empty()); // every Windows has some (File Explorer, Settings, ...)
        QVERIFY(std::all_of(apps.begin(), apps.end(), [](const AppInfo& a) { return !a.name.isEmpty() && !a.id.isEmpty(); }));
        QVERIFY(std::none_of(apps.begin(), apps.end(), [](const AppInfo& a) { return a.target.contains(u"://"_s); }));
        QCOMPARE(installedAppsFingerprint(), installedAppsFingerprint());
    }

    void contentUtf8()
    {
        const QByteArray text = u"first line\n\tSecond line has the NEEDLE here\nthird"_s.toUtf8();
        const ContentScanner scanner(u"needle"_s);
        const auto m = scanner.scan(reader(text), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 2);
        QCOMPARE(m->snippet, u"Second line has the NEEDLE here"_s);
        QCOMPARE(m->snippet.mid(m->matches.at(0).first, m->matches.at(0).second), u"NEEDLE"_s);

        const QByteArray bom = QByteArray("\xEF\xBB\xBF") + u"中文内容：季度报告\n"_s.toUtf8();
        const auto m2 = ContentScanner(u"报告"_s).scan(reader(bom), 4096, {});
        QVERIFY(m2);
        QCOMPARE(m2->line, 1);
        QCOMPARE(m2->snippet, u"中文内容：季度报告"_s);
    }

    void contentTerms()
    {
        // Every term somewhere in the file; the line shown has the most of them.
        const QByteArray text = u"intro\nthe Report is due\nbudget 2025 numbers\nreport on the 2025 budget\n"_s.toUtf8();
        const ContentScanner scanner(QStringList {u"report"_s, u"2025"_s});
        const auto m = scanner.scan(reader(text), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 4);
        QCOMPARE(m->snippet, u"report on the 2025 budget"_s);
        QCOMPARE(m->matches, (QList<std::pair<qsizetype, qsizetype>> {{0, 6}, {14, 4}}));
        for (std::size_t chunk : {16, 18, 32, 100}) {
            for (std::size_t step : {1, 7}) {
                const auto small = scanner.scan(reader(text, step), chunk, {});
                QVERIFY2(small && small->line >= 2, qPrintable(u"chunk %1 step %2"_s.arg(chunk).arg(step)));
            }
        }

        // On different lines: the first one's line, and where the other is after it.
        const auto apart = ContentScanner(QStringList {u"beta"_s, u"alpha"_s}).scan(reader("alpha\nbeta\n"), 4096, {});
        QVERIFY(apart);
        QCOMPARE(apart->line, 1);
        QCOMPARE(apart->snippet, u"alpha … beta"_s);
        QCOMPARE(apart->matches, (QList<std::pair<qsizetype, qsizetype>> {{0, 5}, {8, 4}}));
        QVERIFY(!ContentScanner(QStringList {u"alpha"_s, u"gamma"_s}).scan(reader("alpha\nbeta\n"), 4096, {}));
        QVERIFY(!ContentScanner(u"alpha beta"_s).scan(reader("alpha\nbeta\n"), 4096, {})); // a phrase: just so
        // GBK: every term must be one the code page can write.
        const QByteArray gbk = encode(u"测试文本\n"_s, 936);
        QVERIFY(ContentScanner(QStringList {u"测试"_s, u"文本"_s}, 936).scan(reader(gbk), 4096, {}));
        QVERIFY(!ContentScanner(QStringList {u"测试"_s, u"\U0001F600"_s}, 936).scan(reader(gbk), 4096, {}));
    }

    void contentRanking()
    {
        // Of lines as good, the one with the terms at the start of words.
        const ContentScanner todo(QStringList {u"todo"_s, u"fix"_s});
        const auto m = todo.scan(reader("// todo: prefix it\n// TODO: fix it\n"), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 2);
        QCOMPARE(m->termsOnLine, 2);
        QCOMPARE(m->wordStarts, 0b11u);
        QVERIFY(!m->longLine);
        const auto prefix = todo.scan(reader("// todo: prefix it\n"), 4096, {});
        QVERIFY(prefix);
        QCOMPARE(prefix->wordStarts, 0b01u);
        // "Lock" starts a word in "readLock"; a CJK term anywhere.
        const auto camel = ContentScanner(QStringList {u"lock"_s, u"报告"_s}).scan(reader("readLock 年度报告\n"), 4096, {});
        QVERIFY(camel);
        QCOMPARE(camel->wordStarts, 0b11u);
        // Packed onto one line (no break for 8 KB); a long paragraph is not.
        const QByteArray packed = QByteArray(10000, 'x') + " fix " + QByteArray(10000, 'y');
        const auto minified = ContentScanner(u"fix"_s).scan(reader(packed), 65536, {});
        QVERIFY(minified);
        QVERIFY(minified->longLine);
        const QByteArray paragraph = u"很长的一段话。"_s.repeated(150).toUtf8() + " fix " + u"又一段。"_s.repeated(150).toUtf8();
        const auto prose = ContentScanner(u"fix"_s).scan(reader(paragraph), 65536, {});
        QVERIFY(prose);
        QVERIFY(!prose->longLine);
        const QByteArray halves = QByteArray(5000, 'x') + " fix " + QByteArray(5000, 'y'); // 8 KB together
        QVERIFY(ContentScanner(u"fix"_s).scan(reader(halves), 65536, {})->longLine);

        // The snippet starts where the most terms fit, not at the first one.
        const QString spread = u"alpha "_s + QString(200, u'.') + u" alpha and beta here"_s;
        const auto both = ContentScanner(QStringList {u"alpha"_s, u"beta"_s}).scan(reader(spread.toUtf8()), 4096, {});
        QVERIFY(both);
        QCOMPARE(both->matches.size(), 2);
        QVERIFY(both->snippet.contains(u"alpha and beta"_s));

        // Every term shows within the width of a result (28 ideographs): the
        // ones the line lacks after it, with words around them.
        const auto shows = [](const ContentMatch& m, const QStringList& terms) {
            int width = 0;
            for (qsizetype i = 0; i < m.snippet.size(); ++i) {
                const bool wide = m.snippet[i].unicode() >= 0x2E80;
                for (const auto& [at, length] : m.matches) {
                    if (at == i && width + (wide ? 2 : 1) * length > 60)
                        return false;
                }
                width += wide ? 2 : 1;
            }
            for (const QString& t : terms) {
                if (std::ranges::none_of(m.matches, [&](const auto& r) {
                        return m.snippet.mid(r.first, r.second).compare(t, Qt::CaseInsensitive) == 0;
                    }))
                    return false;
            }
            return true;
        };
        const QStringList three {u"弯管机"_s, u"角度"_s, u"优化"_s};
        const QString manual = u"目录\n弯管机用户手册由于软件会不断进行优化，如说明书与实际界面有差异，请以实际界面为准。\n"_s
            + u"第二章\n设定折弯角度时，先回零点。\n"_s;
        const auto pieced = ContentScanner(three).scan(reader(manual.toUtf8()), 4096, {});
        QVERIFY(pieced);
        QCOMPARE(pieced->line, 2);
        QVERIFY2(pieced->snippet.startsWith(u"弯管机用户手册"_s), qPrintable(pieced->snippet));
        QVERIFY2(pieced->snippet.contains(u" … "_s), qPrintable(pieced->snippet));
        QVERIFY2(shows(*pieced, three), qPrintable(pieced->snippet));
        // Far apart on one line: two pieces of it.
        const QString wide = u"弯管机"_s + u"很长的一段话。"_s.repeated(20) + u"角度和优化"_s + u"后面的话。"_s.repeated(5);
        const auto split = ContentScanner(three).scan(reader(wide.toUtf8()), 4096, {});
        QVERIFY(split);
        QVERIFY2(shows(*split, three), qPrintable(split->snippet));
        // All together: one piece, as before.
        const auto oneLine = ContentScanner(three).scan(reader(u"弯管机角度优化说明\n"_s.toUtf8()), 4096, {});
        QVERIFY(oneLine);
        QCOMPARE(oneLine->snippet, u"弯管机角度优化说明"_s);

        const QStringList terms {u"todo"_s, u"fix"_s};
        const auto rank = [&](const ContentMatch& match, QStringView name = u"a.txt", int priority = 1,
                              std::int64_t age = -1) { return contentRelevance(match, terms, name, priority, age); };
        ContentMatch apart {.termsOnLine = 1, .wordStarts = 0b01};
        ContentMatch together {.termsOnLine = 2, .wordStarts = 0b11};
        QVERIFY(rank(together) > rank(apart));
        QVERIFY(rank(*m) > rank(*prefix));
        QVERIFY(rank(apart, u"todo-fix.txt") > rank(apart));
        QVERIFY(rank(apart, u"a.txt", 0) > rank(apart, u"a.txt", 3)); // your own folders before system ones
        QVERIFY(rank(apart, u"a.txt", 1, 3600) > rank(apart, u"a.txt", 1, 400LL * 24 * 3600));
        ContentMatch packedLine = together;
        packedLine.longLine = true;
        QVERIFY(rank(packedLine) < rank(apart));
    }

    void contentChunkBoundaries()
    {
        // Matches straddling chunk borders, with tiny chunks and odd read sizes.
        QString body;
        for (int i = 0; i < 50; ++i)
            body += u"line %1 filler text\n"_s.arg(i);
        body += u"the target 关键词 is here\n"_s;
        const QByteArray utf8 = body.toUtf8();
        for (std::size_t chunk : {16, 18, 32, 100, 4096}) {
            for (std::size_t step : {1, 3, 7, 1000}) {
                const auto m = ContentScanner(u"关键词"_s).scan(reader(utf8, step), chunk, {});
                QVERIFY2(m, qPrintable(u"chunk %1 step %2"_s.arg(chunk).arg(step)));
                QCOMPARE(m->line, 51);
            }
        }
    }

    void contentUtf16()
    {
        const QByteArray data = utf16le(u"alpha\r\nbeta Gamma 测试\r\n"_s, true);
        for (std::size_t chunk : {16, 20, 4096}) {
            const auto m = ContentScanner(u"gamma 测试"_s).scan(reader(data), chunk, {});
            QVERIFY(m);
            QCOMPARE(m->line, 2);
            QCOMPARE(m->snippet.mid(m->matches.at(0).first, m->matches.at(0).second), u"Gamma 测试"_s);
            if (chunk == 4096)
                QCOMPARE(m->snippet, u"beta Gamma 测试"_s);
        }
    }

    void contentGbk()
    {
        const QByteArray gbk = encode(u"第一行\n这是测试文本 Hello\n"_s, 936);
        QCOMPARE(ContentScanner::detect({gbk.constData(), static_cast<std::size_t>(gbk.size())}, nullptr),
            TextEncoding::Ansi);
        const auto m = ContentScanner(u"测试文本"_s, 936).scan(reader(gbk), 4096, {});
        QVERIFY(m);
        QCOMPARE(m->line, 2);
        QCOMPARE(m->snippet, u"这是测试文本 Hello"_s);

        // A GBK character whose trail byte is 'a' must not match the needle "a".
        QChar tricky;
        for (char16_t c = 0x4E00; c < 0x9FA5 && tricky.isNull(); ++c) {
            const QByteArray b = encode(QString(QChar(c)), 936);
            if (b.size() == 2 && b[1] == 'a')
                tricky = QChar(c);
        }
        QVERIFY(!tricky.isNull());
        const QString line = QString(4, tricky) + u'\n';
        QVERIFY(!ContentScanner(u"a"_s, 936).scan(reader(encode(line + line, 936)), 4096, {}));
        const auto real = ContentScanner(u"a"_s, 936).scan(reader(encode(line + tricky + u" A\n"_s, 936)), 4096, {});
        QVERIFY(real);
        QCOMPARE(real->line, 2);
    }

    void contentNoMatch()
    {
        QVERIFY(!ContentScanner(u"absent"_s).scan(reader("nothing to see\n"), 4096, {}));
        QVERIFY(!ContentScanner(u"x"_s).scan(reader(QByteArray()), 4096, {}));
        const auto cancelled = ContentScanner(u"x"_s).scan(reader("xxxx"), 4096, [] { return true; });
        QVERIFY(!cancelled);
    }

    void contentFile()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath(u"notes.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("first line\nsecond line has the Needle\n");
        file.close();
        const std::wstring path = QDir::toNativeSeparators(file.fileName()).toStdWString();
        const ContentScanner scanner(u"needle"_s);
        const auto match = scanner.scanFile(path, 0, {});
        QVERIFY(match);
        QCOMPARE(match->line, 2);
        bool large = false;
        QVERIFY(!scanner.scanFile(path, 10, {}, &large)); // larger than the limit
        QVERIFY(large);
        QVERIFY(!scanner.scanFile(path + L".missing", 0, {}, &large));
        QVERIFY(!large);
        QVERIFY(scanner.scanFile(path, 0, {}, &large) && !large);
    }

    void gramsOfText()
    {
        const QString text = u"第一行 English 中文内容\n季度报告：收入增长。日本語のテキスト 한국어 𠀀𠀁 end"_s;
        const std::vector<grams::Key> keys = gramsOf(text);
        for (const QString& phrase : {u"中文"_s, u"中文内容"_s, u"文内"_s, u"第一行"_s, u"季度报告"_s, u"中"_s, u"日本語のテキスト"_s,
                 u"한국어"_s, u"𠀀𠀁"_s, u"行 English 中"_s, u"收入增长。"_s})
            QVERIFY2(hasAll(keys, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"行中"_s, u"容季"_s /* across the line break */, u"告收"_s /* across "：" */, u"中国"_s})
            QVERIFY2(!hasAll(keys, phrase), qPrintable(phrase));

        // Whatever the encoding and the chunks it arrives in.
        const QByteArray utf8 = text.toUtf8();
        QByteArray utf16be;
        for (const QChar c : text) {
            utf16be.append(static_cast<char>(c.unicode() >> 8));
            utf16be.append(static_cast<char>(c.unicode() & 0xFF));
        }
        const QString chinese = u"第一行 English 中文内容\n季度报告：收入增长。"_s; // what GBK can hold
        for (const std::size_t chunk : {1, 2, 3, 5, 7, 4096}) {
            QCOMPARE(gramsOf(utf8, TextEncoding::Utf8, chunk), keys);
            QCOMPARE(gramsOf(utf16le(text, false), TextEncoding::Utf16LE, chunk), keys);
            QCOMPARE(gramsOf(utf16be, TextEncoding::Utf16BE, chunk), keys);
            QCOMPARE(gramsOf(encode(chinese, 936), TextEncoding::Ansi, chunk), gramsOf(chinese));
        }
        // Malformed UTF-8 breaks a pair, as the U+FFFD the scanner decodes it to would.
        const QByteArray broken = u"中"_s.toUtf8() + "\xFF" + u"文"_s.toUtf8() + "\xE6\x96" + u"字"_s.toUtf8();
        const std::vector<grams::Key> brokenKeys = gramsOf(broken, TextEncoding::Utf8);
        QVERIFY(hasAll(brokenKeys, u"中"_s) && hasAll(brokenKeys, u"文"_s) && hasAll(brokenKeys, u"字"_s));
        QVERIFY(!hasAll(brokenKeys, u"中文"_s) && !hasAll(brokenKeys, u"文字"_s));

        // Runs of three ASCII characters, whatever their case, spaces and
        // punctuation too; any run of whitespace is one space.
        for (const QString& phrase : {u"English"_s, u"ENGLISH"_s, u"glis"_s, u"end"_s, u"English 中文"_s, u" end"_s})
            QVERIFY2(hasAll(keys, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"Englishes"_s, u"end2"_s, u"lishend"_s, u"English-"_s})
            QVERIFY2(!hasAll(keys, phrase), qPrintable(phrase));
        const std::vector<grams::Key> code = gramsOf(u"auto my_var = getElementById(x2y);\nif (p->ok)  {\r\n\treturn;"_s);
        for (const QString& phrase : {u"my_var"_s, u"y_v"_s, u"getElementById(x2y)"_s, u"x2y"_s, u"auto my_var = get"_s,
                 u"(x2y);"_s, u"if (p->ok) { return;"_s, u"P->OK"_s, u"ok)     {"_s})
            QVERIFY2(hasAll(code, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"automy"_s, u"x2yz"_s, u"auto  my_var=get"_s, u"if(p"_s, u"p.ok"_s, u"var=="_s})
            QVERIFY2(!hasAll(code, phrase), qPrintable(phrase));
        { // a single-byte code page: bytes above 0x7F are not ASCII
            grams::Collector collector(TextEncoding::Ansi, 1252);
            const QByteArray latin = encode(u"café_bar Zürich"_s, 1252);
            collector.feed(latin.constData(), static_cast<std::size_t>(latin.size()));
            const std::vector<grams::Key> latinKeys = collector.finish();
            QVERIFY(hasAll(latinKeys, u"caf"_s) && hasAll(latinKeys, u"_bar"_s) && hasAll(latinKeys, u"rich"_s));
            QVERIFY(!hasAll(latinKeys, u"cafe"_s) && !hasAll(latinKeys, u"zur"_s));
        }

        QCOMPARE(grams::ofPhrase(u"report 2024"_s).size(), std::size_t {9}); // rep epo por ort "rt " "t 2" " 20" 202 024
        QCOMPARE(grams::ofPhrase(u"a  \t b"_s).size(), std::size_t {1}); // "a b"
        QVERIFY(grams::ofPhrase(u"aé bè1"_s).empty()); // no three ASCII characters in a row
        QCOMPARE(grams::ofPhrase(u"季度报告"_s).size(), std::size_t {3}); // pairs only
        QCOMPARE(grams::ofPhrase(u"Qt 窗口 a 类"_s).size(), std::size_t {4}); // "qt ", 窗口, " a ", 类
        QVERIFY(grams::decides(u"中"_s) && grams::decides(u"中文"_s));
        QVERIFY(!grams::decides(u"中文字"_s) && !grams::decides(u"a中"_s) && !grams::decides(u""_s));

        ContentSizeLimits limits;
        limits.bytes = {100, 20, 30, 400};
        QCOMPARE(ContentSizeLimits::kindOf("CPP"), ContentSizeLimits::Code);
        QCOMPARE(ContentSizeLimits::kindOf("json"), ContentSizeLimits::Data);
        QCOMPARE(ContentSizeLimits::kindOf("log"), ContentSizeLimits::Text);
        QCOMPARE(ContentSizeLimits::kindOf("c++"), ContentSizeLimits::Code);
        QCOMPARE(ContentSizeLimits::kindOf("verylongext"), ContentSizeLimits::Text);
        QCOMPARE(ContentSizeLimits::kindOf("PDF"), ContentSizeLimits::Document);
        QCOMPARE(limits.of(L"D:\\Reports\\Q3.xlsx"), std::int64_t {400});
        QVERIFY(isDocumentPath(L"C:\\a\\Report.DOCX") && isDocumentPath(L"x.et") && !isDocumentPath(L"x.docxx"));
        QVERIFY(!isDocumentPath(L"C:\\a.pdf\\notes") && !isDocumentPath(L"C:\\a\\pdf") && !isDocumentPath(L"a.pdf\u00e9"));
        QCOMPARE(documentExtensions().size(), qsizetype {28});
        QCOMPARE(limits.of(L"C:\\src\\main.CPP"), std::int64_t {20});
        QCOMPARE(limits.of(L"C:\\a.b\\data.json"), std::int64_t {30});
        QCOMPARE(limits.of(L"C:\\a.json\\README"), std::int64_t {100}); // no extension: text
        QCOMPARE(limits.of(L"C:\\notes.txt"), std::int64_t {100});
        QCOMPARE(limits.of(L"C:\\x.verylongext"), std::int64_t {100});
        QCOMPARE(limits.of(L"C:\\x.héllo"), std::int64_t {100});
        QCOMPARE(limits.of(L"C:\\bundle.min.js"), std::int64_t {20});

        const ExtensionFilter filter({u".TXT"_s, u"*.md"_s, u"verylongextension"_s});
        QVERIFY(filter.matches("Notes.txt", 3) && filter.matches("README.MD", 2));
        QVERIFY(filter.matches("a.VeryLongExtension", 17));
        QVERIFY(!filter.matches("a.tx", 2) && !filter.matches("a.txt", 0) && !filter.matches("a.markdown", 8));
    }

    // What a content scan passes over, and counts (ContentFilter::classify,
    // OtherTextTypes): the kinds of text the list leaves out, and the files
    // of listed types in system and program folders.
    void contentLeftOut()
    {
        const OtherTextTypes other({u"txt"_s, u".md"_s, u"*.js"_s, u"JSON"_s});
        QVERIFY(other.count() > 50);
        QVERIFY(other.indexOf("App.vue", 3) >= 0 && other.indexOf("Main.KT", 2) >= 0 && other.indexOf("x.rst", 3) >= 0);
        QCOMPARE(other.extension(static_cast<std::size_t>(other.indexOf("App.vue", 3))), u"vue"_s);
        QCOMPARE(other.indexOf("notes.md", 2), -1); // listed
        QCOMPARE(other.indexOf("app.js", 2), -1);
        QCOMPARE(other.indexOf("data.json", 4), -1);
        QCOMPARE(other.indexOf("setup.exe", 3), -1); // not text
        QCOMPARE(other.indexOf("report.docx", 4), -1); // a document: read under its own setting
        QCOMPARE(other.indexOf("x.dat", 3), -1); // may hold anything
        QCOMPARE(other.indexOf("README", 0), -1);
        QVERIFY(other.extensions().contains(u"swift"_s) && !other.extensions().contains(u"txt"_s));
        QCOMPARE(other.extensions().size(), static_cast<qsizetype>(other.count()));

        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId program = index.add(root, "Program Files", EntryFlag::Directory | EntryFlag::LowPriority);
        const EntryId own = index.add(root, "notes.txt", 0);
        const EntryId code = index.add(root, "App.vue", 0);
        const EntryId system = index.add(program, "readme.txt", EntryFlag::LowPriority);
        const EntryId systemCode = index.add(program, "lib.kt", EntryFlag::LowPriority);
        const EntryId lock = index.add(root, "~$notes.txt", 0);
        const EntryId bare = index.add(root, "README", 0);
        ContentFilter filter;
        filter.extensions = ExtensionFilter({u"txt"_s});
        std::string_view name;
        std::size_t extLength = 0;
        const auto verdict = [&](EntryId id) { return filter.classify(index, index.entry(id), name, extLength); };
        QCOMPARE(verdict(own), ContentVerdict::Read);
        QCOMPARE(name, std::string_view("notes.txt"));
        QCOMPARE(extLength, std::size_t {3});
        QCOMPARE(verdict(code), ContentVerdict::OtherType);
        QVERIFY(other.indexOf(name, extLength) >= 0);
        QCOMPARE(verdict(system), ContentVerdict::LowPriority);
        QCOMPARE(verdict(systemCode), ContentVerdict::OtherType); // its type first: the one-off over types would not read it either
        QCOMPARE(verdict(lock), ContentVerdict::Never);
        QCOMPARE(verdict(bare), ContentVerdict::Never);
        QCOMPARE(verdict(program), ContentVerdict::Never);
        QVERIFY(filter.accepts(index, index.entry(own)) && !filter.accepts(index, index.entry(system)));
        filter.includeLowPriority = true; // asked for: read as any other
        QCOMPARE(verdict(system), ContentVerdict::Read);
        QVERIFY(filter.accepts(index, index.entry(system)));
    }


    void contentIndex()
    {
        QTemporaryDir dir;
        ContentIndex content(dir.path());
        QVERIFY(content.add(10, gramsOf(u"季度报告"_s), false, 1, 0));
        QVERIFY(content.add(20, gramsOf(u"年度报告和计划"_s), false, 2, 0));
        QVERIFY(content.add(30, {}, true, 3, 0, {}, true)); // too large: nothing to find in it
        QVERIFY(content.add(5, gramsOf(u"English only"_s), false, 4, 0));

        const auto lookup = content.lookup(u"报告"_s);
        QVERIFY(lookup.usable && lookup.decisive);
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 10, 20, 30}));
        QCOMPARE(lookup.large, (std::vector<EntryId> {30}));
        QCOMPARE(matchesOf(content, u"报告"_s), (std::vector<EntryId> {10, 20}));
        QCOMPARE(matchesOf(content, u"季度报告"_s), (std::vector<EntryId> {10}));
        QVERIFY(!content.lookup(u"季度报告"_s).decisive);
        QCOMPARE(matchesOf(content, u"度报"_s), (std::vector<EntryId> {10, 20}));
        QVERIFY(matchesOf(content, u"计划 report"_s).empty()); // "report" is not in it
        QVERIFY(matchesOf(content, u"中文"_s).empty());
        QCOMPARE(matchesOf(content, u"ENGLISH"_s), (std::vector<EntryId> {5}));
        QCOMPARE(matchesOf(content, u"lish onl"_s), (std::vector<EntryId> {5}));
        QVERIFY(matchesOf(content, u"englishman"_s).empty());
        QVERIFY(content.lookup(u"only"_s).usable && !content.lookup(u"only"_s).decisive);
        QVERIFY(!content.lookup(u"on"_s).usable); // too short to look up

        // Several terms: every one, anywhere.
        const auto both = [&](const QStringList& terms) {
            std::vector<EntryId> out;
            for (const std::uint32_t k : content.lookup(terms).known) {
                if (k & ContentIndex::Lookup::kMatch)
                    out.push_back(k & ~ContentIndex::Lookup::kMatch);
            }
            return out;
        };
        QCOMPARE(both({u"计划"_s, u"年度"_s}), (std::vector<EntryId> {20}));
        QCOMPARE(both({u"报告"_s, u"english"_s}), (std::vector<EntryId> {}));
        QCOMPARE(both({u"english"_s, u"only"_s}), (std::vector<EntryId> {5}));
        QVERIFY(content.lookup(QStringList {u"报告"_s, u"计划"_s}).decisive);
        QVERIFY(!content.lookup(QStringList {u"报告"_s, u"only"_s}).decisive);

        // A file written to is not known until it is read again; one written
        // to while it is being read stays unknown.
        content.markChanged(std::vector<EntryId> {10});
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 20, 30}));
        content.beginReads();
        const std::uint64_t since = content.changeSequence();
        content.markChanged(std::vector<EntryId> {20});
        QVERIFY(content.add(20, gramsOf(u"新的计划"_s), false, 5, since));
        content.endReads();
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 30}));
        QVERIFY(content.add(10, gramsOf(u"报告已更新"_s), false, 6, content.changeSequence()));
        QCOMPARE(matchesOf(content, u"更新"_s), (std::vector<EntryId> {10}));
        const auto docs = content.documents();
        QCOMPARE(docs.size(), std::size_t {4});
        QVERIFY(docs[2].entry == 20 && docs[2].need == ContentIndex::Need::Read);
        QVERIFY(docs[1].entry == 10 && docs[1].need == ContentIndex::Need::None && docs[1].stamp == 6);

        // Unsure (the journal was lost): checked, then current again.
        content.markUnsure([](EntryId e) { return e == 5; });
        QCOMPARE(content.documents()[0].need, ContentIndex::Need::Check);
        content.confirm(5);
        QCOMPARE(content.documents()[0].need, ContentIndex::Need::None);

        // Into a segment file; compaction renumbers; saved and restored.
        QVERIFY(content.merge());
        QCOMPARE(content.stats().segments, std::size_t {1});
        QCOMPARE(content.stats().memoryPairs, std::size_t {0});
        QCOMPARE(matchesOf(content, u"报告"_s), (std::vector<EntryId> {10}));
        content.remap([](EntryId e) { return e == 5 ? kNoEntry : e - 1; });
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {9, 29}));
        QCOMPARE(content.lookup(u"报告"_s).large, (std::vector<EntryId> {29}));
        QCOMPARE(matchesOf(content, u"已更新"_s), (std::vector<EntryId> {9}));
        content.retire(std::vector<EntryId> {29});
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {9}));
        QVERIFY(content.lookup(u"报告"_s).large.empty());

        std::vector<EntryId> newIds(40, kNoEntry);
        newIds[9] = 1;
        newIds[19] = 2;
        std::vector<std::uint64_t> segments;
        const std::vector<char> state = content.serialize(newIds, segments);
        QCOMPARE(segments.size(), std::size_t {1});
        ContentIndex restored(dir.path());
        QVERIFY(restored.restore(state, 3));
        QCOMPARE(matchesOf(restored, u"报告"_s), (std::vector<EntryId> {1}));
        QCOMPARE(restored.documents().size(), std::size_t {2});
        QCOMPARE(restored.documents()[1].need, ContentIndex::Need::Read); // still dirty
        QVERIFY(restored.add(2, gramsOf(u"计划书"_s), false, 7, restored.changeSequence()));
        QCOMPARE(matchesOf(restored, u"计划"_s), (std::vector<EntryId> {2}));

        // A document too large for the size limit stays so when saved and
        // read back, and of the empty documents it alone is read again when
        // the limit goes up. A state of the version before had not recorded
        // which were too large: read back, every empty document is read
        // again, and the state is to be saved anew.
        QVERIFY(restored.add(0, {}, true, 8, 0, {}, true));
        QCOMPARE(restored.lookup(u"报告"_s).large, (std::vector<EntryId> {0}));
        QCOMPARE(knownOf(restored, u"报告"_s), (std::vector<EntryId> {0, 1, 2}));
        const std::vector<EntryId> threeIds {0, 1, 2};
        std::vector<std::uint64_t> moreSegments;
        std::vector<char> saved = restored.serialize(threeIds, moreSegments);
        ContentIndex reread(dir.path());
        QVERIFY(reread.restore(saved, 3));
        QVERIFY(!reread.takeChanged());
        QCOMPARE(reread.lookup(u"报告"_s).large, (std::vector<EntryId> {0}));
        QVERIFY(reread.add(1, {}, true, 9, 0)); // nothing in it, and within the limit
        reread.markEmptyChanged();
        QCOMPARE(reread.documents()[0].need, ContentIndex::Need::Read);
        QCOMPARE(reread.documents()[1].need, ContentIndex::Need::None);
        QCOMPARE(reread.documents()[2].need, ContentIndex::Need::None);
        QVERIFY(reread.lookup(u"报告"_s).large.empty()); // dirty: not known until read again
        saved[0] = 7; // the version, in its first byte
        ContentIndex older(dir.path());
        QVERIFY(older.restore(saved, 3));
        QVERIFY(older.takeChanged());
        QCOMPARE(older.documents()[0].need, ContentIndex::Need::Read);
        QCOMPARE(older.documents()[1].need, ContentIndex::Need::None);
        QCOMPARE(older.documents()[2].need, ContentIndex::Need::None);
        QVERIFY(older.lookup(u"报告"_s).large.empty());
        saved[0] = 6;
        QVERIFY(!ContentIndex(dir.filePath(u"older"_s)).restore(saved, 3)); // older still: not read

        // Enough grams for segments of their own, and a merge of them all.
        ContentIndex big(dir.filePath(u"big"_s));
        std::vector<grams::Key> keys(1001);
        for (EntryId doc = 0; doc < 2500; ++doc) {
            for (std::size_t i = 0; i < 1000; ++i)
                keys[i] = ((grams::Key {0x4E00} + (doc % 7)) << grams::kCharBits) | (0x4E00 + i);
            keys[1000] = (grams::Key {0x6000} + doc) << grams::kCharBits; // a character of its own: no copies
            QVERIFY(big.add(doc * 2, keys, false, 0, 0));
        }
        QCOMPARE(big.stats().segments, std::size_t {2});
        const QString pair = QString(QChar(0x4E00 + 3)) + QChar(0x4E00 + 5); // docs where doc % 7 == 3
        QCOMPARE(matchesOf(big, pair).size(), std::size_t {357});
        big.retire(std::vector<EntryId> {6, 20});
        QVERIFY(big.merge());
        QCOMPARE(big.stats().segments, std::size_t {1});
        const std::vector<EntryId> found = matchesOf(big, pair);
        QCOMPARE(found.size(), std::size_t {355});
        QCOMPARE(found.front(), EntryId {34});
        QVERIFY(std::all_of(found.begin(), found.end(), [](EntryId e) { return (e / 2) % 7 == 3; }));

        // Segments of a level merge into one of the next level as they come.
        ContentIndex tiers(dir.filePath(u"tiers"_s));
        const auto abc = [](std::uint32_t i) { // "000", "004", ... "zz_"
            constexpr char kWord[] = "0123456789_abcdefghijklmnopqrstuvwxyz";
            const std::uint32_t t = i * 4;
            return (grams::Key {static_cast<unsigned char>(kWord[t / (37 * 37)])} << grams::kCharBits)
                | (static_cast<unsigned char>(kWord[t / 37 % 37]) << 8) | static_cast<unsigned char>(kWord[t % 37]);
        };
        const auto tierKeys = [&](EntryId doc) {
            std::vector<grams::Key> many;
            for (std::size_t i = 0; i < 5000; ++i)
                many.push_back(((grams::Key {0x4E00} + (doc % 5)) << grams::kCharBits) | (0x4E00 + i));
            if (doc % 10 == 4) { // most trigrams
                for (std::uint32_t i = 0; i < 10500; ++i)
                    many.push_back(abc(i));
            } else if (doc % 10 == 5) {
                many.push_back(abc(2)); // "008"
            }
            many.push_back((grams::Key {0x6000} + doc) << grams::kCharBits); // no copies
            std::sort(many.begin(), many.end());
            return many;
        };
        for (EntryId doc = 0; doc < 1700; ++doc) {
            QVERIFY(tiers.add(doc, tierKeys(doc), false, 0, 0));
            if (doc == 3)
                tiers.retire(std::vector<EntryId> {2});
            if (tiers.needsMerge())
                QVERIFY(tiers.mergeDue());
        }
        QCOMPARE(tiers.stats().segments, std::size_t {2}); // nine written from memory, eight of them merged
        QVERIFY(tiers.stats().memoryPairs > 0);
        const QString twos = QString(QChar(0x4E00 + 2)) + QChar(0x4E00 + 7); // docs where doc % 5 == 2
        std::vector<EntryId> expected;
        for (EntryId doc = 7; doc < 1700; doc += 5)
            expected.push_back(doc);
        QCOMPARE(matchesOf(tiers, twos), expected);
        std::vector<EntryId> fours; // docs where doc % 10 is 4 or 5
        for (EntryId doc = 4; doc < 1700; doc += 10) {
            fours.push_back(doc);
            fours.push_back(doc + 1);
        }
        QCOMPARE(matchesOf(tiers, u"008"_s), fours);
        QVERIFY(matchesOf(tiers, u"009"_s).empty());
        QVERIFY(tiers.merge());
        QCOMPARE(matchesOf(tiers, twos), expected);
        QCOMPARE(matchesOf(tiers, u"008"_s), fours);

        // Small segments merge while documents come (the indexer's readers
        // go on meanwhile); then everything, on several threads.
        ContentIndex busy(dir.filePath(u"busy"_s));
        {
            std::atomic<bool> adding {true};
            std::jthread merger([&] {
                while (adding.load()) {
                    busy.mergeTails();
                    std::this_thread::yield();
                }
            });
            for (EntryId doc = 0; doc < 1700; ++doc) {
                QVERIFY(busy.add(doc, tierKeys(doc), false, 0, 0));
                if (doc == 3)
                    busy.retire(std::vector<EntryId> {2});
            }
            adding = false;
        }
        QVERIFY(busy.mergeTails());
        QCOMPARE(busy.stats().segments, std::size_t {2});
        QCOMPARE(matchesOf(busy, twos), expected);
        QCOMPARE(matchesOf(busy, u"008"_s), fours);
        QVERIFY(busy.merge(4));
        QCOMPARE(busy.stats().segments, std::size_t {1});
        QCOMPARE(matchesOf(busy, twos), expected);
        QCOMPARE(matchesOf(busy, u"008"_s), fours);
        QVERIFY(matchesOf(busy, u"009"_s).empty());

        // Documents added on several threads at once, each found as soon as
        // it is in, while segments are written (outside the lock) and merged.
        ContentIndex crowd(dir.filePath(u"crowd"_s));
        {
            std::atomic<bool> adding {true};
            std::atomic<int> missed {0};
            std::jthread merger([&] {
                while (adding.load()) {
                    crowd.mergeTails();
                    std::this_thread::yield();
                }
            });
            {
                std::vector<std::jthread> adders;
                for (EntryId first = 0; first < 4; ++first) {
                    adders.emplace_back([&, first] {
                        for (EntryId doc = first; doc < 1700; doc += 4) {
                            const QString own(QChar(char16_t(0x6000 + doc))); // its character of its own
                            if (!crowd.add(doc, tierKeys(doc), false, 0, 0)
                                || matchesOf(crowd, own) != std::vector<EntryId> {doc})
                                ++missed;
                        }
                    });
                }
            } // joins
            adding = false;
            QCOMPARE(missed.load(), 0);
        }
        crowd.retire(std::vector<EntryId> {2}); // as above
        QVERIFY(crowd.mergeTails());
        QCOMPARE(crowd.stats().documents, std::size_t {1699});
        QCOMPARE(matchesOf(crowd, twos), expected);
        QCOMPARE(matchesOf(crowd, u"008"_s), fours);
        QVERIFY(crowd.merge(4));
        QCOMPARE(matchesOf(crowd, twos), expected);
        QCOMPARE(matchesOf(crowd, u"008"_s), fours);

        // Postings of every density, and documents with most trigrams, give
        // what the grams say: in memory, written out, merged, restored.
        ContentIndex coded(dir.filePath(u"coded"_s));
        constexpr char kWord[] = "0123456789_abcdefghijklmnopqrstuvwxyz";
        const auto trigram = [&](std::uint32_t t) {
            t %= 37 * 37 * 37;
            return (grams::Key {static_cast<unsigned char>(kWord[t / (37 * 37)])} << grams::kCharBits)
                | (static_cast<unsigned char>(kWord[t / 37 % 37]) << 8) | static_cast<unsigned char>(kWord[t % 37]);
        };
        const auto spread = [](std::uint32_t x) { return (x * 0x9E37'79B9u) >> 7; };
        constexpr grams::Key kOne = grams::Key {0x4E00} << grams::kCharBits; // "一": about half the documents
        constexpr grams::Key kTwo = grams::Key {0x4E01} << grams::kCharBits; // "丁": about a tenth
        constexpr grams::Key kRun = grams::Key {0x4E03} << grams::kCharBits; // "七": 1000 to 1999
        std::vector<std::vector<grams::Key>> docKeys(3000);
        for (EntryId doc = 0; doc < docKeys.size(); ++doc) {
            std::vector<grams::Key>& k = docKeys[doc];
            if (doc % 25 == 7) { // dense: 10500 trigrams
                for (std::uint32_t i = 0; i < 10500; ++i)
                    k.push_back(trigram(doc * 7919 + i * 3));
            } else {
                for (std::uint32_t i = 0; i < 10; ++i)
                    k.push_back(trigram(doc * 31 + i * 101));
            }
            if (spread(doc) % 2 == 0)
                k.push_back(kOne);
            if (spread(doc) % 10 == 3)
                k.push_back(kTwo);
            if (doc >= 1000 && doc < 2000)
                k.push_back(kRun);
            k.push_back(((grams::Key {0x4E10} + doc % 50) << grams::kCharBits) | 0x4E20);
            std::sort(k.begin(), k.end());
            k.erase(std::unique(k.begin(), k.end()), k.end());
            QVERIFY(coded.add(doc, k, false, 0, 0));
        }
        QCOMPARE(coded.stats().segments, std::size_t {1}); // the dense documents filled one
        QVERIFY(coded.stats().memoryPairs > 0);
        std::vector<QString> phrases = {u"一"_s, u"丁"_s, u"七"_s, u"一丁"_s};
        for (std::uint32_t t = 0; t < 37 * 37 * 37; t += 401) {
            const grams::Key key = trigram(t);
            phrases.push_back(QString(QChar(char16_t(key >> grams::kCharBits))) + QChar(char16_t((key >> 8) & 0xFF))
                + QChar(char16_t(key & 0xFF)));
        }
        phrases.push_back(QString(QChar(0x4E10 + 7)) + QChar(0x4E20));
        const auto check = [&](const ContentIndex& content, const std::vector<bool>& gone) {
            for (const QString& phrase : phrases) {
                const std::vector<grams::Key> wanted = grams::ofPhrase(phrase);
                std::vector<EntryId> expected;
                for (EntryId doc = 0; doc < docKeys.size(); ++doc) {
                    if (!gone[doc]
                        && std::includes(docKeys[doc].begin(), docKeys[doc].end(), wanted.begin(), wanted.end()))
                        expected.push_back(doc);
                }
                if (matchesOf(content, phrase) != expected)
                    return false;
            }
            return true;
        };
        std::vector<bool> gone(docKeys.size(), false);
        QVERIFY(check(coded, gone));
        QVERIFY(coded.merge());
        QVERIFY(check(coded, gone));
        // On several threads, stretches of keys each: the same file.
        ContentIndex twin(dir.filePath(u"twin"_s));
        for (EntryId doc = 0; doc < docKeys.size(); ++doc)
            QVERIFY(twin.add(doc, docKeys[doc], false, 0, 0));
        QVERIFY(twin.merge(4));
        QVERIFY(check(twin, gone));
        const auto segmentBytes = [](const ContentIndex& content) {
            QFile file(content.segmentPaths().front());
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QVERIFY(!segmentBytes(coded).isEmpty());
        QCOMPARE(segmentBytes(twin), segmentBytes(coded));
        std::vector<EntryId> retired;
        for (EntryId doc = 0; doc < docKeys.size(); doc += 3) {
            retired.push_back(doc);
            gone[doc] = true;
        }
        coded.retire(retired);
        QVERIFY(check(coded, gone));
        QVERIFY(coded.merge());
        QVERIFY(check(coded, gone));
        std::vector<EntryId> same(docKeys.size());
        std::iota(same.begin(), same.end(), EntryId {0});
        std::vector<std::uint64_t> codedSegments;
        const std::vector<char> codedState = coded.serialize(same, codedSegments);
        ContentIndex reopened(dir.filePath(u"coded"_s));
        QVERIFY(reopened.restore(codedState, docKeys.size()));
        QVERIFY(check(reopened, gone));

        // Copies of a file share one content: in memory, written out, saved
        // and restored; it goes once the last copy is gone.
        ContentIndex copies(dir.filePath(u"copies"_s));
        const std::vector<grams::Key> report = gramsOf(u"季度报告 quarterly report"_s);
        QVERIFY(copies.add(1, report, false, 0, 0));
        QVERIFY(copies.add(2, report, false, 0, 0));
        QVERIFY(copies.add(3, gramsOf(u"年度计划"_s), false, 0, 0));
        QCOMPARE(copies.stats().contents, std::size_t {2});
        QCOMPARE(matchesOf(copies, u"报告"_s), (std::vector<EntryId> {1, 2}));
        QVERIFY(copies.merge());
        QVERIFY(copies.add(4, report, false, 0, 0)); // found in the segment
        QCOMPARE(copies.stats().contents, std::size_t {2});
        QCOMPARE(copies.stats().memoryPairs, std::size_t {0});
        copies.retire(std::vector<EntryId> {1, 2});
        QVERIFY(copies.merge());
        QCOMPARE(matchesOf(copies, u"quarterly"_s), (std::vector<EntryId> {4}));
        std::vector<EntryId> fiveIds(5);
        std::iota(fiveIds.begin(), fiveIds.end(), EntryId {0});
        std::vector<std::uint64_t> copySegments;
        ContentIndex reloaded(dir.filePath(u"copies"_s));
        QVERIFY(reloaded.restore(copies.serialize(fiveIds, copySegments), 5));
        QVERIFY(reloaded.add(1, report, false, 0, 0));
        QCOMPARE(reloaded.stats().contents, std::size_t {2});
        QCOMPARE(matchesOf(reloaded, u"季度"_s), (std::vector<EntryId> {1, 4}));
        reloaded.retire(std::vector<EntryId> {1, 4});
        QCOMPARE(reloaded.stats().contents, std::size_t {1});
        QVERIFY(reloaded.merge());
        QVERIFY(matchesOf(reloaded, u"季度"_s).empty());
        QVERIFY(reloaded.add(2, report, false, 0, 0)); // a content of its own again, in memory
        QCOMPARE(matchesOf(reloaded, u"季度"_s), (std::vector<EntryId> {2}));
        ContentIndex again(dir.filePath(u"copies"_s));
        QVERIFY(again.restore(reloaded.serialize(fiveIds, copySegments), 5));
        QVERIFY(again.stats().memoryPairs > 0);
        QVERIFY(again.add(4, report, false, 0, 0)); // the one in memory, known from its grams
        QCOMPARE(again.stats().contents, std::size_t {2});
        QCOMPARE(again.stats().memoryPairs, report.size());
        QCOMPARE(matchesOf(again, u"季度报告"_s), (std::vector<EntryId> {2, 4}));
        QVERIFY(again.merge());
        QCOMPARE(matchesOf(again, u"report"_s), (std::vector<EntryId> {2, 4}));
        QCOMPARE(matchesOf(again, u"计划"_s), (std::vector<EntryId> {3}));

        // State that does not fit the snapshot's entries is not restored.
        ContentIndex wrong(dir.filePath(u"other"_s));
        QVERIFY(!wrong.restore(state, 3)); // its segment is in another folder
        QVERIFY(!wrong.restore({}, 0));
        QCOMPARE(wrong.stats().documents, std::size_t {0});
    }

    void contentIndexerReadsFiles()
    {
        QTemporaryDir dir;
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return std::wstring();
            f.write(data);
            return QDir::toNativeSeparators(f.fileName()).toStdWString();
        };
        const std::wstring utf8 = write(u"a.txt"_s, u"第一行\n合同条款"_s.toUtf8());
        const auto text = ContentIndexer::readFile(utf8, 0, {});
        QCOMPARE(text.outcome, ContentIndexer::Outcome::Indexed);
        QVERIFY(hasAll(text.keys, u"合同条款"_s) && !hasAll(text.keys, u"行合"_s));
        const auto stamp = ContentIndexer::stampOf(utf8);
        QVERIFY(stamp && *stamp == text.stamp);

        const std::wstring bom = write(u"b.txt"_s, utf16le(u"合同"_s, true));
        QVERIFY(hasAll(ContentIndexer::readFile(bom, 0, {}).keys, u"合同"_s));
        const auto tooLarge = ContentIndexer::readFile(utf8, 4, {});
        QCOMPARE(tooLarge.outcome, ContentIndexer::Outcome::Empty); // too large
        QVERIFY(tooLarge.large);
        const auto nothing = ContentIndexer::readFile(write(u"c.txt"_s, {}), 0, {});
        QCOMPARE(nothing.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(!nothing.large);
        QCOMPARE(ContentIndexer::readFile(utf8 + L".missing", 0, {}).outcome, ContentIndexer::Outcome::Skipped);
        QVERIFY(!ContentIndexer::stampOf(utf8 + L".missing"));

        // A search's read: the match, and the same grams as the indexer's,
        // the rest of the file read after the match (here: chunks later).
        QByteArray big = u"合同 第一条\n"_s.toUtf8();
        big += QByteArray(static_cast<qsizetype>(ContentScanner::kChunkBytes) * 2, 'x');
        big += u"\n季度报告"_s.toUtf8();
        const std::wstring large = write(u"d.txt"_s, big);
        const ContentScanner contract(u"合同"_s);
        const auto scanned = ContentIndexer::scanFile(large, 0, contract, {});
        QVERIFY(scanned.match && scanned.match->line == 1);
        QCOMPARE(scanned.text.outcome, ContentIndexer::Outcome::Indexed);
        const auto whole = ContentIndexer::readFile(large, 0, {});
        QCOMPARE(scanned.text.keys, whole.keys);
        QCOMPARE(scanned.text.stamp, whole.stamp);
        QVERIFY(hasAll(scanned.text.keys, u"季度报告"_s));
        const auto none = ContentIndexer::scanFile(utf8, 0, ContentScanner(u"nowhere"_s), {});
        QVERIFY(!none.match);
        QCOMPARE(none.text.keys, text.keys);
        const auto ansi = write(u"e.txt"_s, QByteArray("plain \xB0\xA1 GBK"));
        QCOMPARE(ContentIndexer::scanFile(ansi, 0, ContentScanner(u"\U0001F600"_s), {}).text.keys,
            ContentIndexer::readFile(ansi, 0, {}).keys); // the phrase cannot be in it: read anyway
        const auto overLimit = ContentIndexer::scanFile(utf8, 4, contract, {});
        QCOMPARE(overLimit.text.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(overLimit.text.large && !overLimit.match);
        QVERIFY(!scanned.text.large);
        const auto cut = ContentIndexer::scanFile(large, 0, ContentScanner(u"nowhere"_s), [] { return true; });
        QCOMPARE(cut.text.outcome, ContentIndexer::Outcome::Skipped);
    }

    // What a search reads goes into the index, unless the indexer is off,
    // paused, or the file is on a volume it does not follow.
    void contentIntake()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = QDir::toNativeSeparators(tmp.path());
        const std::string rootName = wtf8::fromUtf16(wtf8::view(root));
        auto index = std::make_shared<FileIndex>();
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        bool ready = false;
        source.ready = [&] { return ready; };
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, {}, {1h, 1h, 1h, 0ms}); // no pass of its own
        const std::vector<grams::Key> keys = grams::ofPhrase(u"合同条款"_s);
        const ContentIndexer::FileText text {ContentIndexer::Outcome::Indexed, keys, 7};

        {
            ContentIndexer::Intake intake(&indexer);
            QVERIFY(!intake.wants((root + u"\\a.txt"_s).toStdWString())); // the file index is not complete
        }
        ready = true;
        {
            ContentIndexer::Intake intake(&indexer);
            QVERIFY(intake.wants((root + u"\\a.txt"_s).toStdWString()));
            QVERIFY(!intake.wants(L"Q:\\a.txt"));
            QVERIFY(!intake.wants(root.toStdWString())); // the root itself is no file
            intake.take(1, text, intake.since());
            intake.take(2, {ContentIndexer::Outcome::Empty, {}, 8}, intake.since());
            intake.take(3, {}, intake.since()); // Skipped: not taken
            ContentIndexer::FileText tooLarge;
            tooLarge.outcome = ContentIndexer::Outcome::Empty;
            tooLarge.large = true;
            tooLarge.stamp = 9;
            intake.take(6, tooLarge, intake.since());
        }
        QCOMPARE(content->stats().documents, std::size_t {3});
        QVERIFY(content->knows(1) && content->knows(2) && !content->knows(3) && content->knows(6));
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {1});
        QCOMPARE(knownOf(*content, u"x"_s), (std::vector<EntryId> {1, 2, 6})); // no grams, yet all known
        QCOMPARE(content->lookup(u"x"_s).large, (std::vector<EntryId> {6})); // ... and one for its size

        // Written to while the search read it: taken, but dirty.
        {
            ContentIndexer::Intake intake(&indexer);
            const std::uint64_t since = intake.since();
            content->markChanged(std::vector<EntryId> {4});
            intake.take(4, text, since);
        }
        QVERIFY(!content->knows(4));

        indexer.pause();
        {
            ContentIndexer::Intake intake(&indexer);
            QVERIFY(!intake.wants((root + u"\\a.txt"_s).toStdWString()));
        }
        indexer.resume();
        {
            ContentIndexer::Intake intake(&indexer);
            const std::uint64_t since = intake.since();
            indexer.pause(); // while the search runs
            intake.take(5, text, since);
            indexer.resume();
        }
        QVERIFY(!content->knows(5));
    }

    void contentIndexerFollowsFiles()
    {
        // A volume whose root is a real folder, so the indexer reads real files.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto write = [&](const QString& name, const QString& text) {
            QFile f(tmp.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(text.toUtf8()) >= 0;
        };
        QVERIFY(write(u"a.txt"_s, u"合同条款"_s) && write(u"b.md"_s, u"English"_s) && write(u"c.log"_s, u"季度报告"_s)
            && write(u"d.bin"_s, u"合同"_s));
        const std::string rootName = wtf8::fromUtf16(wtf8::view(QDir::toNativeSeparators(tmp.path())));
        auto index = std::make_shared<FileIndex>();
        EntryId a = kNoEntry;
        EntryId b = kNoEntry;
        EntryId c = kNoEntry;
        {
            auto lock = index->writeLock();
            const EntryId root = index->addRoot(rootName);
            a = index->add(root, "a.txt", 0);
            b = index->add(root, "b.md", 0);
            c = index->add(root, "c.log", 0);
            index->add(root, "d.bin", 0);
        }
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        source.ready = [] { return true; };
        ContentIndexer::Options options;
        options.extensions = {u"txt"_s, u"md"_s, u"log"_s};
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, options, {0ms, 0ms, 1h, 0ms});
        const auto waitFor = [](const auto& done) {
            for (int i = 0; i < 1000 && !done(); ++i)
                std::this_thread::sleep_for(10ms);
            return done();
        };
        QVERIFY(waitFor([&] { return content->stats().documents == 3; }));
        QCOMPARE(matchesOf(*content, u"合同"_s), std::vector<EntryId> {a});
        QCOMPARE(knownOf(*content, u"报告"_s), (std::vector<EntryId> {a, b, c}));
        QCOMPARE(matchesOf(*content, u"english"_s), std::vector<EntryId> {b});

        // Written to (the journal says so): read again.
        QVERIFY(write(u"a.txt"_s, u"新的内容"_s));
        content->markChanged(std::vector<EntryId> {a});
        QVERIFY(waitFor([&] { return matchesOf(*content, u"内容"_s) == std::vector<EntryId> {a}; }));
        QVERIFY(matchesOf(*content, u"合同"_s).empty());

        // Files a search no longer looks in lose their documents.
        options.extensions = {u"txt"_s, u"log"_s};
        indexer.setOptions(options);
        QVERIFY(waitFor([&] { return content->stats().documents == 2; }));
        QCOMPARE(knownOf(*content, u"报告"_s), (std::vector<EntryId> {a, c}));
        options.enabled = false;
        indexer.setOptions(options);
        QCOMPARE(content->stats().documents, std::size_t {0});
    }

    // Folders that directly hold many candidates, nearly all of one
    // extension, are read last; nothing else is.
    void contentCrowdedFolders()
    {
        FileIndex index;
        std::vector<EntryId> candidates;
        EntryId crowded = kNoEntry, edge = kNoEntry;
        {
            auto lock = index.writeLock();
            const EntryId root = index.addRoot("C:");
            const auto folder = [&](const char* name, std::vector<std::pair<std::string, int>> files) {
                const EntryId f = index.add(root, name, EntryFlag::Directory);
                for (const auto& [ext, count] : files) {
                    for (int i = 0; i < count; ++i)
                        candidates.push_back(index.add(f, ext + std::to_string(i) + "." + ext, 0));
                }
                return f;
            };
            crowded = folder("crowded", {{"json", 300}, {"JSON", 300}}); // one extension, as a search sees it
            edge = folder("edge", {{"json", 540}, {"txt", 60}}); // 90%
            folder("mixed", {{"json", 500}, {"txt", 50}, {"xml", 50}}); // 83%
            folder("almost", {{"json", 539}, {"txt", 61}}); // just under 90%
            folder("few", {{"json", 499}}); // all one, but too few
            // Many of one extension, but in subfolders: none holds enough directly.
            const EntryId tree = folder("tree", {});
            for (int s = 0; s < 3; ++s) {
                const EntryId sub = index.add(tree, "s" + std::to_string(s), EntryFlag::Directory);
                for (int i = 0; i < 300; ++i)
                    candidates.push_back(index.add(sub, "f" + std::to_string(i) + ".html", 0));
            }
            candidates.push_back(index.add(root, "notes.txt", 0));
        }
        std::sort(candidates.begin(), candidates.end());
        QCOMPARE(ContentIndexer::crowdedFolders(index, candidates), (std::vector<EntryId> {crowded, edge}));
        QVERIFY(ContentIndexer::crowdedFolders(index, {}).empty());

        // Crowded goes after everything else, hidden files and the profile's too.
        using O = ContentIndexer::Order;
        QCOMPARE(ContentIndexer::orderOf(true, false, false), O::kProfile);
        QCOMPARE(ContentIndexer::orderOf(false, false, false), O::kOther);
        QCOMPARE(ContentIndexer::orderOf(true, true, false), O::kHidden);
        QCOMPARE(ContentIndexer::orderOf(true, false, true), O::kCrowded);
        QCOMPARE(ContentIndexer::orderOf(false, true, true), O::kCrowded);
        QVERIFY(O::kKnown < O::kProfile && O::kProfile < O::kOther && O::kOther < O::kHidden && O::kHidden < O::kCrowded);
    }

    // ---- documents -------------------------------------------------------------

    void extractWriter()
    {
        using doctext::PlaceKind;
        doctext::DocText doc;
        extract::Writer w(doc, 1000);
        w.put(u"  a  \t b ");
        w.newline();
        w.newline();
        const std::uint16_t sheet = w.addSheet("S");
        w.place(PlaceKind::Row, 3, sheet);
        w.putUtf8("x");
        w.newline();
        w.place(PlaceKind::Row, 4, sheet); // follows on: no new place
        w.putUtf8("y");
        w.newline();
        w.place(PlaceKind::Row, 7, sheet);
        w.putUtf8("z");
        w.put(char32_t {0xFEFF});
        w.put(char32_t {0x200B});
        w.put(char32_t {0xA0});
        w.putUtf8("w\xFF");
        w.finish();
        QCOMPARE(doc.text, std::string("a\tb\nx\ny\nz w\xEF\xBF\xBD\n"));
        QCOMPARE(doc.places.size(), std::size_t {2});
        QVERIFY((doc.places[0] == doctext::Place {2, 3, sheet, PlaceKind::Row}));
        QVERIFY((doc.places[1] == doctext::Place {4, 7, sheet, PlaceKind::Row}));
        QCOMPARE(doctext::locate(doc, 3).number, std::uint32_t {4});
        QCOMPARE(doctext::locate(doc, 1).kind, PlaceKind::None);

        // Cut at the limit, on a whole character.
        doctext::DocText small;
        extract::Writer s(small, 8);
        s.putUtf8("合同合同合同");
        s.finish();
        QVERIFY(small.truncated);
        QCOMPARE(small.text, std::string("合同\n"));
    }

    void extractXml()
    {
        const std::string xml = "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY e \"boom\">]><!-- c > -->"
                                "<w:doc a='1>2' w:b=\"&lt;&#x4E2D;&#25991;\" r:id=\"rId9\" id=\"7\">"
                                "<t>A&amp;B &e; &#xZZ;</t><![CDATA[<raw>&amp;]]><e/></w:doc>";
        for (const std::size_t chunk : {std::size_t {1}, std::size_t {3}, std::size_t {1} << 16}) {
            std::size_t at = 0;
            extract::XmlReader r([&](char* buffer, std::size_t capacity) {
                const std::size_t n = std::min({capacity, chunk, xml.size() - at});
                std::memcpy(buffer, xml.data() + at, n);
                at += n;
                return n;
            });
            QStringList events;
            std::string text;
            for (auto t = r.next(); t != extract::XmlReader::Token::Done; t = r.next()) {
                const QString name = QString::fromUtf8(r.name());
                if (t == extract::XmlReader::Token::Start) {
                    events.append(u'<' + name);
                    if (name == u"doc") {
                        QCOMPARE(r.attribute("a"), std::string("1>2"));
                        QCOMPARE(r.attribute("b"), std::string("<中文"));
                        QCOMPARE(r.attribute("id"), std::string("7"));
                        QCOMPARE(r.prefixedAttribute("id"), std::string("rId9"));
                    }
                } else if (t == extract::XmlReader::Token::End) {
                    events.append(u'/' + name);
                } else {
                    text += r.text();
                }
            }
            QCOMPARE(events, (QStringList {u"<doc"_s, u"<t"_s, u"/t"_s, u"<e"_s, u"/e"_s, u"/doc"_s}));
            QCOMPARE(text, std::string("A&B &e; &#xZZ;<raw>&amp;"));
            QVERIFY(!r.failed());
        }
    }

    void extractDocx()
    {
        const std::string rels
            = R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
              R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)";
        const std::string documentRels
            = R"(<Relationships><Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>)"
              R"(<Relationship Id="rId6" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="http://x" TargetMode="External"/></Relationships>)";
        const std::string body
            = R"(<w:document xmlns:w="w" xmlns:mc="mc"><w:body>)"
              R"(<w:p><w:r><w:t>合同</w:t></w:r><w:r><w:t xml:space="preserve">编号 </w:t></w:r><w:r><w:tab/><w:t>A-1</w:t></w:r></w:p>)"
              R"(<w:p><w:pPr><w:tabs><w:tab w:val="left" w:pos="720"/></w:tabs></w:pPr><w:r><w:fldChar w:fldCharType="begin"/></w:r>)"
              R"(<w:r><w:instrText> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>7</w:t></w:r>)"
              R"(<w:r><w:fldChar w:fldCharType="end"/></w:r><w:del><w:r><w:delText>删掉的</w:delText></w:r></w:del></w:p>)"
              R"(<w:tbl><w:tr><w:tc><w:p><w:r><w:t>甲</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>乙</w:t></w:r></w:p>)"
              R"(<w:p><w:r><w:t>丙</w:t></w:r></w:p></w:tc></w:tr></w:tbl>)"
              R"(<w:p><w:r><w:lastRenderedPageBreak/><w:t>第二页</w:t></w:r></w:p>)"
              R"(<w:p><w:r><w:t>长段落前半</w:t></w:r><w:r><w:lastRenderedPageBreak/><w:t>后半</w:t></w:r></w:p>)"
              R"(<w:p><mc:AlternateContent><mc:Choice><w:r><w:t>新</w:t></w:r></mc:Choice>)"
              R"(<mc:Fallback><w:r><w:t>旧</w:t></w:r></mc:Fallback></mc:AlternateContent></w:p>)"
              R"(</w:body></w:document>)";
        const QByteArray docx = makeZip({{"[Content_Types].xml", "<Types/>"}, {"_rels/.rels", rels},
            {"word/_rels/document.xml.rels", documentRels}, {"word/document.xml", body},
            {"word/header1.xml", "<w:hdr><w:p><w:r><w:t>页眉文字</w:t></w:r></w:p></w:hdr>"}});
        const Extracted out = extractBytes(docx);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text),
            (QStringList {u"page 1|合同编号\tA-1"_s, u"page 1|7"_s, u"page 1|甲\t乙 丙"_s, u"page 2|第二页"_s,
                u"page 2|长段落前半后半"_s, u"page 3|新"_s, u"-|页眉文字"_s}));
    }

    void extractXlsx()
    {
        const std::string rels
            = R"(<Relationships><Relationship Id="rId1" Type="http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument" Target="/xl/workbook.xml"/></Relationships>)";
        const std::string workbookRels
            = R"(<Relationships><Relationship Id="rId1" Type="x/worksheet" Target="worksheets/sheet1.xml"/>)"
              R"(<Relationship Id="rId2" Type="x/worksheet" Target="worksheets/sheet2.xml"/>)"
              R"(<Relationship Id="rId4" Type="x/chartsheet" Target="chartsheets/sheet1.xml"/>)"
              R"(<Relationship Id="rId3" Type="x/sharedStrings" Target="sharedStrings.xml"/></Relationships>)";
        const std::string workbook = R"(<workbook xmlns:r="r"><sheets><sheet name="销售" sheetId="1" r:id="rId1"/>)"
                                     R"(<sheet name="Chart" sheetId="3" r:id="rId4"/>)"
                                     R"(<sheet name="Hidden" sheetId="2" state="hidden" r:id="rId2"/></sheets></workbook>)";
        const std::string strings = "<sst><si><t>合同</t></si><si><r><t>富</t></r><r><t>文本</t></r><rPh><t>ふ</t></rPh></si>"
                                    "<si><t>多\n行</t></si></sst>";
        const std::string sheet1
            = R"(<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1"><v>1234.5</v></c><c r="C1" t="b"><v>1</v></c></row>)"
              R"(<row r="2"><c r="A2" t="s"><v>1</v></c></row>)"
              R"(<row r="5"><c r="A5" t="inlineStr"><is><t>内联</t></is></c><c r="B5" t="str"><f>A1&amp;"x"</f><v>合同x</v></c>)"
              R"(<c r="C5" t="s"><v>2</v></c><c r="D5" t="s"><v>99</v></c></row></sheetData></worksheet>)";
        const std::string sheet2
            = R"(<x:worksheet xmlns:x="x"><x:sheetData><x:row r="3"><x:c r="A3" t="s"><x:v>0</x:v></x:c></x:row></x:sheetData></x:worksheet>)";
        const QByteArray xlsx = makeZip({{"_rels/.rels", rels}, {"xl/workbook.xml", workbook},
            {"xl/_rels/workbook.xml.rels", workbookRels}, {"xl/sharedStrings.xml", strings},
            {"xl/worksheets/sheet1.xml", sheet1}, {"xl/worksheets/sheet2.xml", sheet2}});
        const Extracted out = extractBytes(xlsx);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"销售!1|合同\t1234.5"_s, u"销售!2|富文本"_s,
                                           u"销售!5|内联\t合同x\t多 行"_s, u"Hidden!3|合同"_s}));
        QCOMPARE(out.text.places.size(), std::size_t {3}); // rows 1 and 2 follow on
    }

    void extractPptx()
    {
        const std::string rels
            = R"(<Relationships><Relationship Id="rId1" Type="t/officeDocument" Target="ppt/presentation.xml"/></Relationships>)";
        const std::string presentationRels = R"(<Relationships><Relationship Id="rId2" Type="t/slide" Target="slides/slide1.xml"/>)"
                                             R"(<Relationship Id="rId3" Type="t/slide" Target="slides/slide2.xml"/></Relationships>)";
        const std::string presentation
            = R"(<p:presentation xmlns:p="p" xmlns:r="r"><p:sldIdLst><p:sldId id="256" r:id="rId3"/><p:sldId id="257" r:id="rId2"/></p:sldIdLst></p:presentation>)";
        const auto slide = [](const std::string& paragraphs) {
            return R"(<p:sld xmlns:p="p" xmlns:a="a"><p:cSld><p:spTree><p:sp><p:txBody>)" + paragraphs
                + "</p:txBody></p:sp></p:spTree></p:cSld></p:sld>";
        };
        const QByteArray pptx = makeZip({{"_rels/.rels", rels}, {"ppt/presentation.xml", presentation},
            {"ppt/_rels/presentation.xml.rels", presentationRels},
            {"ppt/slides/slide1.xml", slide("<a:p><a:r><a:t>幻灯片一</a:t></a:r></a:p>")},
            {"ppt/slides/slide2.xml",
                slide("<a:p><a:r><a:t>幻灯片二</a:t></a:r><a:br><a:rPr/></a:br><a:r><a:t>B</a:t></a:r></a:p>")},
            {"ppt/slides/_rels/slide2.xml.rels",
                R"(<Relationships><Relationship Id="rId1" Type="t/notesSlide" Target="../notesSlides/notesSlide1.xml"/></Relationships>)"},
            {"ppt/notesSlides/notesSlide1.xml", slide("<a:p><a:r><a:t>备注二</a:t></a:r></a:p>")}});
        const Extracted out = extractBytes(pptx);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"slide 1|幻灯片二 B"_s, u"slide 1|备注二"_s, u"slide 2|幻灯片一"_s}));
    }

    void extractDoc()
    {
        // WordDocument: the FIB (Word 97 layout), then the text of two pieces.
        QByteArray word(0x1000, '\0');
        const auto put16 = [&](int at, quint16 v) { std::memcpy(word.data() + at, &v, 2); };
        const auto put32 = [&](int at, quint32 v) { std::memcpy(word.data() + at, &v, 4); };
        put16(0, 0xA5EC);
        put16(2, 0xC1);
        put16(0x0A, 0x0200); // the table stream is 1Table
        put16(0x20, 14);
        put16(0x3E, 22);
        put16(0x98, 0x5D);
        const std::u16string unicode = u"合同编号\r\x13 PAGE \x14" u"5\x15\r";
        const QByteArray compressed("Caf\xE9 \x93q\x94\r");
        std::memcpy(word.data() + 0x800, unicode.data(), unicode.size() * 2);
        std::memcpy(word.data() + 0x900, compressed.constData(), static_cast<std::size_t>(compressed.size()));
        const auto n0 = static_cast<quint32>(unicode.size());
        const auto n1 = static_cast<quint32>(compressed.size());
        // 1Table: the Clx, a property run first, then the piece table.
        QByteArray plc = le32(0) + le32(n0) + le32(n0 + n1);
        plc += le16({0}) + le32(0x800) + le16({0});
        plc += le16({0}) + le32((0x900 * 2) | 0x40000000u) + le16({0});
        const QByteArray clx = QByteArray("\x01\x02\x00\xAA\xBB", 5) + QByteArray(1, '\x02')
            + le32(static_cast<quint32>(plc.size())) + plc;
        put32(0x1A2, 0);
        put32(0x1A6, static_cast<quint32>(clx.size()));
        const QByteArray doc = makeCfb({{u"WordDocument", word}, {u"1Table", clx}});
        const Extracted out = extractBytes(doc);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(QString::fromUtf8(out.text.text), u"合同编号\n5\nCafé “q”\n"_s);

        put16(0x0A, 0x0300); // encrypted
        QCOMPARE(extractBytes(makeCfb({{u"WordDocument", word}, {u"1Table", clx}})).status, extract::Status::Encrypted);
    }

    void extractXls()
    {
        const QByteArray bof = le16({0x0600, 0x0005}) + QByteArray(12, '\0');
        const QByteArray sheetBof = le16({0x0600, 0x0010}) + QByteArray(12, '\0');
        // Shared strings: "合同", then "Total" cut by a CONTINUE that goes on in UTF-16.
        const QByteArray sst = le32(2) + le32(2) + le16({2}) + QByteArray(1, '\x01') + utf16(u"合同") + le16({5})
            + QByteArray(1, '\0') + QByteArray("Tot");
        const QByteArray continued = QByteArray(1, '\x01') + utf16(u"al");
        const auto boundSheet = [&](quint32 at) {
            return biff(0x0085, le32(at) + QByteArray(2, '\0') + QByteArray(1, '\x02') + QByteArray(1, '\x01') + utf16(u"表一"));
        };
        const QByteArray globalsWithout = biff(0x0809, bof) + boundSheet(0) + biff(0x00FC, sst) + biff(0x003C, continued)
            + biff(0x000A, {});
        const QByteArray globals = biff(0x0809, bof) + boundSheet(static_cast<quint32>(globalsWithout.size()))
            + biff(0x00FC, sst) + biff(0x003C, continued) + biff(0x000A, {});
        const auto cell = [](quint16 row, quint16 col) { return le16({row, col, 0}); };
        double number = 1234.5;
        const QByteArray formulaValue = QByteArray(1, '\0') + QByteArray(5, '\0') + QByteArray("\xFF\xFF", 2);
        const QByteArray sheet = biff(0x0809, sheetBof) + biff(0x00FD, cell(0, 0) + le32(0))
            + biff(0x0203, cell(0, 1) + QByteArray(reinterpret_cast<const char*>(&number), 8))
            + biff(0x0006, cell(0, 2) + formulaValue + QByteArray(8, '\0'))
            + biff(0x0207, le16({2}) + QByteArray(1, '\x01') + utf16(u"公式")) + biff(0x00FD, cell(2, 0) + le32(1))
            + biff(0x027E, cell(4, 0) + le32((42u << 2) | 2u)) + biff(0x000A, {});
        const Extracted out = extractBytes(makeCfb({{u"Workbook", globals + sheet}}));
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"表一!1|合同\t1234.5\t公式"_s, u"表一!3|Total"_s, u"表一!5|42"_s}));

        const QByteArray locked = biff(0x0809, bof) + biff(0x002F, QByteArray(6, '\0')) + biff(0x000A, {});
        QCOMPARE(extractBytes(makeCfb({{u"Workbook", locked}})).status, extract::Status::Encrypted);
    }

    void extractPpt()
    {
        const auto atom = [](quint16 type, const QByteArray& body) { return pptRecord(0, type, body); };
        const QByteArray slideList = atom(0x03F3, QByteArray(20, '\0')) + atom(0x0FA0, utf16(u"标题一\r正文"))
            + atom(0x03F3, QByteArray(20, '\0')) + atom(0x0FA8, "Second slide");
        const QByteArray stream = pptRecord(0x000F, 0x03E8,
                                      pptRecord(0x000F, 0x0FF0, slideList) + pptRecord(0x001F, 0x0FF0, atom(0x0FA0, utf16(u"母版"))))
            + pptRecord(0x000F, 0x03F8, atom(0x0FA0, utf16(u"母版文字")))
            + pptRecord(0x000F, 0x03EE, pptRecord(0x000F, 0xF00D, atom(0x0FA0, utf16(u"文本框"))));
        const Extracted out = extractBytes(makeCfb({{u"PowerPoint Document", stream}}));
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text),
            (QStringList {u"slide 1|标题一"_s, u"slide 1|正文"_s, u"slide 2|Second slide"_s, u"-|文本框"_s}));
    }

    void extractPdf()
    {
        const Extracted out = extractBytes(makePdf({{"Hello", "World"}, {"Second page"}}));
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"page 1|Hello World"_s, u"page 2|Second page"_s}));
        QVERIFY(extractBytes(QByteArray("%PDF-1.4\n1 0 obj << >> garbage")).status != extract::Status::Ok);
    }

    void extractTextUnderDocumentNames()
    {
        // An HTML table a web system saved as ".xls", in GBK.
        const QByteArray html = encode(u"<html><head><meta charset=\"gb2312\"><style>td{}</style>"
                                       u"<script>var a='<td>';</script></head><body><table>"
                                       u"<tr><td>合同</td><td>金额&nbsp;100</td></tr><tr><td>第二行</td></tr></table></body></html>"_s,
            936);
        Extracted out = extractBytes(html);
        QCOMPARE(out.status, extract::Status::Ok);
        QCOMPARE(placedLines(out.text), (QStringList {u"-|合同\t金额 100"_s, u"-|第二行"_s}));
        out = extractBytes(encode(u"a,b\r\n合同,1\r\n"_s, 936));
        QCOMPARE(placedLines(out.text), (QStringList {u"-|a,b"_s, u"-|合同,1"_s}));
    }

    void extractRejects()
    {
        QCOMPARE(extractBytes(QByteArray("\x01\x02\x00\x04 binary", 10)).status, extract::Status::Unsupported);
        QCOMPARE(extractBytes(makeZip({{"a.txt", "x"}})).status, extract::Status::Unsupported);
        const QByteArray docx = makeZip({{"_rels/.rels", "<Relationships/>"}, {"word/document.xml", "<w:document/>"}});
        QCOMPARE(extractBytes(docx.left(docx.size() / 2)).status, extract::Status::Broken);
        QCOMPARE(extractBytes(makeCfb({{u"EncryptedPackage", QByteArray(100, 'x')}})).status, extract::Status::Encrypted);
        QCOMPARE(extractBytes(makeCfb({{u"Other", QByteArray(100, 'x')}})).status, extract::Status::Unsupported);
        QCOMPARE(extractBytes(makeCfb({})).status, extract::Status::Unsupported);
        // A compound file whose FAT points past its end, or into a loop.
        QByteArray cfb = makeCfb({{u"WordDocument", QByteArray(5000, 'x')}});
        QByteArray looped = cfb;
        std::memset(looped.data() + looped.size() - 512, 0, 512); // every sector's next is sector 0
        QVERIFY(extractBytes(looped).status != extract::Status::Ok);
        QVERIFY(extractBytes(cfb.left(1024)).status != extract::Status::Ok);
    }

    void docTextSerialization()
    {
        doctext::DocText doc;
        doc.text = "第一行\n第二行\n";
        doc.sheets = {"表"};
        doc.places = {{1, 3, 0, doctext::PlaceKind::Row}, {2, 9, doctext::Place::kNoSheet, doctext::PlaceKind::Page}};
        doc.truncated = true;
        const std::string bytes = doctext::serialize(doc);
        QVERIFY(doctext::deserialize(bytes, 1000) == doc);
        QVERIFY(!doctext::deserialize(bytes, 4)); // more text than allowed
        QVERIFY(!doctext::deserialize(bytes.substr(0, bytes.size() - 1), 1000));
        QVERIFY(!doctext::deserialize(bytes + "x", 1000));
        std::string badSheet = bytes;
        badSheet[20 + 8] = 5; // the first place's sheet: none such
        QVERIFY(!doctext::deserialize(badSheet, 1000));
        std::string badLine = bytes;
        badLine[20] = 0; // line 0
        QVERIFY(!doctext::deserialize(badLine, 1000));
    }

    void contentIndexKeepsTexts()
    {
        QTemporaryDir dir;
        doctext::DocText a;
        a.text = "合同编号\n";
        a.places = {{1, 3, doctext::Place::kNoSheet, doctext::PlaceKind::Page}};
        doctext::DocText b = a;
        b.text = "新合同\n";
        std::vector<char> state;
        std::vector<std::uint64_t> files;
        {
            ContentIndex content(dir.path());
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 1, 0, ContentIndex::packText(a)));
            QVERIFY(content.add(20, gramsOf(u"其他"_s), false, 2, 0));
            QVERIFY(content.textOf(10) == a);
            QVERIFY(!content.textOf(20));
            QVERIFY(!content.textOf(30));
            QVERIFY(content.add(10, gramsOf(u"新合同"_s), false, 3, 0, ContentIndex::packText(b))); // replaces it
            QVERIFY(content.textOf(10) == b);
            content.markChanged(std::vector<EntryId> {10});
            QVERIFY(!content.textOf(10)); // written to: the text is not current
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 4, content.changeSequence(), ContentIndex::packText(a)));
            QCOMPARE(content.stats().texts, std::size_t {1});
            const std::uint64_t before = content.stats().textBytes;
            QVERIFY(content.merge()); // renumbers documents
            QVERIFY(content.textOf(10) == a);
            QVERIFY(content.compactTexts());
            QVERIFY(content.textOf(10) == a);
            QVERIFY(content.stats().textBytes < before);
            std::vector<EntryId> newIds(40, kNoEntry);
            newIds[10] = 1;
            newIds[20] = 2;
            state = content.serialize(newIds, files);
            content.saved(files);
        }
        QCOMPARE(QDir(dir.path()).entryList({u"*.texts"_s}).size(), qsizetype {1}); // the old one is gone
        ContentIndex restored(dir.path());
        QVERIFY(restored.restore(state, 3));
        QVERIFY(restored.textOf(1) == a);
        QVERIFY(!restored.textOf(2));
        restored.retire(std::vector<EntryId> {1});
        QVERIFY(!restored.textOf(1));
        QCOMPARE(restored.stats().texts, std::size_t {0});

        // Nothing to find in it: an empty text, so a search need not read the file.
        QVERIFY(restored.add(2, {}, true, 7, 0));
        QVERIFY(restored.textOf(2) && restored.textOf(2)->text.empty());

        // Moved to another folder (IndexService::moveTo): copied as
        // indexfolder::copy() does (the text file open for writing meanwhile),
        // then used from there.
        QVERIFY(restored.add(1, gramsOf(u"新合同"_s), false, 8, 0, ContentIndex::packText(b)));
        QTemporaryDir moved;
        for (const QFileInfo& f : QDir(dir.path()).entryInfoList({u"*.grams"_s, u"*.texts"_s}, QDir::Files)) {
            const std::wstring from = QDir::toNativeSeparators(f.absoluteFilePath()).toStdWString();
            const std::wstring to = QDir::toNativeSeparators(moved.filePath(f.fileName())).toStdWString();
            QVERIFY(::CopyFileExW(from.c_str(), to.c_str(), nullptr, nullptr, nullptr, 0));
        }
        QVERIFY(restored.relocate(moved.path()));
        QVERIFY(restored.textOf(1) == b);
        QVERIFY(restored.add(3, gramsOf(u"合同编号"_s), false, 9, 0, ContentIndex::packText(a))); // written there
        QVERIFY(restored.textOf(3) == a);
        const QStringList texts = QDir(moved.path()).entryList({u"*.texts"_s}, QDir::Files);
        QCOMPARE(texts.size(), qsizetype {1});
        // By its path: a directory listing has the size from before the last
        // writes until they are flushed (docs/pitfalls.md).
        QCOMPARE(QFileInfo(moved.filePath(texts.first())).size(), static_cast<qint64>(restored.stats().textBytes));
    }

    // Documents with the same text (copies of a file) share it; a copy found
    // before it is read gets the original's document.
    void contentIndexSharesTexts()
    {
        QTemporaryDir dir;
        doctext::DocText a;
        a.text = "合同编号\n";
        doctext::DocText b;
        b.text = "季度报告\n";
        const QByteArray packed = ContentIndex::packText(a);
        const auto once = static_cast<std::uint64_t>(packed.size());
        std::vector<char> state;
        std::vector<std::uint64_t> files;
        {
            ContentIndex content(dir.path());
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 1, 0, packed));
            QVERIFY(content.add(20, gramsOf(u"合同编号"_s), false, 2, 0, packed));
            QCOMPARE(content.stats().texts, std::size_t {2});
            QCOMPARE(content.stats().distinctTexts, std::size_t {1});
            QCOMPARE(content.stats().textBytes, once);
            QVERIFY(content.add(10, gramsOf(u"合同编号"_s), false, 3, 0, packed)); // read again: the same text
            QCOMPARE(content.stats().distinctTexts, std::size_t {1});
            QCOMPARE(content.stats().textBytes, once);

            QVERIFY(content.addCopy(30, 10, 3, 4, 0));
            QVERIFY(content.textOf(30) == a);
            QCOMPARE(matchesOf(content, u"合同"_s), (std::vector<EntryId> {10, 20, 30}));
            QVERIFY(!content.addCopy(31, 10, 99, 5, 0)); // the original was read from another file
            QVERIFY(!content.addCopy(31, 11, 0, 5, 0)); // no such document
            QVERIFY(content.add(40, {}, true, 6, 0)); // nothing to find in it
            QVERIFY(content.addCopy(41, 40, 6, 7, 0));
            QVERIFY(content.textOf(41) && content.textOf(41)->text.empty());
            content.markChanged(std::vector<EntryId> {20});
            QVERIFY(!content.addCopy(32, 20, 2, 8, 0)); // not current
            QCOMPARE(content.stats().textBytes, once);

            content.retire(std::vector<EntryId> {10, 20});
            QVERIFY(content.textOf(30) == a); // the copy still has it
            QVERIFY(content.add(50, gramsOf(u"季度报告"_s), false, 9, 0, ContentIndex::packText(b)));
            QCOMPARE(content.stats().distinctTexts, std::size_t {2});
            content.retire(std::vector<EntryId> {50});
            QCOMPARE(content.stats().distinctTexts, std::size_t {1});
            QVERIFY(content.merge());
            QVERIFY(content.compactTexts());
            QCOMPARE(content.stats().textBytes, once); // a, once
            QVERIFY(content.textOf(30) == a);
            std::vector<EntryId> newIds(60, kNoEntry);
            newIds[30] = 3;
            newIds[41] = 4;
            state = content.serialize(newIds, files);
            content.saved(files);
        }
        ContentIndex restored(dir.path());
        QVERIFY(restored.restore(state, 10));
        QVERIFY(restored.textOf(3) == a);
        QVERIFY(restored.textOf(4) && restored.textOf(4)->text.empty());
        QVERIFY(restored.add(5, gramsOf(u"合同编号"_s), false, 10, 0, packed)); // shares it still
        QCOMPARE(restored.stats().distinctTexts, std::size_t {1});
        QCOMPARE(restored.stats().textBytes, once);
        QVERIFY(restored.textOf(5) == a);
    }

    void docExtractorSandbox()
    {
        if (!QFile::exists(QString::fromStdWString(DocExtractor::programPath())))
            QSKIP("WinShunExtract.exe is not built with this preset");
        QTemporaryDir dir;
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return std::wstring();
            f.write(data);
            return QDir::toNativeSeparators(f.fileName()).toStdWString();
        };
        const std::wstring pdf = write(u"a.pdf"_s, makePdf({{"Contract", "number"}}));
        const std::wstring bad = write(u"b.docx"_s, QByteArray("\x01\x02\x00\x04 binary", 10));
        DocExtractor extractor(1, DocExtractor::Priority::Normal);
        DocExtractor::Result read = extractor.extractFile(pdf, 1 << 20, {});
        QCOMPARE(read.status, extractproto::Status::Ok);
        QCOMPARE(read.text.text, std::string("Contract number\n"));
        QCOMPARE(extractor.extractFile(bad, 1 << 20, {}).status, extractproto::Status::Unsupported);
        QCOMPARE(extractor.extractFile(pdf + L".missing", 1 << 20, {}).status, extractproto::Status::Failed);
        QVERIFY(!extractor.extractFile(pdf + L".missing", 1 << 20, {}).large);
        const DocExtractor::Result tooLarge = extractor.extractFile(pdf, 10, {});
        QCOMPARE(tooLarge.status, extractproto::Status::Failed); // too large
        QVERIFY(tooLarge.large && !read.large);

        // What the content indexer keeps of a document: its grams and its text.
        const ContentIndexer::FileText text = ContentIndexer::readFile(pdf, 0, {}, &extractor);
        QCOMPARE(text.outcome, ContentIndexer::Outcome::Indexed);
        QVERIFY(hasAll(text.keys, u"contract number"_s));
        QVERIFY(!text.text.isEmpty());
        QVERIFY(text.document && text.document->text == "Contract number\n"); // for a search to look through
        const ContentIndexer::FileText overLimit = ContentIndexer::readFile(pdf, 10, {}, &extractor);
        QCOMPARE(overLimit.outcome, ContentIndexer::Outcome::Empty);
        QVERIFY(overLimit.large && !text.large);
        QCOMPARE(ContentIndexer::readFile(pdf, 0, {}, nullptr).outcome, ContentIndexer::Outcome::Skipped);
        QCOMPARE(ContentIndexer::readFile(bad, 0, {}, &extractor).outcome, ContentIndexer::Outcome::Empty);
        extractor.closeIdle();
    }

    // Copies of a document (the same bytes) are not read again: they get
    // the original's document.
    void contentIndexerReadsCopiesOnce()
    {
        if (!QFile::exists(QString::fromStdWString(DocExtractor::programPath())))
            QSKIP("WinShunExtract.exe is not built with this preset");
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(tmp.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        const QByteArray contract = makePdf({{"Contract", "number"}});
        const QByteArray contrast = makePdf({{"Contrast", "number"}});
        QCOMPARE(contract.size(), contrast.size()); // told apart by their bytes
        QVERIFY(write(u"a.pdf"_s, contract) && write(u"b.pdf"_s, contrast) && write(u"c.pdf"_s, contract)
            && write(u"d.pdf"_s, contract));
        const std::string rootName = wtf8::fromUtf16(wtf8::view(QDir::toNativeSeparators(tmp.path())));
        auto index = std::make_shared<FileIndex>();
        EntryId a = kNoEntry;
        EntryId b = kNoEntry;
        EntryId c = kNoEntry;
        EntryId d = kNoEntry;
        {
            auto lock = index->writeLock();
            const EntryId root = index->addRoot(rootName);
            a = index->add(root, "a.pdf", 0);
            b = index->add(root, "b.pdf", 0);
            c = index->add(root, "c.pdf", 0);
            d = index->add(root, "d.pdf", 0);
        }
        auto content = std::make_shared<ContentIndex>(tmp.filePath(u"content"_s));
        ContentIndexer::Source source;
        source.index = [&] { return index; };
        source.volumes = [&] { return std::vector<ContentIndexer::Volume> {{rootName, false}}; };
        source.ready = [] { return true; };
        using namespace std::chrono_literals;
        ContentIndexer indexer(content, source, {}, {0ms, 0ms, 1h, 0ms});
        for (int i = 0; i < 1000 && content->stats().documents < 4; ++i)
            std::this_thread::sleep_for(10ms);
        QCOMPARE(content->stats().documents, std::size_t {4});
        QCOMPARE(matchesOf(*content, u"contract"_s), (std::vector<EntryId> {a, c, d}));
        QCOMPARE(matchesOf(*content, u"contrast"_s), std::vector<EntryId> {b});
        QCOMPARE(indexer.copies(), std::size_t {2}); // one of a, c and d was read
        QCOMPARE(content->stats().distinctTexts, std::size_t {2});
        QVERIFY(content->textOf(c) && content->textOf(c) == content->textOf(a));
    }

    void doubleTap()
    {
        constexpr std::uint32_t ctrl = 0xA2;
        {
            DoubleTapDetector d;
            QVERIFY(!d.keyDown(ctrl, 0, 0, 0));
            QVERIFY(!d.keyUp(ctrl, 80, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 200, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 230, 0, 0)); // auto-repeat
            QVERIFY(d.keyUp(ctrl, 260, 0, 0));
        }
        { // Ctrl+C in between
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 100, 0, 0);
            d.keyDown('C', 120, 0, 0);
            d.keyUp('C', 140, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 160, 0, 0));
        }
        { // too slow
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 900, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 950, 0, 0));
        }
        { // held too long
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 600, 0, 0);
            d.keyDown(ctrl, 700, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 750, 0, 0));
        }
        { // mouse moved: Ctrl+click multi-select, not a double tap
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 100, 100);
            d.keyUp(ctrl, 50, 100, 100);
            d.keyDown(ctrl, 150, 300, 100);
            QVERIFY(!d.keyUp(ctrl, 200, 300, 100));
        }
        { // a click (or the wheel) while Ctrl is down, twice on the same spot: not a double tap
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 100, 100);
            d.mouseUsed();
            d.keyUp(ctrl, 50, 100, 100);
            d.keyDown(ctrl, 150, 100, 100);
            d.mouseUsed();
            QVERIFY(!d.keyUp(ctrl, 200, 100, 100));
        }
        { // a click right after a tap: no double tap with the next one
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.mouseUsed();
            d.keyDown(ctrl, 150, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 200, 0, 0));
        }
        { // the release was lost (Ctrl+Alt+Del): the next double tap still counts
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyDown(ctrl, 500, 0, 0); // auto-repeat
            d.keyDown(ctrl, 530, 0, 0);
            QVERIFY(!d.keyDown(ctrl, 10'000, 0, 0));
            QVERIFY(!d.keyUp(ctrl, 10'080, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 10'200, 0, 0));
            QVERIFY(d.keyUp(ctrl, 10'260, 0, 0));
        }
        { // ...and the other Ctrl's too, its release still not seen
            constexpr std::uint32_t rightCtrl = 0xA3;
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyDown(ctrl, 500, 0, 0); // auto-repeat
            QVERIFY(!d.keyDown(rightCtrl, 10'000, 0, 0));
            QVERIFY(!d.keyUp(rightCtrl, 10'080, 0, 0));
            QVERIFY(!d.keyDown(rightCtrl, 10'200, 0, 0));
            QVERIFY(d.keyUp(rightCtrl, 10'260, 0, 0));
            DoubleTapDetector e; // while one is held (repeating), the other's press is both at once
            e.keyDown(ctrl, 0, 0, 0);
            e.keyDown(ctrl, 500, 0, 0);
            e.keyDown(ctrl, 533, 0, 0);
            QVERIFY(!e.keyDown(rightCtrl, 600, 0, 0));
            QVERIFY(!e.keyUp(rightCtrl, 650, 0, 0));
            QVERIFY(!e.keyDown(rightCtrl, 700, 0, 0));
            QVERIFY(!e.keyUp(rightCtrl, 750, 0, 0));
        }
        // GitHub #111: a program's slow low-level hook held the first press
        // up (an app busy just after a switch). Windows times a key as it
        // gets to it: the release, the second press and its release, made
        // meanwhile, all came the moment the hook let go, the first press
        // seeming 609 ms long. Let go and pressed again quicker than a
        // finger can: it was a tap.
        {
            DoubleTapDetector d;
            QVERIFY(!d.keyDown(ctrl, 1000, 0, 0));
            QVERIFY(!d.keyUp(ctrl, 1609, 0, 0));
            QVERIFY(!d.keyDown(ctrl, 1609, 0, 0));
            QVERIFY(d.keyUp(ctrl, 1609, 0, 0));
            QCOMPARE(d.queuedMs(), 609u);
            QCOMPARE(d.firstPressTime(), 1000u);
            QCOMPARE(d.takeMiss().reason, DoubleTapDetector::MissReason::None);
        }
        { // on the next tick of the input's clock; the second let go after the hook
            DoubleTapDetector d;
            d.keyDown(ctrl, 1000, 0, 0);
            d.keyUp(ctrl, 1900, 0, 0);
            d.keyDown(ctrl, 1916, 0, 0);
            QVERIFY(d.keyUp(ctrl, 1980, 0, 0));
            QCOMPARE(d.queuedMs(), 900u);
            d.keyDown(ctrl, 5000, 0, 0); // the next double tap, an ordinary one
            d.keyUp(ctrl, 5060, 0, 0);
            d.keyDown(ctrl, 5200, 0, 0);
            QVERIFY(d.keyUp(ctrl, 5260, 0, 0));
            QCOMPARE(d.queuedMs(), 0u);
        }
        { // a finger's: held long, then let go and pressed again in 31 ms or more
            DoubleTapDetector d;
            d.keyDown(ctrl, 1000, 0, 0);
            d.keyUp(ctrl, 1600, 0, 0);
            d.keyDown(ctrl, 1631, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 1690, 0, 0));
            DoubleTapDetector e; // a long hold, and a tap 150 ms later
            e.keyDown(ctrl, 1000, 0, 0);
            e.keyUp(ctrl, 1500, 0, 0);
            e.keyDown(ctrl, 1650, 0, 0);
            QVERIFY(!e.keyUp(ctrl, 1700, 0, 0));
        }
        { // none but a press held too long goes that way: spoiled (Shift), moved, the other Ctrl
            constexpr std::uint32_t rightCtrl = 0xA3, shift = 0xA0;
            DoubleTapDetector d;
            d.keyDown(shift, 990, 0, 0);
            d.keyDown(ctrl, 1000, 0, 0);
            d.keyUp(shift, 1010, 0, 0);
            d.keyUp(ctrl, 1609, 0, 0);
            d.keyDown(ctrl, 1609, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 1609, 0, 0));
            DoubleTapDetector e;
            e.keyDown(ctrl, 1000, 0, 0);
            e.keyUp(ctrl, 1609, 0, 0);
            e.keyDown(ctrl, 1609, 300, 0);
            QVERIFY(!e.keyUp(ctrl, 1609, 300, 0));
            DoubleTapDetector f;
            f.keyDown(ctrl, 1000, 0, 0);
            f.keyUp(ctrl, 1609, 0, 0);
            f.keyDown(rightCtrl, 1609, 0, 0);
            QVERIFY(!f.keyUp(rightCtrl, 1609, 0, 0));
            DoubleTapDetector g; // with a letter typed during it: Ctrl+C
            g.keyDown(ctrl, 1000, 0, 0);
            g.keyDown('C', 1020, 0, 0);
            g.keyUp('C', 1040, 0, 0);
            g.keyUp(ctrl, 1609, 0, 0);
            g.keyDown(ctrl, 1609, 0, 0);
            QVERIFY(!g.keyUp(ctrl, 1609, 0, 0));
        }
        { // held a long while, repeating: not a tap when let go
            DoubleTapDetector d;
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 100, 0, 0);
            for (std::uint32_t t = 600; t <= 5'000; t += 33)
                d.keyDown(ctrl, t, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 5'010, 0, 0));
        }
        { // the gap follows the double-click time
            DoubleTapDetector d;
            d.setMaxGap(700);
            d.keyDown(ctrl, 0, 0, 0);
            d.keyUp(ctrl, 50, 0, 0);
            d.keyDown(ctrl, 650, 0, 0);
            QVERIFY(d.keyUp(ctrl, 700, 0, 0));
        }
        { // timestamps wrap around
            DoubleTapDetector d;
            d.keyDown(ctrl, 0xFFFFFF00u, 0, 0);
            d.keyUp(ctrl, 0xFFFFFF40u, 0, 0);
            d.keyDown(ctrl, 0x00000010u, 0, 0);
            QVERIFY(d.keyUp(ctrl, 0x00000050u, 0, 0));
        }
    }

    void doubleTapKeys()
    {
        constexpr std::uint32_t left = 0xA2, right = 0xA3, alt = 0xA4, shift = 0xA0, win = 0x5B;
        const auto tapTwice = [](DoubleTapDetector& d, std::uint32_t first, std::uint32_t second, std::uint32_t t) {
            d.keyDown(first, t, 0, 0);
            d.keyUp(first, t + 50, 0, 0);
            d.keyDown(second, t + 150, 0, 0);
            return d.keyUp(second, t + 200, 0, 0);
        };
        { // Alt held, its hotkey's key eaten by the tool's hook, Ctrl tapped by
          // the tool after each press (AutoHotkey's menu mask): not a double tap
            DoubleTapDetector d;
            d.keyDown(alt, 0, 0, 0);
            QVERIFY(!tapTwice(d, left, left, 300));
            d.keyUp(alt, 600, 0, 0);
            QVERIFY(tapTwice(d, left, left, 700)); // and after Alt is let go, it works
        }
        { // the same with Shift or Win
            for (const std::uint32_t modifier : {shift, win, 0x10u /*VK_SHIFT*/, 0x12u /*VK_MENU*/}) {
                DoubleTapDetector d;
                d.keyDown(modifier, 0, 0, 0);
                QVERIFY(!tapTwice(d, left, left, 300));
            }
        }
        { // Alt's release was lost (Ctrl+Alt+Del): once it is quiet and up, taps count again
            static bool altDown = true;
            DoubleTapDetector::Config config;
            config.isDown = [](std::uint32_t vk) { return vk == 0x12 && altDown; };
            DoubleTapDetector d(config);
            d.keyDown(alt, 0, 0, 0);
            QVERIFY(!tapTwice(d, left, left, 5'000)); // still held, as the system says
            altDown = false;
            QVERIFY(tapTwice(d, left, left, 6'000));
            altDown = true;
        }
        { // the other Ctrl starts over: right after left is not a double tap
            DoubleTapDetector d;
            QVERIFY(!tapTwice(d, left, right, 0));
            d.keyDown(right, 300, 0, 0);
            QVERIFY(d.keyUp(right, 340, 0, 0)); // with the right one just tapped, it is
        }
        { // both Ctrl keys at once
            DoubleTapDetector d;
            d.keyDown(left, 0, 0, 0);
            d.keyDown(right, 20, 0, 0);
            d.keyUp(right, 40, 0, 0);
            d.keyUp(left, 50, 0, 0);
            d.keyDown(left, 150, 0, 0);
            QVERIFY(!d.keyUp(left, 200, 0, 0));
        }
        { // left only: the right Ctrl is another key
            DoubleTapDetector::Config config;
            config.side = DoubleTapDetector::Side::Left;
            DoubleTapDetector d(config);
            QVERIFY(!tapTwice(d, right, right, 0));
            QVERIFY(tapTwice(d, left, left, 1'000));
            QVERIFY(tapTwice(d, 0x11 /*VK_CONTROL*/, 0x11, 2'000));
            d.keyDown(left, 3'000, 0, 0);
            d.keyUp(left, 3'050, 0, 0);
            d.keyDown(right, 3'100, 0, 0); // between the two
            d.keyUp(right, 3'120, 0, 0);
            d.keyDown(left, 3'150, 0, 0);
            QVERIFY(!d.keyUp(left, 3'200, 0, 0));
        }
        { // right only
            DoubleTapDetector d;
            d.setSide(DoubleTapDetector::Side::Right);
            QVERIFY(!tapTwice(d, left, left, 0));
            QVERIFY(tapTwice(d, right, right, 1'000));
        }
    }

    void doubleTapAlt()
    {
        using Key = DoubleTapDetector::Key;
        using Side = DoubleTapDetector::Side;
        constexpr std::uint32_t alt = 0xA4, rightAlt = 0xA5, ctrl = 0xA2, shift = 0xA0;
        const auto altDetector = [](Side side = Side::Both) {
            return DoubleTapDetector({.key = Key::Alt, .side = side});
        };
        const auto tap = [](DoubleTapDetector& d, std::uint32_t vk, std::uint32_t t, std::uint32_t press = 50,
                             bool sent = false) {
            d.keyDown(vk, t, 0, 0, sent);
            return d.keyUp(vk, t + press, 0, 0, sent);
        };
        const auto tapTwice = [&](DoubleTapDetector& d, std::uint32_t vk, std::uint32_t t) {
            tap(d, vk, t);
            return tap(d, vk, t + 150);
        };
        { // VK_MENU is the left one; AltGr the right one, as Raw Input tells of it
            DoubleTapDetector d = altDetector();
            QVERIFY(d.key() == Key::Alt && d.isKey(alt) && d.isKey(rightAlt) && d.isKey(0x12) && !d.isKey(ctrl));
            QVERIFY(DoubleTapDetector::isKey(Key::Ctrl, 0x11) && !DoubleTapDetector::isKey(Key::Ctrl, alt));
            QVERIFY(tapTwice(d, alt, 0));
            QVERIFY(tapTwice(d, 0x12, 1'000));
            QVERIFY(tapTwice(d, rightAlt, 2'000));
        }
        { // Alt+Tab, Alt+F4, Alt+Shift (the input language), Alt+1 (a clipboard entry), Alt+`: no tap
            for (const std::uint32_t key : {0x09u /*VK_TAB*/, 0x73u /*VK_F4*/, shift, 0x31u /*1*/, 0xC0u /*`*/}) {
                DoubleTapDetector d = altDetector();
                tap(d, alt, 0);
                d.keyDown(alt, 150, 0, 0);
                d.keyDown(key, 170, 0, 0);
                d.keyUp(key, 190, 0, 0);
                QVERIFY(!d.keyUp(alt, 210, 0, 0));
                QVERIFY(!tap(d, alt, 300)); // nor with the next one
            }
        }
        { // another modifier held: Shift+Alt, Ctrl+Alt (+Del, +arrows), Win+Alt
            for (const std::uint32_t modifier : {shift, ctrl, 0x11u /*VK_CONTROL*/, 0x5Bu /*VK_LWIN*/}) {
                DoubleTapDetector d = altDetector();
                d.keyDown(modifier, 0, 0, 0);
                QVERIFY(!tapTwice(d, alt, 100));
                d.keyUp(modifier, 500, 0, 0);
                QVERIFY(tapTwice(d, alt, 600)); // and once it is let go, it works
            }
        }
        { // a hotkey tool taps Ctrl while Alt is down (AutoHotkey's menu mask), at each press
            DoubleTapDetector d = altDetector();
            for (const std::uint32_t t : {0u, 150u}) {
                d.keyDown(alt, t, 0, 0);
                d.keyDown(ctrl, t + 20, 0, 0);
                d.keyUp(ctrl, t + 21, 0, 0);
                QVERIFY(!d.keyUp(alt, t + 40, 0, 0));
            }
        }
        { // the other Alt starts over; one side only: the other one is another key
            DoubleTapDetector d = altDetector();
            tap(d, alt, 0);
            QVERIFY(!tap(d, rightAlt, 150));
            QVERIFY(tap(d, rightAlt, 300));
            DoubleTapDetector left = altDetector(Side::Left);
            QVERIFY(!tapTwice(left, rightAlt, 0));
            QVERIFY(tapTwice(left, alt, 1'000));
            DoubleTapDetector right = altDetector(Side::Right);
            QVERIFY(!tapTwice(right, alt, 0));
            QVERIFY(tapTwice(right, rightAlt, 1'000));
        }
        { // AltGr with the left Ctrl Windows adds to it, were that there: neither Alt's tap nor Ctrl's
            for (const Key key : {Key::Alt, Key::Ctrl}) {
                DoubleTapDetector d({.key = key});
                for (const std::uint32_t t : {0u, 150u}) {
                    d.keyDown(ctrl, t, 0, 0);
                    d.keyDown(rightAlt, t, 0, 0);
                    QVERIFY(!d.keyUp(ctrl, t + 50, 0, 0));
                    QVERIFY(!d.keyUp(rightAlt, t + 50, 0, 0));
                }
            }
        }
        { // each listens to its own key: the other's taps are another key's
            DoubleTapDetector ctrlOnly;
            QVERIFY(!tapTwice(ctrlOnly, alt, 0));
            QVERIFY(tapTwice(ctrlOnly, ctrl, 1'000));
            DoubleTapDetector altOnly = altDetector();
            QVERIFY(!tapTwice(altOnly, ctrl, 0));
            tap(altOnly, alt, 1'000);
            tap(altOnly, ctrl, 1'100); // between the two
            QVERIFY(!tap(altOnly, alt, 1'200));
        }
        { // a tap a program sent whole is none (AutoHotkey's WinActivate: {Alt 2}); one held as long as a finger is
            DoubleTapDetector d = altDetector();
            tap(d, alt, 0, 0, true);
            QVERIFY(!tap(d, alt, 30, 1, true));
            tap(d, alt, 500, 16, true); // the clock moved on in between
            QVERIFY(!tap(d, alt, 530, 15, true));
            QVERIFY(!tap(d, alt, 1'000, 50, true)); // a key remapped to Alt
            QVERIFY(tap(d, alt, 1'150, 50, true));
            tap(d, alt, 2'000);
            QVERIFY(!tap(d, alt, 2'100, 0, true)); // a program's between the user's two
            QVERIFY(!tap(d, alt, 2'200));
            DoubleTapDetector ctrlOnly; // Ctrl's count however they come
            tap(ctrlOnly, ctrl, 0, 0, true);
            QVERIFY(tap(ctrlOnly, ctrl, 30, 0, true));
        }
        { // the mouse used while Alt is down (Alt+click, Alt+drag), or right after a tap
            DoubleTapDetector d = altDetector();
            d.keyDown(alt, 0, 0, 0);
            d.mouseUsed();
            d.keyUp(alt, 50, 0, 0);
            QVERIFY(!tap(d, alt, 150));
            tap(d, alt, 1'000);
            d.mouseUsed();
            QVERIFY(!tap(d, alt, 1'150));
        }
        { // held long, or repeating: not a tap
            DoubleTapDetector d = altDetector();
            QVERIFY(!tap(d, alt, 0, 600));
            QVERIFY(!tap(d, alt, 700));
            QVERIFY(tap(d, alt, 850)); // the long one was not the first of the two
            d.keyDown(alt, 2'000, 0, 0);
            for (std::uint32_t t = 2'500; t <= 4'000; t += 33)
                d.keyDown(alt, t, 0, 0);
            QVERIFY(!d.keyUp(alt, 4'010, 0, 0));
        }
    }

    void doubleTapShift()
    {
        using Key = DoubleTapDetector::Key;
        using Side = DoubleTapDetector::Side;
        constexpr std::uint32_t shift = 0xA0, rightShift = 0xA1, ctrl = 0xA2, alt = 0xA4, win = 0x5B, letter = 0x48 /*H*/;
        const auto shiftDetector = [](Side side = Side::Both) {
            return DoubleTapDetector({.key = Key::Shift, .side = side});
        };
        const auto tap = [](DoubleTapDetector& d, std::uint32_t vk, std::uint32_t t, std::uint32_t press = 50,
                             bool sent = false) {
            d.keyDown(vk, t, 0, 0, sent);
            return d.keyUp(vk, t + press, 0, 0, sent);
        };
        const auto tapTwice = [&](DoubleTapDetector& d, std::uint32_t vk, std::uint32_t t) {
            tap(d, vk, t);
            return tap(d, vk, t + 150);
        };
        { // VK_SHIFT is the left one; either side, the same one twice
            DoubleTapDetector d = shiftDetector();
            QVERIFY(d.key() == Key::Shift && d.isKey(shift) && d.isKey(rightShift) && d.isKey(0x10) && !d.isKey(ctrl));
            QVERIFY(tapTwice(d, shift, 0));
            QVERIFY(tapTwice(d, 0x10, 1'000));
            QVERIFY(tapTwice(d, rightShift, 2'000));
            DoubleTapDetector mixed = shiftDetector();
            tap(mixed, shift, 0);
            QVERIFY(!tap(mixed, rightShift, 150));
        }
        { // a capital letter (Shift down, letter, Shift up) is no tap, nor is the next Shift one half of a double
            DoubleTapDetector d = shiftDetector();
            d.keyDown(shift, 0, 0, 0);
            d.keyDown(letter, 20, 0, 0);
            d.keyUp(letter, 60, 0, 0);
            QVERIFY(!d.keyUp(shift, 90, 0, 0));
            QVERIFY(!tap(d, shift, 150)); // first tap of a possible double, the capital did not arm one
            QVERIFY(tap(d, shift, 300)); // a real double after it
        }
        { // typing "Hello" and "World" with a Shift each: never a double
            DoubleTapDetector d = shiftDetector();
            std::uint32_t t = 0;
            for (int word = 0; word < 2; ++word) {
                d.keyDown(shift, t, 0, 0);
                d.keyDown(letter, t + 30, 0, 0);
                d.keyUp(letter, t + 80, 0, 0);
                QVERIFY(!d.keyUp(shift, t + 100, 0, 0));
                for (const std::uint32_t lower : {0x45u, 0x4Cu, 0x4Cu, 0x4Fu}) {
                    t += 120;
                    d.keyDown(lower, t, 0, 0);
                    d.keyUp(lower, t + 40, 0, 0);
                }
                t += 150;
            }
        }
        { // Shift with Ctrl, Alt or Win held at the press (Ctrl+Shift+Esc, Alt+Shift, Win+Shift+S): no tap
            for (const std::uint32_t modifier : {ctrl, alt, win, 0x11u /*VK_CONTROL*/, 0x12u /*VK_MENU*/}) {
                DoubleTapDetector d = shiftDetector();
                d.keyDown(modifier, 0, 0, 0);
                QVERIFY(!tapTwice(d, shift, 300));
                d.keyUp(modifier, 700, 0, 0);
                QVERIFY(tapTwice(d, shift, 800)); // and after it is let go, it works
            }
        }
        { // the other Shift held: both at once is no tap
            DoubleTapDetector d = shiftDetector();
            d.keyDown(shift, 0, 0, 0);
            d.keyDown(rightShift, 20, 0, 0);
            d.keyUp(rightShift, 40, 0, 0);
            d.keyUp(shift, 50, 0, 0);
            QVERIFY(!tap(d, shift, 150));
        }
        { // left only, right only
            DoubleTapDetector leftOnly = shiftDetector(Side::Left);
            QVERIFY(!tapTwice(leftOnly, rightShift, 0));
            QVERIFY(tapTwice(leftOnly, shift, 1'000));
            QVERIFY(tapTwice(leftOnly, 0x10, 2'000));
            tap(leftOnly, shift, 3'000);
            tap(leftOnly, rightShift, 3'100); // between the two
            QVERIFY(!tap(leftOnly, shift, 3'200));
            DoubleTapDetector rightOnly = shiftDetector(Side::Right);
            QVERIFY(!tapTwice(rightOnly, shift, 0));
            QVERIFY(tapTwice(rightOnly, rightShift, 1'000));
        }
        { // Shift+click (selecting a range) and Shift+wheel: the mouse was used
            DoubleTapDetector d = shiftDetector();
            d.keyDown(shift, 0, 0, 0);
            d.mouseUsed();
            d.keyUp(shift, 50, 0, 0);
            QVERIFY(!tap(d, shift, 150));
        }
        { // held long, or repeating: not a tap; another key between the two taps cancels the first
            DoubleTapDetector d = shiftDetector();
            QVERIFY(!tap(d, shift, 0, 600));
            d.keyDown(shift, 2'000, 0, 0);
            for (std::uint32_t t = 2'500; t <= 4'000; t += 33)
                d.keyDown(shift, t, 0, 0);
            QVERIFY(!d.keyUp(shift, 4'010, 0, 0));
            tap(d, shift, 5'000);
            d.keyDown(letter, 5'100, 0, 0);
            d.keyUp(letter, 5'120, 0, 0);
            QVERIFY(!tap(d, shift, 5'200));
        }
        { // a tap a program sent whole is none; a Shift typed or remapped counts
            DoubleTapDetector d = shiftDetector();
            tap(d, shift, 0, 0, true);
            QVERIFY(!tap(d, shift, 30, 1, true));
            tap(d, shift, 1'000, 50, true);
            QVERIFY(tap(d, shift, 1'150, 50, true));
        }
        { // Ctrl's and Alt's detectors do not take a Shift tap
            DoubleTapDetector ctrlOnly;
            QVERIFY(!tapTwice(ctrlOnly, shift, 0));
            DoubleTapDetector altOnly({.key = Key::Alt});
            QVERIFY(!tapTwice(altOnly, shift, 0));
        }
    }

    void doubleClick()
    {
        constexpr std::uintptr_t view = 0x1234;
        {
            DoubleClickDetector d;
            QVERIFY(!d.leftDown(0, 100, 100, view, false));
            QVERIFY(d.leftDown(300, 101, 99, view, false));
            QVERIFY(!d.firstClosedMenu());
            QVERIFY(!d.leftDown(400, 101, 99, view, false)); // a third press starts over
            QVERIFY(d.leftDown(600, 101, 99, view, false));
        }
        { // too slow
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            QVERIFY(!d.leftDown(501, 100, 100, view, false));
            QVERIFY(d.leftDown(900, 100, 100, view, false)); // that one was a first press
        }
        { // moved out of the box
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            QVERIFY(!d.leftDown(200, 103, 100, view, false));
            DoubleClickDetector e;
            e.leftDown(0, 100, 100, view, false);
            QVERIFY(!e.leftDown(200, 100, 97, view, false));
        }
        { // another window (a press on the menu, then one on File Explorer)
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            QVERIFY(!d.leftDown(200, 100, 100, view + 1, false));
        }
        { // another button or the wheel in between
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, false);
            d.otherInput();
            QVERIFY(!d.leftDown(200, 100, 100, view, false));
        }
        { // the first press closed the menu: a double click that means "close"
            DoubleClickDetector d;
            d.leftDown(0, 100, 100, view, true);
            QVERIFY(d.leftDown(200, 100, 100, view, false));
            QVERIFY(d.firstClosedMenu());
            d.leftDown(1000, 100, 100, view, false);
            QVERIFY(d.leftDown(1200, 100, 100, view, false));
            QVERIFY(!d.firstClosedMenu());
        }
        { // the tick count wraps around
            DoubleClickDetector d;
            d.leftDown(0xFFFFFF00u, 100, 100, view, false);
            QVERIFY(d.leftDown(0x00000010u, 100, 100, view, false));
        }
    }

    // Opus keeps its punctuation-led FAYT modes and all editor input.
    void opusSearchTyping()
    {
        using namespace searchtyping;
        for (const unsigned key : {'A', 'Z', '0', '9'})
            QVERIFY(opusStarts(key, true, false, false, Focus::ItemsView, false));
        QVERIFY(opusStarts('A', true, false, true, Focus::ItemsView, false)); // uppercase/pinyin
        QVERIFY(opusStarts(kVkNumpad0, true, false, false, Focus::ItemsView, false));
        for (const unsigned key : {kVkSpace, kVkBack, kVkReturn, kVkEscape, kVkTab, kVkLeft, kVkDelete, kVkF1,
                 kVkF1 + 1, kVkOem1, kVkOem3, kVkOem4, kVkDivide, kVkNumpad0 + 10})
            QVERIFY(!opusStarts(key, true, false, false, Focus::ItemsView, false));
        QVERIFY(!opusStarts('3', true, false, true, Focus::ItemsView, false)); // # is Opus's range mode
        QVERIFY(!opusStarts('A', false, false, false, Focus::ItemsView, false));
        QVERIFY(!opusStarts('A', true, true, false, Focus::ItemsView, false)); // Ctrl/Alt/Win shortcuts
        QVERIFY(!opusStarts('A', true, false, false, Focus::Menu, false));
        QVERIFY(!opusStarts('A', true, false, false, Focus::Other, false)); // rename/path/FAYT field
        QVERIFY(!opusStarts('A', true, false, false, Focus::ItemsView, true));
    }

    // Typing in a File Explorer folder window: which keys, where the focus
    // is, start a search with Win顺 (platform/TypeToSearch decides by it).
    void searchTyping()
    {
        using namespace searchtyping;
        const auto starts = [](unsigned vk, bool press = true, bool combination = false, Focus focus = Focus::ItemsView,
                                bool elevated = false) { return explorerStarts(vk, press, combination, focus, elevated); };
        // Letters, digits, punctuation, the number pad: typing.
        QVERIFY(starts('A'));
        QVERIFY(starts('Z'));
        QVERIFY(starts('0'));
        QVERIFY(starts(kVkNumpad0));
        QVERIFY(starts(kVkDivide));
        QVERIFY(starts(kVkOem1)); // ;
        QVERIFY(starts(kVkOem3)); // `
        QVERIFY(starts(kVkOem4)); // [
        QVERIFY(starts(kVkOem8));
        QVERIFY(starts(kVkOem102)); // the key by the left Shift on some layouts
        // Commands of Explorer's view, and keys that do nothing in an empty box.
        QVERIFY(!starts(kVkSpace)); // Listary leaves it to Explorer too
        QVERIFY(!starts(kVkBack));
        QVERIFY(!starts(kVkReturn));
        QVERIFY(!starts(kVkEscape));
        QVERIFY(!starts(kVkTab));
        QVERIFY(!starts(kVkDelete));
        QVERIFY(!starts(kVkLeft));
        QVERIFY(!starts(kVkF1));
        QVERIFY(!starts(kVkF1 + 1)); // F2: rename
        QVERIFY(!starts(kVkF24));
        QVERIFY(!starts(kVkProcessKey)); // never seen by the hook; harmless all the same
        // Modifiers alone, and anything with Ctrl, Alt or Win held.
        QVERIFY(!starts(kVkShift));
        QVERIFY(!starts(kVkLShift));
        QVERIFY(!starts(kVkControl));
        QVERIFY(!starts(kVkMenu));
        QVERIFY(!starts(kVkRMenu));
        QVERIFY(!starts(kVkLWin));
        QVERIFY(!starts(kVkRWin));
        QVERIFY(!starts(kVkCapital));
        QVERIFY(!starts(kVkNumlock));
        QVERIFY(!starts('A', true, true)); // Ctrl+A
        QVERIFY(!starts('1', true, true)); // Alt+1
        // Releases: never (the press was taken, or not).
        QVERIFY(!starts('A', false));
        // Not with the focus elsewhere in the window: a name being renamed
        // (an Edit), the address bar, the search box, the navigation pane.
        QVERIFY(!starts('A', true, false, Focus::Other));
        // Not an elevated Explorer: Windows would neither show the hook its
        // keys nor let them be handed back.
        QVERIFY(!starts('A', true, false, Focus::ItemsView, true));
        QVERIFY(isModifier(kVkShift) && isModifier(kVkRMenu) && isModifier(kVkLWin));
        QVERIFY(!isModifier('A') && !isModifier(kVkSpace));
        // A menu open (GitHub #37): its keys, whichever they are. Measured on
        // Windows 11 25H2: the classic context menu (Shift+right-click on a
        // file) leaves the focus on the files with the flags 0x14 (in menu
        // mode, a pop-up menu); nothing open, 0.
        QCOMPARE(focusOf(0, true), Focus::ItemsView);
        QCOMPARE(focusOf(0x14, true), Focus::Menu);
        QCOMPARE(focusOf(kGuiInMenuMode | kGuiSystemMenuMode, true), Focus::Menu); // the system menu
        QCOMPARE(focusOf(kGuiInMenuMode, true), Focus::Menu);
        QCOMPARE(focusOf(0x14, false), Focus::Menu);
        QCOMPARE(focusOf(0, false), Focus::Other); // Windows 11's context menu: the focus in its XAML island
        QCOMPARE(focusOf(0x01, true), Focus::ItemsView); // a caret blinking says nothing of menus
        QVERIFY(!starts('R', true, false, focusOf(0x14, true))); // 属性(R) goes to the menu
        QVERIFY(starts('R', true, false, focusOf(0, true))); // the menu closed (Esc): typing searches again
        // Which Explorer window a key counts in: the one really in front. A
        // window opened in the background said it came forward and got to
        // the front later with no event (a click on it): it counts once it
        // is in front, not before; the window seen at the last event no
        // longer does once another is in front.
        QCOMPARE(typingWindow(1, 1, 0), 1); // in front at the event, still
        QCOMPARE(typingWindow(2, 0, 2), 2); // opened behind, clicked to the front since
        QCOMPARE(typingWindow(3, 0, 2), 0); // opened behind, still behind (Chrome in front)
        QCOMPARE(typingWindow(2, 1, 2), 2); // opened behind another Explorer window, clicked forward
        QCOMPARE(typingWindow(1, 1, 2), 1); // ... that one still in front
        QCOMPARE(typingWindow(3, 1, 0), 0); // another window came forward silently
        QCOMPARE(typingWindow(0, 1, 2), 0); // nothing in front (switching)
        QCOMPARE(typingWindow(0, 0, 0), 0); // no Explorer window: the hook is not there
        // ... and one opened behind before Win顺 started (its event unseen),
        // kept as `behind` at the start: the topmost not in front that is
        // still the active window of its own thread.
        using Folders = std::vector<OpenFolder<int>>;
        QCOMPARE(openedBehind(9, Folders {}), 0); // no folder window open
        QCOMPARE(openedBehind(9, Folders {{1, false}, {2, false}}), 0); // switched away from: they come back with an event
        QCOMPARE(openedBehind(9, Folders {{1, false}, {2, true}}), 2); // opened behind Notepad (9)
        QCOMPARE(openedBehind(2, Folders {{2, true}}), 0); // in front itself: the first look takes it as `seen`
        QCOMPARE(openedBehind(1, Folders {{1, true}, {2, true}}), 2); // behind the folder window in front
        QCOMPARE(openedBehind(9, Folders {{3, true}, {2, true}}), 3); // two: the topmost (only one is kept)
        QCOMPARE(openedBehind(0, Folders {{2, true}}), 2); // nothing in front (switching): kept, the key looks
        // The keys typed again go by first; the ones typed meanwhile wait
        // behind the last of them (GitHub #39: "qz" came out "zq").
        Retyped retyped;
        QVERIFY(!retyped.pending());
        QVERIFY(!retyped.passed('Q', false)); // nothing sent: nothing to wait for
        retyped.sent({{'Q', false}, {'Q', true}});
        QVERIFY(retyped.pending());
        QVERIFY(!retyped.passed(kVkEscape, false)); // another of Win顺's keys (the Start menu's Esc)
        QVERIFY(!retyped.passed('Q', true)); // not the next one
        QVERIFY(!retyped.passed('Q', false));
        QVERIFY(retyped.pending());
        QVERIFY(retyped.passed('Q', true)); // the last: the keys typed meanwhile follow now
        QVERIFY(!retyped.pending());
        QVERIFY(!retyped.passed('Q', true)); // gone by already
        retyped.sent({{kVkLShift, false}, {'A', false}, {kVkLShift, true}, {'A', true}}); // a capital, Shift let go since
        QVERIFY(!retyped.passed('A', false));
        QVERIFY(!retyped.passed(kVkLShift, false));
        retyped.clear(); // the hold ended (it waited too long)
        QVERIFY(!retyped.pending());
        QVERIFY(!retyped.passed('A', false));
        // The keys go to the launcher only while Win顺 is in front (GitHub
        // #66): Qt called it active with the Start menu still in front, or
        // the Start menu took the front back, and they went into Windows'
        // search.
        QCOMPARE(delivery({.oursInFront = true, .launcherReady = true, .forStartMenu = true}), Delivery::Type);
        QCOMPARE(delivery({.oursInFront = true, .launcherReady = true, .startMenuInFront = true, .forStartMenu = true,
                     .typedAlready = true, .msSinceAsked = 5000}),
            Delivery::Type); // ours in front: whatever else
        FrontState back {.launcherReady = true, .startMenuInFront = true, .forStartMenu = true, .msSinceAsked = 300};
        QCOMPARE(delivery(back), Delivery::Wait); // its Esc may still be closing it
        back.msSinceAsked = kAskAgainMs;
        QCOMPARE(delivery(back), Delivery::AskAgain); // back in front: ask again
        back.asksAgain = kMaxAsksAgain - 1;
        back.msSinceAsked = 900;
        QCOMPARE(delivery(back), Delivery::AskAgain);
        back.asksAgain = kMaxAsksAgain;
        back.msSinceAsked = 5000;
        QCOMPARE(delivery(back), Delivery::Wait); // asked enough: the hold runs out
        // Some typed into it already: shown again it would select them, and
        // the rest would replace them.
        back.asksAgain = 0;
        back.typedAlready = true;
        QCOMPARE(delivery(back), Delivery::Wait);
        back.typedAlready = false;
        back.startMenuInFront = false;
        QCOMPARE(delivery(back), Delivery::Wait); // another window (the one before, coming and going)
        back.startMenuInFront = true;
        back.forStartMenu = false;
        QCOMPARE(delivery(back), Delivery::Wait); // typed in Explorer: not the Start menu's to ask for
        // And only while the launcher's word that it is ready holds (GitHub
        // #103). Gone before it was ready (App's hideLauncher), or asked for
        // again, it is shown anew, cloaked, its field not the focus object
        // yet: Win顺 in front is not enough, the keys wait for its word again.
        QCOMPARE(delivery({.oursInFront = true, .forStartMenu = true}), Delivery::Wait);
        QCOMPARE(delivery({.oursInFront = true, .startMenuInFront = true, .forStartMenu = true, .typedAlready = true,
                     .asksAgain = 1, .msSinceAsked = 5000}),
            Delivery::Wait); // whatever else
        QCOMPARE(delivery({.startMenuInFront = true, .forStartMenu = true, .msSinceAsked = kAskAgainMs}),
            Delivery::AskAgain); // the Start menu in front: asked for
        QCOMPARE(delivery({.startMenuInFront = true, .forStartMenu = true, .asksAgain = kMaxAsksAgain, .msSinceAsked = 5000}),
            Delivery::Wait);
        QCOMPARE(delivery({.msSinceAsked = 5000}), Delivery::Wait);
        // One deadline a hold: kWaitForLauncherMs from its first key (App's
        // handoff gives up on a launcher that never became active as long
        // after asking). Asked for again, the launcher gets at most a second
        // more from then, and the hold never runs past kHoldCapMs (it ran to
        // 9 s when each ask started the whole wait over).
        QVERIFY(kAskAgainMs < kAskAgainWaitMs && kAskAgainWaitMs <= 1000);
        QVERIFY(kWaitForLauncherMs < kHoldCapMs);
        QCOMPARE(holdDeadline(kWaitForLauncherMs, 600), kWaitForLauncherMs); // asked early: no longer
        QCOMPARE(holdDeadline(kWaitForLauncherMs, 2800), 3800u); // late: a second from then
        QCOMPARE(holdDeadline(3800, 3400), kHoldCapMs); // never past the cap
        QCOMPARE(holdDeadline(kHoldCapMs, 9000), kHoldCapMs);
        // The keys held back, typed again in order, each with Shift as it was
        // pressed (GitHub #103): Shift goes on as it comes, so by then it can
        // be up for a capital ("Desktop" came out "desktop") or down for a
        // small letter. Wherever an input method would see it pressed and let
        // go with no key in between (its switch between Chinese and English:
        // Sogou in Chinese, typing "Desktop", ended up in English) and the user
        // made no such tap, the mask goes in; one the user made goes as it is.
        const auto press = [](unsigned vk, bool shift = false) { return HeldKey {.vk = vk, .scanCode = 0x1E, .shift = shift}; };
        const auto release = [](unsigned vk) { return HeldKey {.vk = vk, .scanCode = 0x1E, .release = true}; };
        const auto plan = [](const std::vector<HeldKey>& held, ShiftState window, ShiftState user) {
            QStringList strokes;
            for (const Stroke& stroke : retype(held, window, user)) {
                const QString name = stroke.vk == kVkLShift ? u"LShift"_s
                    : stroke.vk == kVkRShift                ? u"RShift"_s
                    : stroke.vk == kMaskKey                 ? u"mask"_s
                    : stroke.vk == kVkPacket                ? u"char"_s
                    : stroke.vk == kVkCapital               ? u"Caps"_s
                                                            : QString(QChar(stroke.vk));
                strokes << name + QLatin1Char(stroke.release ? '-' : '+');
            }
            return strokes.join(u' ');
        };
        const ShiftState up;
        const ShiftState left {.keys = {.left = true}}; // down, keys pressed since
        const ShiftState leftAlone {.keys = {.left = true}, .alone = true}; // down, nothing pressed since
        const ShiftState right {.keys = {.right = true}};
        const ShiftState rightAlone {.keys = {.right = true}, .alone = true};
        // Shift let go before they went: pressed for the capital alone.
        QCOMPARE(plan({press('D', true), release('D'), press('E'), release('E')}, up, up), u"LShift+ D+ D- LShift- E+ E-"_s);
        QCOMPARE(plan({press('A', true)}, up, up), u"LShift+ A+ LShift-"_s);
        // Still held over all of them: nothing to add.
        QCOMPARE(plan({press('A', true), release('A'), press('B', true), release('B')}, left, left), u"A+ A- B+ B-"_s);
        // Pressed since for the next ones ("macOS"): let go for the small
        // letters, the window in front having seen it go down with nothing
        // since (the mask first), pressed again for the capital.
        QCOMPARE(plan({press('M'), release('M'), press('O', true), release('O')}, leftAlone, left),
            u"mask+ mask- LShift- M+ M- LShift+ O+ O-"_s);
        // ... the window saw a key since (the mask the hook sent at once for
        // the O held, or Ctrl): no mask needed to let go.
        QCOMPARE(plan({press('M'), release('M'), press('O', true), release('O')}, left, left),
            u"LShift- M+ M- LShift+ O+ O-"_s);
        // ... with no key held for it yet: pressed again at the end. The
        // user's release comes next: a tap if the user made one (pressed it,
        // nothing since: it reaches the window as one), the mask after it if
        // not (a key typed with it went on, Ctrl).
        QCOMPARE(plan({press('M'), release('M')}, leftAlone, leftAlone), u"mask+ mask- LShift- M+ M- LShift+"_s);
        QCOMPARE(plan({press('M'), release('M')}, leftAlone, left), u"mask+ mask- LShift- M+ M- LShift+ mask+ mask-"_s);
        QCOMPARE(plan({press('M')}, rightAlone, right), u"mask+ mask- RShift- M+ RShift+ mask+ mask-"_s);
        // The mask before the user's own Shift only: Win顺's goes with a key.
        QCOMPARE(plan({press('M'), press('O', true), press('N')}, leftAlone, left),
            u"mask+ mask- LShift- M+ LShift+ O+ LShift- N+ LShift+ mask+ mask-"_s);
        // A program's characters and the releases go as they are.
        QCOMPARE(plan({press(kVkPacket), release('A')}, left, left), u"char+ A-"_s);
        QCOMPARE(plan({press(kVkPacket, true)}, up, up), u"char+"_s);
        QCOMPARE(plan({}, left, left), QString());
        // The user let go of Shift or pressed it while keys typed again were
        // on their way, and those went by around it ("Desktop" typed in
        // Explorer: the e came in among the D's keys): from Shift as the
        // window has it to the user's, with the next keys or alone before the
        // hold ends. Left as it was, it stayed down after the capital.
        QCOMPARE(plan({release('M')}, leftAlone, up), u"M- mask+ mask- LShift-"_s);
        QCOMPARE(plan({press('C'), release('C')}, leftAlone, up), u"mask+ mask- LShift- C+ C-"_s);
        QCOMPARE(plan({}, leftAlone, up), u"mask+ mask- LShift-"_s);
        QCOMPARE(plan({}, left, up), u"LShift-"_s); // after a capital: no tap to see
        QCOMPARE(plan({}, up, left), u"LShift+ mask+ mask-"_s);
        QCOMPARE(plan({}, up, leftAlone), u"LShift+"_s); // the user's tap, still to come, stays one
        QCOMPARE(plan({}, rightAlone, left), u"mask+ mask- RShift- LShift+ mask+ mask-"_s);
        QCOMPARE(plan({press('A', true)}, up, right), u"LShift+ A+ LShift- RShift+ mask+ mask-"_s);
        QCOMPARE(plan({}, up, up), QString());
        QCOMPARE(plan({press('O', true), release('O')}, up, left), u"LShift+ O+ O-"_s);
        // The user's own Shift and Caps Lock held back with the keys (Hold) go
        // where they came, as they came: a tap made while keys typed again
        // were on their way is a tap; Shift pressed between the keys makes
        // the capitals after it; Caps Lock changes only the keys after it.
        const HeldKey shiftDown {.vk = kVkLShift, .scanCode = 0x2A};
        const HeldKey shiftUp {.vk = kVkLShift, .scanCode = 0x2A, .release = true};
        QCOMPARE(plan({shiftDown, shiftUp}, up, up), u"LShift+ LShift-"_s);
        QCOMPARE(plan({press('M'), shiftDown, press('O', true), release('O'), shiftUp}, up, up),
            u"M+ LShift+ O+ O- LShift-"_s);
        QCOMPARE(plan({press('A'), release('A'), {.vk = kVkCapital, .scanCode = 0x3A},
                     {.vk = kVkCapital, .scanCode = 0x3A, .release = true}, press('B'), release('B')},
                     up, up),
            u"A+ A- Caps+ Caps- B+ B-"_s);
        // Ctrl went on between the user's Shift held back and its release: not
        // a tap of theirs, the mask between them (Hold::went puts it there).
        QCOMPARE(plan({shiftDown, {.vk = kMaskKey}, shiftUp}, up, up), u"LShift+ mask+ mask- LShift-"_s);
        // The keys typed with it were dropped (the Start menu's): the press
        // goes, the mask after it, the user's release is no tap.
        QCOMPARE(plan({shiftDown}, up, left), u"LShift+ mask+ mask-"_s);
        // Shift as the hook sees it go by; VK_SHIFT is the left one.
        Shifts shifts;
        shifts.seen(kVkLShift, true);
        QVERIFY((shifts == Shifts {.left = true}));
        shifts.seen(kVkRShift, true);
        shifts.seen(kVkShift, false);
        QVERIFY((shifts == Shifts {.right = true}));
        shifts.seen('A', true); // not Shift
        QVERIFY((shifts == Shifts {.right = true}));
        QVERIFY(shifts.any() && !Shifts {}.any());
        // ... and whether it went down with nothing pressed since: let go
        // then, it is a tap.
        ShiftState state;
        state.seen(kVkLShift, true);
        QVERIFY(state.alone);
        state.seen(kVkLShift, true); // a repeat
        QVERIFY(state.alone);
        state.seen('A', true);
        QVERIFY(!state.alone);
        state.seen(kVkLShift, true); // a repeat after a key: still not
        QVERIFY(!state.alone);
        state.seen(kVkLShift, false);
        QVERIFY((state == ShiftState {}));
        state.seen(kVkRShift, true);
        state.seen(kMaskKey, true); // the mask is a key between
        QVERIFY(!state.alone && state.keys.right);
        state.seen(kVkRShift, false);
        state.seen(kVkLShift, true);
        state.seen('A', false); // a release is not
        QVERIFY(state.alone);
        // Each key with its own scan code and extended flag, the user's Shift
        // with its own; the Shift Win顺 adds and the mask with none (the hook's
        // keyboard layout gives it), the mask never extended.
        const std::vector<Stroke> carried
            = retype({{.vk = kVkLeft, .scanCode = 0x4B, .extended = true}, press('A', true)}, {}, {});
        QCOMPARE(carried.size(), std::size_t(4));
        QVERIFY((carried[0] == Stroke {.vk = kVkLeft, .scanCode = 0x4B, .extended = true}));
        QVERIFY((carried[1] == Stroke {.vk = kVkLShift}));
        QVERIFY((carried[2] == Stroke {.vk = 'A', .scanCode = 0x1E}));
        QVERIFY((carried[3] == Stroke {.vk = kVkLShift, .release = true}));
        const std::vector<Stroke> masked = retype({press('M')}, leftAlone, left);
        QCOMPARE(masked.size(), std::size_t(7));
        QVERIFY((masked[0] == Stroke {.vk = kMaskKey}));
        QVERIFY((masked[1] == Stroke {.vk = kMaskKey, .release = true}));
        QVERIFY((masked[2] == Stroke {.vk = kVkLShift, .release = true}));
        QVERIFY((masked[3] == Stroke {.vk = 'M', .scanCode = 0x1E}));
        QVERIFY((masked[4] == Stroke {.vk = kVkLShift}));
        QVERIFY((masked[5] == Stroke {.vk = kMaskKey}));
        QVERIFY((masked[6] == Stroke {.vk = kMaskKey, .release = true}));
        const std::vector<Stroke> theirs = retype({{.vk = kVkRShift, .scanCode = 0x36}}, up, right);
        QCOMPARE(theirs.size(), std::size_t(3));
        QVERIFY((theirs[0] == Stroke {.vk = kVkRShift, .scanCode = 0x36})); // the user's, as it came
        QVERIFY((theirs[1] == Stroke {.vk = kMaskKey}));
        // What a batch waits to see go by: not the mask; a character as VK_PACKET.
        const std::vector<Retyped::Key> waitedFor = awaited({{.vk = kMaskKey}, {.vk = kMaskKey, .release = true},
            {.vk = kVkLShift, .release = true}, {.vk = kVkPacket, .scanCode = 0x017C}, {.vk = 'A', .scanCode = 0x1E}});
        QCOMPARE(waitedFor.size(), std::size_t(3));
        QVERIFY(waitedFor[0].vk == kVkLShift && waitedFor[0].release);
        QVERIFY(waitedFor[1].vk == kVkPacket && !waitedFor[1].release);
        QVERIFY(waitedFor[2].vk == 'A' && !waitedFor[2].release);
        // AltGr is the right Alt with the left Ctrl Windows makes up for it
        // (its scan code marked): typing. The right Alt alone, or with the
        // left Ctrl pressed for real, makes shortcuts.
        Modifiers modifiers;
        modifiers.seen(kVkLControl, 0x1D | kSimulatedScan, true);
        QVERIFY(!modifiers.chord() && !modifiers.altGr());
        modifiers.seen(kVkRMenu, 0x38, true);
        QVERIFY(!modifiers.chord() && modifiers.altGr());
        modifiers.seen(kVkRMenu, 0x38, false); // the left Ctrl's release, should it come unmarked, is not Ctrl's
        QVERIFY(!modifiers.chord() && !modifiers.altGr());
        modifiers.seen(kVkLControl, 0x1D | kSimulatedScan, false);
        modifiers.seen(kVkRMenu, 0x38, true);
        QVERIFY(modifiers.chord() && !modifiers.altGr()); // no AltGr on the layout: Alt
        modifiers.seen(kVkLControl, 0x1D, true);
        QVERIFY(modifiers.chord() && !modifiers.altGr()); // Ctrl+Alt
        modifiers = {};
        modifiers.seen(kVkLControl, 0x1D, true);
        modifiers.seen(kVkRMenu, 0x38, true); // AltGr with Ctrl held: Windows adds no Ctrl, a shortcut
        QVERIFY(modifiers.chord() && !modifiers.altGr());
        modifiers = {};
        for (const unsigned vk : {kVkControl, kVkRControl, kVkMenu, kVkLMenu, kVkLWin, kVkRWin}) {
            modifiers.seen(vk, 0, true);
            QVERIFY(modifiers.chord());
            modifiers.seen(vk, 0, false);
            QVERIFY(!modifiers.chord());
        }
    }

    // A hold played out (HoldRun, as platform/TypeToSearch drives it): the
    // keys typed while the launcher is asked for reach it in order, with
    // Shift, Caps Lock and AltGr as they were typed; shortcuts go at once;
    // and every way a hold ends leaves Shift as the user holds it, and ends
    // it (its hook may go, the Raw Input set aside comes back).
    void typingHold()
    {
        using namespace searchtyping;
        const Shifts none;
        const Shifts left {.left = true};
        const Hold::Front startMenu {.startMenu = true, .sourceFocused = true};
        // "baidu" in the Start menu, the launcher ready once "ai" was held;
        // "d" typed while those were on their way follows them (GitHub #39,
        // #66: it came out "dbaiu"), "u" after the hold.
        {
            HoldRun run;
            run.start('B');
            QCOMPARE(run.timer.value_or(0), kWaitForLauncherMs);
            run.release('B');
            run.letters("ai");
            run.run();
            QVERIFY(run.window.empty());
            run.letters("d");
            run.during(1);
            run.deliver();
            QVERIFY(run.ended);
            run.letters("u");
            run.run();
            QCOMPARE(run.typed(), u"baidu"_s);
            QCOMPARE(run.batches.size(), std::size_t(2));
            QVERIFY(run.windowShift() == none && run.stuck().isEmpty() && run.taps() == 0);
        }
        // Capitals typed with Shift held from before: the mask goes to the
        // window at the first key held (it saw Shift go down, then nothing),
        // and the user's release later is no tap.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('B', false, left);
            QCOMPARE(run.masksNow, 1);
            run.release('B');
            run.letters("aidu");
            run.run();
            run.deliver();
            run.release(kVkLShift);
            run.run();
            QVERIFY(run.ended);
            QCOMPARE(run.typed(), u"BAIDU"_s);
            QCOMPARE(run.masksNow, 1);
            QVERIFY(run.windowShift() == none && run.taps() == 0 && run.stuck().isEmpty());
        }
        // "macOS": Shift pressed after "mac" was held goes on (nothing on its
        // way); the small letters go without it, the capitals with it.
        {
            HoldRun run;
            run.start('M');
            run.release('M');
            run.letters("ac");
            run.press(kVkLShift);
            run.letters("os");
            run.release(kVkLShift);
            run.run();
            QCOMPARE(run.masksNow, 1); // O held with Shift seen alone by the window
            run.deliver();
            QCOMPARE(run.typed(), u"macOS"_s);
            QVERIFY(run.ended && run.windowShift() == none && run.taps() == 0 && run.stuck().isEmpty());
        }
        // Shift let go while the capitals were held: Win顺 presses it for them.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.letters("b");
            run.release(kVkLShift);
            run.run();
            run.deliver();
            QCOMPARE(run.typed(), u"AB"_s);
            QVERIFY(run.ended && run.windowShift() == none && run.taps() == 0 && run.stuck().isEmpty());
        }
        // ... let go while they were on their way, between them: the release
        // goes on (GitHub #91), so the capital after it is small (the known
        // case, docs/pitfalls.md); Shift ends as the user has it all the same.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.letters("b");
            run.run();
            run.release(kVkLShift);
            run.during(2);
            run.deliver();
            QCOMPARE(run.typed(), u"Ab"_s);
            QVERIFY(run.ended && run.windowShift() == none && run.taps() == 0 && run.stuck().isEmpty());
        }
        // A tap of Shift made while keys typed again were on their way (to
        // switch the input method): held back, its release with it, and typed
        // again as a tap, after the keys before it (GitHub #103).
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.run();
            run.tap(kVkLShift);
            run.letters("b");
            run.during(1);
            run.deliver();
            QCOMPARE(run.trace(), u"A+ A- LShift+ LShift- B+ B-"_s);
            QCOMPARE(run.taps(), 1);
            QVERIFY(run.ended && run.windowShift() == none);
        }
        // Shift pressed while nothing was on its way goes on; the small
        // letter held from before goes without it, then Shift is pressed
        // again for the user: their release makes the tap they made...
        {
            HoldRun run;
            run.start('M');
            run.release('M');
            run.press(kVkLShift);
            run.run();
            run.deliver();
            run.release(kVkLShift);
            run.run();
            QCOMPARE(run.trace(), u"LShift+ mask+ mask- LShift- M+ M- LShift+ LShift-"_s);
            QCOMPARE(run.taps(), 1);
        }
        // ... but not when Ctrl went down with it meanwhile: Win顺's press is
        // masked.
        {
            HoldRun run;
            run.start('M');
            run.release('M');
            run.press(kVkLShift);
            run.tap(kVkLControl);
            run.run();
            run.deliver();
            run.release(kVkLShift);
            run.run();
            QCOMPARE(run.trace(), u"LShift+ LCtrl+ LCtrl- LShift- M+ M- LShift+ mask+ mask- LShift-"_s);
            QCOMPARE(run.taps(), 0);
        }
        // Ctrl+Shift+T while keys typed again are on their way: Shift is not
        // held back with Ctrl down, the shortcut goes at once, whole.
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.run();
            run.press(kVkLControl);
            run.press(kVkLShift);
            run.tap('T');
            run.release(kVkLShift);
            run.release(kVkLControl);
            run.during(1);
            run.deliver();
            QCOMPARE(run.trace(), u"A+ LCtrl+ LShift+ T+ T- LShift- LCtrl- A-"_s);
            QVERIFY(run.ended && run.windowShift() == none);
        }
        // Shortcuts while the keys wait go at once (Ctrl+C), and a key held
        // when Ctrl went down is let go in order: its release, let through,
        // came before its press typed again and left it down.
        {
            HoldRun run;
            run.start('B');
            run.press('A');
            run.press(kVkLControl);
            run.tap('C');
            run.release('A');
            run.release(kVkLControl);
            run.release('B');
            run.run();
            QCOMPARE(run.trace(), u"LCtrl+ C+ C- LCtrl-"_s);
            run.deliver();
            QCOMPARE(run.trace(), u"LCtrl+ C+ C- LCtrl- B+ A+ A- B-"_s);
            QVERIFY(run.ended && run.stuck().isEmpty());
        }
        // A shortcut still held when the launcher is ready (Ctrl+Shift+F24
        // while "ba" waited, live): the keys wait for its Ctrl to be let go;
        // typed with it down, the b came as Ctrl+B ("aidu").
        {
            HoldRun run;
            run.start('B');
            run.release('B');
            run.letters("a");
            run.press(kVkLControl);
            run.press(kVkLShift);
            run.tap(0x87); // F24
            run.release(kVkLShift);
            run.run();
            run.deliver();
            QVERIFY(run.looking && !run.ended);
            QCOMPARE(run.trace(), u"LCtrl+ LShift+ <87>+ <87>- LShift-"_s);
            run.lookAgainAt(300); // still held
            QVERIFY(run.batches.empty() && !run.ended);
            run.release(kVkLControl);
            run.run();
            run.lookAgainAt(320);
            QCOMPARE(run.trace(), u"LCtrl+ LShift+ <87>+ <87>- LShift- LCtrl- B+ B- A+ A-"_s);
            QVERIFY(run.ended && run.stuck().isEmpty() && run.windowShift() == none && run.taps() == 0);
        }
        // ... given back to the file display just the same.
        {
            HoldRun run;
            run.start('A', true);
            run.release('A');
            run.letters("b");
            run.press(kVkLControl);
            run.run();
            run.giveBack();
            QVERIFY(run.looking && !run.ended && run.typed().isEmpty());
            run.release(kVkLControl);
            run.run();
            run.lookAgainAt(200);
            QCOMPARE(run.typed(), u"ab"_s);
            QVERIFY(run.ended && run.stuck().isEmpty());
        }
        // ... held until the hold runs out: dropped, not typed as shortcuts,
        // even in a file display.
        {
            HoldRun run;
            run.start('A', true);
            run.release('A');
            run.press(kVkLControl);
            run.run();
            run.fire();
            QCOMPARE(run.endedAt - run.startedAt, kWaitForLauncherMs);
            QCOMPARE(run.trace(), u"LCtrl+ A-"_s); // the release goes all the same (keepState): it changes nothing
            QVERIFY(run.ended && run.batches.size() == 1);
            run.release(kVkLControl);
            run.run();
            QCOMPARE(run.stuck(), QString());
        }
        // A number pad key with NumLock on: let go under another code (Shift
        // between); told apart by its scan code, let go as it was pressed.
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.press(0x24, 0x47); // VK_HOME
            run.release(0x67, 0x47); // VK_NUMPAD7
            run.run();
            run.deliver();
            QCOMPARE(run.trace(), u"A+ A- <24>+ <24>-"_s);
            QVERIFY(run.stuck().isEmpty());
        }
        // Caps Lock pressed between the keys held: in order with them,
        // "aB", not "AB".
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.tap(kVkCapital);
            run.letters("b");
            run.run();
            QVERIFY(run.window.empty());
            run.deliver();
            QCOMPARE(run.typed(), u"aB"_s);
            QCOMPARE(run.trace(), u"A+ A- Caps+ Caps- B+ B-"_s);
        }
        // AltGr between the keys held (Polish "zażółć", German @): typing, not
        // a shortcut. Each such key is held as the character it types and
        // typed again in order; one that types nothing goes on. Ctrl+C
        // still goes at once.
        {
            HoldRun run;
            run.altGr = [](unsigned vk) {
                return vk == 'Z' ? std::u16string(1, char16_t(0x017C)) : vk == 'Q' ? std::u16string(u"@") : std::u16string();
            };
            run.start('Z');
            run.release('Z');
            run.letters("a");
            run.altGrKey(true);
            run.tap('Z');
            run.tap('Q');
            run.tap('X');
            run.altGrKey(false);
            run.letters("o");
            run.press(kVkLControl);
            run.tap('C');
            run.release(kVkLControl);
            run.run();
            QCOMPARE(run.trace(), u"AltGrCtrl+ RAlt+ X+ X- RAlt- AltGrCtrl- LCtrl+ C+ C- LCtrl-"_s);
            run.deliver();
            QCOMPARE(run.typed().right(5), u"zaż@o"_s);
            QVERIFY(run.ended && run.stuck().isEmpty());
        }

        // Every way a hold ends: Shift as the user holds it, the hold over.
        // Typed again into the launcher: above. Given back to the file
        // display they were typed in...
        {
            HoldRun run;
            run.start('A', true);
            run.release('A');
            run.letters("bc");
            run.run();
            run.giveBack();
            QCOMPARE(run.typed(), u"abc"_s);
            QVERIFY(run.ended && run.windowShift() == none && run.stuck().isEmpty());
        }
        // ... the user moved on (another window, an edit field): nothing typed
        // into it, Shift as they hold it.
        {
            HoldRun run;
            run.start('A', true);
            run.release('A');
            run.run();
            run.front.sourceFocused = false;
            run.giveBack();
            QCOMPARE(run.typed(), QString());
            QVERIFY(run.ended && run.windowShift() == none && run.stuck().isEmpty());
        }
        // ... typed in a file display, the launcher never came: back to it.
        {
            HoldRun run;
            run.start('A', true);
            run.release('A');
            run.run();
            run.fire();
            QCOMPARE(run.endedAt - run.startedAt, kWaitForLauncherMs);
            QCOMPARE(run.typed(), u"a"_s);
            QVERIFY(run.ended && run.windowShift() == none && run.stuck().isEmpty());
        }
        // ... typed in the Start menu, the launcher never in front again: the
        // keys dropped (they would go into Windows' search), but the user's
        // Shift held back with them goes, as they hold it (it was dropped
        // too, and the window never had it), the mask after it (the user typed
        // with it: their release is no tap); Caps Lock pressed among them goes,
        // and so do the releases (B's changes nothing: its press was dropped).
        for (const bool capsLock : {false, true}) {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.run();
            run.press(kVkLShift); // while the A goes by
            run.tap('B');
            if (capsLock)
                run.tap(kVkCapital);
            run.during(1);
            run.postponePassed = true;
            run.deliver();
            run.front = startMenu; // took the front back as they went
            run.postponePassed = false;
            run.passedNow();
            QVERIFY(!run.ended && run.looking);
            run.fire();
            QCOMPARE(run.endedAt - run.startedAt, kWaitForLauncherMs);
            QCOMPARE(run.typed(), u"a"_s);
            QCOMPARE(run.trace(), capsLock ? u"A+ A- LShift+ B- Caps+ Caps-"_s : u"A+ A- LShift+ B- mask+ mask-"_s);
            QVERIFY(run.ended && run.windowShift() == left && run.stuck().isEmpty());
            run.release(kVkLShift);
            run.run();
            QCOMPARE(run.taps(), 0);
        }
        // ... a key typed again whose release comes as that batch goes by (held
        // behind it: its press was taken), and the launcher loses the front:
        // the hold runs out, or Win顺 lets go of it, and the keys held are
        // dropped, but not that release. Dropped, B stayed down for Windows:
        // its press got there, typed again, and its release was taken.
        for (const bool stopped : {false, true}) {
            HoldRun run;
            run.start('B');
            run.release('B');
            run.press('A');
            run.during(1);
            run.postponePassed = true;
            run.deliver();
            run.front = {}; // another window took the front
            run.postponePassed = false;
            run.passedNow();
            QVERIFY(run.looking && !run.ended);
            if (stopped)
                run.stop();
            else
                run.fire();
            QVERIFY(run.ended);
            QCOMPARE(run.trace(), u"B+ B-"_s); // A dropped
            QCOMPARE(run.stuck(), QString());
        }
        // ... the same with Win顺's own Shift left down for the window (a hook
        // after this one took its release): set right while the keys wait,
        // and the hold ends with Shift as the user has it.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.release(kVkLShift);
            run.run();
            int eaten = 0;
            run.takenLater = [&eaten](const HoldRun::Key& key) {
                return key.own && key.vk == kVkLShift && !key.press && eaten++ == 0;
            };
            run.letters("c");
            run.during(2);
            run.postponePassed = true;
            run.deliver();
            QVERIFY(run.windowShift() == left);
            run.front = startMenu;
            run.postponePassed = false;
            run.passedNow();
            QVERIFY(run.windowShift() == none); // set right at once, not at the end
            QVERIFY(!run.ended);
            run.fire();
            QVERIFY(run.ended && run.windowShift() == none && run.stuck().isEmpty());
            QCOMPARE(run.typed(), u"A"_s); // the c dropped
        }
        // ... typed again, and some never went by (a hook before this one took
        // them): after kPassTimeoutMs the rest go after them, and it ends.
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.letters("b");
            run.run();
            run.lostOnTheWay = [](const HoldRun::Key& key) { return key.own && key.vk == 'B' && key.press; };
            run.deliver();
            QVERIFY(!run.ended);
            run.letters("c");
            run.run();
            run.fire();
            QVERIFY(run.ended);
            QCOMPARE(run.endedAt - run.startedAt, kPassTimeoutMs);
            QCOMPARE(run.typed(), u"ac"_s);
            QVERIFY(run.windowShift() == none && run.stuck().isEmpty());
        }
        // ... SendInput put in only some of them: what went down is let go,
        // Shift as the user has it, and it ends.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.release(kVkLShift);
            run.run();
            run.nextSendPutsIn = 2; // LShift+ A+
            run.deliver();
            QVERIFY(run.ended);
            QCOMPARE(run.trace(), u"LShift+ mask+ mask- LShift- LShift+ A+ A- LShift-"_s);
            QVERIFY(run.windowShift() == none && run.stuck().isEmpty());
        }
        // Win顺's Shift taken by a hook after this one every time: set right
        // once more at most, and the hold still ends.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.release(kVkLShift);
            run.run();
            run.takenLater = [](const HoldRun::Key& key) { return key.own && key.vk == kVkLShift && !key.press; };
            run.deliver();
            QVERIFY(run.ended);
            QCOMPARE(run.batches.size(), std::size_t(3));
            QCOMPARE(run.ownShifts(), 4); // its press, and three releases
        }
        // ... a Shift set right that never even goes by: the timer ends it.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.release(kVkLShift);
            run.run();
            run.takenLater = [](const HoldRun::Key& key) { return key.own && key.vk == kVkLShift && !key.press; };
            run.lostOnTheWay = [&run](const HoldRun::Key& key) {
                return key.own && key.vk == kVkLShift && !key.press && run.batches.size() > 1;
            };
            run.deliver();
            QVERIFY(!run.ended);
            run.fire();
            QCOMPARE(run.batches.size(), std::size_t(3)); // the keys, the fix-up lost, Shift once more
            QVERIFY(!run.ended); // ... waited for as well (lost too)
            run.fire();
            QVERIFY(run.ended && run.stuck().isEmpty());
            QCOMPARE(run.batches.size(), std::size_t(3));
        }

        // One deadline a hold: the launcher asked for again from the
        // Start menu gets at most kAskAgainWaitMs from then, never past
        // kHoldCapMs; an early ask moves nothing.
        {
            HoldRun run;
            run.front = startMenu;
            run.start('A');
            run.release('A');
            run.run();
            run.deliver();
            QVERIFY(run.looking && run.asksAgain == 0);
            run.lookAgainAt(700);
            QCOMPARE(run.asksAgain, 1);
            run.lookAgainAt(2800);
            QCOMPARE(run.asksAgain, 2);
            QCOMPARE(run.timer.value_or(0), 1000u);
            QVERIFY(run.hold.timeout(run.front, run.startedAt + kWaitForLauncherMs).timerMs == 800u); // the old deadline: not yet
            run.lookAgainAt(3500);
            QCOMPARE(run.asksAgain, kMaxAsksAgain);
            run.fire();
            QVERIFY(run.ended);
            QCOMPARE(run.endedAt - run.startedAt, 3800u);
            QCOMPARE(run.typed(), QString());
            QVERIFY(run.stuck().isEmpty());
        }
        {
            HoldRun run;
            run.front = startMenu;
            run.start('A');
            run.release('A');
            run.run();
            run.deliver();
            run.lookAgainAt(2600);
            run.lookAgainAt(3300);
            QCOMPARE(run.asksAgain, 2);
            run.fire();
            QCOMPARE(run.endedAt - run.startedAt, kHoldCapMs);
            QVERIFY(run.ended && run.stuck().isEmpty());
        }

        // Win顺 lets go mid-hold (TypeToSearch goes): the keys held are
        // dropped, Shift goes as the user holds it, after the batch still on
        // its way. Here A's release typed again never gets here (a hook before
        // this one took it), and the user pressed Shift meanwhile.
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.run();
            run.lostOnTheWay = [](const HoldRun::Key& key) { return key.vk == 'A' && !key.press; };
            run.deliver();
            run.tap('B');
            run.press(searchtyping::kVkLShift);
            run.run();
            QVERIFY(!run.ended && !run.windowShift().any()); // held behind the batch
            run.stop();
            QVERIFY(run.ended && !run.hold.holding());
            QCOMPARE(run.trace(), u"A+ B- LShift+"_s); // B dropped but for its release, Shift as it came
            QCOMPARE(run.windowShift(), (searchtyping::Shifts {.left = true}));
            run.release(searchtyping::kVkLShift);
            run.run();
            QCOMPARE(run.taps(), 1); // the user's own tap
        }
        {
            HoldRun run; // waiting for Win顺 to be in front again, the user's Shift held among the keys
            run.start('A');
            run.release('A');
            run.run();
            run.press(searchtyping::kVkLShift);
            run.during(1);
            run.postponePassed = true;
            run.deliver();
            run.front.ours = false;
            run.passedNow();
            QVERIFY(run.looking && !run.ended);
            QCOMPARE(run.trace(), u"A+ A-"_s);
            run.stop();
            QVERIFY(run.ended);
            QCOMPARE(run.trace(), u"A+ A- LShift+"_s);
            run.release(searchtyping::kVkLShift);
            run.run();
            QCOMPARE(run.taps(), 1);
            QVERIFY(run.hold.stop().send.empty()); // idle: nothing
        }

        // A character a program sends (VK_PACKET, the character in the scan
        // code) is held as a key, also one with the bit Windows marks its
        // made-up keys with: 你 is U+4F60. Taken for made up, it went on at
        // once, ahead of the keys held before it (second review).
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.press(kVkPacket, 0x4F60);
            run.release(kVkPacket, 0x4F60);
            run.run();
            QVERIFY(run.window.empty());
            run.deliver();
            QCOMPARE(run.typed(), u"a你"_s);
            QVERIFY(run.ended && run.stuck().isEmpty());
        }
        // Dropped in the Start menu, Shift+H left Shift alone: a tap the
        // user never made, which switched the input method. The mask goes in
        // before its release; a tap the user did make stays one.
        for (const bool tapFirst : {false, true}) {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.run();
            if (tapFirst) {
                run.press(kVkLShift);
                run.release(kVkLShift);
            }
            run.press(kVkLShift); // while the A goes by: held, as the keys after it
            run.tap('H');
            run.release(kVkLShift);
            run.during(1);
            run.postponePassed = true;
            run.deliver();
            run.front = startMenu;
            run.postponePassed = false;
            run.passedNow();
            QVERIFY(!run.ended && run.looking);
            run.fire();
            QVERIFY(run.ended);
            QCOMPARE(run.typed(), u"a"_s);
            QCOMPARE(run.taps(), tapFirst ? 1 : 0);
            QCOMPARE(run.windowShift(), none);
            QCOMPARE(run.stuck(), QString());
        }
        // The user lets go of Shift and presses it again while the keys wait
        // (a tap to come): the keys typed again go in after it, so it is let
        // go and pressed again after them, and the user's release is a tap.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.tap('B');
            run.release(kVkLShift); // goes on: nothing on its way, no Shift held among the keys
            run.press(kVkLShift);
            run.run();
            run.deliver();
            QVERIFY(run.ended);
            QCOMPARE(run.typed(), u"AB"_s);
            run.release(kVkLShift);
            run.run();
            QCOMPARE(run.taps(), 1);
            QCOMPARE(run.stuck(), QString());
        }
        // Caps Lock with Ctrl held (a Japanese input method's switch) goes on
        // at once, as every shortcut does.
        {
            HoldRun run;
            run.start('A');
            run.press(kVkLControl);
            run.tap(kVkCapital);
            run.release(kVkLControl);
            run.release('A');
            run.run();
            QCOMPARE(run.trace(), u"LCtrl+ Caps+ Caps- LCtrl-"_s);
            run.deliver();
            QCOMPARE(run.trace(), u"LCtrl+ Caps+ Caps- LCtrl- A+ A-"_s);
        }
        // NumLock on, Shift held: Windows lets go of Shift for a number pad
        // key and presses it again after (made-up keys, 0x22A). Held
        // meanwhile, the key is typed again without it: Home, not Shift+Home.
        {
            HoldRun run;
            run.before(kVkLShift, true);
            run.start('A', false, left);
            run.release('A');
            run.release(kVkLShift, 0x2A | kSimulatedScan);
            run.press(0x24, 0x47); // VK_HOME, number pad 7
            run.release(0x24, 0x47);
            run.press(kVkLShift, 0x2A | kSimulatedScan);
            run.run();
            run.deliver();
            QVERIFY(run.ended);
            QCOMPARE(run.trace(), u"LShift+ mask+ mask- LShift- LShift+ A+ A- LShift- <24>+ <24>- LShift+ mask+ mask-"_s);
            QCOMPARE(run.stuck(), QString());
        }
        // The keys typed again never go by (a hook before this one takes
        // them): at the timeout those held since go after them, and the hold
        // ends once those have gone by. SendInput putting in only some of
        // them, what went down is let go (it was left down: the hold had ended).
        {
            HoldRun run;
            run.start('A');
            run.release('A');
            run.run();
            run.lostOnTheWay = [](const HoldRun::Key& key) { return key.vk == 'A'; };
            run.deliver();
            run.tap('B'); // held: A is still on its way
            run.run();
            QVERIFY(!run.ended && run.window.empty());
            run.lostOnTheWay = nullptr;
            run.nextSendPutsIn = 1; // B's press only
            run.fire();
            QVERIFY(run.ended);
            QCOMPARE(run.trace(), u"B+ B-"_s);
            QCOMPARE(run.stuck(), QString());
        }
    }

    // The keyboard's Raw Input set aside while a hold or a recording needs
    // the hook to see the keys typed into Win顺's own windows (GitHub #66),
    // shared and counted (KeyListener's KeyboardRawInputAside): it goes back
    // when the last holder goes, as KeyListener has it then, and double Ctrl
    // is never lost for good.
    void rawInputAside()
    {
        using searchtyping::RawInputAside;
        const RawInputAside::Registration listener {.window = 0x10, .flags = 0x100};
        const RawInputAside::Registration newer {.window = 0x20, .flags = 0x100};
        {
            RawInputAside aside;
            QVERIFY(aside.take()); // the first takes it away
            aside.taken(listener);
            QVERIFY(aside.aside());
            QVERIFY(aside.give() == listener);
            QVERIFY(!aside.aside());
            QVERIFY(!aside.give()); // one too many: nothing
        }
        {
            // A recording while a hold is on (or the other way round): put
            // back when the last goes, not under the other.
            RawInputAside aside;
            QVERIFY(aside.take());
            aside.taken(listener);
            QVERIFY(!aside.take());
            aside.taken(std::nullopt); // never asked: only the first takes it away
            QVERIFY(!aside.give());
            QVERIFY(aside.aside());
            QVERIFY(aside.give() == listener);
        }
        {
            // A new KeyListener registered meanwhile (the settings changed):
            // the old one's window went first, the new one goes back.
            RawInputAside aside;
            QVERIFY(aside.take());
            aside.taken(listener);
            QVERIFY(!aside.removes(listener.window));
            QVERIFY(!aside.registers(newer));
            QVERIFY(aside.give() == newer);
            QVERIFY(aside.registers(listener)); // set aside no more: Windows' now
            QVERIFY(aside.removes(listener.window));
        }
        {
            // Its KeyListener went (double Ctrl and the drop pie off): nothing
            // goes back; another window going leaves it alone.
            RawInputAside aside;
            QVERIFY(aside.take());
            aside.taken(listener);
            QVERIFY(!aside.removes(newer.window));
            QVERIFY(!aside.removes(listener.window));
            QVERIFY(!aside.give());
        }
        {
            // None registered (double Ctrl off), or Windows kept it: nothing to
            // put back.
            RawInputAside aside;
            QVERIFY(aside.take());
            aside.taken(std::nullopt);
            QVERIFY(!aside.give());
        }
        {
            // Windows refused to put it back: kept, and the next holder to go
            // tries again.
            RawInputAside aside;
            QVERIFY(aside.take());
            aside.taken(listener);
            const auto back = aside.give();
            QVERIFY(back == listener);
            aside.notPutBack(*back);
            QVERIFY(aside.take());
            aside.taken(std::nullopt); // not registered now
            QVERIFY(aside.give() == listener);
            // ... unless KeyListener registered since, set aside no more.
            aside.notPutBack(listener);
            QVERIFY(aside.registers(newer));
            QVERIFY(aside.take());
            aside.taken(newer);
            QVERIFY(aside.give() == newer);
        }
    }

    // The keys held for the launcher go once it takes typing as it would a
    // user's (GitHub #103), or kHandoffCapMs after it first became active all
    // the same.
    void typingHandoff()
    {
        using namespace searchtyping;
        const LauncherState ready {.active = true, .fieldFocused = true, .revealed = true};
        QCOMPARE(handoff(ready, 0u), Handoff::Deliver); // a pinned launcher already up: at once
        QCOMPARE(handoff(ready, 5000u), Handoff::Deliver);
        // Active, but still cloaked (its first frame not drawn) or its field
        // not the focus object yet: they wait, until the cap.
        LauncherState cloaked = ready;
        cloaked.revealed = false;
        QCOMPARE(handoff(cloaked, 0u), Handoff::Wait);
        QCOMPARE(handoff(cloaked, kHandoffCapMs - 1), Handoff::Wait);
        QCOMPARE(handoff(cloaked, kHandoffCapMs), Handoff::DeliverLate); // never stranded
        LauncherState unfocused = ready;
        unfocused.fieldFocused = false;
        QCOMPARE(handoff(unfocused, 30u), Handoff::Wait);
        QCOMPARE(handoff(unfocused, kHandoffCapMs + 500), Handoff::DeliverLate);
        // Not active yet: nothing goes, however long ago it was asked for
        // (the hook's hold gives up on the keys).
        LauncherState inactive = ready;
        inactive.active = false;
        QCOMPARE(handoff(inactive, std::nullopt), Handoff::Wait);
        QCOMPARE(handoff(LauncherState {}, std::nullopt), Handoff::Wait);
        // Active once, then not (the Start menu took the front back, GitHub
        // #66): after the cap the keys go to the hook all the same, which
        // types them only into a window of Win顺's in front and asks for the
        // launcher again.
        QCOMPARE(handoff(inactive, 30u), Handoff::Wait);
        QCOMPARE(handoff(inactive, kHandoffCapMs), Handoff::DeliverLate);
        QCOMPARE(handoff(LauncherState {}, kHandoffCapMs + 1), Handoff::DeliverLate);
    }

    // The folder menu's hotkey (GitHub #25): a press opens, closes or
    // forgets, and where a menu opened by a key goes.
    void menuHotkey()
    {
        using menuhotkey::Toggle;
        using Action = Toggle::Action;
        {
            Toggle t;
            int asks = 0;
            QCOMPARE(t.pressed(false, asks), Action::Open);
            t.asked(++asks);
            QVERIFY(t.pending(asks));
            t.answered(asks); // the menu came up
            QVERIFY(!t.pending(asks));
            QCOMPARE(t.pressed(true, asks), Action::Close);
            QCOMPARE(t.pressed(false, asks), Action::Open); // and again
        }
        { // pressed twice before the menu is up: forgotten, the next press opens
            Toggle t;
            int asks = 0;
            QCOMPARE(t.pressed(false, asks), Action::Open);
            t.asked(++asks);
            QCOMPARE(t.pressed(false, asks), Action::Drop);
            ++asks; // the menu moves on, so that the answer is dropped
            QVERIFY(!t.pending(asks));
            QCOMPARE(t.pressed(false, asks), Action::Open);
        }
        { // nothing came of the ask (the window in front changed): the next press opens
            Toggle t;
            int asks = 0;
            t.pressed(false, asks);
            t.asked(++asks);
            t.answered(asks);
            QCOMPARE(t.pressed(false, asks), Action::Open);
        }
        { // a double click asked after us: ours is no longer the latest, and not pending
            Toggle t;
            int asks = 0;
            t.pressed(false, asks);
            t.asked(++asks);
            ++asks;
            QVERIFY(!t.pending(asks));
            QCOMPARE(t.pressed(false, asks), Action::Open);
            t.answered(1); // the old answer, dropped by the menu: changes nothing for the new ask
            QVERIFY(t.pending(asks) == false); // Open was returned, asked() not yet called
            t.asked(++asks);
            QVERIFY(t.pending(asks));
        }
        { // the menu is up (a double click's) while ours is on its way: Close, and ours is forgotten
            Toggle t;
            int asks = 0;
            t.pressed(false, asks);
            t.asked(++asks);
            QCOMPARE(t.pressed(true, asks), Action::Close);
            QVERIFY(!t.pending(asks));
        }
        using menuhotkey::Point;
        const auto same = [](Point a, Point b) { return a.x == b.x && a.y == b.y; };
        QVERIFY(same(menuhotkey::anchor(true, {500, 600}, {10, 20}, 16), Point {500, 600}));
        QVERIFY(same(menuhotkey::anchor(false, {500, 600}, {10, 20}, 16), Point {26, 36}));
        QVERIFY(same(menuhotkey::anchor(false, {500, 600}, {-1920, 0}, 24), Point {-1896, 24})); // a monitor left of the primary
    }

    // The clipboard's hotkey held after it opened the window (GitHub #107):
    // the hook's side (Watch) key by key, the GUI thread's (Hold) by notes
    // and WM_HOTKEYs in the order they came.
    void hotkeySteps()
    {
        using namespace ws::hotkeysteps;
        constexpr unsigned kBackquote = 0xC0, kF9 = 0x78, kV = 0x56, kA = 0x41;
        constexpr unsigned kLCtrl = 0xA2, kRCtrl = 0xA3, kLShift = 0xA0, kLWin = 0x5B;
        using N = Note;
        const auto noted = [](Watch::Answer a, Note note, Note then = Note::None, bool take = false) {
            return a.note == note && a.then == then && a.take == take;
        };
        const auto sides = [](std::initializer_list<unsigned> down) {
            Sides s;
            for (const unsigned vk : down)
                s.set(vk, true);
            return s;
        };

        { // A modifier is held while either side is; the generic codes are the left ones.
            Sides s = sides({kLCtrl, kRCtrl});
            s.set(kLCtrl, false);
            QCOMPARE(s.held(), kCtrl);
            s.set(kRCtrl, false);
            QCOMPARE(s.held(), 0u);
            s.set(0x11, true); // VK_CONTROL
            s.set(kLShift, true);
            QCOMPARE(s.held(), kCtrl | kShift);
            s.set(kLCtrl, false); // the same side
            QCOMPARE(s.held(), kShift);
            QCOMPARE(modifierOf(kLWin), kWin);
            QCOMPARE(modifierOf(kBackquote), 0u);
        }
        { // Ctrl+`: held, ` again steps, its repeats taken, letting go of all ends it.
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), true, 1000);
            QVERIFY(w.holding());
            QCOMPARE(w.start(), N::None);
            QVERIFY(noted(w.key(kBackquote, true, 1500), N::None, N::None, true)); // its own repeat
            QVERIFY(noted(w.key(kBackquote, false, 1600), N::Lifted));
            QVERIFY(noted(w.key(kBackquote, true, 1700), N::Step)); // goes on: the WM_HOTKEY comes
            QVERIFY(noted(w.key(kBackquote, true, 1750), N::None, N::None, true));
            QVERIFY(noted(w.key(kBackquote, false, 1800), N::None)); // lifted once
            QVERIFY(noted(w.key(kBackquote, true, 1900), N::Step));
            QVERIFY(noted(w.key(kBackquote, false, 1950), N::None));
            QVERIFY(noted(w.key(kLCtrl, true, 1990), N::None)); // Ctrl's own repeat
            QVERIFY(noted(w.key(kLCtrl, false, 2000), N::AllUp));
            QVERIFY(noted(w.key(kBackquote, true, 2100), N::None)); // over: as ever
            QVERIFY(noted(w.key(kVkEscape, true, 2200), N::None)); // Esc too
        }
        { // ... the key let go last: Lifted at Ctrl, AllUp at the key.
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), true, 1000);
            QVERIFY(noted(w.key(kLCtrl, false, 1100), N::Lifted));
            QVERIFY(noted(w.key(kBackquote, false, 1200), N::AllUp));
        }
        { // ... both at once (the last event): Lifted, then AllUp.
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), false, 1000);
            QCOMPARE(w.start(), N::Lifted); // a quick press: the key up before the hook went in
            QCOMPARE(w.start(), N::None);
            QVERIFY(noted(w.key(kLCtrl, false, 1100), N::AllUp));
        }
        { // Let go of one side, the other still down: held.
            Watch w(kCtrl, kBackquote, sides({kLCtrl, kRCtrl}), true, 1000);
            QVERIFY(noted(w.key(kBackquote, false, 1100), N::Lifted));
            QVERIFY(noted(w.key(kLCtrl, false, 1200), N::None));
            QVERIFY(noted(w.key(kBackquote, true, 1300), N::Step));
            QVERIFY(noted(w.key(kBackquote, false, 1350), N::None));
            QVERIFY(noted(w.key(kRCtrl, false, 1400), N::AllUp));
        }
        { // Another modifier with it: another chord, no step and no break.
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), false, 1000);
            QCOMPARE(w.start(), N::Lifted);
            QVERIFY(noted(w.key(kLShift, true, 1100), N::None));
            QVERIFY(noted(w.key(kBackquote, true, 1200), N::None));
            QVERIFY(noted(w.key(kBackquote, false, 1250), N::None));
            QVERIFY(noted(w.key(kLShift, false, 1300), N::None));
            QVERIFY(noted(w.key(kBackquote, true, 1400), N::Step));
            QVERIFY(noted(w.key(kBackquote, false, 1450), N::None));
            QVERIFY(noted(w.key(kLCtrl, false, 1500), N::AllUp)); // not over before
        }
        { // Ctrl+Shift+F9, Shift let go and pressed again: a fresh press, not a step.
            Watch w(kCtrl | kShift, kF9, sides({kLCtrl, kLShift}), true, 1000);
            QVERIFY(noted(w.key(kF9, false, 1100), N::Lifted));
            QVERIFY(noted(w.key(kLShift, false, 1200), N::None)); // Ctrl still down
            QVERIFY(noted(w.key(kLShift, true, 1300), N::None));
            QVERIFY(noted(w.key(kF9, true, 1400), N::Fresh));
        }
        { // Win+V; the modifiers not all down as it began: no hold.
            Watch w(kWin, kV, sides({kLWin}), true, 1000);
            QVERIFY(w.holding());
            QVERIFY(noted(w.key(kV, false, 1100), N::Lifted));
            QVERIFY(noted(w.key(kV, true, 1200), N::Step));
            QVERIFY(!Watch(kCtrl | kShift, kF9, sides({kLCtrl}), true, 1000).holding());
            QVERIFY(!Watch(0, kF9, sides({}), true, 1000).holding()); // a lone F9: nothing to hold
        }
        { // Esc: taken from the start (Ctrl+Esc would open Start), its repeat and release too.
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), true, 1000);
            QVERIFY(noted(w.key(kVkEscape, true, 1100), N::Escape, N::None, true));
            QVERIFY(w.holdsTaken());
            QVERIFY(noted(w.key(kVkEscape, true, 1600), N::None, N::None, true));
            QVERIFY(noted(w.key(kVkEscape, false, 1650), N::None, N::None, true));
            QVERIFY(!w.holdsTaken());
            QVERIFY(noted(w.key(kVkEscape, true, 1700), N::Escape, N::None, true)); // again: the GUI decides
            w.stop(); // the GUI is done with it, the Esc still down
            QVERIFY(noted(w.key(kVkEscape, false, 1750), N::None, N::None, true));
            QVERIFY(!w.holdsTaken());
            QVERIFY(noted(w.key(kVkEscape, true, 1800), N::None)); // stopped: as ever
        }
        { // Another key: the hold means nothing any more (once).
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), true, 1000);
            QVERIFY(noted(w.key(0xFF, true, 1050), N::None)); // part of an escaped sequence
            QVERIFY(noted(w.key(0xE8, true, 1060), N::None)); // a masking key (AutoHotkey's)
            QVERIFY(noted(w.key(kA, false, 1080), N::None)); // a release: no new key
            QVERIFY(noted(w.key(kA, true, 1100), N::Other));
            QVERIFY(noted(w.key(kA, true, 1200), N::None));
            QVERIFY(noted(w.key(kLCtrl, false, 1300), N::None));
        }
        { // A release that went by unseen (the secure desktop): a press long after is a press.
            Watch w(kCtrl, kBackquote, sides({kLCtrl}), true, 1000);
            QVERIFY(noted(w.key(kBackquote, true, 1000 + kRepeatWithinMs - 1), N::None, N::None, true));
            QVERIFY(noted(w.key(kBackquote, true, 1000 + 3 * kRepeatWithinMs), N::Step));
        }

        { // Pressed, let go quickly: it browses; never a hint.
            Hold h(10'000);
            QVERIFY(!h.hint(10'000));
            QCOMPARE(h.note(N::Lifted, 10'120), Hold::Outcome::None);
            QVERIFY(!h.hint(11'500));
            QCOMPARE(h.note(N::AllUp, 10'200), Hold::Outcome::End);
            QCOMPARE(h.note(N::Escape, 10'300), Hold::Outcome::None); // over
            QCOMPARE(h.press(), Hold::Press::Normal); // the window's own from now on
        }
        { // Steps, then let go: pasted. A WM_HOTKEY steps only right after its note.
            Hold h(10'000);
            QCOMPARE(h.press(), Hold::Press::Normal); // no note before it
            QCOMPARE(h.note(N::Lifted, 10'100), Hold::Outcome::None);
            QCOMPARE(h.note(N::Step, 10'200), Hold::Outcome::None);
            QVERIFY(!h.hint(10'200)); // until the WM_HOTKEY is through
            QCOMPARE(h.press(), Hold::Press::Step);
            QVERIFY(h.hint(10'210));
            QCOMPARE(h.press(), Hold::Press::Normal); // each note once
            QCOMPARE(h.note(N::Step, 10'300), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Step);
            QCOMPARE(h.steps(), 2);
            QCOMPARE(h.note(N::Step, 10'400), Hold::Outcome::None);
            QCOMPARE(h.note(N::Fresh, 10'401), Hold::Outcome::None); // another note between: not its WM_HOTKEY
            QCOMPARE(h.press(), Hold::Press::Normal);
            QCOMPARE(h.note(N::AllUp, 10'500), Hold::Outcome::Paste);
            QCOMPARE(h.steps(), 2);
        }
        { // A step 10 ms after the window was there to be seen is a step; the
          // same press as a second one (let go first) would only raise it (A2).
            Hold h(10'000);
            QVERIFY(!seenBy(10'050, 10'060));
            QCOMPARE(h.note(N::Step, 10'060), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Step);
            QCOMPARE(h.note(N::Fresh, 10'200), Hold::Outcome::None); // part of it let go first
            QCOMPARE(h.press(), Hold::Press::Normal);
        }
        { // The whole hotkey held a second, no step: letting go pastes the first row.
            Hold h(10'000);
            QVERIFY(!h.hint(10'000 + kHoldMs - 1));
            QVERIFY(h.hint(10'000 + kHoldMs));
            QCOMPARE(h.note(N::Lifted, 11'100), Hold::Outcome::None);
            QVERIFY(h.hint(11'200));
            QCOMPARE(h.note(N::AllUp, 11'300), Hold::Outcome::Paste);
            QCOMPARE(h.heldFor(12'000), 1'100u);
        }
        { // ... only the whole of it: the key let go early, Ctrl held on.
            Hold h(10'000);
            QCOMPARE(h.note(N::Lifted, 10'300), Hold::Outcome::None);
            QVERIFY(!h.hint(11'500));
            QCOMPARE(h.note(N::AllUp, 11'500), Hold::Outcome::End);
        }
        { // Esc with nothing waiting to be pasted: closes, as ever.
            Hold h(10'000);
            QCOMPARE(h.note(N::Escape, 10'300), Hold::Outcome::Close);
            QCOMPARE(h.note(N::AllUp, 10'400), Hold::Outcome::None); // over
        }
        { // Esc after a step: nothing pasted, the window stays; Esc again closes it.
            Hold h(10'000);
            QCOMPARE(h.note(N::Step, 10'100), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Step);
            QCOMPARE(h.note(N::Escape, 10'200), Hold::Outcome::Cancel);
            QVERIFY(h.cancelled());
            QVERIFY(!h.hint(10'210));
            QCOMPARE(h.note(N::Step, 10'300), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Normal); // the hotkey as ever: closes it
            QCOMPARE(h.note(N::Escape, 10'400), Hold::Outcome::Close);
        }
        { // ... or let go then: nothing pasted, the hold over.
            Hold h(10'000);
            QCOMPARE(h.note(N::Step, 10'100), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Step);
            QCOMPARE(h.note(N::Escape, 10'200), Hold::Outcome::Cancel);
            QCOMPARE(h.note(N::AllUp, 10'300), Hold::Outcome::End);
        }
        { // ... and after the second held, likewise.
            Hold h(10'000);
            QCOMPARE(h.note(N::Escape, 11'200), Hold::Outcome::Cancel);
            QCOMPARE(h.note(N::AllUp, 11'300), Hold::Outcome::End);
        }
        { // Another key: over, nothing pasted; later notes change nothing.
            Hold h(10'000);
            QCOMPARE(h.note(N::Step, 10'100), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Step);
            QCOMPARE(h.note(N::Other, 10'200), Hold::Outcome::End);
            QCOMPARE(h.note(N::AllUp, 10'300), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Normal);
        }
        { // The GUI thread's own look at the keys (poll): the hint once the whole
          // hotkey is held a second, once.
            Hold h(10'000);
            QCOMPARE(h.poll(Hold::Held::Whole, 10'000 + kHoldMs - 1), Hold::Poll::None);
            QCOMPARE(h.poll(Hold::Held::Part, 10'000 + kHoldMs), Hold::Poll::None); // the key let go, Ctrl held on
            QCOMPARE(h.poll(Hold::Held::Whole, 10'000 + kHoldMs + kPollMs), Hold::Poll::Hint);
            QCOMPARE(h.poll(Hold::Held::Whole, 10'000 + kHoldMs + 2 * kPollMs), Hold::Poll::None);
            QCOMPARE(h.note(N::AllUp, 11'200), Hold::Outcome::Paste);
        }
        { // ... not after a step (the hint shows already), nor after Esc.
            Hold h(10'000);
            QCOMPARE(h.note(N::Step, 10'100), Hold::Outcome::None);
            QCOMPARE(h.press(), Hold::Press::Step);
            QCOMPARE(h.poll(Hold::Held::Whole, 11'500), Hold::Poll::None);
            Hold c(10'000);
            QCOMPARE(c.note(N::Escape, 11'100), Hold::Outcome::Cancel); // held a second: pending
            QCOMPARE(c.poll(Hold::Held::Whole, 11'150), Hold::Poll::None);
        }
        { // All of it up kSilentMs, as GetAsyncKeyState has it, and no word from the
          // hook: Windows dropped it. Down again in between: from the start.
            Hold h(10'000);
            QCOMPARE(h.poll(Hold::Held::None, 10'200), Hold::Poll::None);
            QCOMPARE(h.poll(Hold::Held::Part, 10'250), Hold::Poll::None);
            QCOMPARE(h.poll(Hold::Held::None, 10'300), Hold::Poll::None);
            QCOMPARE(h.poll(Hold::Held::None, 10'300 + kSilentMs - 1), Hold::Poll::None);
            QCOMPARE(h.poll(Hold::Held::None, 10'300 + kSilentMs), Hold::Poll::Silent);
            Hold o(10'000);
            QCOMPARE(o.note(N::Other, 10'100), Hold::Outcome::End);
            QCOMPARE(o.poll(Hold::Held::None, 10'200), Hold::Poll::None); // over: nothing more
            QCOMPARE(o.poll(Hold::Held::None, 10'200 + kSilentMs), Hold::Poll::None);
        }
        QCOMPARE(Hold::Press {}, Hold::Press::Normal); // the neutral one first
        // The row a step goes to: the next older; the last stays; gone: the first.
        QCOMPARE(stepRow(0, 6), 1);
        QCOMPARE(stepRow(4, 6), 5);
        QCOMPARE(stepRow(5, 6), 5);
        QCOMPARE(stepRow(-1, 6), 0);
        QCOMPARE(stepRow(0, 1), 0);
        QCOMPARE(stepRow(0, 0), -1);
    }

    void pasteKeys()
    {
        namespace pk = ws::pastekeys;
        using Sequence = std::vector<pk::Key>;
        const pk::Key ctrlDown {.vk = pk::kControl}, ctrlUp {.vk = pk::kControl, .up = true, .restores = true};
        const pk::Key vDown {.vk = pk::kV}, vUp {.vk = pk::kV, .up = true};
        const pk::Key shiftDown {.vk = pk::kShift}, shiftUp {.vk = pk::kShift, .up = true, .restores = true};
        const pk::Key insertDown {.vk = pk::kInsert, .extended = true};
        const pk::Key insertUp {.vk = pk::kInsert, .up = true, .extended = true};
        const pk::Key mask {.vk = 0xE8, .mask = true}, maskUp {.vk = 0xE8, .up = true, .mask = true};
        const auto lift = [](std::uint16_t vk, bool extended = false) {
            return pk::Key {.vk = vk, .up = true, .extended = extended, .mask = true};
        };
        const auto back = [](std::uint16_t vk, bool extended = false) {
            return pk::Key {.vk = vk, .extended = extended, .mask = true, .restores = true};
        };
        // Unwatched: its modifier up, pressed and let go of around the key;
        // down already (either side), the key alone; another one stays.
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0, false) == (Sequence {ctrlDown, vDown, vUp, ctrlUp}));
        QVERIFY(pk::sequence(pk::Keys::ShiftInsert, 0, false) == (Sequence {shiftDown, insertDown, insertUp, shiftUp}));
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x04, false) == (Sequence {vDown, vUp})); // left Ctrl
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x08, false) == (Sequence {vDown, vUp})); // right Ctrl
        QVERIFY(pk::sequence(pk::Keys::ShiftInsert, 0x02, false) == (Sequence {insertDown, insertUp})); // right Shift
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x10, false) == (Sequence {ctrlDown, vDown, vUp, ctrlUp})); // left Alt
        QVERIFY(pk::sequence(pk::Keys::ShiftInsert, 0x04, false) == (Sequence {shiftDown, insertDown, insertUp, shiftUp}));
        // Watched: its own pressed even if down; the others lifted after the
        // mask, pressed back last in turn.
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0, true) == (Sequence {ctrlDown, vDown, vUp, ctrlUp}));
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x04, true) == (Sequence {ctrlDown, vDown, vUp, ctrlUp}));
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x08, true) // the right Ctrl: ours on that side
            == (Sequence {pk::Key {.vk = 0xA3, .extended = true}, vDown, vUp,
                pk::Key {.vk = 0xA3, .up = true, .extended = true, .restores = true}}));
        QVERIFY(pk::sequence(pk::Keys::ShiftInsert, 0x02, true)
            == (Sequence {pk::Key {.vk = 0xA1}, insertDown, insertUp, pk::Key {.vk = 0xA1, .up = true, .restores = true}}));
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x01 | 0x04, true) // Ctrl+Shift, the hotkey again
            == (Sequence {mask, maskUp, lift(0xA0), ctrlDown, vDown, vUp, ctrlUp, back(0xA0)}));
        QVERIFY(pk::sequence(pk::Keys::CtrlV, 0x02 | 0x10 | 0x40, true) // right Shift, left Alt, left Win
            == (Sequence {mask, maskUp, lift(0xA1), lift(0xA4), lift(0x5B, true), ctrlDown, vDown, vUp, ctrlUp,
                back(0x5B, true), back(0xA4), back(0xA1)}));
        QVERIFY(pk::sequence(pk::Keys::ShiftInsert, 0x01 | 0x08 | 0x20, true) // Shift; right Ctrl and Alt
            == (Sequence {mask, maskUp, lift(0xA3, true), lift(0xA5, true), shiftDown, insertDown, insertUp, shiftUp,
                back(0xA5, true), back(0xA3, true)}));
        // The sides as the hook is told them, a program's plain modifiers too.
        QCOMPARE(pk::sided(0x11, false), std::uint16_t {0xA2});
        QCOMPARE(pk::sided(0x11, true), std::uint16_t {0xA3});
        QCOMPARE(pk::sided(0x10, false, 0x36), std::uint16_t {0xA1});
        QCOMPARE(pk::sided(0x10, false, 0x2A), std::uint16_t {0xA0});
        QCOMPARE(pk::sided(0x12, true), std::uint16_t {0xA5});
        QCOMPARE(pk::sided(0x5C, true), std::uint16_t {0x5C});
        QCOMPARE(pk::sideBit(0xA5), 0x20u);
        QCOMPARE(pk::sideBit(0x56), 0u);
    }

    void pasteGuard()
    {
        namespace pk = ws::pastekeys;
        // The paste's key `i` reaching the hook; true: it goes on.
        const auto own = [](pk::Guard& guard, std::span<const pk::Key> keys, std::size_t i) {
            return guard.own(keys, pk::sided(keys[i].vk, keys[i].extended), keys[i].up);
        };
        const auto through = [&](pk::Guard& guard, std::span<const pk::Key> keys, std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i) {
                if (!own(guard, keys, i))
                    return false;
            }
            return true;
        };
        const std::vector<pk::Key> plain = pk::sequence(pk::Keys::CtrlV, 0, true);
        {
            pk::Guard guard; // nothing pressed meanwhile: all go
            QVERIFY(through(guard, plain, 0, plain.size()));
            QCOMPARE(guard.passed(), plain.size());
        }
        {
            pk::Guard guard; // the hand's Ctrl pressed after our V, kept: ours not let go
            QVERIFY(through(guard, plain, 0, 3));
            QVERIFY(!guard.user(0xA2, false));
            QVERIFY(!own(guard, plain, 3));
            QVERIFY(!guard.user(0xA2, true)); // the hand's let-go goes as ever
        }
        {
            pk::Guard guard; // pressed before ours came (on its way through the hooks as the keys were read)
            QVERIFY(!guard.user(0xA2, false));
            QVERIFY(through(guard, plain, 0, 3));
            QVERIFY(!own(guard, plain, 3));
        }
        {
            pk::Guard guard; // the right Ctrl pressed: our left one let go of, the right holds the chord
            QVERIFY(through(guard, plain, 0, 2));
            QVERIFY(!guard.user(0xA3, false));
            QVERIFY(through(guard, plain, 2, 4));
        }
        const std::vector<pk::Key> right = pk::sequence(pk::Keys::CtrlV, 0x08, true);
        {
            pk::Guard guard(0x08); // the right Ctrl let go of before the V: ours on that side goes for it
            QVERIFY(own(guard, right, 0));
            QVERIFY(guard.user(0xA3, true));
            QVERIFY(through(guard, right, 1, 4));
        }
        const std::vector<pk::Key> held = pk::sequence(pk::Keys::CtrlV, 0x04, true);
        {
            pk::Guard guard(0x04); // held all along: ours not let go
            QVERIFY(through(guard, held, 0, 3));
            QVERIFY(!own(guard, held, 3));
        }
        {
            pk::Guard guard(0x04); // let go of before the V: kept back, ours goes for it after the V
            QVERIFY(own(guard, held, 0));
            QVERIFY(guard.user(0xA2, true));
            QVERIFY(through(guard, held, 1, 4));
            QVERIFY(!guard.user(0xA2, false)); // pressed again: as ever
            QVERIFY(!guard.user(0xA2, true));
        }
        const std::vector<pk::Key> alt = pk::sequence(pk::Keys::CtrlV, 0x10, true); // its Alt lifted, last pressed back
        {
            pk::Guard guard(0x10); // still held: back down
            QVERIFY(through(guard, alt, 0, alt.size()));
        }
        {
            pk::Guard guard(0x10); // let go of meanwhile: not pressed back, it would stay down
            QVERIFY(through(guard, alt, 0, 3));
            QVERIFY(!guard.user(0xA4, true));
            QVERIFY(through(guard, alt, 3, alt.size() - 1));
            QVERIFY(!own(guard, alt, alt.size() - 1));
            QCOMPARE(guard.passed(), alt.size());
        }
        {
            pk::Guard guard; // not the paste's next (KeyRouter's mask, a key typed): as it is, the paste where it was
            QVERIFY(guard.own(plain, 0xE8, false));
            QVERIFY(guard.own(plain, 0xA2, true));
            QCOMPARE(guard.passed(), std::size_t {0});
        }
        {
            pk::Guard guard; // the rest lost (fewer went): the hand's let-go not kept back
            QVERIFY(own(guard, plain, 0));
            guard.finish();
            QVERIFY(!guard.user(0xA2, true));
        }
        // Overtaken while the hooks before had it (agrees, once they are
        // through): the hand's key, sent meanwhile, came to the hook first.
        {
            pk::Guard guard; // our Ctrl's let-go judged with the hand's up; the hand's pressed meanwhile: kept back after all
            QVERIFY(through(guard, plain, 0, 3));
            QVERIFY(guard.agrees()); // not a key that puts a side back
            QVERIFY(own(guard, plain, 3));
            QVERIFY(guard.agrees());
            QVERIFY(!guard.user(0xA2, false));
            QVERIFY(!guard.agrees());
        }
        {
            pk::Guard guard(0x10); // the Alt pressed back as held; let go of meanwhile: kept back after all
            QVERIFY(through(guard, alt, 0, alt.size()));
            QVERIFY(guard.agrees());
            QVERIFY(!guard.user(0xA4, true));
            QVERIFY(!guard.agrees());
        }
        {
            pk::Guard guard; // the hand's keys on other sides change nothing
            QVERIFY(through(guard, plain, 0, plain.size()));
            QVERIFY(!guard.user(0xA3, false));
            QVERIFY(!guard.user(0xA4, false));
            QVERIFY(guard.agrees());
        }
        {
            pk::Guard guard; // judged after a key that puts nothing back: nothing to disagree with
            QVERIFY(through(guard, plain, 0, 2));
            QVERIFY(!guard.user(0xA2, false));
            QVERIFY(guard.agrees());
        }
        {
            pk::Guard guard(0x10); // a key not the paste's before its last (KeyRouter's mask): the judgement stays our Ctrl's let-go
            QVERIFY(through(guard, alt, 0, alt.size() - 1));
            QVERIFY(guard.agrees());
            QVERIFY(!guard.user(0xA2, false));
            QVERIFY(!guard.agrees());
            QVERIFY(guard.own(alt, 0xE8, false));
            QVERIFY(!guard.agrees());
            QVERIFY(own(guard, alt, alt.size() - 1)); // the Alt still held: back down, judged on its own
            QVERIFY(guard.agrees());
        }
    }

    void shortcutAgain()
    {
        // shown, active, panel, expand-on-again
        QCOMPARE(shortcutAction(false, false, false, true), ShortcutAction::Show);
        QCOMPARE(shortcutAction(false, true, false, false), ShortcutAction::Show); // hidden or on its way out
        QCOMPARE(shortcutAction(true, false, false, true), ShortcutAction::Show); // behind another window: forward, not expand
        QCOMPARE(shortcutAction(true, false, true, true), ShortcutAction::Show);
        QCOMPARE(shortcutAction(true, true, false, false), ShortcutAction::Hide); // today
        QCOMPARE(shortcutAction(true, true, true, false), ShortcutAction::Hide);
        QCOMPARE(shortcutAction(true, true, false, true), ShortcutAction::ExpandPanel);
        QCOMPARE(shortcutAction(true, true, true, true), ShortcutAction::Hide); // the third press

        // Pressed before it was there to be seen (GitHub #111): it comes, and
        // stays; neither closed nor expanded, active or not.
        QCOMPARE(shortcutAction(true, true, false, false, false), ShortcutAction::Raise);
        QCOMPARE(shortcutAction(true, true, false, true, false), ShortcutAction::Raise);
        QCOMPARE(shortcutAction(true, false, false, false, false), ShortcutAction::Raise);
        QCOMPARE(shortcutAction(false, false, false, false, false), ShortcutAction::Show); // not up: shown

        // seenBy: on the screen at least kSeenBeforeMs before the press.
        QVERIFY(!seenBy(std::nullopt, 10'000)); // hidden, or still cloaked for its first frame
        QVERIFY(!seenBy(10'000, 9'000)); // came after the press: the press waited behind the opening
        QVERIFY(!seenBy(10'000, 10'000 + kSeenBeforeMs - 1)); // too soon to have been seen
        QVERIFY(seenBy(10'000, 10'000 + kSeenBeforeMs));
        // #111's case: the first double Ctrl at 0 got its launcher up at 2130
        // (the program in front busy); the second, made at 1500 because
        // nothing came, got there at 2160 and closed it. Now it stays.
        QVERIFY(!seenBy(2130, 1500));
        QCOMPARE(shortcutAction(true, true, false, false, seenBy(2130, 1500)), ShortcutAction::Raise);
        QCOMPARE(shortcutAction(true, true, false, false, seenBy(2130, 2700)), ShortcutAction::Hide);
        { // A double tap counts from its first press: the GUI behind, the
          // launcher came at 10'000, and a double Ctrl made again as nothing
          // had come began 50 ms before (80 ms presses, 150 ms apart). It
          // let go 260 ms after the launcher came: by that, pressed again.
            DoubleTapDetector d;
            d.keyDown(0xA2, 9'950, 0, 0);
            QVERIFY(!d.keyUp(0xA2, 10'030, 0, 0));
            d.keyDown(0xA2, 10'180, 0, 0);
            QVERIFY(d.keyUp(0xA2, 10'260, 0, 0));
            QCOMPARE(d.firstPressTime(), 9'950u);
            QVERIFY(seenBy(10'000, 10'260)); // the release: it closed the launcher
            QCOMPARE(shortcutAction(true, true, false, false, seenBy(10'000, d.firstPressTime())), ShortcutAction::Raise);
            // Begun once the launcher had been there kSeenBeforeMs: pressed again.
            QCOMPARE(shortcutAction(true, true, false, false, seenBy(10'000, 10'000 + kSeenBeforeMs)),
                ShortcutAction::Hide);
        }
        { // A tap of the other Ctrl starts over: the double tap begins at the next tap.
            DoubleTapDetector d;
            d.keyDown(0xA3, 1000, 0, 0);
            QVERIFY(!d.keyUp(0xA3, 1060, 0, 0));
            d.keyDown(0xA2, 1200, 0, 0);
            QVERIFY(!d.keyUp(0xA2, 1260, 0, 0));
            d.keyDown(0xA2, 1400, 0, 0);
            QVERIFY(d.keyUp(0xA2, 1460, 0, 0));
            QCOMPARE(d.firstPressTime(), 1200u);
        }

        // tickTime: a message's 32-bit time on the 64-bit clock, across the wrap after 49.7 days.
        QCOMPARE(tickTime(1000u, 5000u), std::uint64_t {1000});
        QCOMPARE(tickTime(0xFFFF'FF00u, 0x1'0000'0100ull), std::uint64_t {0xFFFF'FF00}); // made before the wrap, here after
        QCOMPARE(tickTime(0x100u, 0x1'0000'0200ull), std::uint64_t {0x1'0000'0100});
        QCOMPARE(tickTime(5000u, 5000u), std::uint64_t {5000});
    }

    void journalBudget()
    {
        diagnostics::JournalBudget budget({.notablePerHour = 2, .ordinaryAtStart = 2, .ordinaryEveryMs = 1000});
        QVERIFY(budget.take(false, 0)); // the first ordinary ones of a run
        QVERIFY(budget.take(false, 10));
        QVERIFY(!budget.take(false, 20)); // then one every ordinaryEveryMs
        QVERIFY(budget.take(true, 30)); // a notable one: kept...
        QCOMPARE(budget.skipped(), 1); // ...saying how many were left out before it
        QVERIFY(budget.take(true, 40));
        QCOMPARE(budget.skipped(), 0);
        QVERIFY(!budget.take(true, 50)); // up to notablePerHour
        QVERIFY(budget.take(false, 1010));
        QCOMPARE(budget.skipped(), 1);
        QVERIFY(!budget.take(false, 1020));
        QVERIFY(budget.take(true, 3'600'030)); // a new hour
        QCOMPARE(budget.skipped(), 1);
    }

    void gameGuard()
    {
        const GameGuardOptions defaults; // games on, full screen off, no list
        auto in = [](bool covers, bool hidden, bool confined, bool exclusive = false) {
            ForegroundFacts facts;
            facts.program = u"Game.exe"_s;
            facts.coversMonitor = covers;
            facts.cursorHidden = hidden;
            facts.cursorConfined = confined;
            facts.exclusiveFullScreen = exclusive;
            return facts;
        };
        // An ordinary window, a game in its menu, a full-screen page
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, false), defaults), IgnoreReason::None);
        QCOMPARE(doubleCtrlIgnoreReason(in(true, false, false), defaults), IgnoreReason::None);
        // Turning the view: windowed (confined) or full screen
        QCOMPARE(doubleCtrlIgnoreReason(in(false, true, true), defaults), IgnoreReason::MouseTaken);
        QCOMPARE(doubleCtrlIgnoreReason(in(true, true, false), defaults), IgnoreReason::MouseTaken);
        // Hidden while typing in a window, or confined but shown (a strategy game)
        QCOMPARE(doubleCtrlIgnoreReason(in(false, true, false), defaults), IgnoreReason::None);
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, true), defaults), IgnoreReason::None);
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, false, true), defaults), IgnoreReason::ExclusiveFullScreen);

        GameGuardOptions off = defaults;
        off.games = false;
        QCOMPARE(doubleCtrlIgnoreReason(in(true, true, true, true), off), IgnoreReason::None);
        GameGuardOptions fullScreen = defaults;
        fullScreen.fullScreen = true;
        QCOMPARE(doubleCtrlIgnoreReason(in(true, false, false), fullScreen), IgnoreReason::FullScreen);
        GameGuardOptions listed = off;
        listed.programs = {u"game.EXE"_s};
        QCOMPARE(doubleCtrlIgnoreReason(in(false, false, false), listed), IgnoreReason::Listed);

        // Win顺's own windows and the desktop come without a program
        ForegroundFacts ours;
        ours.coversMonitor = ours.cursorHidden = ours.exclusiveFullScreen = true;
        QCOMPARE(doubleCtrlIgnoreReason(ours, fullScreen), IgnoreReason::None);

        // GitHub #111: hidden by Windows on a key press ("hide pointer while
        // typing") in a full-screen page or editor: the mouse is not taken.
        ForegroundFacts typed = in(true, true, false);
        typed.cursorHiddenByTyping = true;
        QCOMPARE(doubleCtrlIgnoreReason(typed, defaults), IgnoreReason::None);
        // Kept in the window all the same: a game's, whatever hid it.
        ForegroundFacts typedConfined = typed;
        typedConfined.cursorConfined = true;
        QCOMPARE(doubleCtrlIgnoreReason(typedConfined, defaults), IgnoreReason::MouseTaken);
        ForegroundFacts typedCaptured = typed;
        typedCaptured.cursorCaptured = true;
        QCOMPARE(doubleCtrlIgnoreReason(typedCaptured, defaults), IgnoreReason::MouseTaken);
        // Exclusive full screen stays out of bounds, and "not over full screen" too.
        ForegroundFacts typedExclusive = in(true, true, false, true);
        typedExclusive.cursorHiddenByTyping = true;
        QCOMPARE(doubleCtrlIgnoreReason(typedExclusive, defaults), IgnoreReason::ExclusiveFullScreen);
        QCOMPARE(doubleCtrlIgnoreReason(typed, fullScreen), IgnoreReason::FullScreen);
        // Captured (SetCapture) rather than confined: a game turning the view, windowed too.
        ForegroundFacts captured = in(false, true, false);
        captured.cursorCaptured = true;
        QCOMPARE(doubleCtrlIgnoreReason(captured, defaults), IgnoreReason::MouseTaken);
        captured.cursorHidden = false; // shown: a drag somewhere, not a game
        QCOMPARE(doubleCtrlIgnoreReason(captured, defaults), IgnoreReason::None);
    }

    void hiddenForTypingRule()
    {
        // A key (not a modifier) went down to this window shortly before the hide.
        QVERIFY(hiddenForTyping(true, 10'015, 10'000u, 9'000));
        QVERIFY(hiddenForTyping(true, 10'000 + kTypingHideMs, 10'000u, 9'000)); // a program slow to take the key
        QVERIFY(!hiddenForTyping(true, 10'001 + kTypingHideMs, 10'000u, 9'000)); // long after it: the program's
        QVERIFY(!hiddenForTyping(true, 10'200, 10'000u, 9'000)); // a game's own (the 500 ms from before passed it)
        QVERIFY(!hiddenForTyping(false, 10'015, 10'000u, 9'000)); // the setting off: always the program's
        QVERIFY(!hiddenForTyping(true, 10'015, std::nullopt, 9'000)); // no key known
        QVERIFY(!hiddenForTyping(true, 9'990, 10'000u, 9'000)); // the key came after the hide
        // The key went down before the window came to the front (Alt+Tab),
        // or on the same tick (Win+1): a game hiding it as it comes up.
        QVERIFY(!hiddenForTyping(true, 10'015, 10'000u, 10'005));
        QVERIFY(!hiddenForTyping(true, 10'015, 10'000u, 10'000));
        QVERIFY(hiddenForTyping(true, 10'015, 10'000u, 9'999));
        // Across the 32-bit clock's wrap.
        QVERIFY(hiddenForTyping(true, 5u, 0xFFFF'FFF0u, 0xFFFF'FF00u));
        QVERIFY(!hiddenForTyping(true, 5u, 0xFFFF'FFF0u, 0xFFFF'FFF8u));
    }

    // The presses hiddenForTyping goes by (GitHub #111): a game played with
    // the keys hid the pointer itself while one was held, and the repeat just
    // before passed for typing; with the GUI busy at the hide, the keys typed
    // meanwhile pushed the press before it out of the last eight.
    void keyPresses()
    {
        using Kept = std::optional<std::uint32_t>;
        constexpr std::uint32_t right = 0x27, w = 0x57, a = 0x41;
        { // a held key: its first press, not its repeats
            KeyPresses p;
            QVERIFY(p.down(right, 10'000));
            for (std::uint32_t t = 10'500; t < 12'000; t += 33)
                QVERIFY(!p.down(right, t)); // the keyboard's repeats, from 500 ms on, every 33 ms
            QVERIFY(p.atOrBefore(11'990) == Kept(10'000));
            // The game hid the pointer 30 ms after a repeat: its own hide.
            QVERIFY(!hiddenForTyping(true, 11'990, p.atOrBefore(11'990), 9'000));
            p.up(right);
            QVERIFY(p.down(right, 12'100)); // let go and pressed again
            QVERIFY(hiddenForTyping(true, 12'115, p.atOrBefore(12'115), 9'000)); // Windows' hide, the next tick
        }
        { // its release unseen (the secure desktop, UIPI): down again after a quiet spell is a press
            KeyPresses p;
            QVERIFY(p.down(w, 1000));
            QVERIFY(!p.down(w, 1000 + KeyPresses::kLostReleaseMs));
            QVERIFY(p.down(w, 1001 + 2 * KeyPresses::kLostReleaseMs));
        }
        { // kept by time, not by count: the hide at 20'010, a second of typing before the GUI got to it
            KeyPresses p;
            QVERIFY(p.down(a, 20'000));
            p.up(a);
            for (std::uint32_t i = 0; i < 12; ++i) {
                QVERIFY(p.down(0x42 + i, 20'100 + i * 80));
                p.up(0x42 + i);
            }
            QVERIFY(p.atOrBefore(20'010) == Kept(20'000));
            QVERIFY(hiddenForTyping(true, 20'010, p.atOrBefore(20'010), 9'000));
            // Gone once more than kKeptMs older than the newest.
            QVERIFY(p.down(a, 20'001 + KeyPresses::kKeptMs));
            QVERIFY(!p.atOrBefore(20'010));
            QVERIFY(p.atOrBefore(20'150) == Kept(20'100));
        }
        { // more than kMost within kKeptMs is no one's typing: the oldest go
            KeyPresses p;
            for (std::uint32_t i = 0; i < KeyPresses::kMost + 10; ++i) {
                QVERIFY(p.down(0x41 + i % 26, 1000 + i));
                p.up(0x41 + i % 26);
            }
            QVERIFY(!p.atOrBefore(1009));
            QVERIFY(p.atOrBefore(1010) == Kept(1010));
        }
        { // across the 32-bit clock's wrap
            KeyPresses p;
            QVERIFY(p.down(a, 0xFFFF'FFF0u));
            p.up(a);
            QVERIFY(p.down(w, 0x10u));
            QVERIFY(p.atOrBefore(5u) == Kept(0xFFFF'FFF0u));
            QVERIFY(p.atOrBefore(0x20u) == Kept(0x10u));
            QVERIFY(!KeyPresses().atOrBefore(0x20u)); // none at all
        }
    }

    void doubleTapNearMiss()
    {
        using Reason = DoubleTapDetector::MissReason;
        using Cause = DoubleTapDetector::Cause;
        constexpr std::uint32_t ctrl = 0xA2, rightCtrl = 0xA3, shift = 0xA0, letter = 0x41;
        const auto tap = [](DoubleTapDetector& d, std::uint32_t vk, std::uint32_t down, std::uint32_t held = 60,
                             int x = 0) {
            d.keyDown(vk, down, x, 0);
            return d.keyUp(vk, down + held, x, 0);
        };
        { // the second came late: 450 ms after the first's release (400 at most)
            DoubleTapDetector d;
            QVERIFY(!tap(d, ctrl, 1000));
            QCOMPARE(d.takeMiss().reason, Reason::None);
            QVERIFY(!tap(d, ctrl, 1060 + 450));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::Late);
            QCOMPARE(miss.ms, 450u);
            QCOMPARE(d.takeMiss().reason, Reason::None); // once
        }
        { // past twice the gap: not meant as one
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            tap(d, ctrl, 1060 + 900);
            QCOMPARE(d.takeMiss().reason, Reason::None);
        }
        { // the second held too long
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            QVERIFY(!tap(d, ctrl, 1200, 450));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::Held);
            QCOMPARE(miss.ms, 450u);
        }
        { // the other Ctrl
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            QVERIFY(!tap(d, rightCtrl, 1200));
            QCOMPARE(d.takeMiss().reason, Reason::OtherSide);
        }
        { // the pointer moved between the taps
            DoubleTapDetector d;
            tap(d, ctrl, 1000, 60, 0);
            QVERIFY(!tap(d, ctrl, 1200, 60, 50));
            QCOMPARE(d.takeMiss().reason, Reason::Moved);
        }
        { // Shift held at the second press
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            d.keyDown(shift, 1150, 0, 0);
            QVERIFY(!tap(d, ctrl, 1200));
            d.keyUp(shift, 1300, 0, 0);
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::Spoiled);
            QCOMPARE(miss.by, Cause::Modifier);
        }
        { // a click between the taps
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            d.mouseUsed();
            QVERIFY(!tap(d, ctrl, 1200));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::Interrupted);
            QCOMPARE(miss.by, Cause::Mouse);
        }
        { // typing between them is none: Ctrl, a letter, Ctrl
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            tap(d, letter, 1100);
            QVERIFY(!tap(d, ctrl, 1200));
            QCOMPARE(d.takeMiss().reason, Reason::None);
        }
        { // a program's key between them is no typing (GitHub #111)
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            d.keyDown(letter, 1100, 0, 0, true);
            d.keyUp(letter, 1100, 0, 0, true);
            QVERIFY(!tap(d, ctrl, 1200));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::Interrupted);
            QCOMPARE(miss.by, Cause::ProgramKey);
        }
        { // a double tap is none, nor a third tap after it
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            QVERIFY(tap(d, ctrl, 1200));
            QCOMPARE(d.takeMiss().reason, Reason::None);
            QVERIFY(!tap(d, ctrl, 1400));
            QCOMPARE(d.takeMiss().reason, Reason::None);
        }
        // GitHub #111: the press before a tap was none, so the tap had
        // nothing to make a double tap with.
        { // Shift held at the first press
            DoubleTapDetector d;
            d.keyDown(shift, 950, 0, 0);
            QVERIFY(!tap(d, ctrl, 1000));
            d.keyUp(shift, 1100, 0, 0);
            QCOMPARE(d.takeMiss().reason, Reason::None); // not yet: no tap followed
            QVERIFY(!tap(d, ctrl, 1200));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::FirstSpoiled);
            QCOMPARE(miss.by, Cause::Modifier);
            QVERIFY(tap(d, ctrl, 1400)); // that tap counts as a first one
            QCOMPARE(d.takeMiss().reason, Reason::None);
        }
        { // a program's key during the first press
            DoubleTapDetector d;
            d.keyDown(ctrl, 1000, 0, 0);
            d.keyDown(letter, 1020, 0, 0, true);
            d.keyUp(letter, 1020, 0, 0, true);
            QVERIFY(!d.keyUp(ctrl, 1060, 0, 0));
            QVERIFY(!tap(d, ctrl, 1200));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::FirstSpoiled);
            QCOMPARE(miss.by, Cause::ProgramKey);
        }
        { // the first held too long
            DoubleTapDetector d;
            QVERIFY(!tap(d, ctrl, 1000, 450));
            QVERIFY(!tap(d, ctrl, 1700));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::FirstHeld);
            QCOMPARE(miss.ms, 450u);
        }
        { // the pointer moved during the first press
            DoubleTapDetector d;
            d.keyDown(ctrl, 1000, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 1060, 50, 0));
            QVERIFY(!tap(d, ctrl, 1200, 60, 50));
            QCOMPARE(d.takeMiss().reason, Reason::FirstMoved);
        }
        { // typing is none: Ctrl+C, then a tap; nor after typing in between, nor much later
            DoubleTapDetector d;
            d.keyDown(ctrl, 1000, 0, 0);
            tap(d, letter, 1020);
            d.keyUp(ctrl, 1100, 0, 0);
            QVERIFY(!tap(d, ctrl, 1300));
            QCOMPARE(d.takeMiss().reason, Reason::None);
            DoubleTapDetector e;
            QVERIFY(!tap(e, ctrl, 1000, 450));
            tap(e, letter, 1500);
            QVERIFY(!tap(e, ctrl, 1600));
            QCOMPARE(e.takeMiss().reason, Reason::None);
            DoubleTapDetector f;
            QVERIFY(!tap(f, ctrl, 1000, 450));
            QVERIFY(!tap(f, ctrl, 1450 + 900));
            QCOMPARE(f.takeMiss().reason, Reason::None);
        }
        { // typed after something else spoiled it first: none (Ctrl+Shift+S, Shift held then Ctrl+S, Ctrl+click then C)
            DoubleTapDetector d;
            d.keyDown(ctrl, 1000, 0, 0);
            d.keyDown(shift, 1010, 0, 0);
            tap(d, 'S', 1020);
            d.keyUp(shift, 1090, 0, 0);
            d.keyUp(ctrl, 1100, 0, 0);
            QVERIFY(!tap(d, ctrl, 1300));
            QCOMPARE(d.takeMiss().reason, Reason::None);
            DoubleTapDetector e;
            e.keyDown(shift, 990, 0, 0);
            e.keyDown(ctrl, 1000, 0, 0);
            tap(e, 'S', 1020);
            e.keyUp(ctrl, 1100, 0, 0);
            e.keyUp(shift, 1110, 0, 0);
            QVERIFY(!tap(e, ctrl, 1300));
            QCOMPARE(e.takeMiss().reason, Reason::None);
            DoubleTapDetector f;
            f.keyDown(ctrl, 1000, 0, 0);
            f.mouseUsed();
            tap(f, 'C', 1050);
            f.keyUp(ctrl, 1100, 0, 0);
            QVERIFY(!tap(f, ctrl, 1300));
            QCOMPARE(f.takeMiss().reason, Reason::None);
        }
        { // held too long twice (a slow hook holding up each key, GitHub #111): told at the second
            DoubleTapDetector d;
            QVERIFY(!tap(d, ctrl, 1000, 344));
            QCOMPARE(d.takeMiss().reason, Reason::None);
            QVERIFY(!tap(d, ctrl, 1344 + 359, 344));
            const auto miss = d.takeMiss();
            QCOMPARE(miss.reason, Reason::Held);
            QCOMPARE(miss.ms, 344u);
            QVERIFY(!tap(d, ctrl, 2400)); // a tap after it: the press before was told already
            QCOMPARE(d.takeMiss().reason, Reason::None);
        }
        { // the second of a double tap held up by a hook after a stray tap: Held told, the double tap counts
            DoubleTapDetector d;
            tap(d, ctrl, 1000);
            d.keyDown(ctrl, 1100, 0, 0);
            QVERIFY(!d.keyUp(ctrl, 1700, 0, 0));
            QCOMPARE(d.takeMiss().reason, Reason::Held);
            d.keyDown(ctrl, 1700, 0, 0);
            QVERIFY(d.keyUp(ctrl, 1700, 0, 0));
            QCOMPARE(d.takeMiss().reason, Reason::None);
        }
        QCOMPARE(QString::fromLatin1(DoubleTapDetector::name(Reason::FirstSpoiled)), u"first-spoiled"_s);
        QCOMPARE(QString::fromLatin1(DoubleTapDetector::name(Cause::ProgramKey)), u"program-key"_s);
        QCOMPARE(QString::fromLatin1(DoubleTapDetector::name(Reason::OtherSide)), u"other-side"_s);
        QCOMPARE(QString::fromLatin1(DoubleTapDetector::name(Reason::None)), u"none"_s);
    }

    // GitHub #111: the service's listener told of a double tap seconds late,
    // and that word was dropped while this process's own was set aside.
    void tapTeller()
    {
        using Verdict = TapTeller::Verdict;
        const auto quiet = [] { return true; };
        const auto busy = [] { return false; };
        { // two words of one tap (both listeners), an older one, a newer one
            TapTeller t;
            QCOMPARE(t.judge(5000, 5001, busy), Verdict::Tell);
            QCOMPARE(t.judge(5000, 5120, busy), Verdict::Duplicate); // the other listener's, 120 ms on
            QCOMPARE(t.judge(4800, 5130, busy), Verdict::Duplicate); // an older tap, told after
            QVERIFY(t.toldAlready(5000, 5200));
            QVERIFY(!t.toldAlready(6000, 6010));
            QCOMPARE(t.judge(6000, 6010, busy), Verdict::Tell);
        }
        { // late: as it is up to 1 s; then only if nothing came since; never past 3 s
            TapTeller t;
            QCOMPARE(t.judge(10'000, 10'999, busy), Verdict::Tell);
            QCOMPARE(t.judge(20'000, 21'000, busy), Verdict::Stale);
            QCOMPARE(t.judge(20'000, 21'000, quiet), Verdict::Tell);
            QCOMPARE(t.judge(30'000, 32'999, quiet), Verdict::Tell);
            QCOMPARE(t.judge(40'000, 43'000, quiet), Verdict::Stale);
            QVERIFY(!t.toldAlready(40'000, 43'000)); // dropped, not told
        }
        { // whether nothing came is asked only for a late one
            TapTeller t;
            int asked = 0;
            const auto counted = [&] { ++asked; return true; };
            t.judge(1000, 1500, counted);
            QCOMPARE(asked, 0);
            t.judge(3000, 4500, counted);
            QCOMPARE(asked, 1);
            t.judge(3000, 4600, counted); // a duplicate: not asked
            QCOMPARE(asked, 1);
        }
        { // across the clock's wrap (49.7 days)
            TapTeller t;
            QCOMPARE(t.judge(0xFFFF'FF00u, 0xFFFF'FF10u, busy), Verdict::Tell);
            QCOMPARE(t.judge(0x0000'0050u, 0x0000'0060u, busy), Verdict::Tell);
            QCOMPARE(t.judge(0xFFFF'FF00u, 0x0000'0070u, busy), Verdict::Duplicate);
        }
        { // one told long ago does not hide a new one whose difference to it turned negative (> 24.8 days)
            TapTeller t;
            QCOMPARE(t.judge(0, 10, busy), Verdict::Tell);
            QCOMPARE(t.judge(0x9000'0000u, 0x9000'0010u, busy), Verdict::Tell);
        }
        { // the other word of a told tap, however late, is a duplicate: never dropped (and noted) as too old
            TapTeller t;
            QCOMPARE(t.judge(5000, 5010, busy), Verdict::Tell);
            QCOMPARE(t.judge(5000, 17'010, quiet), Verdict::Duplicate); // the UIAccess listener's, starved 12 s
            QCOMPARE(t.judge(4000, 5000 + 3'600'000, busy), Verdict::Duplicate); // an older one, an hour on
            QCOMPARE(t.judge(6000, 6010, busy), Verdict::Tell);
        }
        { // a dropped tap is noted once, whichever listener's word comes second
            TapTeller t;
            QVERIFY(t.firstDrop(7000));
            QVERIFY(!t.firstDrop(7000));
            QVERIFY(t.firstDrop(8000));
        }
    }

    // kTap's lParam (GitHub #111): the first press rides in the upper half,
    // which a GUI from before never reads; a listener from before sends 0.
    void tapProtocol()
    {
        const LPARAM times = keyproto::tapTimes(0xFFFF'FFF0u, 310);
        QCOMPARE(keyproto::tapTime(times), 0xFFFF'FFF0u);
        QCOMPARE(keyproto::tapSpan(times), 310u);
        QCOMPARE(static_cast<std::uint32_t>(times), 0xFFFF'FFF0u); // as a GUI from before reads it
        const LPARAM before = std::uint32_t {0xFFFF'FFF0u}; // PostMessageW(…, tap.time) from before
        QCOMPARE(keyproto::tapTime(before), 0xFFFF'FFF0u);
        QCOMPARE(keyproto::tapSpan(before), 0u);
    }

    void settingsMatch()
    {
        using namespace settingsmatch;
        const auto score = [](const QString& query, const Fields& fields) { return match(parse(query), fields).score; };

        // The query made comparable: full-width forms, punctuation, "Win + V".
        const Query query = parse(u"  剪贴板，图片  Ｗｉｎ + Ｖ  “KeePass.exe”"_s);
        QCOMPARE(query.terms.size(), std::size_t {4});
        QCOMPARE(query.terms[0].text, u"剪贴板"_s);
        QCOMPARE(query.terms[1].text, u"图片"_s);
        QCOMPARE(query.terms[2].text, u"win+v"_s);
        QCOMPARE(query.terms[3].text, u"keepass.exe"_s);
        QVERIFY(parse(u"，、。 "_s).isEmpty());
        QVERIFY(parse(u"a"_s).terms[0].single);
        QVERIFY(!parse(u"图"_s).terms[0].single);
        QVERIFY(parse(u"kjj"_s).terms[0].pinyin.valid());
        QVERIFY(!parse(u"快捷键"_s).terms[0].pinyin.valid());

        Fields hotkey;
        hotkey.title = u"快捷键"_s;
        hotkey.keywords = {u"热键"_s, u"hotkey"_s};
        hotkey.context = {u"打开 Win顺"_s};
        hotkey.description = u"再设一个组合键来打开搜索框，例如 Alt + Space"_s;

        // The title by pinyin and initials, keywords in any case.
        QVERIFY(score(u"kjj"_s, hotkey) > 0);
        QVERIFY(score(u"kuaijiejian"_s, hotkey) > 0);
        QVERIFY(score(u"热键"_s, hotkey) > 0);
        QVERIFY(score(u"HOTKEY"_s, hotkey) > 0);
        QCOMPARE(match(parse(u"kjj"_s), hotkey).title, (QList<Span> {{0, 3}}));
        // The description word for word only.
        QVERIFY(score(u"组合键"_s, hotkey) > 0);
        QVERIFY(score(u"space"_s, hotkey) > 0);
        QCOMPARE(score(u"zhj"_s, hotkey), -1);
        // Every word somewhere.
        QVERIFY(score(u"打开 快捷键"_s, hotkey) > 0);
        QCOMPARE(score(u"快捷键 剪贴板"_s, hotkey), -1);
        // A lone letter only where a word starts, and never in a sentence.
        QVERIFY(score(u"h"_s, hotkey) > 0);
        QVERIFY(score(u"w"_s, hotkey) > 0);
        QCOMPARE(score(u"o"_s, hotkey), -1);
        QCOMPARE(score(u"s"_s, hotkey), -1);
        // Spelled out beats pinyin, which beats initials; a keyword spelled
        // out beats the title's initials.
        QVERIFY(score(u"快捷键"_s, hotkey) > score(u"kuaijiejian"_s, hotkey));
        QVERIFY(score(u"kuaijiejian"_s, hotkey) > score(u"kjj"_s, hotkey));
        QVERIFY(score(u"热键"_s, hotkey) > score(u"kjj"_s, hotkey));

        // The option's own words, then its page, then a description.
        Fields images;
        images.title = u"记录图片"_s;
        images.context = {u"剪贴板"_s};
        images.description = u"截图和复制的图片也记下来"_s;
        Fields dataFolder;
        dataFolder.title = u"数据文件夹"_s;
        dataFolder.context = {u"高级"_s};
        dataFolder.description = u"搜索记录、剪贴板历史和日志都存放在这里"_s;
        Fields clearClips;
        clearClips.title = u"清除剪贴板历史"_s;
        clearClips.context = {u"剪贴板"_s};
        QVERIFY(score(u"剪贴板 图片"_s, images) > 0);
        QCOMPARE(score(u"剪贴板 图片"_s, dataFolder), -1);
        QVERIFY(score(u"剪贴板"_s, clearClips) > score(u"剪贴板"_s, images));
        QVERIFY(score(u"剪贴板"_s, images) > score(u"剪贴板"_s, dataFolder));
        // The page by pinyin from three letters: two fit too many pages.
        Fields pasted;
        pasted.title = u"最多保留"_s;
        pasted.context = {u"剪贴板"_s, u"粘贴"_s};
        QVERIFY(score(u"jtb"_s, pasted) > 0);
        QVERIFY(score(u"zhantie"_s, pasted) > 0);
        QCOMPARE(score(u"zt"_s, pasted), -1);
        Fields theme;
        theme.title = u"主题"_s;
        QVERIFY(score(u"zt"_s, theme) > 0);

        // Where the words are, merged.
        const Match spans = match(parse(u"图片 截图"_s), images);
        QCOMPARE(spans.title, (QList<Span> {{2, 2}}));
        QCOMPARE(spans.description, (QList<Span> {{0, 2}, {6, 2}}));
        QCOMPARE(match(parse(u"图片 图"_s), images).title, (QList<Span> {{2, 2}}));

        // "Win + V" as typed, or its words apart.
        Fields winV;
        winV.title = u"用 Win+V 打开，代替 Windows 自带的剪贴板"_s;
        QVERIFY(score(u"win + v"_s, winV) > 0);
        QVERIFY(score(u"win v"_s, winV) > 0);

        // What the user filled in.
        Fields names;
        names.title = u"跳过的文件夹名称"_s;
        names.values = {u"build"_s, u"node_modules"_s};
        const Match value = match(parse(u"node_mod"_s), names);
        QVERIFY(value.score > 0);
        QCOMPARE(value.values, QList<int> {1});
        QCOMPARE(score(u"node_mod"_s, Fields {}), -1);

        QCOMPARE(highlight(u"a<b>\n图片"_s, {{5, 2}}, u"#0067c0"_s),
            u"a&lt;b&gt;<br><font color=\"#0067c0\">图片</font>"_s);
        QCOMPARE(highlight(u"x"_s, {{3, 1}}, u"red"_s), u"x"_s); // past the end: ignored
    }

    // Clipboard entries of copied files that are gone (MissingFiles, GitHub
    // #54): what counts as gone, on a made-up disk, then on Windows' own.
    void missingFilesJudge()
    {
        struct Disk {
            std::unordered_map<wchar_t, std::uint32_t> drives; // GetDriveTypeW by letter; others: no such drive
            QHash<QString, std::uint32_t> answers; // GetLastError by path; others: not found
            QStringList asked; // what GetFileAttributesW was asked
            int drivesAsked = 0;
        };
        const auto fake = std::make_shared<Disk>();
        fake->drives = {{L'C', DRIVE_FIXED}, {L'E', DRIVE_REMOVABLE}, {L'R', DRIVE_REMOVABLE}, {L'Z', DRIVE_REMOTE}};
        fake->answers = {
            {u"C:\\"_s, ERROR_SUCCESS},
            {u"E:\\"_s, ERROR_SUCCESS}, // a USB stick that is there
            {u"R:\\"_s, ERROR_NOT_READY}, // a card reader without its card
            {u"Z:\\"_s, ERROR_SUCCESS},
            {u"\\\\?\\C:\\here\\a.txt"_s, ERROR_SUCCESS},
            {u"\\\\?\\C:\\locked\\b.txt"_s, ERROR_ACCESS_DENIED}, // Win顺 runs unelevated
            {u"\\\\?\\C:\\no folder\\c.txt"_s, ERROR_PATH_NOT_FOUND},
        };
        const MissingFiles::Disk disk {
            .driveType = [fake](const std::wstring& root) -> std::uint32_t {
                ++fake->drivesAsked;
                const auto it = fake->drives.find(root.front());
                return root.size() == 3 && it != fake->drives.end() ? it->second : DRIVE_NO_ROOT_DIR;
            },
            .lookUp = [fake](const std::wstring& path) -> std::uint32_t {
                const QString p = QString::fromStdWString(path);
                fake->asked.append(p);
                return fake->answers.value(p, ERROR_FILE_NOT_FOUND);
            },
        };
        using Presence = MissingFiles::Presence;
        MissingFiles::Judge judge(disk);
        QCOMPARE(judge.file(u"C:\\here\\a.txt"_s), Presence::Present);
        QCOMPARE(judge.file(u"C:\\here\\gone.txt"_s), Presence::Gone);
        QCOMPARE(judge.file(u"c:\\here\\gone.txt"_s), Presence::Gone);
        QCOMPARE(judge.file(u"C:\\no folder\\c.txt"_s), Presence::Gone); // its folder gone too
        QCOMPARE(judge.file(u"E:\\gone.txt"_s), Presence::Gone);
        // Anything else is not known to be gone.
        QCOMPARE(judge.file(u"C:\\locked\\b.txt"_s), Presence::Unknown);
        QCOMPARE(judge.file(u"Z:\\gone.txt"_s), Presence::Unknown); // a mapped network drive
        QCOMPARE(judge.file(u"X:\\gone.txt"_s), Presence::Unknown); // no such drive: pulled out
        QCOMPARE(judge.file(u"R:\\gone.txt"_s), Presence::Unknown); // not ready
        QCOMPARE(judge.file(u"\\\\server\\share\\gone.txt"_s), Presence::Unknown);
        QCOMPARE(judge.file(u"C:gone.txt"_s), Presence::Unknown);
        QCOMPARE(judge.file(QString()), Presence::Unknown);
        // Each drive asked about once a pass; shares and the drives that do
        // not answer never asked about their files; files by their long-path form.
        QCOMPARE(fake->drivesAsked, 5); // C, E, Z, X, R
        for (const QString& asked : std::as_const(fake->asked)) {
            QVERIFY2(QStringList({u"C:\\"_s, u"E:\\"_s, u"R:\\"_s}).contains(asked)
                    || asked.startsWith(u"\\\\?\\C:\\"_s, Qt::CaseInsensitive) || asked.startsWith(u"\\\\?\\E:\\"_s),
                qPrintable(asked));
        }

        // An entry is gone when all its files are; the first that is not ends it.
        QVERIFY(judge.allGone({u"C:\\here\\gone.txt"_s, u"C:\\no folder\\c.txt"_s, u"E:\\gone.txt"_s}));
        fake->asked.clear();
        QVERIFY(!judge.allGone({u"C:\\here\\a.txt"_s, u"C:\\here\\gone.txt"_s}));
        QCOMPARE(fake->asked.size(), 1);
        QVERIFY(!judge.allGone({u"C:\\here\\gone.txt"_s, u"C:\\here\\a.txt"_s}));
        QVERIFY(!judge.allGone({u"C:\\here\\gone.txt"_s, u"C:\\locked\\b.txt"_s}));
        QVERIFY(!judge.allGone({u"C:\\here\\gone.txt"_s, u"Z:\\gone.txt"_s}));
        QVERIFY(!judge.allGone({u"C:\\here\\gone.txt"_s, u"X:\\gone.txt"_s}));
        QVERIFY(!judge.allGone({u"C:\\here\\gone.txt"_s, u"\\\\server\\share\\gone.txt"_s}));
        QVERIFY(!judge.allGone({}));

        // Windows' answers.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = QDir::toNativeSeparators(dir.filePath(u"there.txt"_s));
        QFile there(file);
        QVERIFY(there.open(QIODevice::WriteOnly));
        there.close();
        MissingFiles::Judge windows(MissingFiles::Disk::windows());
        QCOMPARE(windows.file(file), Presence::Present);
        QCOMPARE(windows.file(QDir::toNativeSeparators(dir.path())), Presence::Present); // a folder
        QCOMPARE(windows.file(QDir::toNativeSeparators(dir.filePath(u"not there.txt"_s))), Presence::Gone);
        QCOMPARE(windows.file(QDir::toNativeSeparators(dir.filePath(u"no folder/not there.txt"_s))), Presence::Gone);
        QVERIFY(there.remove());
        QCOMPARE(windows.file(file), Presence::Gone);
    }

    // Cleaned out only when two passes, a wait apart, found it gone.
    void missingFilesSightings()
    {
        using namespace std::chrono_literals;
        using Ids = std::vector<qint64>;
        MissingFiles::Sightings seen;
        const MissingFiles::Clock::time_point t0 {};
        QCOMPARE(seen.update(Ids {1, 2, 3}, t0, 10min), Ids {}); // the first pass to find them
        QCOMPARE(seen.update(Ids {1, 2, 3}, t0 + 9min, 10min), Ids {}); // too soon
        QCOMPARE(seen.update(Ids {1, 3}, t0 + 10min, 10min), (Ids {1, 3})); // 2 found there: forgotten
        QCOMPARE(seen.update(Ids {1, 2}, t0 + 15min, 10min), Ids {1}); // 2 starts over, 3 is forgotten
        QCOMPARE(seen.update(Ids {2}, t0 + 25min, 10min), Ids {2});
        // Two passes, however short the wait (WINSHUN_MISSING_SECONDS=0).
        seen.clear();
        QCOMPARE(seen.update(Ids {4}, t0, 0s), Ids {});
        QCOMPARE(seen.update(Ids {4}, t0, 0s), Ids {4});
    }

    // On a history in a folder of its own, the disk made up: what is hidden
    // and when, what a search still finds, what is cleaned out, and what is
    // never touched.
    void missingFilesInTheHistory()
    {
        qputenv("WINSHUN_MISSING_SECONDS", "0"); // no waits: after the start, between passes, before cleaning
        const auto unset = qScopeGuard([] { qunsetenv("WINSHUN_MISSING_SECONDS"); });
        struct Gone {
            std::mutex mutex;
            QSet<QString> paths;
        };
        const auto gone = std::make_shared<Gone>(); // asked on the pass's thread
        const auto setGone = [&gone](const QStringList& paths) {
            const std::scoped_lock lock(gone->mutex);
            gone->paths = QSet<QString>(paths.begin(), paths.end());
        };
        const MissingFiles::Disk disk {
            .driveType = [](const std::wstring&) -> std::uint32_t { return DRIVE_FIXED; },
            .lookUp = [gone](const std::wstring& path) -> std::uint32_t {
                QString p = QString::fromStdWString(path);
                if (p.startsWith(u"\\\\?\\"_s))
                    p.remove(0, 4);
                const std::scoped_lock lock(gone->mutex);
                return gone->paths.contains(p) ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
            },
        };

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ClipStore store(dir.path());
        QVERIFY(store.open(1000));
        const auto add = [&store](ClipKind kind, const QString& text, qint64 at) {
            return store.add({.kind = kind, .text = text}, at);
        };
        const qint64 files = add(ClipKind::Files, u"C:\\old\\a.txt\nC:\\old\\b.txt"_s, 1000);
        const qint64 half = add(ClipKind::Files, u"C:\\old\\c.txt\nC:\\kept\\d.txt"_s, 1001); // one still there
        const qint64 pinned = add(ClipKind::Files, u"C:\\old\\e.txt"_s, 1002);
        store.setGroup(std::vector {pinned}, ClipStore::kPinned);
        const qint64 path = add(ClipKind::Path, u"C:\\old\\f.txt"_s, 1003); // a path copied as text
        setGone({u"C:\\old\\a.txt"_s, u"C:\\old\\b.txt"_s, u"C:\\old\\c.txt"_s, u"C:\\old\\e.txt"_s, u"C:\\old\\f.txt"_s});
        const auto listed = [&store](const ClipFilter& filter) {
            QList<qint64> ids;
            for (const int row : store.find(filter))
                ids.append(store.clips()[static_cast<std::size_t>(row)].id);
            return ids;
        };

        // Hidden from the lists after the first pass, not from a search.
        MissingFiles missing(&store, MissingFiles::Mode::Hide, disk);
        QTRY_COMPARE(store.hiddenCount(), 1);
        QVERIFY(store.hidden(*store.clip(files)));
        QCOMPARE(listed({}), (QList<qint64> {path, pinned, half}));
        QCOMPARE(listed({.category = ClipFilter::Category::Files}), (QList<qint64> {path, pinned, half}));
        QCOMPARE(listed({.query = u"a.txt"_s}), QList<qint64> {files});

        // Copied again (its files back, it seems): listed again at once.
        add(ClipKind::Files, u"C:\\old\\a.txt\nC:\\old\\b.txt"_s, 2000);
        QVERIFY(!store.hidden(*store.clip(files)));
        QCOMPARE(store.hiddenCount(), 0);

        // While the window is up, a pass's findings wait until it is put away.
        missing.setListShown(true);
        missing.setMode(MissingFiles::Mode::Keep);
        missing.setMode(MissingFiles::Mode::Hide); // turned on: a pass at once
        QVERIFY(missing.checking());
        QTRY_VERIFY(!missing.checking());
        QCOMPARE(store.hiddenCount(), 0);
        missing.setListShown(false); // and another pass, none being due later
        QCOMPARE(store.hiddenCount(), 1);
        QTRY_VERIFY(!missing.checking());

        // 不处理: listed again at once.
        QSignalSpy changed(&missing, &MissingFiles::changed);
        missing.setMode(MissingFiles::Mode::Keep);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(store.hiddenCount(), 0);
        missing.setListShown(true);
        missing.setListShown(false);
        QVERIFY(!missing.checking()); // no passes then

        // Cleaned out once a second pass found it gone too; hidden till then.
        missing.setMode(MissingFiles::Mode::Clean);
        QTRY_COMPARE(store.hiddenCount(), 1);
        QVERIFY(store.clip(files));
        missing.setListShown(true);
        missing.setListShown(false);
        QTRY_VERIFY(!store.clip(files));
        QCOMPARE(store.hiddenCount(), 0);
        QVERIFY(store.clip(half) && store.clip(pinned) && store.clip(path)); // never those
        QVERIFY(store.clip(pinned)->group == ClipStore::kPinned);
        QTRY_VERIFY(!missing.checking());
        missing.setListShown(true);
        missing.setListShown(false);
        QTRY_VERIFY(!missing.checking());
        QCOMPARE(store.clips().size(), std::size_t {3});
    }

    // GitHub #52: what the SCM says about the index service, told apart.
    void serviceStateFromScm()
    {
        using ntfs::ServiceState;
        using S = ntfs::ServiceVolumeAccess;
        constexpr unsigned long unknownType = 0xFFFFFFFF;
        QCOMPARE(S::classify(ERROR_SERVICE_DOES_NOT_EXIST, unknownType, 0), ServiceState::NotInstalled);
        QCOMPARE(S::classify(ERROR_ACCESS_DENIED, unknownType, 0), ServiceState::Unknown); // the old way, by the pipe
        QCOMPARE(S::classify(0, unknownType, 0), ServiceState::Unknown); // status not readable
        QCOMPARE(S::classify(0, SERVICE_AUTO_START, SERVICE_STOPPED), ServiceState::Stopped);
        QCOMPARE(S::classify(0, SERVICE_DISABLED, SERVICE_STOPPED), ServiceState::Disabled);
        QCOMPARE(S::classify(0, unknownType, SERVICE_STOPPED), ServiceState::Stopped); // config unreadable: not "disabled"
        QCOMPARE(S::classify(0, SERVICE_AUTO_START, SERVICE_STOP_PENDING), ServiceState::Stopped);
        QCOMPARE(S::classify(0, SERVICE_AUTO_START, SERVICE_START_PENDING), ServiceState::Starting);
        QCOMPARE(S::classify(0, SERVICE_AUTO_START, SERVICE_RUNNING), ServiceState::Running);
        QCOMPARE(S::classify(0, SERVICE_DISABLED, SERVICE_RUNNING), ServiceState::Running); // still answering: the pipe decides
        QCOMPARE(S::classify(0, SERVICE_AUTO_START, SERVICE_PAUSED), ServiceState::Running);
        QCOMPARE(QString::fromLatin1(S::stateName(ServiceState::NotInstalled)), u"not_installed"_s);
        QCOMPARE(QString::fromLatin1(S::stateName(ServiceState::Unknown)), u"unknown"_s);
    }

    // WINSHUN_SERVICE=none|disabled|stopped pretends, installed() follows.
    void serviceStateSwitch()
    {
        using ntfs::ServiceState;
        using S = ntfs::ServiceVolumeAccess;
        QVERIFY(S::forcedState(L"none") == std::optional(ServiceState::NotInstalled));
        QVERIFY(S::forcedState(L"disabled") == std::optional(ServiceState::Disabled));
        QVERIFY(S::forcedState(L"stopped") == std::optional(ServiceState::Stopped));
        QVERIFY(!S::forcedState(L"running"));
        QVERIFY(!S::forcedState(L""));

        wchar_t before[64] {};
        const bool had = ::GetEnvironmentVariableW(L"WINSHUN_SERVICE", before, 64) > 0;
        const auto restore = qScopeGuard([&] { ::SetEnvironmentVariableW(L"WINSHUN_SERVICE", had ? before : nullptr); });
        ::SetEnvironmentVariableW(L"WINSHUN_SERVICE", L"none");
        QCOMPARE(S::state(), ServiceState::NotInstalled);
        QVERIFY(!S::installed());
        ::SetEnvironmentVariableW(L"WINSHUN_SERVICE", L"disabled");
        QCOMPARE(S::state(), ServiceState::Disabled);
        QVERIFY(S::installed());
        ::SetEnvironmentVariableW(L"WINSHUN_SERVICE", L"stopped");
        QCOMPARE(S::state(), ServiceState::Stopped);
        QVERIFY(S::installed());
        // Anything else is ignored: the real SCM answers (one of the six).
        ::SetEnvironmentVariableW(L"WINSHUN_SERVICE", L"nonsense");
        const ServiceState real = S::state();
        QVERIFY(real >= ServiceState::Unknown && real <= ServiceState::Running);
    }

    // GitHub #52: the Run-key value that stands in for the logon task.
    void autostartRunValue()
    {
        const std::wstring exe = LR"(C:\Program Files\WinShun\WinShun.exe)";
        const std::wstring command = autostart::runCommand(exe);
        QCOMPARE(QString::fromStdWString(command), uR"("C:\Program Files\WinShun\WinShun.exe" --background)"_s);
        QCOMPARE(autostart::runCommandProgram(command), exe);
        QVERIFY(autostart::runCommandStarts(command, exe));
        QVERIFY(autostart::runCommandStarts(command, LR"(c:\program files\winshun\winshun.exe)")); // paths ignore case
        QVERIFY(!autostart::runCommandStarts(command, LR"(C:\Other\WinShun.exe)"));
        QVERIFY(!autostart::runCommandStarts(command, LR"(C:\Program Files\WinShun\WinShun.exe.bak)"));
        QCOMPARE(autostart::runCommandProgram(LR"(C:\x\WinShun.exe --background)"), std::wstring(LR"(C:\x\WinShun.exe)")); // unquoted
        QCOMPARE(autostart::runCommandProgram(LR"(C:\x\WinShun.exe)"), std::wstring(LR"(C:\x\WinShun.exe)"));
        QCOMPARE(autostart::runCommandProgram(LR"(  "D:\a b\w.exe" -x)"), std::wstring(LR"(D:\a b\w.exe)"));
        QVERIFY(autostart::runCommandProgram(L"").empty());
        QVERIFY(autostart::runCommandProgram(L"   ").empty());
        QVERIFY(!autostart::runCommandStarts(L"", exe));
    }

    // The index service must be registered from a place only administrators
    // can write to; svcloc judges that from a security descriptor. The owner
    // and DACL cases below are the heart of the local privilege-escalation fix.
    void protectedLocationJudgment()
    {
        // A security descriptor built from SDDL, judged in one of the three
        // roles. unsafe = a non-admin could change it (Object) / plant a file
        // beside the exe (Parent) / replace the child in it (Parent, Ancestor).
        const auto unsafe = [](const wchar_t* sddl, svcloc::Role role) -> bool {
            PSECURITY_DESCRIPTOR sd = nullptr;
            [&] { QVERIFY2(::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr),
                      "bad test SDDL"); }();
            PSID owner = nullptr;
            PACL dacl = nullptr;
            BOOL ownerDefaulted = FALSE, present = FALSE, defaulted = FALSE;
            ::GetSecurityDescriptorOwner(sd, &owner, &ownerDefaulted);
            ::GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
            const bool verdict = svcloc::writableByNonAdmin(owner, dacl, role);
            ::LocalFree(sd);
            return verdict;
        };
        using svcloc::Role;

        // Only SYSTEM / Administrators have rights, users may read: safe.
        QVERIFY(!unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)", Role::Object));
        QVERIFY(!unsafe(L"O:BAD:P(A;;FR;;;BU)(A;;FA;;;SY)(A;;FA;;;BA)", Role::Object));
        QVERIFY(!unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", Role::Object)); // empty-ish DACL, admin owner
        // Users or Authenticated Users can write / modify: unsafe.
        QVERIFY(unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)", Role::Object)); // Modify
        QVERIFY(unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;AU)", Role::Object));
        QVERIFY(unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FW;;;WD)", Role::Object)); // Everyone, write
        QVERIFY(unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;CO)", Role::Object)); // CREATOR OWNER
        // A non-administrator owner can rewrite the DACL: unsafe, either role.
        QVERIFY(unsafe(L"O:S-1-5-21-1-2-3-1001D:P(A;;FA;;;SY)(A;;FA;;;BA)", Role::Object));
        QVERIFY(unsafe(L"O:S-1-5-21-1-2-3-1001D:P(A;;FA;;;SY)(A;;FA;;;BA)", Role::Ancestor));
        // Deny aces do not grant; inherit-only aces do not apply here: safe.
        QVERIFY(!unsafe(L"O:BAD:P(D;;FA;;;BU)(A;;FA;;;SY)(A;;FA;;;BA)", Role::Object));
        QVERIFY(!unsafe(L"O:BAD:P(A;OICIIO;FA;;;WD)(A;;FA;;;SY)(A;;FA;;;BA)", Role::Object));
        // The default C:\ : users may create a *folder* (0x4) but not delete a
        // child. As an ancestor it is safe (or every path would be unsafe); as
        // an object (a folder that is itself the exe's folder) it is not.
        const wchar_t* rootLike
            = L"O:SYD:P(A;OICIIO;GA;;;CO)(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;0x1200a9;;;BU)(A;CI;0x000004;;;AU)";
        QVERIFY(!unsafe(rootLike, Role::Ancestor));
        QVERIFY(unsafe(rootLike, Role::Object));
        // The exe's own folder must not let anyone else add a file (a DLL the
        // loader would take before System32's) or a folder: the C:\ above,
        // or a ProgramData-like folder (users may add files and folders, but
        // not delete others'), is fine further up, not as the exe's folder.
        QVERIFY(unsafe(rootLike, Role::Parent));
        const wchar_t* programDataLike = L"O:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;GA;;;CO)"
                                         L"(A;OICI;0x1200a9;;;BU)(A;CI;0x116;;;BU)";
        QVERIFY(!unsafe(programDataLike, Role::Ancestor));
        QVERIFY(unsafe(programDataLike, Role::Parent));
        QVERIFY(unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)(A;;0x2;;;BU)", Role::Parent)); // add a file
        QVERIFY(!unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)", Role::Parent)); // read/run only
        QVERIFY(unsafe(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x10000;;;BU)", Role::Parent)); // delete it
        QVERIFY(unsafe(L"O:S-1-5-21-1-2-3-1001D:P(A;;FA;;;SY)(A;;FA;;;BA)", Role::Parent));

        // A NULL DACL grants everyone everything, even with an admin owner.
        BYTE adminSid[SECURITY_MAX_SID_SIZE] {};
        DWORD adminSize = sizeof adminSid;
        QVERIFY(::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSize));
        QVERIFY(svcloc::writableByNonAdmin(adminSid, nullptr, Role::Object));

        // On a real machine: a genuine Windows file is protected (this is the
        // end-to-end check that the folder and ancestor walk is not
        // over-strict); a file in the user's temp is not.
        QVERIFY(svcloc::pathProtected(LR"(C:\Windows\System32\cmd.exe)"));
        QVERIFY(svcloc::folderProtected(LR"(C:\Windows\System32)"));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString file = temp.filePath(u"x.txt"_s);
        QVERIFY(QFile(file).open(QIODevice::WriteOnly));
        QVERIFY(!svcloc::pathProtected(QDir::toNativeSeparators(file).toStdWString()));
        QVERIFY(!svcloc::folderProtected(QDir::toNativeSeparators(temp.path()).toStdWString()));

        // The dedicated folder, and the parent test.
        const std::wstring dir = svcloc::protectedServiceDir();
        QVERIFY(dir.ends_with(L"\\WinShun Service"));
        QVERIFY(svcloc::parentIs(dir + L"\\WinShunService.exe", dir));
        QVERIFY(svcloc::parentIs(dir + L"\\winshunservice.exe", dir)); // paths ignore case
        QVERIFY(!svcloc::parentIs(dir + L"\\sub\\WinShunService.exe", dir));
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
