// Win顺 answering Everything's queries (core/EverythingIpc,
// platform/EverythingServer). The window is made under a class of the test's
// own: a real Everything running here is never asked, nor stood in for.
#include "EverythingIpc.h"
#include "FileIndex.h"
#include "WorkerPool.h"
#include "platform/EverythingServer.h"

#include <QDir>
#include <QFileInfo>
#include <QTest>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>

using namespace ws;
using namespace Qt::StringLiterals;
namespace ev = ws::everything;

namespace {

constexpr FileTime kDay = 24 * 60 * 60;
constexpr FileTime kT0 = 1'700'000'000; // seconds since 1970

// C:\Work\{报告 2024.docx (1000), 报告 2025.docx (3000), notes.txt (20), Archive\{old report.pdf (5000)}}
// C:\Other\report.txt (7), D:\report.md (9)
std::shared_ptr<FileIndex> sampleIndex()
{
    auto index = std::make_shared<FileIndex>();
    const auto file = [&](EntryId parent, const char* name, std::uint64_t size, FileTime modified) {
        const EntryId id = index->add(parent, name, 0);
        index->setSize(id, packSize(size));
        index->setModified(id, modified);
        index->setCreated(id, modified - kDay);
        return id;
    };
    const EntryId c = index->addRoot("C:");
    const EntryId work = index->add(c, "Work", EntryFlag::Directory);
    file(work, "报告 2024.docx", 1000, kT0);
    file(work, "报告 2025.docx", 3000, kT0 + 2 * kDay);
    file(work, "notes.txt", 20, kT0 + kDay);
    const EntryId archive = index->add(work, "Archive", EntryFlag::Directory);
    file(archive, "old report.pdf", 5000, kT0 - kDay);
    const EntryId other = index->add(c, "Other", EntryFlag::Directory);
    file(other, "report.txt", 7, kT0);
    const EntryId d = index->addRoot("D:");
    file(d, "report.md", 9, kT0 + 3 * kDay);
    return index;
}

std::uint32_t u32(const QByteArray& bytes, qsizetype at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.constData() + at, sizeof value);
    return value;
}
std::uint64_t u64(const QByteArray& bytes, qsizetype at)
{
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.constData() + at, sizeof value);
    return value;
}
QString wideAt(const QByteArray& bytes, qsizetype at)
{
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData() + at));
}

QStringList paths(const ev::Rows& rows)
{
    QStringList out;
    for (const ev::Row& row : rows.rows)
        out.append(QString::fromStdWString(row.path.empty() ? row.name : row.path + L'\\' + row.name));
    return out;
}

ev::Query ask(const QString& search, std::uint32_t sortType = ev::sort::Name)
{
    ev::Query query;
    query.version = 2;
    query.search = search;
    query.sortType = sortType;
    query.requestFlags = ev::request::Name | ev::request::Path | ev::request::Size | ev::request::Modified;
    return query;
}

// A query's bytes as a program sends them.
QByteArray queryBytes(const ev::Query& query)
{
    QByteArray bytes;
    const auto put = [&](std::uint32_t value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof value); };
    for (const std::uint32_t value : {query.replyWindow, query.replyMessage, query.searchFlags, query.offset, query.maxResults})
        put(value);
    if (query.version == 2) {
        put(query.requestFlags);
        put(query.sortType);
    }
    if (query.wide) {
        bytes.append(reinterpret_cast<const char*>(query.search.utf16()), (query.search.size() + 1) * 2);
    } else {
        bytes.append(query.search.toLocal8Bit());
        bytes.append('\0');
    }
    return bytes;
}

// A program asking: its window takes the answer.
struct Client {
    HWND window = nullptr;
    QByteArray answer;
    std::uint32_t answerKind = 0;
    HWND from = nullptr;
    int answers = 0;

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_COPYDATA) {
            auto* self = reinterpret_cast<Client*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            self->answer = QByteArray(static_cast<const char*>(data->lpData), data->cbData);
            self->answerKind = static_cast<std::uint32_t>(data->dwData);
            self->from = reinterpret_cast<HWND>(wParam);
            ++self->answers;
            return TRUE;
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    Client()
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = &Client::proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinShunEverythingTestClient";
        ::RegisterClassW(&wc);
        window = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    ~Client() { ::DestroyWindow(window); }
    bool send(HWND server, ev::Query query, std::uint32_t kind)
    {
        query.replyWindow = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(window));
        query.replyMessage = 0x5753;
        QByteArray bytes = queryBytes(query);
        COPYDATASTRUCT data {};
        data.dwData = kind;
        data.cbData = static_cast<DWORD>(bytes.size());
        data.lpData = bytes.data();
        return ::SendMessageW(server, WM_COPYDATA, reinterpret_cast<WPARAM>(window), reinterpret_cast<LPARAM>(&data)) != 0;
    }
};

} // namespace

class EverythingTest : public QObject {
    Q_OBJECT

private slots:
    void translates_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("query");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QStringList>("folders");
        QTest::addColumn<QString>("directIn");
        QTest::addColumn<QString>("unsupported");
        const auto row = [](const char* name, const QString& text, const QString& query, ev::Kind kind = ev::Kind::Any,
                             const QStringList& folders = {}, const QString& directIn = {}, const QString& unsupported = {}) {
            QTest::newRow(name) << text << query << static_cast<int>(kind) << folders << directIn << unsupported;
        };
        row("words", u"年度 报告"_s, u"年度 报告"_s);
        row("quoted", u"\"annual report\" !draft"_s, u"\"annual report\" !draft"_s);
        row("bar", u"a|b c"_s, u"a | b c"_s);
        row("bar-quoted", u"\"a|b\""_s, u"\"a|b\""_s);
        row("wildcard", u"*.pdf"_s, u"*.pdf"_s);
        // What Directory Opus and Total Commander put in front: the folder to look in.
        row("folder-quoted", u"\"C:\\My Work\\\" 报告"_s, u"报告"_s, ev::Kind::Any, {u"C:\\My Work"_s});
        row("folder-bare", u"D:\\Data\\ *.txt"_s, u"*.txt"_s, ev::Kind::Any, {u"D:\\Data"_s});
        row("folder-drive", u"C:\\ abc"_s, u"abc"_s, ev::Kind::Any, {u"C:"_s});
        row("folder-share", u"\"\\\\nas\\docs\\\" abc"_s, u"abc"_s, ev::Kind::Any, {u"\\\\nas\\docs"_s});
        row("folder-path-modifier", u"path:\"C:\\Work\\\" abc"_s, u"abc"_s, ev::Kind::Any, {u"C:\\Work"_s});
        row("folder-alone", u"\"C:\\Work\\\""_s, QString(), ev::Kind::Any, {u"C:\\Work"_s});
        row("path-not-folder", u"C:\\Work\\rep"_s, u"C:\\Work\\rep"_s);
        row("not-a-folder", u"!\"C:\\Temp\\\" abc"_s, u"!\"C:\\Temp\\\" abc"_s);
        row("parent", u"parent:\"C:\\Work\" abc"_s, u"abc"_s, ev::Kind::Any, {}, u"C:\\Work"_s);
        row("infolder-slash", u"infolder:C:\\Work\\"_s, QString(), ev::Kind::Any, {}, u"C:\\Work"_s);
        row("files", u"file: abc"_s, u"abc"_s, ev::Kind::Files);
        row("folders-word", u"folder:abc"_s, u"abc"_s, ev::Kind::Folders);
        row("modifiers", u"case:ABC ww:def nopath:file:ghi"_s, u"ABC def ghi"_s, ev::Kind::Files);
        row("attrib-folders", u"attrib:D abc"_s, u"abc"_s, ev::Kind::Folders);
        row("attrib-files", u"!attrib:d abc"_s, u"abc"_s, ev::Kind::Files);
        row("ext", u"ext:jpg;png abc"_s, u"ext:jpg,png abc"_s);
        row("own-functions", u"size:>1mb dm:today regex:^a datemodified:2026"_s, u"size:>1mb dm:today regex:^a dm:2026"_s);
        row("drive-word", u"C: abc"_s, u"C: abc"_s);
        row("unsupported", u"abc attrib:H"_s, u"abc"_s, ev::Kind::Any, {}, {}, u"attrib:"_s);
        row("content", u"content:\"hello world\""_s, QString(), ev::Kind::Any, {}, {}, u"content:"_s);
        row("groups", u"<a b> | c"_s, u"| c"_s, ev::Kind::Any, {}, {}, u"<>"_s);
        // Directory Opus: the folders in a group, then the words.
        row("opus-folder", u"<\"C:\\My Work\\\"> 报告"_s, u"报告"_s, ev::Kind::Any, {u"C:\\My Work"_s});
        row("opus-folders", u"<\"C:\\a\\\" | \"D:\\\"> x y"_s, u"x y"_s, ev::Kind::Any, {u"C:\\a"_s, u"D:"_s});
        row("group-open", u"<\"C:\\a\\\" x"_s, QString(), ev::Kind::Any, {}, {}, u"<>"_s);
        row("root-word", u"root: abc"_s, u"abc"_s, ev::Kind::Any, {}, {}, u"root:"_s);
        row("size-not-group", u"size:<1kb"_s, u"size:<1kb"_s);
        row("empty", QString(), QString());
    }
    void translates()
    {
        QFETCH(QString, text);
        const ev::Search search = ev::translate(text);
        QTEST(search.query, "query");
        QTEST(static_cast<int>(search.kind), "kind");
        QTEST(search.folders, "folders");
        QTEST(search.directIn, "directIn");
        QTEST(search.unsupported, "unsupported");
        QVERIFY(!search.regex);
        QVERIFY(!search.roots || !search.unsupported.isEmpty());
    }
    void translatesRegexAndCount()
    {
        const ev::Search regex = ev::translate(u"^IMG_\\d+ a|b"_s, ev::searchflag::Regex);
        QVERIFY(regex.regex);
        QCOMPARE(regex.query, u"^IMG_\\d+ a|b"_s);
        QVERIFY(!ev::translate(QString(), ev::searchflag::Regex).regex);
        const ev::Search counted = ev::translate(u"abc count:5"_s);
        QCOMPARE(counted.query, u"abc"_s);
        QCOMPARE(counted.count, std::optional<std::uint32_t>(5));
    }

    void readsQueries()
    {
        ev::Query sent = ask(u"\"C:\\Work\\\" 报告"_s, ev::sort::Modified + 1);
        sent.replyWindow = 0x1234;
        sent.replyMessage = 7;
        sent.searchFlags = 4;
        sent.offset = 3;
        sent.maxResults = 50;
        const QByteArray bytes = queryBytes(sent);
        const auto span = [](const QByteArray& b) { return std::span(reinterpret_cast<const std::byte*>(b.constData()), static_cast<std::size_t>(b.size())); };
        const auto read = ev::readQuery(ev::copydata::Query2W, span(bytes));
        QVERIFY(read);
        QCOMPARE(read->version, 2);
        QCOMPARE(read->search, sent.search);
        QCOMPARE(read->replyWindow, 0x1234u);
        QCOMPARE(read->replyMessage, 7u);
        QCOMPARE(read->offset, 3u);
        QCOMPARE(read->maxResults, 50u);
        QCOMPARE(read->requestFlags, sent.requestFlags);
        QCOMPARE(read->sortType, ev::sort::Modified + 1);

        ev::Query old;
        old.search = u"report"_s;
        old.wide = false;
        const QByteArray oldBytes = queryBytes(old);
        const auto readOld = ev::readQuery(ev::copydata::QueryA, span(oldBytes));
        QVERIFY(readOld);
        QCOMPARE(readOld->version, 1);
        QVERIFY(!readOld->wide);
        QCOMPARE(readOld->search, u"report"_s);
        QCOMPARE(readOld->requestFlags, ev::request::Name | ev::request::Path);

        QVERIFY(!ev::readQuery(0, span(bytes))); // a command line for Everything
        QVERIFY(!ev::readQuery(ev::copydata::Query2W, span(bytes.first(10))));
        // No zero at the end: read to the end, not past it.
        const auto cut = ev::readQuery(ev::copydata::Query2W, span(bytes.first(bytes.size() - 2)));
        QVERIFY(cut);
        QCOMPARE(cut->search, sent.search);
    }

    void finds()
    {
        const auto index = sampleIndex();
        WorkerPool pool(2);
        const auto found = [&](const ev::Query& query) { return ev::find(*index, pool, query); };

        ev::Rows rows = found(ask(u"report"_s));
        QCOMPARE(paths(rows), (QStringList {u"C:\\Work\\Archive\\old report.pdf"_s, u"D:\\report.md"_s, u"C:\\Other\\report.txt"_s}));
        QCOMPARE(rows.totalItems, 3u);
        QCOMPARE(rows.totalFiles, 3u);
        QCOMPARE(rows.totalFolders, 0u);
        QCOMPARE(rows.rows[0].size, 5000u);
        QCOMPARE(rows.rows[0].modified, (std::uint64_t {kT0 - kDay} + 11'644'473'600ull) * 10'000'000ull);
        QVERIFY(!rows.rows[0].folder);

        // In a folder, as a file manager asks: at any depth; by size, largest first.
        rows = found(ask(u"\"C:\\Work\\\" 报告 | report"_s, ev::sort::Size + 1));
        QCOMPARE(paths(rows), (QStringList {u"C:\\Work\\Archive\\old report.pdf"_s, u"C:\\Work\\报告 2025.docx"_s, u"C:\\Work\\报告 2024.docx"_s}));
        // Only what is right in it.
        rows = found(ask(u"parent:C:\\Work"_s));
        QCOMPARE(paths(rows), (QStringList {u"C:\\Work\\Archive"_s, u"C:\\Work\\notes.txt"_s, u"C:\\Work\\报告 2024.docx"_s, u"C:\\Work\\报告 2025.docx"_s}));
        QVERIFY(rows.rows[0].folder);
        QCOMPARE(rows.rows[0].size, 5000u); // a folder's: what the index has in it
        QCOMPARE(rows.totalFolders, 1u);
        QCOMPARE(rows.totalFiles, 3u);
        rows = found(ask(u"parent:C:\\Work file:"_s));
        QCOMPARE(rows.rows.size(), 3u);
        QCOMPARE(rows.totalItems, 3u);
        rows = found(ask(u"folder: \"C:\\Work\\\""_s));
        QCOMPARE(paths(rows), QStringList {u"C:\\Work\\Archive"_s});
        // Everything in a folder, newest first; then a window of it.
        ev::Query all = ask(u"C:\\Work\\ file:"_s, ev::sort::Modified + 1);
        rows = found(all);
        QCOMPARE(paths(rows), (QStringList {u"C:\\Work\\报告 2025.docx"_s, u"C:\\Work\\notes.txt"_s, u"C:\\Work\\报告 2024.docx"_s, u"C:\\Work\\Archive\\old report.pdf"_s}));
        all.offset = 1;
        all.maxResults = 2;
        rows = found(all);
        QCOMPARE(paths(rows), (QStringList {u"C:\\Work\\notes.txt"_s, u"C:\\Work\\报告 2024.docx"_s}));
        QCOMPARE(rows.totalItems, 4u);
        all.offset = 9;
        QCOMPARE(found(all).rows.size(), 0u);
        // Nothing asked: all there is (the drives themselves are no rows of Win顺's).
        rows = found(ask(QString()));
        QCOMPARE(rows.totalItems, 9u);
        QCOMPARE(rows.rows.front().path, std::wstring(L"C:\\Work"));
        QCOMPARE(rows.rows.front().name, std::wstring(L"Archive"));
        rows = found(ask(u"ext:txt;md"_s));
        QCOMPARE(rows.totalItems, 3u);
        rows = found(ask(u"report count:2"_s));
        QCOMPARE(rows.rows.size(), 2u);
        QCOMPARE(rows.totalItems, 2u);
        // Not in the index, not Win顺's to do, cancelled: nothing rather than something else.
        QCOMPARE(found(ask(u"\"C:\\Nowhere\\\" report"_s)).totalItems, 0u);
        QCOMPARE(found(ask(u"report attrib:H"_s)).totalItems, 0u);
        QCOMPARE(found(ask(u"parent:C:\\Nowhere"_s)).totalItems, 0u);
        QCOMPARE(ev::find(*index, pool, ask(u"report"_s), [] { return true; }).totalItems, 0u);
        // The drives, as Directory Opus asks before anything else; then its search.
        QVERIFY(ev::translate(u"root:"_s).roots);
        rows = found(ask(u"root:"_s));
        QCOMPARE(paths(rows), (QStringList {u"C:"_s, u"D:"_s}));
        QVERIFY(rows.rows[0].root && rows.rows[0].folder && rows.rows[0].path.empty());
        QCOMPARE(rows.totalItems, 2u);
        rows = found(ask(u"<\"C:\\Work\\\" | \"D:\\\"> report"_s));
        QCOMPARE(paths(rows), (QStringList {u"C:\\Work\\Archive\\old report.pdf"_s, u"D:\\report.md"_s}));
        ev::Query regex = ask(u"^report\\.(md|txt)$"_s);
        regex.searchFlags = ev::searchflag::Regex;
        QCOMPARE(found(regex).totalItems, 2u);
    }

    void encodes()
    {
        const auto index = sampleIndex();
        WorkerPool pool(1);
        ev::Query query = ask(u"parent:C:\\Work"_s);
        query.requestFlags = ev::request::Name | ev::request::Path | ev::request::FullPath | ev::request::Extension
            | ev::request::Size | ev::request::Modified | ev::request::Attributes;
        query.offset = 1;
        const ev::Rows rows = ev::find(*index, pool, query);
        QCOMPARE(rows.rows.size(), 3u);
        const QByteArray list = ev::encode(query, rows);
        QCOMPARE(u32(list, 0), 4u); // totitems
        QCOMPARE(u32(list, 4), 3u); // numitems
        QCOMPARE(u32(list, 8), 1u); // offset
        QCOMPARE(u32(list, 12), query.requestFlags);
        QCOMPARE(u32(list, 16), ev::sort::Name);
        // The first item (notes.txt), read as the SDK reads it.
        QCOMPARE(u32(list, 20), 0u); // a file
        qsizetype at = u32(list, 24);
        const auto text = [&] {
            const std::uint32_t length = u32(list, at);
            const QString value = wideAt(list, at + 4);
            at += 4 + (length + 1) * 2;
            return value.size() == static_cast<qsizetype>(length) ? value : u"<length>"_s;
        };
        QCOMPARE(text(), u"notes.txt"_s);
        QCOMPARE(text(), u"C:\\Work"_s);
        QCOMPARE(text(), u"C:\\Work\\notes.txt"_s);
        QCOMPARE(text(), u"txt"_s);
        QCOMPARE(u64(list, at), 20u);
        QCOMPARE(u64(list, at + 8), (std::uint64_t {kT0 + kDay} + 11'644'473'600ull) * 10'000'000ull);
        QCOMPARE(u32(list, at + 16), std::uint32_t {FILE_ATTRIBUTE_NORMAL});
        QCOMPARE(at + 20, static_cast<qsizetype>(u32(list, 20 + 8 + 4))); // the next item's data follows

        // The old list, in the system's code page too.
        ev::Query old;
        old.search = u"parent:C:\\Work"_s;
        const ev::Rows oldRows = ev::find(*index, pool, old);
        QByteArray bytes = ev::encode(old, oldRows);
        QCOMPARE(u32(bytes, 0), 1u); // totfolders
        QCOMPARE(u32(bytes, 4), 3u); // totfiles
        QCOMPARE(u32(bytes, 8), 4u); // totitems
        QCOMPARE(u32(bytes, 12), 1u); // numfolders
        QCOMPARE(u32(bytes, 16), 3u);
        QCOMPARE(u32(bytes, 20), 4u);
        QCOMPARE(u32(bytes, 28), 1u); // Archive: a folder
        QCOMPARE(wideAt(bytes, u32(bytes, 28 + 4)), u"Archive"_s);
        QCOMPARE(wideAt(bytes, u32(bytes, 28 + 8)), u"C:\\Work"_s);
        QCOMPARE(wideAt(bytes, u32(bytes, 28 + 3 * 12 + 4)), u"报告 2025.docx"_s);
        old.wide = false;
        bytes = ev::encode(old, oldRows);
        QCOMPARE(QByteArray(bytes.constData() + u32(bytes, 28 + 12 + 4)), QByteArray("notes.txt"));
        QCOMPARE(QString::fromLocal8Bit(bytes.constData() + u32(bytes, 28 + 3 * 12 + 4)), u"报告 2025.docx"_s);
    }

    void commands()
    {
        QCOMPARE(ev::answerCommand(ev::command::MajorVersion, 0, true), 1u);
        QCOMPARE(ev::answerCommand(ev::command::MinorVersion, 0, true), 4u);
        QCOMPARE(ev::answerCommand(ev::command::IsDbLoaded, 0, true), 1u);
        QCOMPARE(ev::answerCommand(ev::command::IsDbLoaded, 0, false), 0u);
        QCOMPARE(ev::answerCommand(ev::command::IsFastSort, ev::sort::Size + 1, true), 1u);
        QCOMPARE(ev::answerCommand(ev::command::IsFastSort, 19, true), 0u); // run count
        QCOMPARE(ev::answerCommand(ev::command::IsFileInfoIndexed, ev::fileinfo::FolderSize, true), 1u);
        QCOMPARE(ev::answerCommand(ev::command::IsFileInfoIndexed, ev::fileinfo::Attributes, true), 1u);
        QCOMPARE(ev::answerCommand(ev::command::IsFileInfoIndexed, 5, true), 0u); // date accessed
        QCOMPARE(ev::answerCommand(4, 0, true), 0u); // exit: Win顺 is not closed by it
        QCOMPARE(ev::answerCommand(405, 0, true), 0u); // rebuild
    }

    // The window, asked as a program asks Everything's.
    void answersOverTheWindow()
    {
        const auto index = sampleIndex();
        const std::wstring windowClass = L"WinShunEverythingTest" + std::to_wstring(::GetCurrentProcessId());
        auto server = std::make_unique<EverythingServer>([index] { return index; }, windowClass);
        QTRY_VERIFY(server->serving());
        const HWND window = ::FindWindowW(windowClass.c_str(), nullptr); // as programs find it
        QCOMPARE(window, server->window());
        QVERIFY(!::IsWindowVisible(window));
        QCOMPARE(::SendMessageW(window, WM_USER, ev::command::MajorVersion, 0), LRESULT {1});
        QCOMPARE(::SendMessageW(window, WM_USER, ev::command::IsDbLoaded, 0), LRESULT {1});
        QCOMPARE(::SendMessageW(window, WM_USER, 4, 0), LRESULT {0});
        QVERIFY(::IsWindow(window));

        Client client;
        QVERIFY(client.send(window, ask(u"\"C:\\Work\\\" 报告"_s), ev::copydata::Query2W));
        QTRY_COMPARE(client.answers, 1);
        QCOMPARE(client.answerKind, 0x5753u);
        QCOMPARE(client.from, window);
        QCOMPARE(u32(client.answer, 0), 2u);
        QCOMPARE(u32(client.answer, 4), 2u);
        QCOMPARE(wideAt(client.answer, u32(client.answer, 24) + 4), u"报告 2024.docx"_s);

        ev::Query old;
        old.search = u"report"_s;
        QVERIFY(client.send(window, old, ev::copydata::QueryW));
        QTRY_COMPARE(client.answers, 2);
        QCOMPARE(u32(client.answer, 8), 3u);

        // Asked three times in a row, as typing does: the last is answered
        // (the ones before it may be, when they were quick).
        QVERIFY(client.send(window, ask(u"notes"_s), ev::copydata::Query2W));
        QVERIFY(client.send(window, ask(u"report"_s), ev::copydata::Query2W));
        QVERIFY(client.send(window, ask(u"ext:docx"_s), ev::copydata::Query2W));
        QTRY_VERIFY(client.answers >= 3 && u32(client.answer, 0) == 2);
        QTest::qWait(200);
        QCOMPARE(u32(client.answer, 0), 2u);

        // Not a query, or nobody to answer: not taken.
        COPYDATASTRUCT other {};
        other.dwData = 0;
        char line[] = "-search abc";
        other.cbData = sizeof line;
        other.lpData = line;
        QCOMPARE(::SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&other)), LRESULT {0});

        // A second one beside it (Everything itself running, to Win顺): none made.
        EverythingServer second([index] { return index; }, windowClass);
        QTest::qWait(300);
        QVERIFY(!second.serving());

        server.reset();
        QVERIFY(!::FindWindowW(windowClass.c_str(), nullptr));
    }

    // By hand, with real programs (es.exe, the Everything SDK, Directory
    // Opus): the files of WINSHUN_EVERYTHING_LIVE_ROOT under Everything's own
    // class for WINSHUN_EVERYTHING_LIVE_SECONDS. Not while Everything runs.
    void live()
    {
        const QString root = QDir::toNativeSeparators(qEnvironmentVariable("WINSHUN_EVERYTHING_LIVE_ROOT"));
        if (root.isEmpty())
            QSKIP("WINSHUN_EVERYTHING_LIVE_ROOT is not set");
        auto index = std::make_shared<FileIndex>();
        const QStringList parts = root.split(u'\\', Qt::SkipEmptyParts);
        EntryId folder = index->addRoot(parts.first().toStdString());
        for (const QString& part : parts.sliced(1))
            folder = index->add(folder, part.toUtf8().toStdString(), EntryFlag::Directory);
        std::size_t count = 0;
        const std::function<void(const QString&, EntryId)> walk = [&](const QString& path, EntryId parent) {
            for (const QFileInfo& info : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                const EntryId id = index->add(parent, info.fileName().toUtf8().toStdString(), info.isDir() ? EntryFlag::Directory : 0);
                index->setModified(id, static_cast<FileTime>(info.lastModified().toSecsSinceEpoch()));
                index->setCreated(id, static_cast<FileTime>(info.birthTime().toSecsSinceEpoch()));
                ++count;
                if (info.isDir() && !info.isSymLink())
                    walk(info.filePath(), id);
                else
                    index->setSize(id, packSize(static_cast<std::uint64_t>(info.size())));
            }
        };
        walk(root, folder);
        EverythingServer server([index] { return index; }, ev::kWindowClass, [](const ev::Query& query, const ev::Rows& rows) {
            const ev::Search search = ev::translate(query.search, query.searchFlags);
            std::fprintf(stderr,
                "v%d flags %x request %x sort %u offset %u max %u\n  asked:  %s\n  search: %s | folders %s | in %s | "
                "unsupported %s\n  rows %zu of %u\n",
                query.version, query.searchFlags, query.requestFlags, query.sortType, query.offset, query.maxResults,
                query.search.toUtf8().constData(), search.query.toUtf8().constData(),
                search.folders.join(u';').toUtf8().constData(), search.directIn.toUtf8().constData(),
                search.unsupported.toUtf8().constData(), rows.rows.size(), rows.totalItems);
            std::fflush(stderr);
        });
        QTRY_VERIFY2(server.serving(), "Everything itself is running");
        qInfo() << "answering for" << count << "entries under" << root;
        QTest::qWait(qEnvironmentVariableIntValue("WINSHUN_EVERYTHING_LIVE_SECONDS") * 1000);
    }
};

QTEST_GUILESS_MAIN(EverythingTest)
#include "tst_everything.moc"
