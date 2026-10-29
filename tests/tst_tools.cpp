// The computing pieces of the tools plugin (src/tools-plugin): the
// calculator, the unit conversions, colours and the codecs, at their edges.

#include "Calc.h"
#include "Codec.h"
#include "Color.h"
#include "Units.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

#include <cmath>
#include <string>

using namespace tools;
using namespace Qt::StringLiterals;

namespace {

double value(const std::string& expression)
{
    const calc::Result r = calc::evaluate(expression);
    if (!r.ok())
        return std::nan("");
    return r.value;
}

calc::Error errorOf(const std::string& expression)
{
    return calc::evaluate(expression).error;
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

} // namespace

class TestTools : public QObject {
    Q_OBJECT

private slots:
    void calcArithmetic()
    {
        QCOMPARE(value("1+2*3"), 7.0);
        QCOMPARE(value("(1+2)*3"), 9.0);
        QCOMPARE(value("2^10"), 1024.0);
        QCOMPARE(value("2^3^2"), 512.0); // right-associative
        QCOMPARE(value("-2^2"), -4.0); // unary minus binds looser than the power
        QCOMPARE(value("(-2)^2"), 4.0);
        QCOMPARE(value("10/4"), 2.5);
        QCOMPARE(value("10 % 3"), 1.0); // the remainder
        QCOMPARE(value("200*15%"), 30.0); // a hundredth
        QCOMPARE(value("15%"), 0.15);
        QCOMPARE(value("5!"), 120.0);
        QCOMPARE(value("2(3+4)"), 14.0); // implied multiplication
        QCOMPARE(value("(1+2)(3+4)"), 21.0);
        QCOMPARE(value("1,000 + 1"), 1001.0); // thousands separators
        QCOMPARE(value("1e3"), 1000.0);
        QCOMPARE(value("0xff"), 255.0);
        QCOMPARE(value("0b101"), 5.0);
        QCOMPARE(value("--3"), 3.0);
        QCOMPARE(value("3 × 4 ÷ 2"), 6.0); // typographic operators
        QCOMPARE(value("（1＋2）＊3"), 9.0); // full-width ones
        QVERIFY(std::fabs(value("sqrt(2)") - 1.41421356) < 1e-6);
        QVERIFY(std::fabs(value("sin(rad(30))") - 0.5) < 1e-9);
        QVERIFY(std::fabs(value("2pi") - 6.2831853) < 1e-6);
        QVERIFY(std::fabs(value("log(1000)") - 3) < 1e-9);
        QVERIFY(std::fabs(value("ln(e)") - 1) < 1e-9);
        QCOMPARE(value("max(1, 5, 3)"), 5.0);
        QCOMPARE(value("min(4,2)"), 2.0);
        QCOMPARE(value("pow(2, 8)"), 256.0);
        QCOMPARE(value("abs(-3)"), 3.0);
        QCOMPARE(value("round(2.5)"), 3.0);
        QCOMPARE(value("mod(10, 4)"), 2.0);
        QCOMPARE(value("SQRT(16)"), 4.0); // names are not case-sensitive
    }

    void calcErrors()
    {
        QCOMPARE(errorOf(""), calc::Error::Empty);
        QCOMPARE(errorOf("   "), calc::Error::Empty);
        QCOMPARE(errorOf("1/0"), calc::Error::DivisionByZero);
        QCOMPARE(errorOf("5 % 0"), calc::Error::DivisionByZero);
        QCOMPARE(errorOf("mod(1, 0)"), calc::Error::DivisionByZero);
        QCOMPARE(errorOf("0^-1"), calc::Error::DivisionByZero);
        QCOMPARE(errorOf("1e308*10"), calc::Error::OutOfRange); // overflow
        QCOMPARE(errorOf("10^400"), calc::Error::OutOfRange);
        QCOMPARE(errorOf("171!"), calc::Error::OutOfRange);
        QCOMPARE(errorOf("(-1)!"), calc::Error::OutOfRange);
        QCOMPARE(errorOf("2.5!"), calc::Error::OutOfRange);
        QCOMPARE(errorOf("sqrt(-1)"), calc::Error::OutOfRange); // no real result
        QCOMPARE(errorOf("ln(0)"), calc::Error::OutOfRange);
        QCOMPARE(errorOf("1+"), calc::Error::Syntax);
        QCOMPARE(errorOf("(1+2"), calc::Error::Syntax);
        QCOMPARE(errorOf("1+2)"), calc::Error::Syntax);
        QCOMPARE(errorOf("*3"), calc::Error::Syntax);
        QCOMPARE(errorOf("1 2"), calc::Error::Syntax);
        QCOMPARE(errorOf("abc"), calc::Error::UnknownName);
        QCOMPARE(calc::evaluate("foo(1)").detail, std::string("foo"));
        QCOMPARE(errorOf("sqrt(1, 2)"), calc::Error::WrongArguments);
        QCOMPARE(errorOf("pow(2)"), calc::Error::WrongArguments);
        QCOMPARE(errorOf("max()"), calc::Error::WrongArguments);
        QCOMPARE(errorOf("0x"), calc::Error::Syntax);
        QCOMPARE(errorOf("0xZZ"), calc::Error::Syntax);
        QCOMPARE(errorOf("报告"), calc::Error::Syntax); // not a name, not a number
    }

    void calcLooksLikeExpression()
    {
        QVERIFY(calc::looksLikeExpression("1+2*3"));
        QVERIFY(calc::looksLikeExpression("(2+3)*4"));
        QVERIFY(calc::looksLikeExpression("3 × 4"));
        QVERIFY(!calc::looksLikeExpression("2024-01-02")); // a date: minus only
        QVERIFY(!calc::looksLikeExpression("13800138000"));
        QVERIFY(!calc::looksLikeExpression("report"));
        QVERIFY(!calc::looksLikeExpression("v2.1+beta")); // letters
        QVERIFY(!calc::looksLikeExpression("1+(2")); // unbalanced
    }

    // The manifest's regex triggers decide which typed text reaches the
    // calculator without "=": a subtraction alone (#123) must, a date must not.
    void calcTriggerCoversSubtraction()
    {
        QFile file(QStringLiteral(WINSHUN_TOOLS_MANIFEST));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray commands = QJsonDocument::fromJson(file.readAll()).object().value(u"commands"_s).toArray();
        QVERIFY(!commands.isEmpty());
        QList<QRegularExpression> triggers;
        for (const QJsonValue t : commands.first().toObject().value(u"triggers"_s).toArray())
            triggers.append(QRegularExpression(t.toObject().value(u"regex"_s).toString()));
        QVERIFY(triggers.size() >= 3);
        const auto fires = [&](const QString& text) {
            if (text.size() < 3 || text.size() > 64)
                return false;
            for (const QRegularExpression& re : triggers) {
                if (re.match(text).hasMatch())
                    return true;
            }
            return false;
        };
        for (const QString& yes : {u"99-9"_s, u"10 - 4"_s, u"10 -4"_s, u"5-3*2"_s, u"-5+3"_s, u"1+2"_s, u"3.5-1.5"_s, u"100 - 20 - 5"_s, u"2026 - 10"_s, u"1999-5000"_s, u"2024 - 2023"_s})
            QVERIFY2(fires(yes), qPrintable(yes));
        for (const QString& no : {u"2026-10"_s, u"2026-1"_s, u"2024-01-02"_s, u"1999-12"_s, u"2023-2024"_s, u"1998-2003"_s, u"400-800-1234"_s, u"2024-01-02 15:04"_s,
                 u"report-2"_s, u"99"_s, u"-5"_s, u"abc-9"_s})
            QVERIFY2(!fires(no), qPrintable(no));
        // What the plugin then computes.
        QCOMPARE(value("2023-2024"), -1.0); // "= 2023-2024" still computes
        QCOMPARE(value("99-9"), 90.0);
        QCOMPARE(value("10 - 4"), 6.0);
        QCOMPARE(value("100 - 20 - 5"), 75.0);
    }

    void calcFormat()
    {
        QCOMPARE(qs(calc::format(7)), u"7"_s);
        QCOMPARE(qs(calc::format(1234567)), u"1,234,567"_s);
        QCOMPARE(qs(calc::format(1234567, true)), u"1234567"_s);
        QCOMPARE(qs(calc::format(-1234.5)), u"-1,234.5"_s);
        QCOMPARE(qs(calc::format(0.1 + 0.2)), u"0.3"_s); // 12 significant digits hide the binary dust
        QCOMPARE(qs(calc::format(2.5)), u"2.5"_s);
        QCOMPARE(qs(calc::format(1e20)), u"1e+20"_s);
        QCOMPARE(qs(calc::format(1.5e-12)), u"1.5e-12"_s);
        QCOMPARE(qs(calc::format(0)), u"0"_s);
        QCOMPARE(qs(calc::hex(255)), u"0xFF"_s);
        QCOMPARE(qs(calc::binary(5)), u"0b101"_s);
        QCOMPARE(qs(calc::octal(8)), u"0o10"_s);
        QCOMPARE(qs(calc::hex(-16)), u"-0x10"_s);
        QVERIFY(calc::hex(2.5).empty());
        QVERIFY(calc::hex(1e19).empty());
    }

    void unitsParse()
    {
        auto r = units::parse("12 inch to cm");
        QVERIFY(r);
        QCOMPARE(r->value, 12.0);
        QCOMPARE(qs(r->from->id), u"inch"_s);
        QCOMPARE(qs(r->to->id), u"cm"_s);
        r = units::parse("1.5 GB in MB");
        QVERIFY(r && r->to);
        QCOMPARE(qs(r->from->id), u"GB"_s);
        r = units::parse("12 in to cm"); // "in" the unit, "to" the separator
        QVERIFY(r && r->to);
        QCOMPARE(qs(r->from->id), u"inch"_s);
        r = units::parse("100 °F → °C");
        QVERIFY(r && r->to);
        QCOMPARE(qs(r->to->id), u"°C"_s);
        r = units::parse("3 斤 换成 kg");
        QVERIFY(r && r->to);
        QCOMPARE(qs(r->from->id), u"斤"_s);
        r = units::parse("12inch cm"); // no separator: a space
        QVERIFY(r && r->to);
        r = units::parse("12 inch"); // no target: the whole category
        QVERIFY(r && !r->to);
        r = units::parse("1,000 m to km");
        QVERIFY(r);
        QCOMPARE(r->value, 1000.0);
        QVERIFY(!units::parse("12 inch to kg")); // categories differ
        QVERIFY(!units::parse("12 furlong to m")); // unknown unit
        QVERIFY(!units::parse("inch to cm")); // no number
        QVERIFY(!units::parse("report"));
        QVERIFY(!units::parse(""));
        QVERIFY(units::find("GB") == units::find("gib")); // 1024-based, both spellings
        QVERIFY(units::find("g")->category == units::Category::Mass); // the gram, not GB
    }

    void unitsConvert()
    {
        const auto rowsOf = [](const char* text) { return units::convert(*units::parse(text)); };
        auto rows = rowsOf("12 inch to cm");
        QCOMPARE(rows.size(), std::size_t(1));
        QCOMPARE(qs(rows[0].value), u"30.48"_s);
        rows = rowsOf("100 f to c");
        QCOMPARE(qs(rows[0].value), u"37.77777778"_s);
        rows = rowsOf("0 c to f");
        QCOMPARE(qs(rows[0].value), u"32"_s);
        rows = rowsOf("-40 c to f");
        QCOMPARE(qs(rows[0].value), u"-40"_s);
        rows = rowsOf("0 k to c");
        QCOMPARE(qs(rows[0].value), u"-273.15"_s);
        rows = rowsOf("1.5 GB in MB");
        QCOMPARE(rows.size(), std::size_t(2)); // 1024 and 1000
        QCOMPARE(qs(rows[0].value), u"1536"_s);
        QCOMPARE(qs(rows[1].value), u"1500"_s);
        QVERIFY(rows[1].alternate);
        rows = rowsOf("1 斤 to g");
        QCOMPARE(qs(rows[0].value), u"500"_s);
        rows = rowsOf("2 h to min");
        QCOMPARE(qs(rows[0].value), u"120"_s);
        rows = rowsOf("1 mile to km");
        QCOMPARE(qs(rows[0].value), u"1.609344"_s);
        rows = rowsOf("1 亩 to m2");
        QCOMPARE(qs(rows[0].value), u"666.6666667"_s);
        rows = rowsOf("12 inch");
        QVERIFY(rows.size() > 8); // every other length unit
        QCOMPARE(qs(units::formatValue(1e30)), u"1e+30"_s);
        QCOMPARE(qs(units::formatValue(0)), u"0"_s);
    }

    void colorParse()
    {
        auto c = color::parse("#ff8800");
        QVERIFY(c);
        QCOMPARE(c->r, 255);
        QCOMPARE(c->g, 136);
        QCOMPARE(c->b, 0);
        QCOMPARE(c->a, 1.0);
        c = color::parse("#f80");
        QVERIFY(c && c->r == 255 && c->g == 136 && c->b == 0);
        c = color::parse("#ff880080");
        QVERIFY(c && std::fabs(c->a - 128.0 / 255) < 1e-9);
        c = color::parse("rgb(255, 136, 0)");
        QVERIFY(c && c->r == 255 && c->g == 136 && c->b == 0);
        c = color::parse("rgba(255 136 0 / 50%)");
        QVERIFY(c && std::fabs(c->a - 0.5) < 1e-9);
        c = color::parse("rgb(100%, 53%, 0%)");
        QVERIFY(c && c->r == 255 && c->g == 135);
        c = color::parse("hsl(32, 100%, 50%)");
        QVERIFY(c && c->r == 255 && c->g == 136 && c->b == 0);
        c = color::parse("HSLA(32deg 100% 50% / 0.5);");
        QVERIFY(c && c->r == 255 && std::fabs(c->a - 0.5) < 1e-9);
        c = color::parse("hsv(32, 100%, 100%)");
        QVERIFY(c && c->r == 255 && c->g == 136);
        QVERIFY(color::parse("#ff88")); // #rgba
        QVERIFY(!color::parse("#ff888"));
        QVERIFY(!color::parse("#gg0000"));
        QVERIFY(!color::parse("rgb(300, 0, 0)"));
        QVERIFY(!color::parse("rgb(1, 2)"));
        QVERIFY(!color::parse("hsl(0, 150%, 50%)"));
        QVERIFY(!color::parse("report"));
        QVERIFY(!color::parse(""));
    }

    void colorNotations()
    {
        const auto list = color::notations(*color::parse("#ff8800"));
        QCOMPARE(list.size(), std::size_t(4));
        QCOMPARE(qs(list[0].value), u"#FF8800"_s);
        QCOMPARE(qs(list[1].value), u"rgb(255, 136, 0)"_s);
        QCOMPARE(qs(list[2].value), u"hsl(32, 100%, 50%)"_s);
        QCOMPARE(qs(list[3].value), u"hsv(32, 100%, 100%)"_s);
        const auto translucent = color::notations(*color::parse("rgba(255, 136, 0, 0.5)"));
        QCOMPARE(qs(translucent[0].value), u"#FF880080"_s);
        QCOMPARE(qs(translucent[1].label), u"ARGB"_s);
        QCOMPARE(qs(translucent[1].value), u"#80FF8800"_s);
        QCOMPARE(qs(translucent[2].value), u"rgba(255, 136, 0, 0.5)"_s);
        // Round trips through HSL keep the channels.
        double h, s, l;
        color::toHsl(color::Color {30, 60, 90, 1}, h, s, l);
        const color::Color back = color::fromHsl(h, s, l);
        QVERIFY(std::abs(back.r - 30) <= 1 && std::abs(back.g - 60) <= 1 && std::abs(back.b - 90) <= 1);
    }

    void base64()
    {
        QCOMPARE(qs(codec::base64Encode("")), u""_s);
        QCOMPARE(qs(codec::base64Encode("f")), u"Zg=="_s);
        QCOMPARE(qs(codec::base64Encode("fo")), u"Zm8="_s);
        QCOMPARE(qs(codec::base64Encode("foo")), u"Zm9v"_s);
        QCOMPARE(qs(codec::base64Encode("你好")), u"5L2g5aW9"_s);
        QCOMPARE(qs(*codec::base64Decode("5L2g5aW9")), u"你好"_s);
        QCOMPARE(qs(*codec::base64Decode("Zm9v")), u"foo"_s);
        QCOMPARE(qs(*codec::base64Decode("Zg")), u"f"_s); // padding optional
        QCOMPARE(qs(*codec::base64Decode("Zm 9v\n")), u"foo"_s); // whitespace ignored
        QVERIFY(!codec::base64Decode("Zg!"));
        QVERIFY(!codec::base64Decode("Z")); // one leftover sextet
        QVERIFY(!codec::base64Decode(""));
        QVERIFY(codec::validUtf8("你好 hello"));
        QVERIFY(!codec::validUtf8(std::string("\xff\xfe", 2)));
        QVERIFY(!codec::validUtf8(std::string("\x01", 1)));
    }

    void hashes()
    {
        QCOMPARE(qs(codec::hashHex("md5", "")), u"d41d8cd98f00b204e9800998ecf8427e"_s);
        QCOMPARE(qs(codec::hashHex("md5", "abc")), u"900150983cd24fb0d6963f7d28e17f72"_s);
        QCOMPARE(qs(codec::hashHex("sha1", "abc")), u"a9993e364706816aba3e25717850c26c9cd0d89d"_s);
        QCOMPARE(qs(codec::hashHex("sha256", "abc")), u"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"_s);
        QCOMPARE(qs(codec::hashHex("sha256", "你好")), u"670d9743542cae3ea7ebe36af56bd53648b0a1126162e78d81a32934a711302e"_s);
        QVERIFY(codec::hashHex("crc32", "abc").empty()); // unknown
        const QString path = QDir::tempPath() + u"/wstools-hash.txt"_s;
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("abc");
        file.close();
        bool tooLarge = false;
        QCOMPARE(qs(codec::hashFileHex("sha256", path.toStdWString(), 1 << 20, &tooLarge)), u"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"_s);
        QVERIFY(!tooLarge);
        QVERIFY(codec::hashFileHex("sha256", path.toStdWString(), 2, &tooLarge).empty());
        QVERIFY(tooLarge);
        QVERIFY(codec::hashFileHex("sha256", L"C:\\no\\such\\file.bin", 1 << 20, &tooLarge).empty());
        QFile::remove(path);
    }

    void uuid()
    {
        const std::string a = codec::uuid4();
        const std::string b = codec::uuid4();
        QCOMPARE(a.size(), std::size_t(36));
        QVERIFY(a != b);
        QCOMPARE(a[14], '4'); // the version
        QVERIFY(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b'); // the variant
        QCOMPARE(a[8], '-');
    }

    void times()
    {
        auto m = codec::parseTimestamp("1700000000");
        QVERIFY(m);
        QCOMPARE(m->seconds, std::int64_t(1700000000));
        QCOMPARE(m->milliseconds, 0);
        QCOMPARE(qs(codec::formatUtc(*m)), u"2023-11-14 22:13:20"_s);
        QCOMPARE(qs(codec::formatIsoUtc(*m)), u"2023-11-14T22:13:20Z"_s);
        m = codec::parseTimestamp("1700000000123"); // milliseconds
        QVERIFY(m);
        QCOMPARE(m->seconds, std::int64_t(1700000000));
        QCOMPARE(m->milliseconds, 123);
        m = codec::parseTimestamp("1700000000.5");
        QVERIFY(m && m->milliseconds == 500);
        m = codec::parseTimestamp("1 700 000 000");
        QVERIFY(m && m->seconds == 1700000000);
        QVERIFY(!codec::parseTimestamp("abc"));
        QVERIFY(!codec::parseTimestamp(""));
        QVERIFY(!codec::parseTimestamp("99999999999999999")); // too long
        QVERIFY(!codec::parseTimestamp("999999999999999")); // beyond FILETIME's years
        m = codec::parseDate("2023-11-14T22:13:20Z");
        QVERIFY(m);
        QCOMPARE(m->seconds, std::int64_t(1700000000));
        m = codec::parseDate("2023-11-15 06:13:20+08:00");
        QVERIFY(m);
        QCOMPARE(m->seconds, std::int64_t(1700000000));
        m = codec::parseDate("2023/11/14 22:13:20 UTC");
        QVERIFY(m && m->seconds == 1700000000);
        m = codec::parseDate("2023-11-14T22:13:20.250Z");
        QVERIFY(m && m->milliseconds == 250);
        // Local time comes back as it went in.
        m = codec::parseDate("2024-01-02 15:04:05");
        QVERIFY(m);
        QCOMPARE(qs(codec::formatLocal(*m)), u"2024-01-02 15:04:05"_s);
        m = codec::parseDate("2024-01-02");
        QVERIFY(m);
        QCOMPARE(qs(codec::formatLocal(*m)), u"2024-01-02 00:00:00"_s);
        QVERIFY(!codec::parseDate("2024-13-02"));
        QVERIFY(!codec::parseDate("2024-01-02 25:00"));
        QVERIFY(!codec::parseDate("report"));
        QVERIFY(!codec::parseDate("2024-01-02 junk"));
        const QString iso = qs(codec::formatIsoLocal(*codec::parseTimestamp("1700000000")));
        QVERIFY(iso.startsWith(u"2023-11-1"));
        QVERIFY(iso.contains(u'T'));
        QVERIFY(iso.endsWith(u":00") || iso.endsWith(u":30") || iso.endsWith(u":45"));
    }
};

QTEST_GUILESS_MAIN(TestTools)
#include "tst_tools.moc"
