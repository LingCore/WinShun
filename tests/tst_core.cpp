#include "AppCatalog.h"
#include "AppLogo.h"
#include "ChangeWatcher.h"
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "DoubleTapDetector.h"
#include "FileIndex.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "PathText.h"
#include "Pinyin.h"
#include "Query.h"
#include "Release.h"
#include "Snapshot.h"
#include "SystemCatalog.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
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
        QVERIFY(!snapshot::load(file)); // the end marker is gone
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

        const NameMatcher matcher(ws::parseQuery(u"file_1"_s));
        WorkerPool pool(4);
        const auto out = searchNames(index, matcher, 10, pool, {});
        QVERIFY(!out.cancelled);
        QCOMPARE(out.hits.size(), std::size_t {10});
        QCOMPARE(index.name(out.hits.front().id), std::string_view("file_1"));
        // file_1, file_10..19, ..., file_10000..19999 (.txt), plus the exact name
        QCOMPARE(out.totalMatches, std::size_t {1 + 10 + 100 + 1000 + 10000 + 1});
        for (std::size_t i = 1; i < out.hits.size(); ++i)
            QVERIFY(out.hits[i - 1].score >= out.hits[i].score);

        const auto cancelled = searchNames(index, matcher, 10, pool, [] { return true; });
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
            // English first in the user's languages: Windows names it in English.
            app(u"Notepad"_s, u"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"_s, uR"(C:\Program Files\WindowsApps\Notepad)"_s,
                AppKind::Store),
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
        QVERIFY(!scanner.scanFile(path, 10, {})); // larger than the limit
        QVERIFY(!scanner.scanFile(path + L".missing", 0, {}));
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

        // Runs of three ASCII letters, digits or '_', whatever their case.
        for (const QString& phrase : {u"English"_s, u"ENGLISH"_s, u"glis"_s, u"end"_s, u"English 中文"_s})
            QVERIFY2(hasAll(keys, phrase), qPrintable(phrase));
        for (const QString& phrase : {u"Englishes"_s, u"end2"_s, u"lishend"_s})
            QVERIFY2(!hasAll(keys, phrase), qPrintable(phrase));
        const std::vector<grams::Key> code = gramsOf(u"auto my_var = getElementById(x2y);"_s);
        for (const QString& phrase : {u"my_var"_s, u"y_v"_s, u"getElementById(x2y)"_s, u"x2y"_s})
            QVERIFY2(hasAll(code, phrase), qPrintable(phrase));
        QVERIFY(!hasAll(code, u"automy"_s) && !hasAll(code, u"x2yz"_s));
        { // a single-byte code page: bytes above 0x7F are not ASCII
            grams::Collector collector(TextEncoding::Ansi, 1252);
            const QByteArray latin = encode(u"café_bar Zürich"_s, 1252);
            collector.feed(latin.constData(), static_cast<std::size_t>(latin.size()));
            const std::vector<grams::Key> latinKeys = collector.finish();
            QVERIFY(hasAll(latinKeys, u"caf"_s) && hasAll(latinKeys, u"_bar"_s) && hasAll(latinKeys, u"rich"_s));
            QVERIFY(!hasAll(latinKeys, u"cafe"_s) && !hasAll(latinKeys, u"zur"_s));
        }

        QCOMPARE(grams::ofPhrase(u"report 2024"_s).size(), std::size_t {6}); // rep epo por ort, 202 024
        QVERIFY(grams::ofPhrase(u"ab 12 éèê"_s).empty()); // no three word characters in a row
        QCOMPARE(grams::ofPhrase(u"季度报告"_s).size(), std::size_t {3}); // pairs only
        QCOMPARE(grams::ofPhrase(u"Qt 窗口 a 类"_s).size(), std::size_t {2}); // 窗口, 类
        QVERIFY(grams::decides(u"中"_s) && grams::decides(u"中文"_s));
        QVERIFY(!grams::decides(u"中文字"_s) && !grams::decides(u"a中"_s) && !grams::decides(u""_s));

        const ExtensionFilter filter({u".TXT"_s, u"*.md"_s, u"verylongextension"_s});
        QVERIFY(filter.matches("Notes.txt", 3) && filter.matches("README.MD", 2));
        QVERIFY(filter.matches("a.VeryLongExtension", 17));
        QVERIFY(!filter.matches("a.tx", 2) && !filter.matches("a.txt", 0) && !filter.matches("a.markdown", 8));
    }

    void contentIndex()
    {
        QTemporaryDir dir;
        ContentIndex content(dir.path());
        QVERIFY(content.add(10, gramsOf(u"季度报告"_s), false, 1, 0));
        QVERIFY(content.add(20, gramsOf(u"年度报告和计划"_s), false, 2, 0));
        QVERIFY(content.add(30, {}, true, 3, 0)); // too large: nothing to find in it
        QVERIFY(content.add(5, gramsOf(u"English only"_s), false, 4, 0));

        const auto lookup = content.lookup(u"报告"_s);
        QVERIFY(lookup.usable && lookup.decisive);
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {5, 10, 20, 30}));
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
        QCOMPARE(matchesOf(content, u"已更新"_s), (std::vector<EntryId> {9}));
        content.retire(std::vector<EntryId> {29});
        QCOMPARE(knownOf(content, u"报告"_s), (std::vector<EntryId> {9}));

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

        // Enough grams for segments of their own, and a merge of them all.
        ContentIndex big(dir.filePath(u"big"_s));
        std::vector<grams::Key> keys(1000);
        for (EntryId doc = 0; doc < 2500; ++doc) {
            for (std::size_t i = 0; i < keys.size(); ++i)
                keys[i] = ((grams::Key {0x4E00} + (doc % 7)) << grams::kCharBits) | (0x4E00 + i);
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
        std::vector<grams::Key> many;
        const auto abc = [](std::uint32_t i) { // "000", "004", ... "zz_"
            constexpr char kWord[] = "0123456789_abcdefghijklmnopqrstuvwxyz";
            const std::uint32_t t = i * 4;
            return (grams::Key {static_cast<unsigned char>(kWord[t / (37 * 37)])} << grams::kCharBits)
                | (static_cast<unsigned char>(kWord[t / 37 % 37]) << 8) | static_cast<unsigned char>(kWord[t % 37]);
        };
        for (EntryId doc = 0; doc < 1700; ++doc) {
            many.clear();
            for (std::size_t i = 0; i < 5000; ++i)
                many.push_back(((grams::Key {0x4E00} + (doc % 5)) << grams::kCharBits) | (0x4E00 + i));
            if (doc % 10 == 4) { // dense
                for (std::uint32_t i = 0; i < 10500; ++i)
                    many.push_back(abc(i));
            } else if (doc % 10 == 5) {
                many.push_back(abc(2)); // "008"
            }
            QVERIFY(tiers.add(doc, many, false, 0, 0));
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
        QCOMPARE(ContentIndexer::readFile(utf8, 4, {}).outcome, ContentIndexer::Outcome::Empty); // too large
        QCOMPARE(ContentIndexer::readFile(write(u"c.txt"_s, {}), 0, {}).outcome, ContentIndexer::Outcome::Empty);
        QCOMPARE(ContentIndexer::readFile(utf8 + L".missing", 0, {}).outcome, ContentIndexer::Outcome::Skipped);
        QVERIFY(!ContentIndexer::stampOf(utf8 + L".missing"));
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
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
