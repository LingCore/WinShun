#include "AppCatalog.h"
#include "AppLogo.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "DoubleTapDetector.h"
#include "FileIndex.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "Pinyin.h"
#include "Query.h"
#include "Snapshot.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <unordered_map>

using namespace qf;
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

// A 1 KB MFT file record as NTFS writes it, update sequence included.
class RecordBuilder {
public:
    explicit RecordBuilder(std::uint16_t flags, std::uint64_t baseRecord = 0)
        : m_data(1024)
    {
        put<std::uint32_t>(m_data, 0, 0x454C4946); // "FILE"
        put<std::uint16_t>(m_data, 0x04, 0x30); // update sequence array
        put<std::uint16_t>(m_data, 0x06, 3); // check value + one per 512 bytes
        put<std::uint16_t>(m_data, 0x10, 1); // sequence number
        put<std::uint16_t>(m_data, 0x14, 0x38); // first attribute
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

    std::vector<std::byte> finish()
    {
        put<std::uint32_t>(m_data, m_pos, 0xFFFF'FFFF);
        put<std::uint32_t>(m_data, 0x18, static_cast<std::uint32_t>(m_pos + 8)); // bytes in use
        put<std::uint32_t>(m_data, 0x1C, 1024);
        // Each 512-byte stride ends in the check value; the real bytes move to the array.
        put<std::uint16_t>(m_data, 0x30, 0x0042);
        for (std::size_t i = 1; i <= 2; ++i) {
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
    std::size_t m_pos = 0x38;
};

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

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

private slots:
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
        const ParsedQuery q = qf::parseQuery(uR"(report "annual plan" !draft ext:pdf,.docx proj\read)"_s);
        QCOMPARE(q.terms.size(), std::size_t {4});
        QCOMPARE(q.terms[0].text, std::string("report"));
        QCOMPARE(q.terms[1].text, std::string("annual plan"));
        QVERIFY(q.terms[2].negated);
        QCOMPARE(q.terms[3].text, std::string("read"));
        QCOMPARE(q.terms[3].ancestors, std::vector<std::string> {"proj"});
        QCOMPARE(q.extensions, (std::vector<std::string> {"pdf", "docx"}));
        QCOMPARE(q.highlights, (QStringList {u"report"_s, u"annual plan"_s, u"read"_s}));

        QVERIFY(qf::parseQuery(u"   "_s).isEmpty());
        QVERIFY(qf::parseQuery(u"*.TXT"_s).terms[0].wildcard);
        QCOMPARE(qf::parseQuery(u"rep*2024?.PDF"_s).terms[0].literal, std::string("2024"));
    }

    void matcherRanking()
    {
        const NameMatcher m(qf::parseQuery(u"report"_s));
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

        const NameMatcher negated(qf::parseQuery(u"report !draft"_s));
        QCOMPARE(negated.matchPath(u"C:\\report_draft.txt"_s, false), -1);

        const NameMatcher ext(qf::parseQuery(u"ext:pdf"_s));
        QVERIFY(ext.matchPath(u"C:\\a.PDF"_s, false) >= 0);
        QCOMPARE(ext.matchPath(u"C:\\a.pdf"_s, true), -1); // folders have no extension

        const NameMatcher path(qf::parseQuery(u"users\\notes"_s));
        QVERIFY(path.matchPath(u"C:\\Users\\me\\notes.txt"_s, false) >= 0);
        QCOMPARE(path.matchPath(u"D:\\Other\\notes.txt"_s, false), -1);
    }

    void excludedFolders()
    {
        // "!folder\" leaves out what is inside such a folder, not every name.
        const NameMatcher folder(qf::parseQuery(u"index !node_modules\\"_s));
        QCOMPARE(folder.matchPath(u"C:\\app\\node_modules\\lib\\index.js"_s, false), -1);
        QVERIFY(folder.matchPath(u"C:\\app\\src\\index.js"_s, false) >= 0);
        QVERIFY(folder.matchName("index") >= 0); // apps have no folders

        // With a name too: only those names inside such a folder.
        const NameMatcher logs(qf::parseQuery(u"!tmp\\*.log"_s));
        QCOMPARE(logs.matchPath(u"C:\\tmp\\a.log"_s, false), -1);
        QVERIFY(logs.matchPath(u"C:\\tmp\\a.txt"_s, false) >= 0);
        QVERIFY(logs.matchPath(u"C:\\docs\\a.log"_s, false) >= 0);

        // The same on the index.
        SampleTree t;
        const NameMatcher underUsers(qf::parseQuery(u"!users\\"_s));
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

        const NameMatcher bg(qf::parseQuery(u"bg"_s));
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
        const int common = NameMatcher(qf::parseQuery(u"yx"_s)).match(index, index.entry(bank));
        const int lessCommon = NameMatcher(qf::parseQuery(u"yh"_s)).match(index, index.entry(bank));
        QVERIFY(lessCommon > 0);
        QVERIFY(common > lessCommon);
        QVERIFY(NameMatcher(qf::parseQuery(u"bj"_s)).match(index, index.entry(renamed)) > 0);

        // Folders in path terms, and history paths.
        QVERIFY(NameMatcher(qf::parseQuery(u"xmzl\\fa"_s)).match(index, index.entry(plan)) > 0);
        QCOMPARE(NameMatcher(qf::parseQuery(u"xx\\fa"_s)).match(index, index.entry(plan)), -1);
        QVERIFY(NameMatcher(qf::parseQuery(u"fa"_s)).matchPath(u"C:\\项目资料\\方案.doc"_s, false) > 0);
        // Exclusions stay literal.
        QVERIFY(NameMatcher(qf::parseQuery(u"txt !bg"_s)).match(index, index.entry(report)) > 0);
    }

    void indexAddFindRemoveMove()
    {
        SampleTree t;
        QCOMPARE(t.index.liveCount(), std::size_t {7});
        QCOMPARE(t.index.findPath(L"c:\\users\\ME\\documents\\report.DOCX"), t.report);
        QCOMPARE(t.index.findPath(L"C:\\"), t.root);
        QCOMPARE(t.index.findPath(L"C:\\nope"), kNoEntry);
        QCOMPARE(t.index.entry(t.report).extLength, std::uint8_t {4});

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
        const std::vector<VolumeInfo> volumes {{L"C:", 0x1234, true}};
        const std::vector<JournalPosition> journals {{0xABCDEF, 123456789}};
        QTemporaryDir dir;
        const QString file = dir.filePath(u"index.bin"_s);
        QVERIFY(snapshot::save(t.index, volumes, journals, file));

        std::vector<JournalPosition> loadedJournals;
        const auto loaded = snapshot::load(file, volumes, &loadedJournals);
        QVERIFY(loaded);
        QCOMPARE(loaded->liveCount(), std::size_t {6});
        const EntryId report = loaded->findPath(L"C:\\Users\\me\\Documents\\Report.docx");
        QVERIFY(report != kNoEntry);
        QCOMPARE(loaded->path(report), u"C:\\Users\\me\\Documents\\Report.docx"_s);
        QVERIFY(loaded->entry(loaded->findPath(L"C:\\Windows")).flags & EntryFlag::LowPriority);
        QCOMPARE(loaded->findPath(L"C:\\Users\\me\\Documents\\notes.txt"), kNoEntry);

        QVERIFY(loadedJournals == journals);
        const EntryId root = loaded->roots().front();
        QCOMPARE(loaded->folderByRecord(root, ntfs::kRootRecord), root);
        QCOMPARE(loaded->folderByRecord(root, 100), loaded->findPath(L"C:\\Users\\me\\Documents"));
        QCOMPARE(loaded->folderByRecord(root, 101), loaded->findPath(L"C:\\Windows"));
        QCOMPARE(loaded->folderRecords(root)->size(), std::size_t {3}); // the removed folder is not kept

        QVERIFY(!snapshot::load(file, {{L"C:", 0x9999, true}})); // different volume serial
        QVERIFY(!snapshot::load(dir.filePath(u"missing.bin"_s), volumes));
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
        UsnApplier applier(index, crawler, rootName);
        applier.apply(batch, {});
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
        QCOMPARE(index.slotCount(), std::size_t {10});
        QCOMPARE(index.liveCount(), std::size_t {7});

        QCOMPARE(index.compact(), std::size_t {3});
        QCOMPARE(index.slotCount(), std::size_t {7});
        QCOMPARE(index.liveCount(), std::size_t {7});
        for (std::size_t i = 0; i < index.slotCount(); ++i)
            QVERIFY(!index.entry(static_cast<EntryId>(i)).isDeleted());
        QCOMPARE(index.roots().size(), std::size_t {1});
        QCOMPARE(index.path(index.roots().front()), u"C:\\"_s);

        const EntryId report = index.findPath(L"C:\\Users\\Report-final.docx");
        QVERIFY(report != kNoEntry);
        QCOMPARE(index.entry(report).extLength, std::uint8_t {4});
        const EntryId js = index.findPath(L"C:\\Users\\me\\Documents\\index.js");
        QVERIFY(js != kNoEntry);
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

    void nameSearchTopK()
    {
        FileIndex index;
        const EntryId root = index.addRoot("C:");
        const EntryId dir = index.add(root, "data", EntryFlag::Directory);
        for (int i = 0; i < 70000; ++i)
            index.add(dir, "file_" + std::to_string(i) + ".txt", 0);
        index.add(dir, "file_1", 0); // exact name: must win

        const NameMatcher matcher(qf::parseQuery(u"file_1"_s));
        WorkerPool pool(4);
        const auto out = searchNames(index, matcher, Scope::All, 10, pool, {});
        QVERIFY(!out.cancelled);
        QCOMPARE(out.hits.size(), std::size_t {10});
        QCOMPARE(index.name(out.hits.front().id), std::string_view("file_1"));
        // file_1, file_10..19, ..., file_10000..19999 (.txt), plus the exact name
        QCOMPARE(out.totalMatches, std::size_t {1 + 10 + 100 + 1000 + 10000 + 1});
        for (std::size_t i = 1; i < out.hits.size(); ++i)
            QVERIFY(out.hits[i - 1].score >= out.hits[i].score);

        const auto folders = searchNames(index, matcher, Scope::Folders, 10, pool, {});
        QCOMPARE(folders.totalMatches, std::size_t {0});
        const auto cancelled = searchNames(index, matcher, Scope::All, 10, pool, [] { return true; });
        QVERIFY(cancelled.cancelled);
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
        };
        const auto names = [&](const QString& text, const QStringList& history = {}) {
            const ParsedQuery query = qf::parseQuery(text);
            const NameMatcher matcher(query);
            QStringList out;
            for (const AppHit& hit : searchApps(apps, query, matcher, history))
                out.append(apps[hit.index].name);
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
        QCOMPARE(m->snippet.mid(m->matchStart, m->matchLength), u"NEEDLE"_s);

        const QByteArray bom = QByteArray("\xEF\xBB\xBF") + u"中文内容：季度报告\n"_s.toUtf8();
        const auto m2 = ContentScanner(u"报告"_s).scan(reader(bom), 4096, {});
        QVERIFY(m2);
        QCOMPARE(m2->line, 1);
        QCOMPARE(m2->snippet, u"中文内容：季度报告"_s);
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
            QCOMPARE(m->snippet.mid(m->matchStart, m->matchLength), u"Gamma 测试"_s);
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
        { // timestamps wrap around
            DoubleTapDetector d;
            d.keyDown(ctrl, 0xFFFFFF00u, 0, 0);
            d.keyUp(ctrl, 0xFFFFFF40u, 0, 0);
            d.keyDown(ctrl, 0x00000010u, 0, 0);
            QVERIFY(d.keyUp(ctrl, 0x00000050u, 0, 0));
        }
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
