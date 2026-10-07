// Fuzzes the parsers that read data from disks: MFT file records, run lists,
// change journal records, file names and text file contents. Built with the
// "asan" preset; run it for a while to look for crashes and bad reads:
//
//   build\asan\qffuzz.exe -max_total_time=300
//
// The first input byte picks the parser, the rest is its input.

#include "ContentScanner.h"
#include "Ntfs.h"
#include "Pinyin.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::uint8_t which = data[0];
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data + 1), size - 1);
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);

    switch (which % 5) {
    case 0: { // an MFT record, as MftReader hands it over (fixed up in place)
        std::vector<std::byte> record(input.begin(), input.end());
        qf::ntfs::FileRecord out;
        qf::ntfs::parseFileRecord(record.data(), record.size(), out);
        for (const auto& name : out.names)
            (void)qf::wtf8::fromUtf16(name.name);
        break;
    }
    case 1: // the MFT's run list
        (void)qf::ntfs::decodeRunList(input, 4096);
        break;
    case 2: { // what FSCTL_READ_USN_JOURNAL returns
        std::vector<qf::ntfs::UsnRecord> records;
        (void)qf::ntfs::parseUsnRecords(input, records);
        break;
    }
    case 3: { // a file name in the index (WTF-8), matched every way the search does
        std::u16string decoded;
        qf::wtf8::decodeAppend(decoded, text);
        (void)qf::pinyin::hasHan(text);
        (void)qf::pinyin::Matcher("bg").findUtf8(text);
        (void)qf::pinyin::Matcher("baogao").findUtf16(decoded);
        (void)qf::text::globMatch(text, "*a?b*.t?t");
        (void)qf::text::findFolded(text, "abc");
        for (std::size_t i = 0; i <= text.size(); ++i)
            (void)qf::text::isWordStart(text, i);
        // The SIMD search may read past the name: give it the padding the index has.
        std::vector<char> padded(text.begin(), text.end());
        padded.resize(padded.size() + qf::text::kReadPastEnd);
        (void)qf::text::findFoldedPadded({padded.data(), text.size()}, "ab");
        break;
    }
    case 4: { // a text file's contents, read in small chunks
        static const qf::ContentScanner scanner(QStringLiteral(u"needle 报告"), 936);
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
    }
    return 0;
}
