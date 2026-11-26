// 公式识别's reader (src/app/platform/FormulaReader): the picture made what
// the model sees exactly as Pillow makes it, and read as Python reads it.
// The reading needs the model (~120 MB, not in the repository): in
// formula-model\ in the build folder, or where WINSHUN_FORMULA_MODEL says;
// without it those tests are skipped.
#include "platform/FormulaReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QTest>

using namespace Qt::StringLiterals;
using ws::FormulaReader;

namespace {

QString data(const QString& name)
{
    return QDir(QString::fromUtf8(WINSHUN_FORMULA_DATA)).filePath(name);
}

QString modelFolder()
{
    const QString given = qEnvironmentVariable("WINSHUN_FORMULA_MODEL");
    return given.isEmpty() ? QString::fromUtf8(WINSHUN_FORMULA_MODEL_DIR) : given;
}

QByteArray pixelsHash(const QImage& image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(QByteArrayView(image.constScanLine(y), image.width() * 3));
    return hash.result().toHex();
}

// Ink's extent: the rows and columns with a pixel darker than the ground.
QRect ink(const QImage& image)
{
    QRect found;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 128)
                found |= QRect(x, y, 1, 1);
    return found;
}

} // namespace

class TestFormulaReader : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QString folder = modelFolder();
        if (!QFileInfo::exists(QDir(folder).filePath(u"decoder_model.onnx"_s)))
            return;
        m_reader = FormulaReader::load(QCoreApplication::applicationDirPath(), folder, 4, &m_why);
        QVERIFY2(m_reader, qPrintable(m_why));
    }

    // What the model sees, byte for byte what Python's Pillow made of the
    // same pictures (the margin, the dark one turned, the bicubic resampling)
    // when the reading was measured.
    void preparesAsPillow_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("hash");
        QTest::newRow("quadratic") << u"quadratic.png"_s << "9a081e7f287abe58c54c8450d5065e8789ba60d58915334164a13d28eb95ba7c"_ba;
        QTest::newRow("product, dark") << u"product-dark.png"_s << "cf6865bca761e53c4c593b759d2c69d8ec091ffa82544fa6d203b380e8f71fa2"_ba;
        QTest::newRow("limit") << u"limit.png"_s << "41b20e6e7d0a7c158636d9ee62a9705b690fccfce9461575335f9460a0a2552f"_ba;
    }

    void preparesAsPillow()
    {
        QFETCH(QString, file);
        QFETCH(QByteArray, hash);
        const QImage picture(data(file));
        QVERIFY(!picture.isNull());
        const QImage seen = FormulaReader::prepare(picture);
        QCOMPARE(seen.size(), QSize(384, 384));
        QCOMPARE(seen.format(), QImage::Format_RGB888);
        QCOMPARE(pixelsHash(seen), hash);
    }

    void preparesWhatIsThere()
    {
        // Nothing stands out: nothing to read.
        QImage blank(200, 80, QImage::Format_RGB32);
        blank.fill(QColor(30, 30, 30));
        QVERIFY(FormulaReader::prepare(blank).isNull());
        QVERIFY(FormulaReader::prepare({}).isNull());

        // A bar anywhere on a large ground: cut to it, its margin around, dark on light.
        QImage dark(900, 500, QImage::Format_RGB32);
        dark.fill(QColor(20, 24, 28));
        {
            QPainter painter(&dark);
            painter.fillRect(QRect(600, 300, 200, 50), QColor(240, 240, 240));
        }
        const QImage seen = FormulaReader::prepare(dark);
        QCOMPARE(qGray(seen.pixel(0, 0)), 255);
        // 200 x 50 with 15 around (0.3 of 50): 230 x 80, made 384 x 384.
        const QRect bar = ink(seen);
        QVERIFY2(qAbs(bar.left() - 384 * 15 / 230) <= 2 && qAbs(bar.right() - 384 * 215 / 230) <= 2, qPrintable(u"%1..%2"_s.arg(bar.left()).arg(bar.right())));
        QVERIFY2(qAbs(bar.top() - 384 * 15 / 80) <= 2 && qAbs(bar.bottom() - 384 * 65 / 80) <= 2, qPrintable(u"%1..%2"_s.arg(bar.top()).arg(bar.bottom())));

        // A pin of a transparent PNG: over white, not over black.
        QImage clear(300, 100, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        {
            QPainter painter(&clear);
            painter.fillRect(QRect(100, 40, 100, 20), Qt::black);
        }
        const QImage over = FormulaReader::prepare(clear);
        QVERIFY(!over.isNull());
        QCOMPARE(qGray(over.pixel(0, 0)), 255);
        QCOMPARE(qGray(over.pixel(192, 192)), 0);
    }

    // As Python read them (evalpre.py, variant 3) with the same model.
    void reads_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("latex");
        QTest::newRow("quadratic") << u"quadratic.png"_s
                                   << uR"(x = \frac { - b \pm \sqrt { b ^ { 2 } - 4 a c \, } } { 2 a })"_s;
        QTest::newRow("product, dark") << u"product-dark.png"_s
                                       << uR"(\underset { i \mathop { = } 1 } { \overset { n } { \prod } } x _ { i } \leq \bigg ( \frac { 1 } { n } \underset { i \mathop { = } 1 } { \overset { n } { \sum } } x _ { i } \bigg ) ^ { n })"_s;
        QTest::newRow("limit") << u"limit.png"_s
                               << uR"(f \! ( x ) = \underset { h \to 0 } { \operatorname* { l i m } } \frac { f ( x + h ) - f \! ( x ) } { h })"_s;
    }

    void reads()
    {
        if (!m_reader)
            QSKIP("no formula model: put it in formula-model\\ beside the test or set WINSHUN_FORMULA_MODEL");
        QFETCH(QString, file);
        QFETCH(QString, latex);
        QElapsedTimer timer;
        timer.start();
        QString why;
        const std::optional<FormulaReader::Reading> reading = m_reader->read(QImage(data(file)), &why);
        QVERIFY2(reading, qPrintable(why));
        qInfo().noquote() << file << timer.elapsed() << "ms, score" << reading->score;
        QCOMPARE(reading->latex, latex);
        QVERIFY(reading->score >= FormulaReader::kSure);
        QVERIFY(!reading->cut);
    }

    void readsNothingFromNothing()
    {
        if (!m_reader)
            QSKIP("no formula model");
        QImage blank(300, 100, QImage::Format_RGB32);
        blank.fill(Qt::white);
        const std::optional<FormulaReader::Reading> reading = m_reader->read(blank);
        QVERIFY(reading);
        QVERIFY(reading->latex.isEmpty());
    }

    void stopsWhenCancelled()
    {
        if (!m_reader)
            QSKIP("no formula model");
        const std::atomic_bool cancelled {true};
        QString why;
        QVERIFY(!m_reader->read(QImage(data(u"quadratic.png"_s)), &why, &cancelled));
        QCOMPARE(why, u"cancelled"_s);
    }

    void missingModel()
    {
        QString why;
        QVERIFY(!FormulaReader::load(QCoreApplication::applicationDirPath(), {}, 1, &why));
        QVERIFY(!why.isEmpty());
        const QString empty = QDir::temp().filePath(u"winshun-no-formula-model"_s);
        QVERIFY(!FormulaReader::load(QCoreApplication::applicationDirPath(), empty, 1, &why));
        QVERIFY(!why.isEmpty());
    }

    // Every picture in WINSHUN_FORMULA_CORPUS read, "name<TAB>score<TAB>latex"
    // a line into formula-corpus.txt beside the test, for the measuring
    // script (docs/validation/formula-ocr).
    void corpus()
    {
        const QString folder = qEnvironmentVariable("WINSHUN_FORMULA_CORPUS");
        if (folder.isEmpty() || !m_reader)
            QSKIP("set WINSHUN_FORMULA_CORPUS (and have the model) to read a corpus");
        QFile out(QCoreApplication::applicationDirPath() + u"/formula-corpus.txt"_s);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QElapsedTimer timer;
        timer.start();
        const QStringList names = QDir(folder).entryList({u"*.png"_s}, QDir::Files, QDir::Name);
        for (const QString& name : names) {
            QString why;
            const auto reading = m_reader->read(QImage(QDir(folder).filePath(name)), &why);
            QVERIFY2(reading, qPrintable(why));
            out.write(u"%1\t%2\t%3\n"_s.arg(name).arg(reading->score, 0, 'f', 4).arg(reading->latex).toUtf8());
        }
        qInfo().noquote() << names.size() << "pictures," << timer.elapsed() / std::max<qsizetype>(1, names.size()) << "ms each";
    }

private:
    std::unique_ptr<FormulaReader> m_reader;
    QString m_why;
};

QTEST_GUILESS_MAIN(TestFormulaReader)
#include "tst_formulareader.moc"
