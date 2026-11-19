// The words in pictures (PaddleOcr): the models and onnxruntime.dll the
// build put next to this test, on text drawn as a screenshot shows it.

#include "Geometry.h"
#include "PaddleOcr.h"
#include "SmartRedact.h"
#include "CaptureOcr.h"
#include "TranslateOverlay.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <cmath>
#include <stdexcept>

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

    // 翻译 (GitHub #191): asked with a ticket, answered once; an answer for
    // words no longer shown, or to an older request, is dropped.
    void translationOfTheWords()
    {
        using namespace ws::capture;
        CaptureOcr reader;
        QSignalSpy asked(&reader, &CaptureOcr::translateRequested);
        QSignalSpy changed(&reader, &CaptureOcr::translationChanged);
        QSignalSpy words(&reader, &CaptureOcr::changed);
        reader.showText(u"The quick brown fox jumps over the lazy\ndog."_s);
        QVERIFY(!reader.translationShown());
        reader.translate(u" \n "_s);
        QCOMPARE(asked.count(), 0); // nothing to translate: not asked

        reader.translate(reader.text());
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked.last().at(0).toString(), u"The quick brown fox jumps over the lazy dog."_s); // as paragraphs
        QVERIFY(reader.translating() && reader.translationShown());
        const int first = asked.last().at(1).toInt();
        reader.translate(u"again"_s);
        const int second = asked.last().at(1).toInt();
        QVERIFY(second != first);
        reader.setTranslation(first, CaptureOcr::Translated::Done, u"old"_s);
        QVERIFY(reader.translating()); // the older request's answer
        reader.setTranslation(second, CaptureOcr::Translated::Done, u" 再来 \n"_s);
        QVERIFY(!reader.translating());
        QCOMPARE(reader.translation(), u"再来"_s);
        QVERIFY(reader.translationNote().isEmpty() && reader.translationNeeds().isEmpty());
        reader.setTranslation(second, CaptureOcr::Translated::Failed, u"twice"_s);
        QCOMPARE(reader.translation(), u"再来"_s); // answered already
        const qsizetype wordsChanged = words.count();
        QCOMPARE(wordsChanged, 1); // the words' own signal stayed quiet: what the user edited is not put back

        const auto answer = [&](CaptureOcr::Translated outcome, const QString& text) {
            reader.translate(u"x"_s);
            reader.setTranslation(asked.last().at(1).toInt(), outcome, text);
        };
        answer(CaptureOcr::Translated::Failed, u"HTTP 401"_s);
        QVERIFY(reader.translation().isEmpty() && reader.translationNote().contains(u"HTTP 401"_s) && reader.translationNeeds().isEmpty());
        answer(CaptureOcr::Translated::Failed, {});
        QVERIFY(!reader.translationNote().isEmpty());
        answer(CaptureOcr::Translated::NoService, {});
        QCOMPARE(reader.translationNeeds(), u"service"_s);
        QVERIFY(reader.translationShown() && !reader.translationNote().isEmpty());
        answer(CaptureOcr::Translated::NoPlugin, {});
        QCOMPARE(reader.translationNeeds(), u"plugin"_s);
        answer(CaptureOcr::Translated::PluginOff, {}); // installed but off (#340): turn it on, not install it again
        QCOMPARE(reader.translationNeeds(), u"on"_s);
        QVERIFY(reader.translationNote().contains(u"关着"_s));
        answer(CaptureOcr::Translated::Done, {}); // nothing to show (an older Win顺 took the words to its search box)
        QVERIFY(!reader.translationShown());

        // Other words: the translation goes, and the answer still on its way with it.
        reader.translate(u"x"_s);
        const int late = asked.last().at(1).toInt();
        changed.clear();
        reader.showText(u"other"_s);
        QVERIFY(!reader.translationShown() && changed.count() == 1);
        reader.setTranslation(late, CaptureOcr::Translated::Done, u"late"_s);
        QVERIFY(!reader.translationShown());

        QSignalSpy setup(&reader, &CaptureOcr::translationSetupRequested);
        reader.setupTranslation();
        QCOMPARE(setup.count(), 1);
    }

    // 截图翻译 in place, the whole way but the service: a page drawn as a
    // screenshot shows it, read, its blocks' "translations" drawn over the
    // words, and the result read again: the new words are there, the old gone,
    // what was not text untouched.
    void translatedInPlace()
    {
        using namespace ws::capture;
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(22);
        QImage page(820, 330, QImage::Format_RGB32);
        page.fill(QColor(250, 250, 252));
        {
            QPainter painter(&page);
            painter.setFont(font);
            painter.setPen(QColor(30, 30, 36));
            painter.drawText(30, 50, u"Screenshots are read on this computer. Nothing leaves it"_s);
            painter.drawText(30, 82, u"until you ask for a translation, and then only the words"_s);
            painter.drawText(30, 114, u"go to the service you chose."_s);
            painter.drawText(30, 190, u"Settings"_s);
            painter.fillRect(QRect(600, 160, 180, 120), QColor(200, 60, 60)); // not text
            painter.fillRect(QRect(0, 290, 820, 40), QColor(32, 34, 40)); // a dark bar with light words
            painter.setPen(QColor(235, 235, 240));
            painter.drawText(30, 318, u"Ready"_s);
        }
        CaptureOcr reader;
        QList<RedactLine> lines;
        bool done = false;
        reader.scan(page, 1.5, [&](QList<RedactLine> read) { lines = std::move(read); done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        const QList<TextBlock> blocks = ocrBlocks(lines);
        QStringList found;
        for (const TextBlock& b : blocks)
            found.append(b.text);
        QVERIFY2(blocks.size() == 3, qPrintable(found.join(u" | "_s)));
        QCOMPARE(blocks[0].lines, 3);
        QVERIFY2(blocks[0].text.startsWith(u"Screenshots are read"_s) && blocks[0].text.endsWith(u"you chose."_s), qPrintable(blocks[0].text));
        QCOMPARE(blocks[1].text, u"Settings"_s);
        QCOMPARE(blocks[2].text, u"Ready"_s);

        QCOMPARE(translatedPicture(page, blocks, {u"one"_s}, font), QImage()); // not a translation a block
        const QStringList translations {u"截图在这台电脑上识别，在你要求翻译之前什么都不会离开；翻译时也只有文字会发给你选的服务。"_s, u"设置"_s, u"就绪"_s};
        const QImage translated = translatedPicture(page, blocks, translations, font);
        QCOMPARE(translated.size(), page.size());
        QCOMPARE(translated.copy(600, 160, 180, 120), page.copy(600, 160, 180, 120).convertToFormat(QImage::Format_RGB32)); // the red box
        QCOMPARE(translated.pixelColor(810, 20), QColor(250, 250, 252));
        QCOMPARE(translated.pixelColor(400, 325), QColor(32, 34, 40)); // the bar is still the bar
        const QString again = read(translated);
        QVERIFY2(again.contains(u"截图在这台电脑上识别"_s) && again.contains(u"你选的服务"_s), qPrintable(again));
        QVERIFY2(again.contains(u"设置"_s) && again.contains(u"就绪"_s), qPrintable(again));
        QVERIFY2(!again.contains(u"Screenshots"_s) && !again.contains(u"Settings"_s) && !again.contains(u"Ready"_s), qPrintable(again));

        // The other way: longer words than the label's box holds run on over the empty ground beside it.
        const QImage longer = translatedPicture(page, blocks, {QString(), u"Settings and preferences"_s, QString()}, font);
        const QString wide = read(longer);
        QVERIFY2(wide.contains(u"Settings and preferences"_s), qPrintable(wide));
        QVERIFY2(wide.contains(u"Screenshots are read"_s) && wide.contains(u"Ready"_s), qPrintable(wide)); // no translation: as they were
        // The same words as there were: nothing drawn.
        QCOMPARE(translatedPicture(page, blocks, found, font), page.convertToFormat(QImage::Format_RGB32));
    }

    // Bold words stay bold, and words in the middle of a button stay there.
    void weightAndCentring()
    {
        using namespace ws::capture;
        const auto measured = [](const QString& family, int px, bool bold, const QString& text, QColor ink, QColor paper) {
            QFont font(family);
            font.setPixelSize(px);
            font.setBold(bold);
            QImage image(px * 26, px * 3, QImage::Format_RGB32);
            image.fill(paper);
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(ink);
            painter.drawText(px, px * 2, text);
            painter.end();
            const QRect box(px - 3, static_cast<int>(px * 0.85), QFontMetrics(font).horizontalAdvance(text) + 6, static_cast<int>(px * 1.3));
            return strokeWeight(image, box, blockColors(image, box), px * 1.3);
        };
        for (const QString& family : {u"Segoe UI"_s, u"Microsoft YaHei UI"_s}) {
            for (const int px : {14, 18, 24, 34, 48}) {
                for (const QString& text : {u"Translation in place today"_s, u"就地翻译这一段文字"_s}) {
                    const double regular = measured(family, px, false, text, Qt::black, Qt::white);
                    const double bold = measured(family, px, true, text, Qt::black, Qt::white);
                    const double light = measured(family, px, true, text, QColor(235, 235, 240), QColor(32, 34, 40));
                    const double thin = measured(family, px, false, text, QColor(235, 235, 240), QColor(32, 34, 40));
                    const double onBlue = measured(family, px, false, text, Qt::white, QColor(15, 108, 189));
                    QVERIFY2(thin < kBoldStrokeLight - 0.02 && onBlue < kBoldStrokeLight - 0.02, qPrintable(u"%1 %2"_s.arg(thin).arg(onBlue)));
                    const QByteArray what = u"%1 %2px '%3': regular %4, bold %5, bold light on dark %6"_s.arg(family).arg(px).arg(text)
                                                .arg(regular, 0, 'f', 3).arg(bold, 0, 'f', 3).arg(light, 0, 'f', 3).toUtf8();
                    qInfo("%s", what.constData());
                    QVERIFY2(regular < kBoldStroke - 0.015, what.constData());
                    QVERIFY2(bold > regular * 1.25 && light > regular * 1.25, what.constData());
                    // CJK in Segoe UI comes from a fallback font whose bold is thinner at large sizes: allowed to stay regular.
                    if (family == u"Microsoft YaHei UI" || text.front().unicode() < 0x80 || px < 24)
                        QVERIFY2(bold >= kBoldStroke + 0.01, what.constData());
                }
            }
        }

        // A button with its words in the middle, a label at the left of a wide row.
        QImage page(600, 200, QImage::Format_RGB32);
        page.fill(Qt::white);
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(20);
        const QRect button(40, 40, 220, 44);
        {
            QPainter painter(&page);
            painter.fillRect(button, QColor(15, 108, 189));
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(button, Qt::AlignCenter, u"Save changes"_s);
            painter.setPen(Qt::black);
            painter.drawText(40, 150, u"Name"_s);
        }
        const int wide = QFontMetrics(font).horizontalAdvance(u"Save changes"_s);
        const QRect words(button.center().x() - wide / 2 - 2, 48, wide + 4, 28);
        const Margins inButton = blockMargins(page, words, QColor(15, 108, 189));
        QVERIFY2(inButton.centred(), qPrintable(u"%1 %2"_s.arg(inButton.left).arg(inButton.right)));
        const Margins label = blockMargins(page, QRect(38, 130, 60, 28), Qt::white);
        QVERIFY(!label.centred());
        QCOMPARE(label.right, -1); // the ground runs to the edge
        // Drawn: the shorter words are in the middle of the button, as far from one edge as from the other.
        TextBlock block;
        block.box = words;
        block.text = u"Save changes"_s;
        block.lineHeight = 28;
        block.lines = 1;
        const QImage out = translatedPicture(page, {block}, {u"保存"_s}, font);
        int first = -1, last = -1;
        for (int x = button.left(); x <= button.right(); ++x) {
            bool ink = false;
            for (int y = button.top(); y <= button.bottom() && !ink; ++y)
                ink = qGray(out.pixel(x, y)) > 200;
            if (ink) {
                if (first < 0)
                    first = x;
                last = x;
            }
        }
        QVERIFY(first > 0);
        QVERIFY2(std::abs((first - button.left()) - (button.right() - last)) <= 3, qPrintable(u"%1..%2"_s.arg(first).arg(last)));
        // Longer words than the box: on both ways, still inside the button.
        const QImage longer = translatedPicture(page, {block}, {u"Save all the changes"_s}, font);
        QCOMPARE(longer.pixelColor(button.left() + 1, button.center().y()), QColor(15, 108, 189));
        QCOMPARE(longer.pixelColor(button.right() - 1, button.center().y()), QColor(15, 108, 189));
        QVERIFY2(read(longer).contains(u"Save all the changes"_s), qPrintable(read(longer)));
    }

    // 翻译 on the toolbar: read, then asked for at once.
    void readThenTranslate()
    {
        using namespace ws::capture;
        CaptureOcr reader;
        QSignalSpy asked(&reader, &CaptureOcr::translateRequested);
        reader.read(drawn({u"Press Ctrl twice"_s}, 28, Qt::black, Qt::white), 1.5, true);
        QTRY_VERIFY_WITH_TIMEOUT(!reader.busy(), 15000);
        QTRY_COMPARE(asked.count(), 1);
        QVERIFY2(asked.first().at(0).toString().contains(u"Ctrl"_s), qPrintable(asked.first().at(0).toString()));
        QVERIFY(reader.translating());
        // Nothing read: nothing asked.
        QImage blank(300, 80, QImage::Format_RGB32);
        blank.fill(Qt::white);
        reader.read(blank, 1.5, true);
        QTRY_VERIFY_WITH_TIMEOUT(!reader.busy(), 15000);
        QTest::qWait(50);
        QCOMPARE(asked.count(), 1);
        QVERIFY(!reader.translationShown());
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

    // A strip of text 35 px high and 33 000 wide, as a user copied one: it
    // was looked at as 28 062 x 7014 (the width kept for the text's height,
    // then padded to a quarter of it), several GB, and Win顺 ended. Now the
    // reading takes no more than a 2000 x 2000 screenshot's: looked at in
    // tiles across, read in parts, every piece of it once.
    void veryWideStrip()
    {
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(20);
        const QFontMetrics metrics(font);
        QString text;
        int marks = 0;
        while (metrics.horizontalAdvance(text) < 32700)
            text += u"第%1段 WinShun 读图 "_s.arg(marks++);
        QImage strip(metrics.horizontalAdvance(text) + 40, 35, QImage::Format_RGB32);
        strip.fill(Qt::white);
        {
            QPainter painter(&strip);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(20, 25, text);
        }
        static QStringList steps;
        steps.clear();
        ws::PaddleOcr::setTrace([](const QString& step) { steps.append(step); });
        const QString words = read(strip);
        ws::PaddleOcr::setTrace(nullptr);
        static const QRegularExpression detect(u"^detect (\\d+) x (\\d+)$"_s);
        static const QRegularExpression recognize(u"^recognize (\\d+) lines, 48 x (\\d+)"_s);
        int detections = 0;
        for (const QString& step : std::as_const(steps)) {
            if (const QRegularExpressionMatch m = detect.match(step); m.hasMatch()) {
                ++detections;
                QVERIFY2(m.captured(1).toInt() <= 2016 && m.captured(2).toInt() <= 2016, qPrintable(step));
            }
            if (const QRegularExpressionMatch m = recognize.match(step); m.hasMatch()) {
                QVERIFY2(m.captured(2).toInt() <= 12000, qPrintable(step));
                QVERIFY2(m.captured(1).toInt() * m.captured(2).toInt() <= 36000, qPrintable(step));
            }
        }
        QVERIFY(detections > 1);
        for (int i = 0; i < marks; ++i)
            QVERIFY2(words.count(u"第%1段"_s.arg(i)) == 1, qPrintable(u"第%1段 in: %2"_s.arg(i).arg(words)));
    }

    // OCR search's watchdog hears from a long read after each strip looked
    // at and each batch of lines read; the reading itself is the same, and a
    // progress callback that throws abandons the read with its message.
    void progressDuringOneRead()
    {
        QStringList lines;
        for (int i = 0; i < 120; ++i)
            lines.append(u"第%1行 文字识别"_s.arg(i));
        const QImage tall = drawn(lines, 14, Qt::black, Qt::white);
        QCOMPARE(tall.width(), 336); // not made smaller: strips of 2000 rows overlapping by 160
        QVERIFY(tall.height() > 2000 && tall.height() <= 2000 + 1840);
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        QVERIFY2(ocr, qPrintable(error));
        QList<ws::OcrPiece> plain, watched;
        QVERIFY2(ocr->readPieces(tall, &plain, &error), qPrintable(error));
        int calls = 0;
        ws::OcrOptions options;
        options.progress = [&] { ++calls; };
        QVERIFY2(ocr->readPieces(tall, &watched, &error, options), qPrintable(error));
        QCOMPARE(watched.size(), plain.size());
        for (qsizetype i = 0; i < plain.size(); ++i)
            QCOMPARE(watched[i].text, plain[i].text);
        // Two strips, then at least one call per 6 lines read (some boxes
        // read too poorly are dropped after reading, so not fewer calls).
        QVERIFY2(calls >= 2 + (plain.size() + 5) / 6, qPrintable(u"%1 calls"_s.arg(calls)));
        int left = 3;
        options.progress = [&] {
            if (--left == 0)
                throw std::runtime_error("stopped by the watchdog");
        };
        QList<ws::OcrPiece> abandoned;
        QVERIFY(!ocr->readPieces(tall, &abandoned, &error, options));
        QCOMPARE(error, u"stopped by the watchdog"_s);
    }

    // A 24-bit picture (how OCR search hands over its pictures, to hold a big
    // one once) reads exactly as the same picture in 32 bits, made smaller
    // to look for lines (wider than 2000) or not.
    void twentyFourBitPictureReadsTheSame()
    {
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr
            = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        QVERIFY2(ocr, qPrintable(error));
        QStringList lines;
        for (int i = 0; i < 12; ++i)
            lines.append(u"第%1行 文字识别 invoice 731905"_s.arg(i));
        for (const int px : {14, 120}) {
            const QImage image = drawn(lines, px, Qt::black, Qt::white);
            QList<ws::OcrPiece> rgb32, rgb888;
            QVERIFY2(ocr->readPieces(image, &rgb32, &error), qPrintable(error));
            QVERIFY2(ocr->readPieces(image.convertToFormat(QImage::Format_RGB888), &rgb888, &error), qPrintable(error));
            QVERIFY(rgb32.size() >= 12);
            QCOMPARE(rgb888.size(), rgb32.size());
            for (qsizetype i = 0; i < rgb32.size(); ++i) {
                QCOMPARE(rgb888[i].text, rgb32[i].text);
                QCOMPARE(rgb888[i].score, rgb32[i].score);
                QCOMPARE(rgb888[i].quad, rgb32[i].quad);
            }
        }
    }

    // OCR search reads a page that came in upside down (a scan fed the wrong
    // way round); an upright page reads the same with or without the retry.
    void upsideDownPageIsReadAgainTurned()
    {
        QStringList lines;
        for (int i = 0; i < 12; ++i)
            lines.append(u"报销单第%1行 金额 %2 元"_s.arg(i).arg(1200 + i * 37));
        const QImage upright = drawn(lines, 22, Qt::black, Qt::white);
        const QImage turned = upright.flipped(Qt::Horizontal | Qt::Vertical);
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr
            = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        QVERIFY2(ocr, qPrintable(error));
        const auto words = [&](const QImage& image, bool retry) {
            ws::OcrOptions options;
            options.retryUpsideDown = retry;
            QList<ws::OcrPiece> pieces;
            if (!ocr->readPieces(image, &pieces, &error, options))
                return QString();
            QStringList texts;
            for (const ws::OcrPiece& piece : std::as_const(pieces))
                texts.append(piece.text);
            texts.sort();
            return texts.join(u'\n');
        };
        const QString plain = words(upright, false);
        int found = 0;
        for (int i = 0; i < 12; ++i)
            found += plain.contains(u"第%1行"_s.arg(i)) ? 1 : 0;
        QCOMPARE(found, 12);
        QCOMPARE(words(upright, true), plain);
        const QString without = words(turned, false), with = words(turned, true);
        int before = 0, after = 0;
        for (int i = 0; i < 12; ++i) {
            before += without.contains(u"第%1行"_s.arg(i)) ? 1 : 0;
            after += with.contains(u"第%1行"_s.arg(i)) ? 1 : 0;
        }
        QVERIFY2(before <= 2, qPrintable(without));
        QVERIFY2(after >= 11, qPrintable(with));
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

    static ws::OcrPiece piece(double left, double top, double right, const QString& text, double height = 16)
    {
        ws::OcrPiece p;
        p.quad = QPolygonF(QRectF(left, top, right - left, height));
        p.text = text;
        return p;
    }

    // Columns where the lines with pieces side by side leave a gap all the
    // way down; a title across them goes in the first; two pieces close
    // together are one cell.
    void tableFromPieces()
    {
        const QList<ws::OcrPiece> pieces {
            piece(100, 0, 400, u"2026 年销售表"_s),
            piece(0, 30, 60, u"名称"_s), piece(200, 30, 260, u"数量"_s), piece(400, 30, 460, u"单价"_s),
            piece(0, 60, 40, u"Apple"_s), piece(46, 60, 120, u"Pie"_s), piece(200, 60, 230, u"12"_s), piece(400, 60, 470, u"3.50"_s),
            piece(0, 90, 80, u"香蕉"_s), piece(400, 91, 450, u"2.00"_s), // no quantity
        };
        const QList<QStringList> table = ws::ocrTable(pieces);
        QCOMPARE(table.size(), 4);
        QCOMPARE(table[0], (QStringList {u"2026 年销售表"_s, {}, {}}));
        QCOMPARE(table[1], (QStringList {u"名称"_s, u"数量"_s, u"单价"_s}));
        QCOMPARE(table[2], (QStringList {u"Apple Pie"_s, u"12"_s, u"3.50"_s}));
        QCOMPARE(table[3], (QStringList {u"香蕉"_s, {}, u"2.00"_s}));
    }

    // Not a table: lines one piece each, or gaps that do not line up.
    void noTableFromPieces()
    {
        QVERIFY(ws::ocrTable({piece(0, 0, 300, u"第一行"_s), piece(0, 30, 280, u"第二行"_s), piece(0, 60, 200, u"第三行"_s)}).isEmpty());
        QVERIFY(ws::ocrTable({piece(0, 0, 50, u"a"_s), piece(60, 0, 200, u"b"_s), piece(0, 30, 120, u"c"_s), piece(130, 30, 200, u"d"_s)}).isEmpty());
        QVERIFY(ws::ocrTable({piece(0, 0, 50, u"a"_s), piece(200, 0, 260, u"b"_s)}).isEmpty()); // one line
    }

    // A table drawn and read for real: its cells come back in place.
    void tableRead()
    {
        const QList<QStringList> cells {{u"姓名"_s, u"部门"_s, u"工号"_s}, {u"张三"_s, u"研发部"_s, u"1024"_s},
            {u"李四"_s, u"市场部"_s, u"2048"_s}, {u"Alice"_s, u"Sales"_s, u"4096"_s}};
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(16);
        QImage image(520, 40 * static_cast<int>(cells.size()) + 20, QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(Qt::black);
            for (qsizetype r = 0; r < cells.size(); ++r) {
                for (qsizetype c = 0; c < cells[r].size(); ++c)
                    painter.drawText(20 + 170 * static_cast<int>(c), 40 * static_cast<int>(r + 1), cells[r][c]);
            }
        }
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        QVERIFY2(ocr, qPrintable(error));
        QStringList lines;
        QList<QStringList> table;
        QVERIFY2(ocr->read(image, &lines, &error, &table), qPrintable(error));
        QCOMPARE(lines.size(), cells.size());
        QCOMPARE(table, cells);
    }
};

QTEST_MAIN(TestOcr)
#include "tst_ocr.moc"
