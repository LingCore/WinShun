#include "ColorText.h"

#include <QTest>

#include <cstdlib>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

// Channels compared in 8 bits, where QColor's 16-bit storage cannot differ.
QString channels(const QColor& c)
{
    const QColor rgb = c.toRgb();
    return u"%1,%2,%3,%4"_s.arg(rgb.red()).arg(rgb.green()).arg(rgb.blue()).arg(rgb.alpha());
}

bool near(const QColor& a, const QColor& b) // within rounding: HSL is shown in whole numbers
{
    const QColor x = a.toRgb();
    const QColor y = b.toRgb();
    return std::abs(x.red() - y.red()) <= 2 && std::abs(x.green() - y.green()) <= 2
        && std::abs(x.blue() - y.blue()) <= 2 && std::abs(x.alpha() - y.alpha()) <= 1;
}

} // namespace

class ColorTest : public QObject {
    Q_OBJECT

private slots:
    void parsesColors_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected"); // r,g,b,a

        QTest::newRow("hex") << u"#3b82f6"_s << u"59,130,246,255"_s;
        QTest::newRow("hex upper") << u"#3B82F6"_s << u"59,130,246,255"_s;
        QTest::newRow("hex short") << u"#f80"_s << u"255,136,0,255"_s;
        QTest::newRow("hex alpha last") << u"#3B82F680"_s << u"59,130,246,128"_s;
        QTest::newRow("hex short alpha") << u"#f808"_s << u"255,136,0,136"_s;
        QTest::newRow("rgb") << u"rgb(255, 136, 0)"_s << u"255,136,0,255"_s;
        QTest::newRow("rgba") << u"rgba(255, 136, 0, 0.5)"_s << u"255,136,0,128"_s;
        QTest::newRow("rgba tight") << u"RGBA(255,136,0,.25)"_s << u"255,136,0,64"_s;
        QTest::newRow("rgba percent alpha") << u"rgba(255, 136, 0, 50%)"_s << u"255,136,0,128"_s;
        QTest::newRow("rgb slash") << u"rgb(255 136 0 / 50%)"_s << u"255,136,0,128"_s;
        QTest::newRow("rgb slash number") << u"rgb(255 136 0 / 0.2)"_s << u"255,136,0,51"_s;
        QTest::newRow("rgb percent") << u"rgb(100%, 53.3%, 0%)"_s << u"255,136,0,255"_s;
        QTest::newRow("rgb percent slash") << u"rgb(100% 0% 0% / 0.5)"_s << u"255,0,0,128"_s;
        QTest::newRow("rgb decimals") << u"rgb(59.4 130.2 245.6)"_s << u"59,130,246,255"_s;
        QTest::newRow("alpha clamped") << u"rgba(0, 0, 0, 1.5)"_s << u"0,0,0,255"_s;
        QTest::newRow("transparent") << u"rgba(0, 0, 0, 0)"_s << u"0,0,0,0"_s;
        QTest::newRow("semicolon") << u"rgba(59, 130, 246, 0.5);"_s << u"59,130,246,128"_s;
        QTest::newRow("spaces and semicolon") << u"  #fff ;\n"_s << u"255,255,255,255"_s;
        QTest::newRow("hsl") << u"hsl(0, 100%, 50%)"_s << u"255,0,0,255"_s;
        QTest::newRow("hsla slash") << u"hsla(120deg 100% 25% / 50%)"_s << u"0,128,0,128"_s;
        QTest::newRow("hsl hue wraps") << u"hsl(-120, 100%, 50%)"_s << u"0,0,255,255"_s;
    }
    void parsesColors()
    {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        const std::optional<QColor> color = colortext::parse(text);
        QVERIFY2(color.has_value(), qPrintable(text));
        QCOMPARE(channels(*color), expected);
    }

    void rejectsOtherText_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("empty") << QString();
        QTest::newRow("semicolon only") << u";"_s;
        QTest::newRow("five digits") << u"#12345"_s;
        QTest::newRow("not hex") << u"#ggg"_s;
        QTest::newRow("no hash") << u"3b82f6"_s;
        QTest::newRow("name") << u"red"_s;
        QTest::newRow("in a sentence") << u"#3b82f6 is blue"_s;
        QTest::newRow("two channels") << u"rgb(1, 2)"_s;
        QTest::newRow("channel over 255") << u"rgb(256, 0, 0)"_s;
        QTest::newRow("percent over 100") << u"rgb(101%, 0%, 0%)"_s;
        QTest::newRow("two semicolons") << u"rgb(255, 136, 0);;"_s;
        QTest::newRow("saturation over 100") << u"hsl(0, 101%, 50%)"_s;
        QTest::newRow("hsl without percent") << u"hsl(0, 100, 50)"_s;
        QTest::newRow("css declaration") << u"color: #fff;"_s;
        QTest::newRow("too long") << u"rgba(255.000000000, 136.000000000, 0.0000000000, 0.5)"_s;
    }
    void rejectsOtherText()
    {
        QFETCH(QString, text);
        QVERIFY2(!colortext::parse(text).has_value(), qPrintable(text));
    }

    void showsEachNotation()
    {
        const auto values = [](const QColor& color) {
            QStringList list;
            for (const auto& [label, value, readsBack] : colortext::notations(color))
                list.append(label + u' ' + value);
            return list;
        };
        QCOMPARE(values(QColor(59, 130, 246)),
            QStringList({u"HEX #3B82F6"_s, u"RGB rgb(59, 130, 246)"_s, u"HSL hsl(217, 91%, 60%)"_s}));
        QColor half(59, 130, 246);
        half.setAlphaF(0.5f);
        QCOMPARE(values(half), QStringList({u"HEX #3B82F680"_s, u"ARGB #803B82F6"_s, u"RGB rgba(59, 130, 246, 0.5)"_s,
                                   u"HSL hsla(217, 91%, 60%, 0.5)"_s}));
        QCOMPARE(values(QColor(0, 0, 0, 0)).constLast(), u"HSL hsla(0, 0%, 0%, 0)"_s);
    }

    // Copying a notation and looking at it again gives the same colour,
    // except #AARRGGBB, which is read as CSS's #RRGGBBAA (and says so).
    void notationsReadBack_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("opaque") << u"#3b82f6"_s;
        QTest::newRow("half") << u"rgba(59, 130, 246, 0.5)"_s;
        QTest::newRow("faint") << u"#00000014"_s;
        QTest::newRow("hsl") << u"hsla(32, 100%, 50%, 0.75)"_s;
        QTest::newRow("grey") << u"rgb(128 128 128 / 30%)"_s;
    }
    void notationsReadBack()
    {
        QFETCH(QString, text);
        const std::optional<QColor> color = colortext::parse(text);
        QVERIFY(color);
        for (const auto& [label, value, readsBack] : colortext::notations(*color)) {
            const std::optional<QColor> back = colortext::parse(value);
            QVERIFY2(back && near(*back, *color) == readsBack, qPrintable(label + u' ' + value));
        }
    }
};

QTEST_APPLESS_MAIN(ColorTest)
#include "tst_color.moc"
