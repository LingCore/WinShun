// The words in pictures (PaddleOcr): the models and onnxruntime.dll the
// build put next to this test, on text drawn as a screenshot shows it.

#include "Geometry.h"
#include "OcrTable.h"
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

        // Multiple pin line-scans are queued independently (none dropped), cancelled ones are
        // skipped, and smart-redaction cancelScan() does not cancel queued pin scans.
        const QImage firstImg = drawn({u"PinOne111"_s}, 24, Qt::black, Qt::white);
        const QImage skippedImg = drawn({u"Skipped222"_s}, 24, Qt::black, Qt::white);
        const QImage secondImg = drawn({u"PinTwo333"_s}, 24, Qt::black, Qt::white);
        QString firstText;
        QString secondText;
        bool skippedCalled = false;
        auto cancelMid = std::make_shared<std::atomic<bool>>(false);
        reader.scan(firstImg, 1.5, [&](QList<RedactLine> lines) {
            firstText = lines.isEmpty() ? u"<empty>"_s : lines.first().text;
        });
        reader.scan(skippedImg, 1.5, [&](QList<RedactLine>) { skippedCalled = true; }, cancelMid);
        cancelMid->store(true, std::memory_order_release);
        reader.scan(secondImg, 1.5, [&](QList<RedactLine> lines) {
            secondText = lines.isEmpty() ? u"<empty>"_s : lines.first().text;
        });
        reader.cancelScan(); // only cancels smart redaction, not queued pin scans
        QTRY_VERIFY_WITH_TIMEOUT(!firstText.isEmpty() && !secondText.isEmpty(), 15000);
        QVERIFY(firstText.contains(u"PinOne111"_s));
        QVERIFY(secondText.contains(u"PinTwo333"_s));
        QVERIFY(!skippedCalled);
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
        // The translate plugin said what to set (a key the service refused: #444).
        answer(CaptureOcr::Translated::NoService, u"翻译服务的密钥不对或没有权限（接口返回 401）"_s);
        QCOMPARE(reader.translationNeeds(), u"service"_s);
        QVERIFY2(reader.translationNote().startsWith(u"翻译服务的密钥不对或没有权限（接口返回 401）"_s), qPrintable(reader.translationNote()));
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

    // A screenshot's lines are looked for in strips of 512 rows, not in the
    // whole of it at once (GitHub #467: ~80 bytes a pixel looked at, 184 MB
    // for 1920 x 1080 each time one was copied); every line read once.
    void screenshotLookedAtInStrips()
    {
        QStringList lines;
        for (int i = 0; i < 38; ++i)
            lines.append(u"第%1行 文字识别"_s.arg(i));
        const QImage screen = drawn(lines, 14, Qt::black, Qt::white);
        QVERIFY(screen.height() > 1000 && screen.height() < 2000);
        static QStringList steps;
        steps.clear();
        ws::PaddleOcr::setTrace([](const QString& step) { steps.append(step); });
        const QString words = read(screen);
        ws::PaddleOcr::setTrace(nullptr);
        static const QRegularExpression detect(u"^detect (\\d+) x (\\d+)$"_s);
        int detections = 0;
        for (const QString& step : std::as_const(steps)) {
            if (const QRegularExpressionMatch m = detect.match(step); m.hasMatch()) {
                ++detections;
                QVERIFY2(m.captured(2).toInt() <= 512, qPrintable(step));
            }
            QVERIFY2(!step.contains(u"looked at again"_s), qPrintable(step));
        }
        QVERIFY(detections >= 3);
        for (int i = 0; i < 38; ++i)
            QVERIFY2(words.count(u"第%1行"_s.arg(i)) == 1, qPrintable(u"line %1 in: %2"_s.arg(i).arg(words)));
    }

    // A title higher than the strips' overlap is whole in none of them: cut
    // by a strip's edge, its picture is looked at again in strips as high as
    // before, and it is read whole, once.
    void tallTitleAcrossAStrip()
    {
        QImage poster(1400, 900, QImage::Format_RGB32);
        poster.fill(Qt::white);
        {
            QPainter painter(&poster);
            QFont font(u"Microsoft YaHei UI"_s);
            font.setPixelSize(260);
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QRect(0, 300, 1400, 320), Qt::AlignCenter, u"WinShun"_s); // rows ~330 to ~590: across row 512
            font.setPixelSize(20);
            font.setBold(false);
            painter.setFont(font);
            painter.drawText(60, 80, u"上面的小字照常读"_s);
        }
        static QStringList steps;
        steps.clear();
        ws::PaddleOcr::setTrace([](const QString& step) { steps.append(step); });
        const QString words = read(poster);
        ws::PaddleOcr::setTrace(nullptr);
        QVERIFY2(std::ranges::any_of(steps, [](const QString& step) { return step.contains(u"looked at again"_s); }),
            qPrintable(steps.join(u" | "_s)));
        QCOMPARE(words.count(u"WinShun"_s, Qt::CaseInsensitive), 1);
        QVERIFY2(words.contains(u"上面的小字照常读"_s), qPrintable(words));
    }

    // A strip of text 35 px high and 33 000 wide, as a user copied one: it
    // was looked at as 28 062 x 7014 (the width kept for the text's height,
    // then padded to a quarter of it), several GB, and Win顺 ended. Now the
    // reading takes no more than a 2000 x 512 strip's: looked at in
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
        QCOMPARE(tall.width(), 336); // not made smaller: strips of 512 rows overlapping by 160
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
        // Two strips at the least, then one call or more per 6 lines read (some
        // boxes read too poorly are dropped after reading, so not fewer calls).
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

    // The table model's words as cells: placed as HTML places them, a cell
    // from above pushing those under it to the right.
    void tableStructureFromWords()
    {
        const QStringList words {u"<thead>"_s, u"<tr>"_s, u"<td"_s, u" rowspan=\"2\""_s, u">"_s, u"</td>"_s, u"<td"_s, u" colspan=\"2\""_s, u">"_s, u"</td>"_s,
            u"</tr>"_s, u"<tr>"_s, u"<td></td>"_s, u"<td></td>"_s, u"</tr>"_s, u"</thead>"_s, u"<tbody>"_s, u"<tr>"_s, u"<td></td>"_s, u"<td></td>"_s,
            u"<td></td>"_s, u"</tr>"_s, u"</tbody>"_s};
        std::vector<QRectF> boxes;
        for (int i = 0; i < 7; ++i)
            boxes.emplace_back(10.0 * i, 0, 8, 8);
        const ws::table::Structure s = ws::table::structureOf(words, boxes);
        QCOMPARE(s.rows, 3);
        QCOMPARE(s.columns, 3);
        QCOMPARE(s.cells.size(), std::size_t(7));
        const auto at = [&](std::size_t i) { return QList<int> {s.cells[i].row, s.cells[i].column, s.cells[i].rows, s.cells[i].columns}; };
        QCOMPARE(at(0), (QList<int> {0, 0, 2, 1}));
        QCOMPARE(at(1), (QList<int> {0, 1, 1, 2}));
        QCOMPARE(at(2), (QList<int> {1, 1, 1, 1})); // beside the cell from above
        QCOMPARE(at(3), (QList<int> {1, 2, 1, 1}));
        QCOMPARE(at(6), (QList<int> {2, 2, 1, 1}));
        QCOMPARE(s.cells[3].box, QRectF(30, 0, 8, 8)); // the boxes in the cells' order

        // A cell said to reach below the last row ends there; nothing is no table.
        const ws::table::Structure deep = ws::table::structureOf({u"<tr>"_s, u"<td"_s, u" rowspan=\"9\""_s, u">"_s, u"</td>"_s, u"<td></td>"_s, u"</tr>"_s}, {});
        QCOMPARE(deep.rows, 1);
        QCOMPARE(deep.cells[0].rows, 1);
        QCOMPARE(ws::table::structureOf({}, {}).rows, 0);
    }

    // Lines put into the model's cells: by the cell they cover, two lines of
    // one cell together, a line read across two cells cut between them, and
    // a line beside the cells in the nearest.
    void tableFilledFromPieces()
    {
        ws::table::Structure s;
        s.rows = 3;
        s.columns = 3;
        const auto cell = [&](int row, int column, QRectF box, int rows = 1, int columns = 1) { s.cells.push_back({row, column, rows, columns, box}); };
        cell(0, 0, {0, 0, 100, 60}, 2, 1); // 部门, down two rows
        cell(0, 1, {110, 0, 200, 25}, 1, 2); // 上半年, across two columns
        cell(1, 1, {110, 32, 90, 25});
        cell(1, 2, {215, 32, 90, 25});
        cell(2, 0, {0, 68, 100, 50});
        cell(2, 1, {110, 68, 90, 50});
        cell(2, 2, {215, 68, 90, 50});
        ws::OcrPiece both = piece(120, 36, 300, u"一季度二季度"_s); // read as one line
        for (int i = 0; i < 6; ++i)
            both.chars.append({i, i < 3 ? 0.08f + 0.1f * static_cast<float>(i) : 0.62f + 0.1f * static_cast<float>(i - 3)});
        const QList<ws::OcrPiece> pieces {
            piece(20, 20, 70, u"部门"_s), piece(180, 4, 240, u"上半年"_s), both,
            piece(10, 72, 90, u"研发"_s), piece(10, 94, 90, u"一部"_s), // two lines in a cell
            piece(120, 72, 190, u"Quarterly"_s), piece(120, 94, 180, u"report"_s),
            piece(310, 80, 360, u"8192"_s), // just outside the last cell
        };
        const ws::OcrTable table = ws::table::filled(s, pieces);
        QCOMPARE(table.rows.size(), 3);
        QCOMPARE(table.rows[0], (QStringList {u"部门"_s, u"上半年"_s, {}}));
        QCOMPARE(table.rows[1], (QStringList {{}, u"一季度"_s, u"二季度"_s}));
        QCOMPARE(table.rows[2], (QStringList {u"研发一部"_s, u"Quarterly report"_s, u"8192"_s}));
        QCOMPARE(table.spans, (QList<ws::OcrTable::Span> {{0, 0, 2, 1}, {0, 1, 1, 2}}));
        QVERIFY(table.standsAsTable());
        QCOMPARE(table.tabbed(), u"部门\t上半年\t\n\t一季度\t二季度\n研发一部\tQuarterly report\t8192"_s);
        QCOMPARE(table.html(), u"<table><tr><td rowspan=\"2\">部门</td><td colspan=\"2\">上半年</td></tr><tr><td>一季度</td><td>二季度</td></tr>"
                               "<tr><td>研发一部</td><td>Quarterly report</td><td>8192</td></tr></table>"_s);

        // No line of writing is torn here; a title above and a note below the
        // cells get rows of their own, the merged cells moving down with theirs.
        int torn = -1;
        QList<ws::OcrPiece> more = pieces;
        more.append(piece(60, -40, 260, u"上半年业绩"_s));
        more.append(piece(0, 130, 160, u"注：单位万元"_s));
        const ws::OcrTable titled = ws::table::filled(s, more, &torn);
        QCOMPARE(torn, 0);
        QCOMPARE(titled.rows.size(), 5);
        QCOMPARE(titled.rows.first(), (QStringList {u"上半年业绩"_s, {}, {}}));
        QCOMPARE(titled.rows.last(), (QStringList {u"注：单位万元"_s, {}, {}}));
        QCOMPARE(titled.spans, (QList<ws::OcrTable::Span> {{1, 0, 2, 1}, {1, 1, 1, 2}}));

        // A row too many, as the model now and then sees one: 一季度 and 二季度,
        // side by side on the screen, would be in two rows.
        ws::table::Structure wrong = s;
        wrong.rows = 4;
        wrong.cells[3] = {3, 2, 1, 1, QRectF(215, 32, 90, 25)};
        ws::OcrPiece first = piece(120, 36, 190, u"一季度"_s), second = piece(225, 36, 300, u"二季度"_s);
        ws::table::filled(wrong, {first, second, piece(20, 20, 70, u"部门"_s), piece(10, 80, 90, u"研发"_s)}, &torn);
        QCOMPARE(torn, 1);
    }

    // A ruled table's cells from the lines drawn in the picture: the squares
    // between them, two with no line between them one cell. Selected inside
    // its outer border it is the same table; ruled across only it has no
    // cells to be told from its lines.
    void tableFromRuledLines()
    {
        const int x[] = {20, 160, 300, 440};
        const int y[] = {20, 60, 100, 140, 180};
        const auto drawn = [&](bool down, const QColor& paper, const QColor& ink) {
            QImage image(460, 200, QImage::Format_RGB32);
            image.fill(paper);
            QPainter painter(&image);
            painter.setPen(ink);
            painter.drawRect(x[0], y[0], x[3] - x[0], y[4] - y[0]);
            for (int r = 1; r < 4; ++r)
                painter.drawLine(r == 3 ? x[1] : x[0], y[r], x[3], y[r]); // the first column's last cell is two rows high
            if (down) {
                painter.drawLine(x[1], y[0], x[1], y[4]);
                painter.drawLine(x[2], y[1], x[2], y[4]); // the heading's second cell is two columns wide
            }
            // Writing in a cell, with strokes as tall as writing is: none of it a line of the table.
            QFont font(u"Microsoft YaHei UI"_s);
            font.setPixelSize(16);
            font.setUnderline(true);
            painter.setFont(font);
            painter.setPen(ink);
            painter.drawText(QRect(x[1], y[2], x[2] - x[1], y[3] - y[2]), Qt::AlignCenter, u"Illustrated|11"_s);
            return image;
        };
        const auto check = [&](const ws::table::Structure& s, const char* what) {
            QVERIFY2(s.rows == 4 && s.columns == 3 && s.cells.size() == 10,
                qPrintable(u"%1: %2 x %3, %4 cells"_s.arg(QLatin1StringView(what)).arg(s.rows).arg(s.columns).arg(s.cells.size())));
            const auto at = [&](std::size_t i) { return QList<int> {s.cells[i].row, s.cells[i].column, s.cells[i].rows, s.cells[i].columns}; };
            QCOMPARE(at(0), (QList<int> {0, 0, 1, 1}));
            QCOMPARE(at(1), (QList<int> {0, 1, 1, 2}));
            QCOMPARE(at(5), (QList<int> {2, 0, 2, 1}));
            QCOMPARE(at(9), (QList<int> {3, 2, 1, 1}));
        };
        const ws::table::Structure s = ws::table::ruledStructure(drawn(true, Qt::white, QColor(0x90, 0x90, 0x90)), 20);
        check(s, "light");
        QVERIFY(std::abs(s.cells[1].box.left() - x[1]) <= 1.5 && std::abs(s.cells[1].box.right() - x[3]) <= 1.5);
        QVERIFY(std::abs(s.cells[5].box.top() - y[2]) <= 1.5 && std::abs(s.cells[5].box.bottom() - y[4]) <= 1.5);
        check(ws::table::ruledStructure(drawn(true, QColor(0x1f, 0x1f, 0x1f), QColor(0x60, 0x60, 0x60)), 20), "dark");
        // Without its outer border (what was selected ends 3 pixels inside it).
        check(ws::table::ruledStructure(drawn(true, Qt::white, QColor(0x90, 0x90, 0x90)).copy(x[0] + 3, y[0] + 3, x[3] - x[0] - 6, y[4] - y[0] - 6), 20), "inside");
        QCOMPARE(ws::table::ruledStructure(drawn(false, Qt::white, QColor(0x90, 0x90, 0x90)), 20).rows, 0);
        QCOMPARE(ws::table::ruledStructure(QImage(), 20).rows, 0);
    }

    // What a spreadsheet would change is marked as text; the rest is escaped
    // and left for it to read as it likes. One column of words is no table.
    void tableAsHtml()
    {
        ws::OcrTable table;
        table.rows = {{u"编号"_s, u"A<B & C"_s}, {u"007"_s, u"123456789012345678"_s}, {u"3-4"_s, u"1E5"_s}, {u"12.50"_s, u"2026-11-26"_s}};
        const QString text = u" style='mso-number-format:\"\\@\"'"_s;
        QCOMPARE(table.html(), u"<table><tr><td>编号</td><td>A&lt;B &amp; C</td></tr><tr><td%1>007</td><td%1>123456789012345678</td></tr>"
                               "<tr><td%1>3-4</td><td%1>1E5</td></tr><tr><td>12.50</td><td>2026-11-26</td></tr></table>"_s.arg(text));
        QVERIFY(table.standsAsTable());
        ws::OcrTable lines;
        lines.rows = {{u"第一行"_s, {}}, {u"第二行"_s, {}}, {u"第三行"_s, u"旁边"_s}};
        QVERIFY(!lines.standsAsTable());
        QVERIFY(!ws::OcrTable().standsAsTable());
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
        // readTable without the table model: the same rows, nothing merged.
        ws::PaddleOcr::setTableModel({});
        ws::OcrTable plain;
        QVERIFY2(ocr->readTable(image, 1.0, &lines, &plain, &error), qPrintable(error));
        QCOMPARE(plain.rows, cells);
        QVERIFY(plain.spans.isEmpty());
    }

    // With the table model (what the screenshot plugin has in table\): a ruled
    // table with a heading across two columns and a cell down two rows comes
    // back with both merged; sentences are no table to it either.
    void tableReadWithModel()
    {
        ws::PaddleOcr::setTableModel(QString::fromUtf8(WINSHUN_TABLE_MODEL));
        QVERIFY(ws::PaddleOcr::hasTableModel());
        const auto restore = qScopeGuard([] { ws::PaddleOcr::setTableModel({}); });
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(16);
        QImage image(460, 190, QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(QColor(0x90, 0x90, 0x90));
            const int x[] = {20, 160, 300, 440};
            const int y[] = {20, 60, 100, 140, 180};
            painter.drawRect(x[0], y[0], x[3] - x[0], y[4] - y[0]);
            for (int r = 1; r < 4; ++r)
                painter.drawLine(r == 3 ? x[1] : x[0], y[r], x[3], y[r]);
            painter.drawLine(x[1], y[0], x[1], y[4]);
            painter.drawLine(x[2], y[1], x[2], y[4]);
            painter.setPen(Qt::black);
            const auto cell = [&](int column, int row, int columns, int rows, const QString& text) {
                painter.drawText(QRect(x[column], y[row], x[column + columns] - x[column], y[row + rows] - y[row]), Qt::AlignCenter, text);
            };
            cell(0, 0, 1, 1, u"部门"_s);
            cell(1, 0, 2, 1, u"上半年"_s);
            cell(0, 1, 1, 1, u"名称"_s);
            cell(1, 1, 1, 1, u"一季度"_s);
            cell(2, 1, 1, 1, u"二季度"_s);
            cell(0, 2, 1, 2, u"研发部"_s);
            cell(1, 2, 1, 1, u"1024"_s);
            cell(2, 2, 1, 1, u"2048"_s);
            cell(1, 3, 1, 1, u"4096"_s);
            cell(2, 3, 1, 1, u"8192"_s);
        }
        QString error;
        const std::unique_ptr<ws::PaddleOcr> ocr = ws::PaddleOcr::load(QCoreApplication::applicationDirPath(), 2, &error);
        QVERIFY2(ocr, qPrintable(error));
        QStringList lines;
        ws::OcrTable table;
        QVERIFY2(ocr->readTable(image, 1.0, &lines, &table, &error), qPrintable(error));
        const QList<QStringList> cells {{u"部门"_s, u"上半年"_s, {}}, {u"名称"_s, u"一季度"_s, u"二季度"_s}, {u"研发部"_s, u"1024"_s, u"2048"_s},
            {{}, u"4096"_s, u"8192"_s}};
        QVERIFY2(table.rows == cells, qPrintable(table.html()));
        QCOMPARE(table.spans, (QList<ws::OcrTable::Span> {{0, 1, 1, 2}, {2, 0, 2, 1}}));

        QImage words(520, 150, QImage::Format_RGB32);
        words.fill(Qt::white);
        {
            QPainter painter(&words);
            painter.setFont(font);
            painter.setPen(Qt::black);
            const QStringList sentences {u"截图是日常办公里最常用的功能之一，很多时候截下来的是一段文字。"_s,
                u"文字识别把图片里的字变回可以选中、复制和搜索的文本。"_s, u"识别完成后可以直接复制全部，也可以只选中其中一部分。"_s,
                u"如果是外文，还能顺手翻译成中文，译文显示在原文下方。"_s};
            for (qsizetype i = 0; i < sentences.size(); ++i)
                painter.drawText(16, 34 + 30 * static_cast<int>(i), sentences[i]);
        }
        QVERIFY2(ocr->readTable(words, 1.0, &lines, &table, &error), qPrintable(error));
        QCOMPARE(lines.size(), 4);
        QVERIFY2(table.isEmpty(), qPrintable(table.tabbed()));
    }

    // 合并换行: joins soft-wrapped lines inside paragraphs while preserving
    // blank-line paragraph breaks, sentence boundaries, and list items.
    void mergeLines()
    {
        using ws::capture::CaptureOcr;
        QCOMPARE(CaptureOcr::mergeLines(u"这是一段中文\n被截断了。\n第二段还没\n说完。"_s),
            u"这是一段中文被截断了。\n第二段还没说完。"_s);
        QCOMPARE(CaptureOcr::mergeLines(u"The quick brown fox\njumps over the lazy dog.\n\nNext para\ncontinues."_s),
            u"The quick brown fox jumps over the lazy dog.\n\nNext para continues."_s);
        QCOMPARE(CaptureOcr::mergeLines(u"The trans-\nlation works."_s), u"The translation works."_s);
        QCOMPARE(CaptureOcr::mergeLines(u"先用 Win\nShun 处理"_s), u"先用 Win Shun 处理"_s);
        QCOMPARE(CaptureOcr::mergeLines(u"清单：\n- 第一项\n1. 第二项\n2、第三项"_s),
            u"清单：\n- 第一项\n1. 第二项\n2、第三项"_s);
        QVERIFY(CaptureOcr::mergeLines(u" \n\t "_s).isEmpty());
    }
};

QTEST_MAIN(TestOcr)
#include "tst_ocr.moc"
