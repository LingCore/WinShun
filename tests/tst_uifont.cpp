#include "UiFont.h"

#include <QRawFont>
#include <QSet>
#include <QFontDatabase>
#include <QFile>
#include <QFontInfo>
#include <QGlyphRun>
#include <QPainter>
#include <QTextLayout>
#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>

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
        const auto loaded = ws::uifont::initialize();
        m_family = loaded.value(u"family"_s).toString();
        QVERIFY(m_family.contains(u"Alibaba"));
    }

    void startupFamilyAndFallback()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nText { text: '文件搜索 WinShun 123'; font.pixelSize: 18 }", QUrl());
        std::unique_ptr<QObject> label(component.create());
        QVERIFY2(label, qPrintable(component.errorString()));
        const auto font = [&] { return label->property("font").value<QFont>(); };
        QCOMPARE(font().family(), m_family);
        QVERIFY(ws::uifont::available(u"Microsoft YaHei UI"_s));
        ws::uifont::apply(u"Microsoft YaHei UI"_s);
        // Plain Text keeps its construction-time font: the settings provide
        // a bound preview and explicitly apply the whole UI after restart.
        QCOMPARE(font().family(), m_family);
        label.reset(component.create());
        QCOMPARE(font().family(), u"Microsoft YaHei UI"_s);
        QCOMPARE(font().pixelSize(), 18); // a family preference never changes layout sizing
        QVERIFY(ink(font()) > 0);
        QVERIFY(!ws::uifont::available(u"Missing-WinShun-Test-Font"_s));
        ws::uifont::apply(u"Missing-WinShun-Test-Font"_s);
        label.reset(component.create());
        QCOMPARE(font().family(), m_family);
        ws::uifont::apply(u"Microsoft YaHei UI"_s);
        ws::uifont::apply({});
        label.reset(component.create());
        QCOMPARE(font().family(), m_family);
    }

    // A font is installed under a name in each of its languages, and Windows
    // lists it under one of them: saved under either, it is that font (GitHub
    // #325: "LXGW WenKai" was not in Qt's list, where 霞鹜文楷 was).
    void eitherNameOfAFont()
    {
        const QString chinese = u"微软雅黑"_s;
        const QString english = u"Microsoft YaHei"_s;
        if (!ws::uifont::available(chinese) && !ws::uifont::available(english))
            QSKIP("Microsoft YaHei is not installed here");
        QVERIFY(ws::uifont::available(chinese));
        QVERIFY(ws::uifont::available(english));
        const QString listed = ws::uifont::listedFamily(english);
        QVERIFY(listed == chinese || listed == english);
        QCOMPARE(ws::uifont::listedFamily(chinese), listed);
        QCOMPARE(ws::uifont::listedFamily(u"microsoft yahei"_s), listed);
        QVERIFY(ws::uifont::families().contains(listed));
        for (const QString& saved : {chinese, english}) {
            ws::uifont::apply({});
            ws::uifont::apply(saved);
            QCOMPARE(QGuiApplication::font().family(), listed);
            QCOMPARE(ws::uifont::resolvedFamily(saved), listed);
            const QString drawn = QRawFont::fromFont(QGuiApplication::font()).familyName();
            QVERIFY2(drawn == chinese || drawn == english, qPrintable(drawn)); // that font, not a fallback
        }
        ws::uifont::apply({});
        QCOMPARE(QGuiApplication::font().family(), m_family);
        QVERIFY(ws::uifont::listedFamily(u"Missing-WinShun-Test-Font"_s).isEmpty());
        QVERIFY(ws::uifont::listedFamily(QString(40, u'x')).isEmpty()); // longer than a face name
        // The list: no vertical faces, nothing twice, none Windows does not know.
        const QStringList all = ws::uifont::families();
        QVERIFY(all.size() > 10);
        QCOMPARE(QSet<QString>(all.cbegin(), all.cend()).size(), all.size());
        for (const QString& family : all) {
            QVERIFY2(!family.startsWith(u'@'), qPrintable(family));
            QCOMPARE(ws::uifont::listedFamily(family), family);
        }
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
