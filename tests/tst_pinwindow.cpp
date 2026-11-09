// The pin window's text pick (GitHub #258): the pill that says what to do
// stays while the words are being read, then fades off the picture so it
// does not sit on the bottom line of what one wants to pick; a pick brings
// it back for a moment.
#include "PinWindow.h"

#include <QGuiApplication>
#include <QImage>
#include <QTest>

#include <functional>

using namespace ws::capture;

class TestPinWindow : public QObject {
    Q_OBJECT

private slots:
    void textPillFadesOffTheWords()
    {
        QImage image(480, 240, QImage::Format_RGB32);
        image.fill(Qt::white);
        std::function<void(QList<RedactLine>)> wordsRead; // the reading, answered when the test says
        PinWindow::Host host;
        host.readLines = [&](const QImage&, double, std::function<void(QList<RedactLine>)> done) { wordsRead = std::move(done); };
        PinWindow pin(image, QPoint(40, 40), std::move(host));
        QVERIFY(pin.hwnd());
        QCOMPARE(pin.textHintAlpha(), 0); // nothing said while no text is picked
        pin.beginTextPick();
        QVERIFY(pin.textMode());
        QCOMPARE(pin.textHintAlpha(), 255); // 识别文字中…
        QTest::qWait(2800); // past the moment a ready pill would get: still reading, still there
        QCOMPARE(pin.textHintAlpha(), 255);
        QVERIFY(wordsRead);
        wordsRead({}); // the words came: 拖选文字 · Esc 退出, for a moment
        QCOMPARE(pin.textHintAlpha(), 255);
        QTest::qWait(1200);
        QCOMPARE(pin.textHintAlpha(), 255); // long enough to read
        QTRY_COMPARE_WITH_TIMEOUT(pin.textHintAlpha(), 0, 4000); // then off the words
        pin.endTextPick();
        QVERIFY(!pin.textMode());
        QCOMPARE(pin.textHintAlpha(), 0);
    }
};

QTEST_MAIN(TestPinWindow)
#include "tst_pinwindow.moc"
