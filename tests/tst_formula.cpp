// 公式识别's LaTeX (src/capture/Latex*): the formula model's spelling read
// and tidied into what a person writes, that LaTeX read again unchanged,
// and the MathML and Typst it is copied as.

#include "FormulaPicture.h"
#include "Latex.h"

#include <QDir>
#include <QFile>
#include <QTest>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;
using namespace ws::capture::latex;

namespace {

// What the model wrote (from the prototype's runs on rendered formulas and
// on the user's screenshot) and the LaTeX the source field shows for it.
struct Case {
    const char* name;
    QString model;
    QString tidy;
};

QList<Case> cases()
{
    return {
        {"limit", u"\\operatorname* { l i m } _ { h \\to 0 } \\frac { f ( x + h ) - f ( x ) } { h }"_s,
            u"\\lim_{h \\to 0} \\frac{f(x + h) - f(x)}{h}"_s},
        {"root of", u"\\root 3 \\of { x ^ { 3 } + y ^ { 3 } }"_s, u"\\sqrt[3]{x^3 + y^3}"_s},
        {"underset sum", u"\\underset { i = 1 } { \\overset { n } { \\sum } } a _ { i }"_s, u"\\sum_{i=1}^n a_i"_s},
        {"bf switch", u"{ \\bf 1 } _ { A }"_s, u"\\mathbf{1}_A"_s},
        {"negative spaces", u"x \\! = \\! 1"_s, u"x = 1"_s},
        {"mathrm words", u"a = 1 \\mathrm { ~ o r ~ } b = 2"_s, u"a = 1 \\text{ or } b = 2"_s},
        {"mathrm function", u"\\mathrm { l n } x"_s, u"\\ln x"_s},
        {"operatorname function", u"\\operatorname { s i n } x"_s, u"\\sin x"_s},
        {"operatorname name", u"\\operatorname { r a n k } A"_s, u"\\operatorname{rank} A"_s},
        {"mathop relation", u"a \\mathop { = } b"_s, u"a = b"_s},
        {"mathrm digit", u"\\mathrm { 2 } x"_s, u"2x"_s},
        {"mathrm d", u"\\int f ( x ) \\, \\mathrm { d } x"_s, u"\\int f(x) \\, \\mathrm{d}x"_s},
        {"fence", u"\\left( \\frac { a } { b } \\right)"_s, u"\\left(\\frac{a}{b}\\right)"_s},
        {"left alone", u"\\left( x + 1"_s, u"(x + 1"_s},
        {"right alone", u"x + 1 \\right)"_s, u"x + 1)"_s},
        {"left dot right dot", u"\\left. x \\right."_s, u"x"_s},
        {"prime", u"f ^ { \\prime } ( x )"_s, u"f'(x)"_s},
        {"times", u"\\frac { 1 } { 2 } \\times 3"_s, u"\\frac{1}{2} \\times 3"_s},
        {"unary minus", u"- 1 + x"_s, u"-1 + x"_s},
        {"unary after operator", u"a \\cdot - b"_s, u"a \\cdot -b"_s},
        {"limits dropped", u"\\sum \\limits _ { i } x _ { i }"_s, u"\\sum_i x_i"_s},
        {"integral limits kept", u"\\int \\limits _ { 0 } ^ { 1 } x"_s, u"\\int\\limits_0^1 x"_s},
        {"thin space meant", u"\\int _ { 0 } ^ { 1 } f ( x ) \\, d x"_s, u"\\int_0^1 f(x) \\, dx"_s},
        {"thin spaces around relation", u"x \\, = \\, 1"_s, u"x = 1"_s},
        {"spaces run", u"x \\; \\; \\; y"_s, u"x \\; y"_s},
        {"wide space kept", u"x = 1 , \\quad y = 2"_s, u"x = 1, \\quad y = 2"_s},
        {"not equal", u"x \\not = y"_s, u"x \\neq y"_s},
        {"prescript", u"{ } ^ { 1 4 } C"_s, u"{}^{14}C"_s},
        {"text spaced out", u"\\text { h e l l o }"_s, u"\\text{hello}"_s},
        {"blackboard", u"\\mathbb { R } ^ { n }"_s, u"\\mathbb{R}^n"_s},
        {"over", u"{ a \\over b }"_s, u"\\frac{a}{b}"_s},
        {"euler", u"e ^ { i \\pi } + 1 = 0"_s, u"e^{i\\pi} + 1 = 0"_s},
        {"list", u"a _ { 1 } , a _ { 2 } , \\ldots , a _ { n }"_s, u"a_1, a_2, \\ldots, a_n"_s},
        {"quadratic", u"x = \\frac { - b \\pm \\sqrt { b ^ { 2 } - 4 a c } } { 2 a }"_s,
            u"x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}"_s},
        {"braces", u"{ { x } ^ { 2 } } + { y }"_s, u"x^2 + y"_s},
        {"style", u"\\displaystyle \\sum _ { k } k"_s, u"\\sum_k k"_s},
        {"pmatrix", u"\\begin{pmatrix} 1 & 0 \\\\ 0 & 1 \\end{pmatrix}"_s,
            u"\\begin{pmatrix}\n1 & 0 \\\\\n0 & 1\n\\end{pmatrix}"_s},
        {"cases", u"| x | = \\left\\{ \\begin{array} { l l } { x , } & { x \\geq 0 } \\\\ { - x , } & { x < 0 } \\end{array} \\right."_s,
            u"|x| = \\begin{cases}\nx, & x \\geq 0 \\\\\n-x, & x < 0\n\\end{cases}"_s},
        {"array matrix", u"\\left( \\begin{array} { c c } { 1 } & { 2 } \\\\ { 3 } & { 4 } \\end{array} \\right)"_s,
            u"\\begin{pmatrix}\n1 & 2 \\\\\n3 & 4\n\\end{pmatrix}"_s},
        {"array left aligned", u"\\left( \\begin{array} { l l } { 1 } & { 2 } \\\\ { 3 } & { 4 } \\end{array} \\right)"_s,
            u"\\left(\\begin{array}{ll}\n1 & 2 \\\\\n3 & 4\n\\end{array}\\right)"_s},
        {"aligned one row", u"\\begin{aligned} { a } & { = b + c } \\end{aligned}"_s, u"a = b + c"_s},
        {"last row empty", u"\\begin{matrix} a \\\\ b \\\\ \\end{matrix}"_s, u"\\begin{matrix}\na \\\\\nb\n\\end{matrix}"_s},
        {"end alone", u"x \\end{array} + 1"_s, u"x + 1"_s},
        {"brace alone", u"x } + 1"_s, u"x + 1"_s},
        {"hat", u"\\hat { x } + \\vec { v }"_s, u"\\hat{x} + \\vec{v}"_s},
        {"sized", u"\\bigl( x \\bigr)"_s, u"\\bigl(x\\bigr)"_s},
        {"unknown command", u"\\foo { x } + 1"_s, u"\\foo{x} + 1"_s},
        {"double script", u"x ^ { 2 } ^ { 3 }"_s, u"{x^2}^3"_s},
        {"binom", u"\\binom { n } { k }"_s, u"\\binom{n}{k}"_s},
        {"choose", u"{ n \\choose k }"_s, u"\\binom{n}{k}"_s},
        {"text escapes", u"\\text { 5 \\% }"_s, u"\\text{5\\%}"_s},
        {"function power", u"\\sin ^ { 2 } x + \\cos ^ { 2 } x = 1"_s, u"\\sin^2 x + \\cos^2 x = 1"_s},
    };
}

QString dumpPath()
{
    return qEnvironmentVariable("WINSHUN_FORMULA_DUMP");
}

} // namespace

class TestFormula : public QObject {
    Q_OBJECT

private slots:
    void tidiesTheModel_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("written");
        for (const Case& c : cases())
            QTest::newRow(c.name) << c.model << c.tidy;
    }

    void tidiesTheModel()
    {
        QFETCH(QString, model);
        QFETCH(QString, written);
        QCOMPARE(tidyModel(model), written);
    }

    // What the source field shows is read again as written and stays the same.
    void writesWhatItReads_data() { tidiesTheModel_data(); }

    void writesWhatItReads()
    {
        QFETCH(QString, written);
        QCOMPARE(write(tidy(parse(written))), written);
    }

    // Nothing is refused, and no source makes it loop or overflow.
    void readsAnything()
    {
        const QStringList odd {u""_s, u"{"_s, u"}"_s, u"\\"_s, u"^"_s, u"_{"_s, u"\\left"_s, u"\\right"_s,
            u"\\begin"_s, u"\\begin{"_s, u"\\end{x}"_s, u"&"_s, u"\\\\"_s, u"\\frac"_s, u"\\sqrt["_s,
            u"\\text{"_s, u"x^"_s, u"\\left( \\right"_s, u"\\begin{matrix} a & \\left( b \\\\ c \\end{matrix}"_s,
            u"''''"_s, u"\\root"_s, u"\\over"_s, u"%comment"_s, u"😀^2"_s};
        for (const QString& source : odd) {
            const Node formula = tidy(parse(source, Source::Model));
            const QString written = write(formula);
            QVERIFY(!toMathMl(formula).isEmpty());
            Q_UNUSED(toTypst(formula));
            Q_UNUSED(written);
        }
        QString deep;
        for (int i = 0; i < 5000; ++i)
            deep += u"{\\frac{"_s;
        QVERIFY(!toMathMl(tidy(parse(deep))).isEmpty());
    }

    void mathMl_data()
    {
        QTest::addColumn<QString>("latex");
        QTest::addColumn<QString>("mathMl");
        QTest::newRow("script") << u"x^2"_s << u"<msup><mi>x</mi><mn>2</mn></msup>"_s;
        QTest::newRow("fraction") << u"\\frac{a}{b}"_s << u"<mfrac><mi>a</mi><mi>b</mi></mfrac>"_s;
        QTest::newRow("sum") << u"\\sum_{i=1}^n"_s
                             << u"<munderover><mo>∑</mo><mrow><mi>i</mi><mo>=</mo><mn>1</mn></mrow><mi>n</mi></munderover>"_s;
        QTest::newRow("integral") << u"\\int_0^1"_s << u"<msubsup><mo>∫</mo><mn>0</mn><mn>1</mn></msubsup>"_s;
        QTest::newRow("function") << u"\\sin x"_s << u"<mi>sin</mi><mo>&#x2061;</mo><mi>x</mi>"_s;
        QTest::newRow("limit") << u"\\lim_{x \\to 0} x"_s
                               << u"<munder><mi>lim</mi><mrow><mi>x</mi><mo>→</mo><mn>0</mn></mrow></munder><mo>&#x2061;</mo><mi>x</mi>"_s;
        QTest::newRow("number") << u"3.14 r"_s << u"<mn>3.14</mn><mi>r</mi>"_s;
        QTest::newRow("minus") << u"a - b"_s << u"<mi>a</mi><mo>−</mo><mi>b</mi>"_s;
        QTest::newRow("fence") << u"\\left(x\\right)"_s
                               << u"<mrow><mo fence=\"true\" form=\"prefix\">(</mo><mi>x</mi><mo fence=\"true\" form=\"postfix\">)</mo></mrow>"_s;
        QTest::newRow("paren") << u"f(x)"_s
                               << u"<mi>f</mi><mo stretchy=\"false\">(</mo><mi>x</mi><mo stretchy=\"false\">)</mo>"_s;
        QTest::newRow("bold") << u"\\mathbf{v}"_s << u"<mi mathvariant=\"bold\">v</mi>"_s;
        QTest::newRow("capital greek") << u"\\Gamma"_s << u"<mi mathvariant=\"normal\">Γ</mi>"_s;
        QTest::newRow("escaped") << u"a < b"_s << u"<mi>a</mi><mo>&lt;</mo><mi>b</mi>"_s;
        QTest::newRow("words") << u"\\text{ or }"_s << u"<mtext>\u00a0or\u00a0</mtext>"_s;
        QTest::newRow("root") << u"\\sqrt[3]{x}"_s << u"<mroot><mi>x</mi><mn>3</mn></mroot>"_s;
        QTest::newRow("prime") << u"f'"_s << u"<msup><mi>f</mi><mo>′</mo></msup>"_s;
        QTest::newRow("accent") << u"\\hat{x}"_s
                                << u"<mover accent=\"true\"><mi>x</mi><mo stretchy=\"false\">^</mo></mover>"_s;
        QTest::newRow("matrix") << u"\\begin{pmatrix}1&0\\\\0&1\\end{pmatrix}"_s
                                << u"<mrow><mo fence=\"true\" form=\"prefix\">(</mo><mtable><mtr><mtd><mn>1</mn></mtd><mtd><mn>0</mn></mtd></mtr><mtr><mtd><mn>0</mn></mtd><mtd><mn>1</mn></mtd></mtr></mtable><mo fence=\"true\" form=\"postfix\">)</mo></mrow>"_s;
        QTest::newRow("cases") << u"\\begin{cases}1&x>0\\\\0&x\\le 0\\end{cases}"_s
                               << u"<mrow><mo fence=\"true\" form=\"prefix\">{</mo><mtable columnalign=\"left left\"><mtr><mtd><mn>1</mn></mtd><mtd><mrow><mi>x</mi><mo>&gt;</mo><mn>0</mn></mrow></mtd></mtr><mtr><mtd><mn>0</mn></mtd><mtd><mrow><mi>x</mi><mo>≤</mo><mn>0</mn></mrow></mtd></mtr></mtable></mrow>"_s;
    }

    void mathMl()
    {
        QFETCH(QString, latex);
        QFETCH(QString, mathMl);
        const QString head = u"<math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\"block\">"_s;
        QCOMPARE(toMathMl(tidy(parse(latex))), head + mathMl + u"</math>"_s);
    }

    // Every case's MathML is well-formed XML.
    void mathMlIsXml()
    {
        for (const Case& c : cases()) {
            QXmlStreamReader reader(toMathMl(tidy(parse(c.tidy))));
            while (!reader.atEnd())
                reader.readNext();
            QVERIFY2(!reader.hasError(), qPrintable(c.tidy + u": "_s + reader.errorString()));
        }
    }

    void typst_data()
    {
        QTest::addColumn<QString>("latex");
        QTest::addColumn<QString>("typst");
        QTest::newRow("fraction") << u"\\frac{a+b}{2}"_s << u"(a + b)/2"_s;
        QTest::newRow("scripts") << u"x^{2} + x_{n+1}"_s << u"x^2 + x_(n + 1)"_s;
        QTest::newRow("letters") << u"\\alpha\\beta x"_s << u"alpha beta x"_s;
        QTest::newRow("roots") << u"\\sqrt{x} + \\sqrt[3]{y}"_s << u"sqrt(x) + root(3, y)"_s;
        QTest::newRow("sum") << u"\\sum_{i=1}^{n} i"_s << u"sum_(i = 1)^n i"_s;
        QTest::newRow("limit") << u"\\lim_{x \\to 0} f(x)"_s << u"lim_(x -> 0) f(x)"_s;
        QTest::newRow("blackboard") << u"\\mathbb{R}"_s << u"bb(R)"_s;
        QTest::newRow("matrix") << u"\\begin{pmatrix}1&2\\\\3&4\\end{pmatrix}"_s << u"mat(1, 2; 3, 4)"_s;
        QTest::newRow("bmatrix") << u"\\begin{bmatrix}a&b\\end{bmatrix}"_s << u"mat(delim: \"[\", a, b)"_s;
        QTest::newRow("cases") << u"\\begin{cases}x&x>0\\\\-x&x\\le 0\\end{cases}"_s
                               << u"cases(x & x > 0, -x & x <= 0)"_s;
        QTest::newRow("words") << u"x \\text{ if } y"_s << u"x \" if \" y"_s;
        QTest::newRow("absolute") << u"\\left|x\\right|"_s << u"abs(x)"_s;
        QTest::newRow("accents") << u"\\hat{x} + \\ddot{y}"_s << u"hat(x) + accent(y, dot.double)"_s;
        QTest::newRow("integral") << u"\\int_0^1 f(x) \\, dx"_s << u"integral_0^1 f(x) thin d x"_s;
        QTest::newRow("comma in argument") << u"\\sqrt{a, b}"_s << u"sqrt(a\\, b)"_s;
        QTest::newRow("upright name") << u"\\mathrm{kg}"_s << u"\"kg\""_s;
        QTest::newRow("operator") << u"\\operatorname{rank} A"_s << u"op(\"rank\") A"_s;
        QTest::newRow("aligned") << u"\\begin{aligned}a&=1\\\\b&=2\\end{aligned}"_s << u"a & = 1 \\\nb & = 2"_s;
        QTest::newRow("slash") << u"a/b"_s << u"a \\/ b"_s;
        QTest::newRow("prime") << u"f'(x)"_s << u"f'(x)"_s;
        QTest::newRow("double script") << u"{x^2}^3"_s << u"attach(x^2, t: 3)"_s;
        QTest::newRow("not equal") << u"x \\neq y"_s << u"x != y"_s;
        QTest::newRow("factorial") << u"n! = 1"_s << u"n! = 1"_s;
        QTest::newRow("after relation") << u"x = \\frac{-b}{2a}"_s << u"x = (-b)/(2 a)"_s;
        QTest::newRow("after scripts") << u"\\lim_{h \\to 0} \\frac{1}{h}"_s << u"lim_(h -> 0) 1/h"_s;
        QTest::newRow("Vmatrix") << u"\\begin{Vmatrix}a\\end{Vmatrix}"_s << u"mat(delim: \"‖\", a)"_s;
    }

    void typst()
    {
        QFETCH(QString, latex);
        QFETCH(QString, typst);
        QCOMPARE(toTypst(tidy(parse(latex))), typst);
    }

    // MicroTeX lays out every LaTeX the tidying writes, and draws ink.
    void drawsWhatItWrites()
    {
        for (const Case& c : cases()) {
            const ws::capture::FormulaPicture picture(c.tidy, 24, Qt::black);
            QVERIFY2(picture.isValid(), qPrintable(c.tidy + u": "_s + picture.error()));
            QVERIFY(picture.size().width() > 8 && picture.size().height() > 8);
            const QImage image = picture.image(1, 4, Qt::white);
            bool ink = false;
            for (int y = 0; y < image.height() && !ink; ++y) {
                for (int x = 0; x < image.width() && !ink; ++x)
                    ink = qGray(image.pixel(x, y)) < 128;
            }
            QVERIFY2(ink, qPrintable(c.tidy));
        }
    }

    void drawing()
    {
        using ws::capture::FormulaPicture;
        // A sum on its own line has its limits under and over it: taller than inline.
        const FormulaPicture sum(u"\\sum_{i=1}^n i"_s, 20, Qt::black);
        const FormulaPicture letter(u"x"_s, 20, Qt::black);
        QVERIFY(sum.size().height() > 2.5 * letter.size().height());
        // So do \lim and \max (MicroTeX's \mathop, changed) and \operatorname*
        // (defined for it): taller than with them beside (\nolimits, \operatorname).
        const auto height = [](const QString& latex) { return FormulaPicture(latex, 20, Qt::black).size().height(); };
        QVERIFY(height(u"\\lim_{h \\to 0} x"_s) > 1.2 * height(u"\\lim\\nolimits_{h \\to 0} x"_s));
        QVERIFY(height(u"\\max_{a} x"_s) > 1.2 * height(u"\\max\\nolimits_{a} x"_s));
        QVERIFY(height(u"\\operatorname*{argmax}_{a} x"_s) > 1.2 * height(u"\\operatorname{argmax}_{a} x"_s));
        // Twice the size, twice as large.
        const FormulaPicture large(u"\\frac{a}{b}"_s, 40, Qt::black);
        const FormulaPicture small(u"\\frac{a}{b}"_s, 20, Qt::black);
        QVERIFY(qAbs(large.size().height() - 2 * small.size().height()) <= 3);
        // Chinese words in \text{}: drawn with Windows' fonts.
        const FormulaPicture words(u"x \\text{当且仅当} y"_s, 20, Qt::black);
        QVERIFY(words.isValid());
        QVERIFY(words.size().width() > 4 * 20);
        // A command nobody knows: drawn as it is written, in red, for the
        // user to see what to change.
        const FormulaPicture unknown(u"x + \\notacommandatall"_s, 20, Qt::black);
        QVERIFY(unknown.isValid());
        const QImage marked = unknown.image(1, 2, Qt::white);
        bool red = false;
        for (int y = 0; y < marked.height() && !red; ++y) {
            for (int x = 0; x < marked.width() && !red; ++x) {
                const QColor c = marked.pixelColor(x, y);
                red = c.red() > 200 && c.green() < 80 && c.blue() < 80;
            }
        }
        QVERIFY(red);
        // A brace closed that was never opened: nothing drawn, and why (the
        // window says the formula is wrong). One left open is drawn as if closed.
        const FormulaPicture extra(u"\\frac{a}{b}}"_s, 20, Qt::black);
        QVERIFY(!extra.isValid() && !extra.error().isEmpty());
        QVERIFY(FormulaPicture(u"\\frac{a}{b"_s, 20, Qt::black).isValid());
        // Nothing: no picture and no complaint.
        const FormulaPicture empty(u"  "_s, 20, Qt::black);
        QVERIFY(!empty.isValid() && empty.error().isEmpty());
        // The picture at a screen's scale is that many pixels.
        const QImage sharp = small.image(2, 0, Qt::white);
        QCOMPARE(sharp.width(), qCeil(small.size().width() * 2));
        // SVG with the letters as outlines.
        const QByteArray svg = small.svg(2);
        QVERIFY(svg.contains("<svg"));
        QVERIFY(svg.contains("<path"));
    }

    // With WINSHUN_FORMULA_DUMP set, every case's LaTeX, MathML and Typst
    // in a file, and its picture beside it, for the checks outside
    // (Typst's compiler, Word, the eye).
    void dump()
    {
        const QString path = dumpPath();
        if (path.isEmpty())
            QSKIP("WINSHUN_FORMULA_DUMP not set");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QString out;
        const QDir pictures = QFileInfo(path).dir();
        int index = 0;
        for (const Case& c : cases()) {
            const Node formula = tidy(parse(c.tidy));
            out += u"#name "_s + QString::fromLatin1(c.name) + u'\n';
            out += u"#latex\n"_s + c.tidy + u'\n';
            out += u"#mathml\n"_s + toMathMl(formula) + u'\n';
            out += u"#typst\n"_s + toTypst(formula) + u'\n';
            const ws::capture::FormulaPicture picture(c.tidy, 22, Qt::black);
            picture.image(1, 6, Qt::white).save(pictures.filePath(u"formula-%1.png"_s.arg(index++, 2, 10, u'0')));
        }
        file.write(out.toUtf8());
    }
};

QTEST_MAIN(TestFormula)
#include "tst_formula.moc"
