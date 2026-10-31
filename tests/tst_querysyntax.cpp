#include "NameSearch.h"
#include "Query.h"
#include "AppCatalog.h"

#include <QTest>
#include <algorithm>

using namespace ws;
using namespace Qt::StringLiterals;

class QuerySyntaxTest : public QObject {
    Q_OBJECT

private slots:
    void matching_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("expected");
        QTest::newRow("and-before-or") << u"report | summary 2024"_s << u"summary 2023.pdf"_s << false;
        QTest::newRow("or-first") << u"report | summary 2024"_s << u"report 2023.pdf"_s << true;
        QTest::newRow("or-second") << u"report | summary 2024"_s << u"summary 2024.pdf"_s << true;
        QTest::newRow("quoted-pipe") << u"\"a | b\""_s << u"a | b.txt"_s << true;
        QTest::newRow("extension-pipe") << u"ext:png|mp4"_s << u"clip.mp4"_s << true;
        QTest::newRow("prefix") << u"^IMG"_s << u"IMG_123.png"_s << true;
        QTest::newRow("prefix-no") << u"^IMG"_s << u"MY_IMG.png"_s << false;
        QTest::newRow("suffix") << u".pdf$"_s << u"report.pdf.bak"_s << false;
        QTest::newRow("whole-name") << u"^readme.md$"_s << u"README.md"_s << true;
        QTest::newRow("no-pinyin-anchor") << u"^baogao"_s << u"报告.md"_s << false;
        QTest::newRow("regex") << u"regex:^IMG_\\d+\\.(jpg|png)$"_s << u"img_012.PNG"_s << true;
        QTest::newRow("re-alias") << u"re:^IMG_\\d+\\.(jpg|png)$"_s << u"img_012.PNG"_s << true;
        QTest::newRow("re-uppercase") << u"RE:^IMG_\\d+\\.(jpg|png)$"_s << u"IMG_a.png"_s << false;
        QTest::newRow("re-negative") << u"*.png !re:^temp"_s << u"temp123.png"_s << false;
        QTest::newRow("re-space") << u"re:\"^annual report\\.pdf$\""_s << u"Annual Report.pdf"_s << true;
        QTest::newRow("quoted-re-is-literal") << u"\"re:[\""_s << u"re:[.txt"_s << true;
        QTest::newRow("regex-not-number") << u"regex:^IMG_\\d+\\.(jpg|png)$"_s << u"IMG_a.png"_s << false;
        QTest::newRow("regex-case-escape") << u"regex:^\\D+$"_s << u"abc.png"_s << true;
        QTest::newRow("regex-negative") << u"*.png !regex:^temp"_s << u"temp123.png"_s << false;
        QTest::newRow("regex-space") << u"regex:\"^annual report\\.pdf$\""_s << u"Annual Report.pdf"_s << true;
        QTest::newRow("quoted-regex-is-literal") << u"\"regex:[\""_s << u"regex:[.txt"_s << true;
        QTest::newRow("empty-quote-before-regex") << u"\"\" regex:^\\D+$"_s << u"abc.png"_s << true;
        QTest::newRow("empty-anchored") << u"^$"_s << u"file.txt"_s << false;
        QTest::newRow("old-pinyin") << u"bg !draft"_s << u"报告.txt"_s << true;
        QTest::newRow("program-initials") << u"hsm"_s << u"HiBit Startup Manager.exe"_s << true;
        QTest::newRow("shortcut-initials") << u"hsm"_s << u"HiBit Startup Manager.lnk"_s << true;
        QTest::newRow("camel-initials") << u"vsc"_s << u"VisualStudioCode.exe"_s << true;
        QTest::newRow("initials-in-order") << u"hms"_s << u"HiBit Startup Manager.exe"_s << false;
        QTest::newRow("initials-not-subsequence") << u"hsm"_s << u"Highstream.exe"_s << false;
        QTest::newRow("initials-not-document") << u"hsm"_s << u"HiBit Startup Manager.txt"_s << false;
        QTest::newRow("initials-filter") << u"hsm ext:exe"_s << u"HiBit Startup Manager.exe"_s << true;
        QTest::newRow("initials-regex-literal") << u"re:^hsm"_s << u"HiBit Startup Manager.exe"_s << false;
        QTest::newRow("empty-regex-match") << u"regex:.*"_s << u"anything.txt"_s << true;
    }

    void matching()
    {
        QFETCH(QString, query);
        QFETCH(QString, name);
        QFETCH(bool, expected);
        const auto parsed = parseQuery(query);
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        QCOMPARE(NameMatcher(parsed).matchPath(u"D:\\work\\"_s + name, false) >= 0, expected);
    }

    // The search box's regex mode (Ctrl+R): every term a regex.
    void regexMode_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("expected");
        QTest::newRow("whole") << u"^IMG_\\d{4}\\.(jpg|png)$"_s << u"img_2024.PNG"_s << true;
        QTest::newRow("whole-no") << u"^IMG_\\d{4}\\.(jpg|png)$"_s << u"IMG_24.png"_s << false;
        QTest::newRow("two-terms") << u"^IMG \\d{3}"_s << u"IMG_123.png"_s << true;
        QTest::newRow("two-terms-no") << u"^IMG \\d{3}"_s << u"IMG_12.png"_s << false;
        QTest::newRow("case") << u"(?-i)^IMG"_s << u"img_1.png"_s << false;
        QTest::newRow("case-yes") << u"(?-i)^IMG"_s << u"IMG_1.png"_s << true;
        QTest::newRow("ext") << u"^a\\d ext:txt"_s << u"a1.txt"_s << true;
        QTest::newRow("ext-no") << u"^a\\d ext:txt"_s << u"a1.md"_s << false;
        QTest::newRow("negated") << u"\\.png$ !^temp"_s << u"temp1.png"_s << false;
        QTest::newRow("or") << u"^a\\d$ | ^b"_s << u"b.txt"_s << true;
        QTest::newRow("quoted-space") << u"\"^annual report\""_s << u"Annual Report.pdf"_s << true;
        QTest::newRow("no-pinyin") << u"bg"_s << u"报告.txt"_s << false;
        QTest::newRow("backslash-not-path") << u"\\d+\\.log$"_s << u"run12.log"_s << true;
    }

    void regexMode()
    {
        QFETCH(QString, query);
        QFETCH(QString, name);
        QFETCH(bool, expected);
        const auto parsed = parseQuery(query, true);
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        QCOMPARE(NameMatcher(parsed).matchPath(u"D:\\work\\"_s + name, false) >= 0, expected);
        QVERIFY(parseQuery(u"a["_s, true).error.size() > 0);
    }

    void regexModeInIndex()
    {
        FileIndex index;
        const auto root = index.addRoot("T:");
        const auto upper = index.add(root, "IMG_1.png", 0);
        index.add(root, "img_2.png", 0);
        WorkerPool pool(2);
        const auto lock = index.readLock();
        const auto output = searchNames(index, NameMatcher(parseQuery(u"(?-i)^IMG_\\d"_s, true)), {.limit = 20}, pool, {});
        QCOMPARE(output.totalMatches, std::size_t(1));
        QCOMPARE(output.hits.front().id, upper);
    }

    void programInitialsInIndexAndApps()
    {
        FileIndex index;
        const auto root = index.addRoot("T:");
        const auto program = index.add(root, "HiBit Startup Manager.exe", 0);
        index.add(root, "HiBit Startup Manager.txt", 0);
        index.add(root, "Highstream.exe", 0);
        WorkerPool pool(2);
        const auto query = parseQuery(u"hsm"_s);
        {
            const auto lock = index.readLock();
            const auto output = searchNames(index, NameMatcher(query), {.limit = 20}, pool, {});
            QCOMPARE(output.totalMatches, std::size_t(1));
            QCOMPARE(output.hits.front().id, program);
        }
        AppInfo app;
        app.name = u"HiBit Startup Manager"_s;
        app.id = u"test.hibit"_s;
        app.target = u"T:\\HiBit Startup Manager.exe"_s;
        app.prepare();
        const auto found = searchApps({app}, query, NameMatcher(query), {});
        QCOMPARE(found.size(), std::size_t(1));
    }

    void invalidQueries()
    {
        for (const auto& text : {u"regex:["_s, u"regex:"_s, u"re:["_s, u"re:"_s, u"report |"_s, u"| report"_s, u"a | | b"_s, u"\"\" regex:["_s}) {
            const auto query = parseQuery(text);
            QVERIFY2(!query.error.isEmpty(), qPrintable(text));
            QVERIFY(!query.isEmpty());
            QVERIFY(NameMatcher(query).matchPath(u"D:\\report.txt"_s, false) < 0);
        }
    }

    void branchMetadataAndFolderCaches()
    {
        FileIndex index;
        const auto root = index.addRoot("T:");
        const auto docs = index.add(root, "docs", EntryFlag::Directory);
        const auto images = index.add(root, "images", EntryFlag::Directory);
        const auto large = index.add(docs, "report.pdf", 0);
        const auto small = index.add(docs, "tiny.pdf", 0);
        const auto photo = index.add(images, "report.png", 0);
        index.setSize(large, packSize(2 * 1024 * 1024));
        index.setSize(small, packSize(1024));
        const auto query = parseQuery(u"ext:pdf size:>1mb | images report"_s);
        QVERIFY(query.hasFilters());
        WorkerPool pool(2);
        const auto lock = index.readLock();
        const auto output = searchNames(index, NameMatcher(query), {.limit = 20}, pool, {});
        QCOMPARE(output.totalMatches, std::size_t(2));
        QVERIFY(std::ranges::any_of(output.hits, [&](const auto& hit) { return hit.id == large; }));
        QVERIFY(std::ranges::any_of(output.hits, [&](const auto& hit) { return hit.id == photo; }));
        QVERIFY(std::ranges::none_of(output.hits, [&](const auto& hit) { return hit.id == small; }));
    }

    void typeKeywordPosition_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("words");
        QTest::addColumn<int>("type");
        QTest::newRow("leading") << u":folder 报告"_s << u"报告"_s << -2;
        QTest::newRow("suffix") << u"down :folder"_s << u"down"_s << -2;
        QTest::newRow("trailing-colon") << u"folder: 报告"_s << u"报告"_s << -2;
        QTest::newRow("trailing-both") << u"down folder:"_s << u"down"_s << -2;
        QTest::newRow("middle") << u"annual :pic report"_s << u"annual report"_s << 0;
        QTest::newRow("case") << u"report PIC:"_s << u"report"_s << 0;
        QTest::newRow("quoted") << u"\":folder\""_s << u"\":folder\""_s << -1;
        QTest::newRow("unclosed-quote") << u"\"report :folder"_s << u"\"report :folder"_s << -1;
        QTest::newRow("quoted-words") << u"\"annual report\" :folder"_s << u"\"annual report\""_s << -2;
        QTest::newRow("unknown") << u"report :unknown"_s << u"report :unknown"_s << -1;
        QTest::newRow("drive") << u"C:"_s << u"C:"_s << -1;
        QTest::newRow("path") << u"C:\\work\\folder:"_s << u"C:\\work\\folder:"_s << -1;
        QTest::newRow("empty") << QString() << QString() << -1;
    }

    void typeKeywordPosition()
    {
        QFETCH(QString, query);
        QFETCH(QString, words);
        QFETCH(int, type);
        const FileTypes types {{u"图片"_s, {u"png"_s}, u"pictures"_s, u"pic"_s},
            {u"视频"_s, {u"mp4"_s}, u"videos"_s, u"video"_s}};
        const auto result = splitFileTypeQuery(query, types, u"folder");
        QCOMPARE(result.words, words);
        QCOMPARE(result.type.value_or(-1), type);
    }

    void multipleTypePrefixes()
    {
        const FileTypes types {{u"图片"_s, {u"png"_s}, u"pictures"_s, u"pic"_s},
            {u"视频"_s, {u"mp4"_s}, u"videos"_s, u"video"_s}};
        const auto query = splitFileTypeQuery(u":pic,video 测试"_s, types, u"folder");
        QCOMPARE(query.words, u"测试"_s);
        QCOMPARE(query.types, (QList<int>{0, 1}));
        const auto suffix = splitFileTypeQuery(u"测试 pic,video:"_s, types, u"folder");
        QCOMPARE(suffix.words, u"测试"_s);
        QCOMPARE(suffix.types, (QList<int>{0, 1}));
        QVERIFY(!splitFileTypeQuery(u":pic,unknown 测试"_s, types, u"folder").type);
    }
};

QTEST_GUILESS_MAIN(QuerySyntaxTest)
#include "tst_querysyntax.moc"
