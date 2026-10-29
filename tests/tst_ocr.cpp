// The words in pictures (PaddleOcr): the models and onnxruntime.dll the
// build put next to this test, on text drawn as a screenshot shows it.

#include "Geometry.h"
#include "PaddleOcr.h"
#include "SmartRedact.h"
#include "CaptureOcr.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QTest>
#include <QThread>

#include <cmath>

using namespace Qt::StringLiterals;

class TestOcr : public QObject {
    Q_OBJECT

    static QImage drawn(const QStringList& lines, int px, QColor ink, QColor paper)
    {
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(px);
        QImage image(px * 24, px * 2 * static_cast<int>(lines.size()) + px, QImage::Format_RGB32);
        image.fill(paper);
        QPainter painter(&image);
        painter.setFont(font);
        painter.setPen(ink);
        for (qsizetype i = 0; i < lines.size(); ++i)
            painter.drawText(px, static_cast<int>(px * 2 * (i + 1)), lines[i]);
        return image;
    }

    static QString read(const QImage& image)
    {
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        if (!ocr) {
            qWarning().noquote() << error;
            return {};
        }
        QStringList lines;
        if (!ocr->read(image, &lines, &error))
            qWarning().noquote() << error;
        return lines.join(u'\n');
    }

private slots:
    void installed() { QVERIFY(ws::PaddleOcr::installed(QCoreApplication::applicationDirPath())); }

    void backgroundScanCropsAndReturnsGray()
    {
        using namespace ws::capture;
        const QImage words = drawn({u"13812345678"_s}, 24, Qt::black, Qt::white);
        QImage frozen(words.width() + 160, words.height() + 100, QImage::Format_RGB32);
        frozen.fill(Qt::red);
        const QRect region(QPoint(70, 40), words.size());
        { QPainter painter(&frozen); painter.drawImage(region.topLeft(), words); }
        CaptureOcr reader;
        bool done = false;
        RedactScan result;
        reader.scan(frozen, region, 1.5, [&](RedactScan scan) {
            QCOMPARE(QThread::currentThread(), reader.thread());
            result = std::move(scan);
            done = true;
        });
        frozen.fill(Qt::blue); // the queued job owns a snapshot, even if its caller edits the image
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QCOMPARE(result.gray, words.convertToFormat(QImage::Format_Grayscale8));
        QVERIFY(!result.lines.isEmpty());
        QVERIFY(result.lines.first().text.contains(u"13812345678"_s));
        for (const RedactLine& line : result.lines) {
            QVERIFY(words.rect().contains(line.quad.boundingRect().toAlignedRect()));
        }
        done = false;
        reader.scan(words, 1.5, [&](QList<RedactLine> lines) {
            QVERIFY(!lines.isEmpty());
            QCOMPARE(lines.first().text, result.lines.first().text); // the pin menu's existing entry still works
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        done = false;
        reader.scan(words, words.rect(), 1.5, [&](RedactScan) { done = true; });
        reader.cancelScan();
        QTest::qWait(500);
        QVERIFY(!done);
    }

    // Small UI text, Chinese with English and decimals, one line each.
    void screenshotText()
    {
        const QString words = read(drawn({u"机器人暂停中"_s, u"起压 12.35 终止 18.402"_s, u"Press Ctrl twice"_s}, 14, Qt::black, Qt::white));
        const QStringList lines = words.split(u'\n');
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0], u"机器人暂停中"_s);
        QVERIFY2(lines[1].contains(u"12.35"_s) && lines[1].contains(u"18.402"_s), qPrintable(lines[1]));
        QVERIFY2(lines[2].contains(u"Ctrl"_s), qPrintable(lines[2]));
    }

    // Light text on dark, as in a dark theme.
    void darkTheme()
    {
        QCOMPARE(read(drawn({u"剪贴板历史"_s}, 16, QColor(230, 230, 230), QColor(32, 32, 32))), u"剪贴板历史"_s);
    }

    // A long screenshot, looked at in strips: every line once.
    void tallPicture()
    {
        QStringList lines;
        for (int i = 0; i < 120; ++i)
            lines.append(u"第%1行 文字识别"_s.arg(i));
        const QString words = read(drawn(lines, 14, Qt::black, Qt::white));
        for (int i : {0, 59, 60, 61, 119})
            QVERIFY2(words.count(u"第%1行"_s.arg(i)) == 1, qPrintable(u"line %1"_s.arg(i)));
    }

    // ---- smart redaction: where the characters are (docs/design-capture-roadmap.md, 2.3, item 2) ----

    // The columns with any ink in `image`: first and last.
    static QPair<int, int> inkColumns(const QImage& image, QColor paper)
    {
        int first = -1, last = -1;
        for (int x = 0; x < image.width(); ++x) {
            bool ink = false;
            for (int y = 0; y < image.height() && !ink; ++y)
                ink = QColor(image.pixel(x, y)) != paper;
            if (ink) {
                if (first < 0)
                    first = x;
                last = x;
            }
        }
        return {first, last};
    }

    static QImage drawnAt(const QString& text, const QString& family, int logicalPx, double dpr, QPoint origin, QSize size)
    {
        QFont font(family);
        font.setPixelSize(int(std::lround(logicalPx * dpr)));
        QImage image(size, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(origin, text);
        return image;
    }

    void boxesFollowTheDigits_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<int>("px");
        QTest::addColumn<double>("dpr");
        for (const QString family : {u"Microsoft YaHei"_s, u"Consolas"_s}) {
            for (int px : {12, 16, 20, 24}) {
                for (double dpr : {1.0, 1.5})
                    QTest::addRow("%s %d px %d%%", qPrintable(family), px, int(dpr * 100)) << family << px << dpr;
            }
        }
    }

    // "电话 13812345678 转 8": the box of the number covers its digits within
    // 2 px at each end and does not reach 电话.
    void boxesFollowTheDigits()
    {
        QFETCH(QString, family);
        QFETCH(int, px);
        QFETCH(double, dpr);
        const QString text = u"电话 13812345678 转 8"_s;
        const int pad = int(std::lround(px * dpr));
        const QPoint origin(pad * 2, pad * 2);
        const QSize size(pad * 2 * 2 + pad * 14, pad * 4);
        const QImage image = drawnAt(text, family, px, dpr, origin, size);
        // The truth: the ink of the digits drawn alone at the same place,
        // and of the two characters before them.
        QFont font(family);
        font.setPixelSize(int(std::lround(px * dpr)));
        const QFontMetricsF fm(font);
        const QString before = u"电话 "_s;
        const QPoint digitsAt(origin.x() + int(std::lround(fm.horizontalAdvance(before))), origin.y());
        const QPair<int, int> digits = inkColumns(drawnAt(u"13812345678"_s, family, px, dpr, digitsAt, size), Qt::white);
        const QPair<int, int> word = inkColumns(drawnAt(u"电话"_s, family, px, dpr, origin, size), Qt::white);
        QVERIFY(digits.first > word.second);

        // As CaptureOcr reads: larger first below 150 %, positions divided back.
        const double scale = ws::capture::ocrUpscale(image.size(), dpr);
        QImage look = image;
        if (scale > 1.0)
            look = image.scaled(int(std::lround(image.width() * scale)), int(std::lround(image.height() * scale)), Qt::IgnoreAspectRatio,
                Qt::SmoothTransformation);
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        QVERIFY2(ocr, qPrintable(error));
        QList<ws::OcrPiece> pieces;
        QVERIFY2(ocr->readPieces(look, &pieces, &error), qPrintable(error));
        // The line with the number (the reader may cut "转 8" off as a line of its own).
        const ws::OcrPiece* piece = nullptr;
        for (const ws::OcrPiece& p : std::as_const(pieces)) {
            if (p.text.contains(u"13812345678"_s))
                piece = &p;
        }
        QVERIFY(piece);
        ws::capture::RedactLine line;
        line.text = piece->text;
        for (const QPointF& p : piece->quad)
            line.quad.append(p / scale);
        line.chars = piece->chars;
        QCOMPARE(line.chars.size(), line.text.size());
        const int from = int(line.text.indexOf(u"13812345678"_s));
        const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
        const QRect rough = ws::capture::matchBox(line, from, from + 11);
        const QRect box = ws::capture::matchBox(line, from, from + 11, gray);
        QString runs; // the ink round the number's right end, for a failure to be understood
        {
            bool inRun = false;
            for (int x = digits.second - 10; x < std::min(image.width(), digits.second + 4 * px); ++x) {
                bool ink = false;
                for (int y = 0; y < gray.height() && !ink; ++y)
                    ink = qGray(gray.pixel(x, y)) < 128;
                if (ink != inRun) {
                    runs += (ink ? u" ["_s : u"-"_s) + QString::number(ink ? x : x - 1) + (ink ? u""_s : u"]"_s);
                    inRun = ink;
                }
            }
        }
        const QString report = u"box %1..%2 (reader alone %6..%7), digits %3..%4, word ends %5, ink%8, chars %9 after the number %10, quad %11..%12"_s
                                   .arg(box.left()).arg(box.right()).arg(digits.first).arg(digits.second).arg(word.second)
                                   .arg(rough.left()).arg(rough.right()).arg(runs)
                                   .arg(line.chars.size()).arg(line.text.mid(from + 11))
                                   .arg(line.quad[0].x()).arg(line.quad[1].x());
        QVERIFY2(std::abs(box.left() - digits.first) <= 2, qPrintable(report));
        QVERIFY2(std::abs(box.right() - digits.second) <= 2, qPrintable(report));
        QVERIFY2(box.left() > word.second, qPrintable(report));
        // And findSensitive finds that one number and nothing else.
        const QList<ws::capture::RedactHit> hits = ws::capture::findSensitive({line}, ws::capture::RedactRules(), {}, gray);
        QCOMPARE(hits.size(), 1);
        QVERIFY2(hits[0].box.left() <= digits.first && hits[0].box.right() >= digits.second, qPrintable(report));
        QVERIFY2(hits[0].box.left() > word.second, qPrintable(report));
    }

    void nothing() { QCOMPARE(read(drawn({}, 14, Qt::black, Qt::white)), QString()); }
};

QTEST_MAIN(TestOcr)
#include "tst_ocr.moc"
