#include "NameSearch.h"
#include "Query.h"

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
        QTest::newRow("regex-not-number") << u"regex:^IMG_\\d+\\.(jpg|png)$"_s << u"IMG_a.png"_s << false;
        QTest::newRow("regex-case-escape") << u"regex:^\\D+$"_s << u"abc.png"_s << true;
        QTest::newRow("regex-negative") << u"*.png !regex:^temp"_s << u"temp123.png"_s << false;
        QTest::newRow("regex-space") << u"regex:\"^annual report\\.pdf$\""_s << u"Annual Report.pdf"_s << true;
        QTest::newRow("quoted-regex-is-literal") << u"\"regex:[\""_s << u"regex:[.txt"_s << true;
        QTest::newRow("empty-quote-before-regex") << u"\"\" regex:^\\D+$"_s << u"abc.png"_s << true;
        QTest::newRow("empty-anchored") << u"^$"_s << u"file.txt"_s << false;
        QTest::newRow("old-pinyin") << u"bg !draft"_s << u"报告.txt"_s << true;
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

    void invalidQueries()
    {
        for (const auto& text : {u"regex:["_s, u"regex:"_s, u"report |"_s, u"| report"_s, u"a | | b"_s, u"\"\" regex:["_s}) {
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

    void multipleTypePrefixes()
    {
        const FileTypes types {{u"图片"_s, {u"png"_s}, u"pictures"_s, u"pic"_s},
            {u"视频"_s, {u"mp4"_s}, u"videos"_s, u"video"_s}};
        const auto query = splitFileTypeQuery(u":pic,video 测试"_s, types, u"folder");
        QCOMPARE(query.words, u"测试"_s);
        QCOMPARE(query.types, (QList<int>{0, 1}));
        QVERIFY(!splitFileTypeQuery(u":pic,unknown 测试"_s, types, u"folder").type);
    }
};

QTEST_GUILESS_MAIN(QuerySyntaxTest)
#include "tst_querysyntax.moc"
