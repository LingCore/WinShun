// Long names and paths shortened for a row (elide::shortenedHtml): the
// extension and the end of a name stay in sight, matches too, and what is
// shown fits the width.
#include "Elide.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QTextDocumentFragment>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

// What a Text shows of the HTML: plain, unescaped.
QString plainOf(const QString& html)
{
    return QTextDocumentFragment::fromHtml(html).toPlainText();
}

QFont rowFont()
{
    QFont font(u"Microsoft YaHei UI"_s);
    font.setPixelSize(15);
    return font;
}

} // namespace

class TestElide : public QObject {
    Q_OBJECT

private slots:
    void fitsUnchanged()
    {
        const QString name = u"report.pdf"_s;
        QCOMPARE(elide::shortenedHtml(name, {}, 400, rowFont(), Qt::blue), name);
        // With a match: highlighted as the model's own HTML would be (the row compares the two).
        QCOMPARE(elide::shortenedHtml(name, {{0, 6}}, 400, rowFont(), Qt::blue),
            elide::highlighted(name, {{0, 6}}, Qt::blue));
    }

    void noMatchKeepsExtensionAndEnd_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("end");
        QTest::newRow("chinese")
            << u"二〇二六年度第三季度华东区域销售业绩分析与下一阶段市场推广策略建议报告（修订稿）最终版v3.docx"_s
            << u"最终版v3.docx"_s;
        QTest::newRow("english")
            << u"Quarterly_Business_Review_East_Region_Sales_Performance_Analysis_and_Strategy_FINAL_v3.pptx"_s
            << u"FINAL_v3.pptx"_s;
        QTest::newRow("dots") << u"backup.2026.10.16.full.database.snapshot.compressed.archive.part001.tar.gz"_s
                              << u"part001.tar.gz"_s;
    }
    void noMatchKeepsExtensionAndEnd()
    {
        QFETCH(QString, name);
        QFETCH(QString, end);
        const qreal width = 320;
        const QString shown = plainOf(elide::shortenedHtml(name, {}, width, rowFont(), Qt::blue));
        QVERIFY2(shown.contains(u'…'), qPrintable(shown));
        QVERIFY2(shown.endsWith(end), qPrintable(shown));
        QVERIFY2(shown.startsWith(name.left(4)), qPrintable(shown));
        QVERIFY2(QFontMetricsF(rowFont()).horizontalAdvance(shown) <= width, qPrintable(shown));
    }

    void matchStaysInSightWithTheEnd()
    {
        const QString name = u"关于年度预算编制工作的补充通知以及各部门弯管机采购计划汇总表和附件说明第二版.xlsx"_s;
        const qsizetype at = name.indexOf(u"弯管机"_s);
        const QString html = elide::shortenedHtml(name, {{at, 3}}, 300, rowFont(), Qt::blue);
        const QString shown = plainOf(html);
        QVERIFY2(shown.contains(u"弯管机"_s), qPrintable(shown));
        QVERIFY2(shown.endsWith(u".xlsx"_s), qPrintable(shown));
        // More of the end than the extension alone ("…第二版.xlsx", not "….xlsx").
        QVERIFY2(!shown.endsWith(u"….xlsx"_s), qPrintable(shown));
        QVERIFY2(html.contains(u"<b>弯管机</b>"_s), qPrintable(html));
    }

    void pathCutAtAFolder()
    {
        const QString folder = u"F:\\项目资料\\二〇二六年\\华东区域\\客户往来函件\\合同与补充协议\\已归档"_s;
        const QString shown = plainOf(elide::shortenedHtml(folder, {}, 260, rowFont(), Qt::blue));
        QVERIFY2(shown.startsWith(u"F:\\"_s), qPrintable(shown));
        QVERIFY2(shown.endsWith(u"已归档"_s), qPrintable(shown));
        // The end starts with a folder's name, whole.
        const qsizetype dots = shown.indexOf(u'…');
        QVERIFY(dots > 0);
        QCOMPARE(shown.at(dots + 1), u'\\');
    }

    void discreteFuzzyRanges()
    {
        // Discrete FZF ranges (e.g. "nsrch" -> N, S, rch in NameSearch.cpp)
        // highlight cleanly both when full text fits and when elided.
        const QString name = u"NameSearch.cpp"_s;
        const elide::Ranges ranges {{0, 1}, {4, 1}, {7, 3}};
        const QString fit = elide::shortenedHtml(name, ranges, 400, rowFont(), Qt::blue);
        QCOMPARE(plainOf(fit), name);
        QVERIFY(fit.contains(u"<b>N</b>"_s));
        QVERIFY(fit.contains(u"<b>S</b>"_s));
        QVERIFY(fit.contains(u"<b>rch</b>"_s));

        const QString longName = u"prefix_folder_segment_NameSearch_implementation_final_v3.cpp"_s;
        const qsizetype base = longName.indexOf(u"NameSearch"_s);
        const elide::Ranges longRanges {{base, 1}, {base + 4, 1}, {base + 7, 3}};
        const QString elided = elide::shortenedHtml(longName, longRanges, 260, rowFont(), Qt::blue);
        const QString shown = plainOf(elided);
        QVERIFY2(shown.contains(u"NameSearch"_s), qPrintable(shown));
        QVERIFY2(elided.contains(u"<b>N</b>"_s) && elided.contains(u"<b>S</b>"_s) && elided.contains(u"<b>rch</b>"_s),
            qPrintable(elided));
    }

    void surrogatesNotSplit()
    {
        QString name;
        for (int i = 0; i < 60; ++i)
            name += u"😀"_s;
        name += u".txt"_s;
        const QString shown = plainOf(elide::shortenedHtml(name, {}, 200, rowFont(), Qt::blue));
        QString units;
        for (const QChar c : shown)
            units += QString::number(c.unicode(), 16) + u' ';
        for (qsizetype i = 0; i < shown.size(); ++i) {
            if (shown[i].isHighSurrogate())
                QVERIFY2(i + 1 < shown.size() && shown[++i].isLowSurrogate(), qPrintable(units));
            else
                QVERIFY2(!shown[i].isLowSurrogate(), qPrintable(units));
        }
        QVERIFY(shown.endsWith(u".txt"_s));
    }
};

QTEST_MAIN(TestElide)
#include "tst_elide.moc"
