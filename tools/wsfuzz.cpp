// Fuzzes the parsers that read data from disks: MFT file records, run lists,
// change journal records, file names, text file contents, the content
// index's files, and documents (all but PDF). Built with the "asan" preset;
// run it for a while to look for crashes and bad reads:
//
//   build\asan\wsfuzz.exe -max_total_time=300
//
// The first input byte picks the parser, the rest is its input. Documents
// (8) make good seeds: a real .doc, .xls, .docx... with a byte 8 in front.

#include "ContentIndex.h"
#include "ContentScanner.h"
#include "Extract.h"
#include "Formats.h"
#include "miniz.h"
#include "Ntfs.h"
#include "Pinyin.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {

// A content index of 64 documents in one segment file, and its saved state:
// the segment file's contents are what gets fuzzed. Some documents are
// copies of others; one has most trigrams. The segment is also left as
// seed.bin: with a byte 7 in front, a seed for the fuzzer's corpus.
struct ContentFixture {
    QString directory = QDir::tempPath() + QStringLiteral("/wsfuzz-content");
    QString segment;
    std::vector<char> state;

    ContentFixture()
    {
        QDir(directory).removeRecursively();
        ws::ContentIndex index(directory);
        for (ws::EntryId e = 0; e < 64; ++e) {
            std::vector<ws::grams::Key> keys;
            for (char32_t c = 0x4E00 + e % 5; c < 0x4E00 + 40; c += 3)
                keys.push_back((ws::grams::Key {c} << ws::grams::kCharBits) | (c + 1));
            for (char a = 'a'; a < 'a' + 1 + e % 3; ++a) // trigrams too: "abc", "bbc", "cbc"
                keys.push_back((ws::grams::Key {static_cast<unsigned char>(a)} << ws::grams::kCharBits) | ('b' << 8) | 'c');
            if (e == 40) { // "000", "001", ...
                constexpr char kWord[] = "0123456789_abcdefghijklmnopqrstuvwxyz";
                for (unsigned t = 0; t < 10001; ++t)
                    keys.push_back((ws::grams::Key {static_cast<unsigned char>(kWord[t / (37 * 37)])} << ws::grams::kCharBits)
                        | (static_cast<unsigned char>(kWord[t / 37 % 37]) << 8) | static_cast<unsigned char>(kWord[t % 37]));
            }
            index.add(e, keys, e % 9 == 0, 0, 0);
        }
        index.merge();
        std::vector<ws::EntryId> ids(64);
        for (ws::EntryId e = 0; e < 64; ++e)
            ids[e] = e;
        std::vector<std::uint64_t> segments;
        state = index.serialize(ids, segments);
        segment = directory + QStringLiteral("/%1.grams").arg(segments.at(0));
        QFile::copy(segment, directory + QStringLiteral("/seed.bin"));
    }
};

// Reads a document out of memory, as the extractor reads a file.
void extractDocument(std::string_view bytes)
{
    ws::extract::MemorySource source(bytes);
    ws::extract::Options options;
    options.maxText = 1u << 20;
    options.codePage = 936;
    options.unpackBudget = 16u << 20;
    ws::doctext::DocText text;
    (void)ws::extract::extract(source, options, text);
}

std::string zipOf(const std::vector<std::pair<const char*, std::string_view>>& entries)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    for (const auto& [name, data] : entries)
        mz_zip_writer_add_mem(&zip, name, data.data(), data.size(), MZ_NO_COMPRESSION);
    void* buffer = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    std::string out(static_cast<const char*>(buffer), size);
    mz_zip_writer_end(&zip);
    return out;
}

// A compound file with these streams in its root, each in regular sectors.
std::string cfbOf(const std::vector<std::pair<std::u16string_view, std::string_view>>& streams)
{
    constexpr std::uint32_t kEnd = 0xFFFFFFFE;
    constexpr std::uint32_t kFree = 0xFFFFFFFF;
    std::string sectors;
    std::vector<std::uint32_t> fat;
    const auto chain = [&](std::string data) -> std::uint32_t {
        data.resize(std::max<std::size_t>((data.size() + 511) / 512 * 512, 512), '\0');
        const auto start = static_cast<std::uint32_t>(fat.size());
        for (std::size_t at = 0; at < data.size(); at += 512)
            fat.push_back(static_cast<std::uint32_t>(fat.size() + 1));
        fat.back() = kEnd;
        sectors += data;
        return start;
    };
    std::string directory(128 * (streams.size() + 1), '\0');
    const auto entry = [&](std::size_t i, std::u16string_view name, char type, std::uint32_t child,
                           std::uint32_t right, std::uint32_t start, std::uint32_t size) {
        char* e = directory.data() + 128 * i;
        std::memcpy(e, name.data(), name.size() * 2);
        const auto length = static_cast<std::uint16_t>((name.size() + 1) * 2);
        std::memcpy(e + 0x40, &length, 2);
        e[0x42] = type;
        std::memcpy(e + 0x44, &kFree, 4);
        std::memcpy(e + 0x48, &right, 4);
        std::memcpy(e + 0x4C, &child, 4);
        std::memcpy(e + 0x74, &start, 4);
        std::memcpy(e + 0x78, &size, 4);
    };
    for (std::size_t i = 0; i < streams.size(); ++i) {
        // At least 4096 bytes: in regular sectors, not the mini stream.
        std::string data(streams[i].second);
        const auto size = static_cast<std::uint32_t>(std::max<std::size_t>(data.size(), 4096));
        data.resize(size, '\0');
        entry(i + 1, streams[i].first, 2, kFree, i + 1 < streams.size() ? static_cast<std::uint32_t>(i + 2) : kFree,
            chain(data), size);
    }
    entry(0, u"Root Entry", 5, 1, kFree, kEnd, 0);
    const std::uint32_t directoryStart = chain(directory);
    const auto dataSectors = static_cast<std::uint32_t>(fat.size());
    std::uint32_t fatSectors = 1;
    while (dataSectors + fatSectors > fatSectors * 128)
        ++fatSectors;
    for (std::uint32_t k = 0; k < fatSectors; ++k)
        fat.push_back(0xFFFFFFFD);
    fat.resize(fatSectors * 128, kFree);
    std::string header(512, '\0');
    std::memcpy(header.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    const std::uint16_t fields[] = {0x3E, 3, 0xFFFE, 9, 6};
    std::memcpy(header.data() + 0x18, fields, sizeof fields);
    const std::uint32_t values[] = {fatSectors, directoryStart, 0, 4096, kEnd, 0, kEnd, 0};
    std::memcpy(header.data() + 0x2C, values, sizeof values);
    for (std::uint32_t k = 0; k < 109; ++k) {
        const std::uint32_t at = k < fatSectors ? dataSectors + k : kFree;
        std::memcpy(header.data() + 0x4C + 4 * k, &at, 4);
    }
    return header + sectors
        + std::string(reinterpret_cast<const char*>(fat.data()), fat.size() * sizeof(std::uint32_t));
}

} // namespace

// PDF is left out (Pdf.cpp is not built in).
namespace ws::extract {
bool loadPdfium()
{
    return false;
}
Status extractPdf(Source&, Writer&)
{
    return Status::Unsupported;
}
} // namespace ws::extract

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::uint8_t which = data[0];
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data + 1), size - 1);
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);

    switch (which % 14) {
    case 0: { // an MFT record, as MftReader hands it over (fixed up in place)
        std::vector<std::byte> record(input.begin(), input.end());
        ws::ntfs::FileRecord out;
        ws::ntfs::parseFileRecord(record.data(), record.size(), out);
        for (const auto& name : out.names)
            (void)ws::wtf8::fromUtf16(name.name);
        break;
    }
    case 1: // the MFT's run list
        (void)ws::ntfs::decodeRunList(input, 4096);
        break;
    case 2: { // what FSCTL_READ_USN_JOURNAL returns
        std::vector<ws::ntfs::UsnRecord> records;
        (void)ws::ntfs::parseUsnRecords(input, records);
        break;
    }
    case 3: { // a file name in the index (WTF-8), matched every way the search does
        std::u16string decoded;
        ws::wtf8::decodeAppend(decoded, text);
        (void)ws::pinyin::hasHan(text);
        (void)ws::pinyin::Matcher("bg").findUtf8(text);
        (void)ws::pinyin::Matcher("baogao").findUtf16(decoded);
        (void)ws::text::globMatch(text, "*a?b*.t?t");
        (void)ws::text::findFolded(text, "abc");
        for (std::size_t i = 0; i <= text.size(); ++i)
            (void)ws::text::isWordStart(text, i);
        // The SIMD search may read past the name: give it the padding the index has.
        std::vector<char> padded(text.begin(), text.end());
        padded.resize(padded.size() + ws::text::kReadPastEnd);
        (void)ws::text::findFoldedPadded({padded.data(), text.size()}, "ab");
        break;
    }
    case 4: { // a text file's contents, read in small chunks
        static const ws::ContentScanner scanner(QStringLiteral(u"needle 报告"), 936);
        std::size_t offset = 0;
        const auto read = [&](char* buffer, std::size_t capacity) -> std::size_t {
            const std::size_t n = std::min({capacity, std::size_t {37}, text.size() - offset});
            std::memcpy(buffer, text.data() + offset, n);
            offset += n;
            return n;
        };
        (void)scanner.scan(read, 64, {});
        break;
    }
    case 5: { // a text file's contents, for the content index, in every encoding
        for (const auto encoding : {ws::TextEncoding::Utf8, ws::TextEncoding::Utf16LE, ws::TextEncoding::Utf16BE,
                 ws::TextEncoding::Ansi}) {
            ws::grams::Collector collector(encoding, 936);
            for (std::size_t i = 0; i < text.size(); i += 37)
                collector.feed(text.data() + i, std::min<std::size_t>(37, text.size() - i));
            (void)collector.finish();
        }
        (void)ws::grams::ofPhrase(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        break;
    }
    case 6: { // the content index's state, as saved in the snapshot (no segment files)
        ws::ContentIndex index(QDir::tempPath() + QStringLiteral("/wsfuzz-content-none"));
        if (index.restore(std::span<const char>(text.data(), text.size()), 1000)) {
            (void)index.lookup(QStringLiteral(u"中文"));
            (void)index.lookup(QStringLiteral(u"needle"));
            (void)index.documents();
        }
        break;
    }
    case 7: { // a segment file of the content index
        static const ContentFixture fixture;
        {
            QFile file(fixture.segment);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                break;
            file.write(text.data(), static_cast<qint64>(text.size()));
        }
        ws::ContentIndex index(fixture.directory);
        if (index.restore(fixture.state, 64)) {
            (void)index.lookup(QStringLiteral(u"一丁"));
            (void)index.lookup(QStringLiteral(u"丂七"));
            (void)index.lookup(QStringLiteral(u"万"));
            (void)index.lookup(QStringLiteral(u"bbc abc"));
            (void)index.lookup(QStringLiteral(u"0001"));
            (void)index.merge(); // walks every key and posting
        }
        break;
    }
    case 8: // a document file
        if (text.substr(0, 1024).find("%PDF-") == std::string_view::npos)
            extractDocument(text);
        break;
    case 9: // a Word document's body
    case 10: { // a sheet of a workbook, and its shared strings
        constexpr std::string_view kWordRels
            = R"(<Relationships><Relationship Id="1" Type="x/officeDocument" Target="word/document.xml"/></Relationships>)";
        constexpr std::string_view kBookRels
            = R"(<Relationships><Relationship Id="1" Type="x/officeDocument" Target="xl/workbook.xml"/></Relationships>)";
        constexpr std::string_view kWorkbook = R"(<workbook><sheets><sheet name="s" r:id="1"/></sheets></workbook>)";
        constexpr std::string_view kWorkbookRels
            = R"(<Relationships><Relationship Id="1" Type="x/worksheet" Target="sheet.xml"/>)"
              R"(<Relationship Id="2" Type="x/sharedStrings" Target="strings.xml"/></Relationships>)";
        extractDocument(which % 14 == 9 ? zipOf({{"_rels/.rels", kWordRels}, {"word/document.xml", text}})
                                        : zipOf({{"_rels/.rels", kBookRels}, {"xl/workbook.xml", kWorkbook},
                                              {"xl/_rels/workbook.xml.rels", kWorkbookRels}, {"xl/sheet.xml", text},
                                              {"xl/strings.xml", text}}));
        break;
    }
    case 11: // an Excel 97 workbook stream
        extractDocument(cfbOf({{u"Workbook", text}}));
        break;
    case 12: { // a Word 97 document: its first bytes say where the table stream starts
        const std::size_t split = text.size() < 2 ? 0 : (static_cast<unsigned char>(text[0]) << 8 | static_cast<unsigned char>(text[1])) % text.size();
        extractDocument(cfbOf({{u"WordDocument", text.substr(0, split)}, {u"1Table", text.substr(split)},
            {u"0Table", text.substr(split)}}));
        break;
    }
    case 13: // a PowerPoint 97 document stream
        extractDocument(cfbOf({{u"PowerPoint Document", text}}));
        break;
    }
    return 0;
}
