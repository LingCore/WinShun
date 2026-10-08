// Fuzzes the parsers that read data from disks: MFT file records, run lists,
// change journal records, file names, text file contents and the content
// index's files. Built with the "asan" preset; run it for a while to look for
// crashes and bad reads:
//
//   build\asan\wsfuzz.exe -max_total_time=300
//
// The first input byte picks the parser, the rest is its input.

#include "ContentIndex.h"
#include "ContentScanner.h"
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
// the segment file's contents are what gets fuzzed. One document is dense
// (has most trigrams). The segment is also left as seed.bin: with a byte 7
// in front, a seed for the fuzzer's corpus.
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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::uint8_t which = data[0];
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data + 1), size - 1);
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);

    switch (which % 8) {
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
    }
    return 0;
}
