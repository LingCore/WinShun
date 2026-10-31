#include <QFontDatabase>
#include <QFile>
#include <QFontInfo>
#include <QGlyphRun>
#include <QPainter>
#include <QTextLayout>
#include <QtTest>

using namespace Qt::StringLiterals;

class TestUiFont : public QObject {
    Q_OBJECT
    QString m_family;

    static int ink(const QFont& font)
    {
        QImage image(600, 80, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(font);
        painter.drawText(12, 45, u"搜索 设置 WinShun 123.45"_s);
        painter.end();
        int sum = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                sum += 255 - qGray(image.pixel(x, y));
        return sum;
    }

private slots:
    void initTestCase()
    {
        const QString path = QCoreApplication::applicationDirPath() + u"/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s;
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int id = QFontDatabase::addApplicationFontFromData(file.readAll());
        QVERIFY(id >= 0);
        const auto families = QFontDatabase::applicationFontFamilies(id);
        QVERIFY(!families.isEmpty());
        m_family = families.first();
    }

    void mixedTextAndFallbackHaveGlyphs()
    {
        QFont font(m_family);
        font.setPixelSize(18);
        QTextLayout layout(QString::fromUcs4(U"文件搜索 龘 扩展名 Settings 123.45 Ελληνικά Русский 日本語 한글 😀 𠮷"), font);
        layout.beginLayout();
        auto line = layout.createLine();
        QVERIFY(line.isValid());
        line.setLineWidth(1600);
        layout.endLayout();
        const auto runs = layout.glyphRuns();
        QVERIFY(!runs.isEmpty());
        for (const auto& run : runs) {
            QVERIFY(run.rawFont().isValid());
            for (const auto glyph : run.glyphIndexes())
                QVERIFY2(glyph != 0, qUtf8Printable(run.rawFont().familyName()));
        }
    }

    void synthesizedHeadingIsBolderAtSupportedScales()
    {
        for (const double scale : {1.0, 1.25, 1.5, 2.0}) {
            QFont regular(m_family);
            regular.setPixelSize(qRound(16 * scale));
            QFont bold = regular;
            bold.setWeight(QFont::Bold);
            QCOMPARE(QFontInfo(regular).family(), QFontInfo(bold).family());
            QVERIFY(ink(bold) > ink(regular));
        }
    }
};

QTEST_MAIN(TestUiFont)
#include "tst_uifont.moc"
