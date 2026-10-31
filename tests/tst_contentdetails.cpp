#include "ContentDetails.h"
#include "ContentReport.h"
#include "ContentScanner.h"
#include "FileText.h"
#include "platform/ContentReportPrint.h"

#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>
#include <miniz.h>

using namespace Qt::StringLiterals;
using namespace ws;
namespace cd = ws::contentdetails;

class ContentDetailsTest : public QObject {
    Q_OBJECT
private slots:
    void everyOccurrenceAndUnicodeOffsets()
    {
        doctext::DocText text;
        text.text = u"😀 needle NEEDLE\n中文报告 报告\nneedle\n"_s.toUtf8().toStdString();
        const auto found = cd::find(cd::document(text), {u"needle"_s, u"报告"_s, u"NEEDLE"_s});
        QCOMPARE(found.terms.size(), 2);
        QCOMPARE(found.counts, (std::vector<qint64>{3, 2}));
        QCOMPARE(found.items.front().start, 3);
        QCOMPARE(found.items.back().line, 2);
        const auto rows = cd::filter(found, {true, true}, {}, 1);
        QCOMPARE(rows.matches.size(), 5u);
        QCOMPARE(rows.lines, (std::vector<int>{0, 1, 2}));
    }
    void termsAcrossDifferentLinesRemainValid()
    {
        doctext::DocText text {"alpha\nbeta\nalpha alpha\n", {}, {}, false};
        const auto found = cd::find(cd::document(text), {u"alpha"_s, u"beta"_s});
        QCOMPARE(found.counts, (std::vector<qint64>{3, 1}));
        QCOMPARE(cd::filter(found, {false, true}, {}, 0).lines, (std::vector<int>{1}));
        QVERIFY(cd::filter(found, {false, false}, {}, 3).lines.empty());
    }
    void refinementIsIndependentOfContext()
    {
        const auto found = cd::find(cd::document({"Needle region\nneedle regional\nneedle other\nREGION\n", {}, {}, false}), {u"needle"_s});
        cd::Filter filter {u"region"_s, false, true, false};
        const auto rows = cd::filter(found, {true}, filter, 3);
        QCOMPARE(rows.matches.size(), 1u);
        QCOMPARE(rows.filterCount, 1);
        QVERIFY(rows.refinements && rows.refinements->size() == 1);
        QCOMPARE(cd::filter(found, {true}, filter, 0).matches, rows.matches);
        filter.enabled = false;
        QCOMPARE(cd::filter(found, {true}, filter, 0).matches.size(), 3u);
        filter.enabled = true;
        filter.matchCase = true;
        filter.text = u"Region"_s;
        QVERIFY(cd::filter(found, {true}, filter, 1).matches.empty());
        filter = {u"region(?:al)?$"_s, false, false, true};
        QCOMPARE(cd::filter(found, {true}, filter, 0).matches.size(), 2u);
        filter.text = u"["_s;
        QVERIFY(!cd::filter(found, {true}, filter, 0).error.isEmpty());
        filter.text = u"(?=needle)"_s;
        QCOMPARE(cd::filter(found, {true}, filter, 0).matches.size(), 3u); // zero-length regex terminates
    }
    void pageAndSparseSpreadsheetBoundaries()
    {
        doctext::DocText text {"needle\ncontext\nneedle\nneedle\nnext\nneedle\n", {
            {1, 7, 0, doctext::PlaceKind::Row}, {2, 50, 0, doctext::PlaceKind::Row},
            {3, 90, 0, doctext::PlaceKind::Row}, {4, 5, 1, doctext::PlaceKind::Row},
            {6, 4, doctext::Place::kNoSheet, doctext::PlaceKind::Page}}, {"North", "South"}, false};
        const auto found = cd::find(cd::document(text), {u"needle"_s});
        QCOMPARE(found.document->lines[0].number, 7u);
        QCOMPARE(found.document->lines[1].number, 50u);
        QCOMPARE(found.document->lines[3].number, 5u);
        const auto north = cd::filter(found, {true}, {}, 3, 0);
        QCOMPARE(north.matches.size(), 2u);
        QCOMPARE(north.lines, (std::vector<int>{0, 2})); // no invented contiguous row numbers
        const auto south = cd::filter(found, {true}, {}, 3, 1);
        QCOMPARE(south.lines, (std::vector<int>{3, 4}));
        QVERIFY(!cd::filter(found, {true}, {}, 3).lines.empty());
    }
    void partialAndCancellation()
    {
        doctext::DocText text {"needle\n", {}, {}, true};
        QVERIFY(cd::find(cd::document(text), {u"needle"_s}).truncated);
        QVERIFY(!cd::document(text, [] { return true; }));
        const auto doc = cd::document(text);
        QVERIFY(cd::find(doc, {u"needle"_s}, [] { return true; }).cancelled);
        text.text.assign(cd::kMaxMatches + 8, 'a');
        const auto many = cd::find(cd::document(text), {u"a"_s});
        QCOMPARE(many.items.size(), cd::kMaxMatches);
        QVERIFY(many.truncated);
        QVERIFY(cd::filter(many, {true}, {}, 0, -1, [] { return true; }).cancelled);
    }
    void fileDecodingAndReadErrors()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString original = u"中文 needle\r\n第2行 NEEDLE\n"_s;
        const auto check = [&](const QByteArray& bytes, const QString& name) {
            const QString path = dir.filePath(name);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
            file.close();
            const auto read = filetext::read(QDir::toNativeSeparators(path).toStdWString(), {}, 1ll << 20, {});
            if (read.status != filetext::Status::Ok) return false;
            const auto found = cd::find(cd::document({read.text.toUtf8().toStdString(), {}, {}, read.cut}), {u"needle"_s});
            return found.items.size() == 2 && found.items[1].line == 1;
        };
        QVERIFY(check(original.toUtf8(), u"utf8.txt"_s));
        QByteArray le("\xff\xfe", 2), be("\xfe\xff", 2);
        for (QChar c : original) {
            le.append(static_cast<char>(c.unicode() & 255)); le.append(static_cast<char>(c.unicode() >> 8));
            be.append(static_cast<char>(c.unicode() >> 8)); be.append(static_cast<char>(c.unicode() & 255));
        }
        QVERIFY(check(le, u"utf16le.txt"_s));
        QVERIFY(check(be, u"utf16be.txt"_s));
        bool large = true, unreadable = false;
        QVERIFY(!ContentScanner(u"needle"_s).scanFile(dir.filePath(u"gone.txt"_s).toStdWString(), 1000, {}, &large, &unreadable));
        QVERIFY(unreadable);
        QVERIFY(!large);
        const auto cancelled = filetext::read(QDir::toNativeSeparators(dir.filePath(u"utf8.txt"_s)).toStdWString(), {}, 1000, [] { return true; });
        QVERIFY(cancelled.status != filetext::Status::Ok);
    }
    void exportsKeepTextAndEscapeMarkup()
    {
        const contentreport::Report report {u"搜索 <test>"_s, {u"query: needle"_s}, {
            {u"Hits"_s, {u"File"_s, u"Text"_s}, {{u"中文.xlsx"_s, u"=SUM(1,2)"_s},
                {u"a,b\".txt"_s, u"<script>alert('x')</script>\nnext"_s}}}}};
        const QByteArray html = contentreport::render(report, contentreport::Format::Html);
        QVERIFY(!html.contains("<script>"));
        QVERIFY(html.contains("&lt;script&gt;"));
        const QByteArray csv = contentreport::render(report, contentreport::Format::Csv);
        QVERIFY(csv.startsWith(QByteArray("\xef\xbb\xbf", 3)));
        QVERIFY(csv.contains("'=SUM(1,2)"));
        QVERIFY(csv.contains("a,b\"\".txt"));
        QVERIFY(contentreport::render(report, contentreport::Format::Text).contains("query: needle"));
        const QByteArray workbook = contentreport::render(report, contentreport::Format::Excel);
        QVERIFY(workbook.startsWith("PK"));
        mz_zip_archive zip {};
        QVERIFY(mz_zip_reader_init_mem(&zip, workbook.constData(), static_cast<size_t>(workbook.size()), 0));
        QVERIFY(mz_zip_reader_locate_file(&zip, "[Content_Types].xml", nullptr, 0) >= 0);
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
            size_t length = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &length, 0);
            QVERIFY(data);
            const QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(length));
            mz_free(data);
            QXmlStreamReader xml(bytes);
            while (!xml.atEnd()) xml.readNext();
            QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        }
        size_t length = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, "xl/worksheets/sheet2.xml", &length, 0);
        QVERIFY(data);
        const QByteArray sheet(static_cast<const char*>(data), static_cast<qsizetype>(length));
        mz_free(data);
        QVERIFY(sheet.contains("inlineStr"));
        QVERIFY(!sheet.contains("<f>"));
        QVERIFY(sheet.contains("=SUM(1,2)"));
        mz_zip_reader_end(&zip);
    }
    void realPrintToPdf()
    {
        QTemporaryDir dir;
        const HDC dc = ::CreateDCW(L"WINSPOOL", L"Microsoft Print to PDF", nullptr, nullptr);
        if (!dc) QSKIP("Microsoft Print to PDF is not installed");
        contentreport::Report report {u"WinShun search"_s, {u"Isolated test"_s}, {
            {u"Matches"_s, {u"File"_s, u"Text"_s}, {}}}};
        for (int i = 0; i < 250; ++i) report.tables[0].rows.push_back({u"report.txt"_s, u"中文 needle "_s.repeated(20)});
        const auto path = dir.filePath(u"report.pdf"_s).toStdWString();
        const auto result = reportprint::render(dc, report, path.c_str(), {});
        ::DeleteDC(dc);
        QCOMPARE(result, reportprint::Result::Printed);
        QFile file(QString::fromStdWString(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.read(8).startsWith("%PDF-"));
        QVERIFY(file.size() > 1000);
    }
    void atomicReportFiles()
    {
        QTemporaryDir dir;
        const contentreport::Report report {u"Report"_s, {u"A snapshot"_s}, {{u"Hits"_s, {u"Text"_s}, {{u"中文 needle"_s}}}}};
        for (int i = 0; i < 4; ++i) {
            const auto format = static_cast<contentreport::Format>(i);
            const QString path = dir.filePath(u"报告 %1"_s.arg(i));
            QVERIFY(contentreport::save(report, format, path));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), contentreport::render(report, format));
        }
        QVERIFY(QDir(dir.path()).mkdir(u"existing"_s));
        const QString original = dir.filePath(u"existing/original.txt"_s);
        QFile file(original);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("preserve user data"); file.close();
        QVERIFY(!contentreport::save(report, contentreport::Format::Text, dir.filePath(u"existing"_s)));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("preserve user data"));
    }
};
QTEST_GUILESS_MAIN(ContentDetailsTest)
#include "tst_contentdetails.moc"
