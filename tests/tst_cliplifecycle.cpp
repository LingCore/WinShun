#include "platform/ClipboardWatcher.h"
#include "platform/TextRecognizer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>

using namespace ws;
using namespace Qt::StringLiterals;

class ClipLifecycleTest : public QObject {
    Q_OBJECT

private slots:
    void disabledWatcherStartsAndRetiresWithoutReadingTheClipboard()
    {
        std::atomic<unsigned> captured = 0;
        ClipboardWatcher watcher({}, {.captured = [&](ClipCapture) { ++captured; }});
        QVERIFY(!watcher.running());
        QVERIFY(!watcher.listening());
        for (int cycle = 0; cycle != 3; ++cycle) {
            const DWORD sequence = ::GetClipboardSequenceNumber();
            const unsigned before = captured.load();
            // Enabling does not backfill anything copied while disabled.
            watcher.setOptions({true});
            QTRY_VERIFY_WITH_TIMEOUT(watcher.running() && watcher.listening(), 2000);
            watcher.setOptions({false});
            QTRY_VERIFY_WITH_TIMEOUT(!watcher.listening(), 2000);
            QTRY_VERIFY_WITH_TIMEOUT(!watcher.running(), 3000);
            // A user or another program may copy while the listener is on.
            // Such a capture is legitimate. Check backfill only while the
            // observed clipboard generation stayed the same.
            if (::GetClipboardSequenceNumber() == sequence)
                QCOMPARE(captured.load(), before);
            else
                qInfo("Clipboard changed during this cycle; worker lifecycle was checked");
        }
        // No test writes or replaces the user's actual clipboard contents.
    }

    void recognizerHasNoIdleWorkerAndCanRestart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TextRecognizer recognizer;
        QSignalSpy done(&recognizer, &TextRecognizer::done);
        QVERIFY(!recognizer.running());
        for (qint64 id = 1; id != 4; ++id) {
            recognizer.read(id, dir.filePath(u"not-there.png"_s));
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), id, 2000);
            QCOMPARE(done.back().at(0).toLongLong(), id);
            QVERIFY(!done.back().at(2).toBool());
            QTRY_VERIFY_WITH_TIMEOUT(!recognizer.running(), 2000);
        }
        recognizer.cancel();
        QVERIFY(!recognizer.running());
    }

    // A start that finds the guard file: the picture read when the last run
    // ended is read once more; the same one twice is given up. The file goes.
    void crashedReadingIsGivenUpTheSecondTime()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString guard = dir.filePath(u"reading-picture"_s);
        const auto leave = [&](const QByteArray& text) {
            QFile file(guard);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(text);
        };
        QCOMPARE(takeCrashedReading(guard, {}).id, qint64(0)); // none left
        leave("42");
        CrashedReading crashed = takeCrashedReading(guard, {});
        QCOMPARE(crashed.id, qint64(42));
        QVERIFY(!crashed.giveUp);
        QVERIFY(!QFile::exists(guard));
        leave("42");
        crashed = takeCrashedReading(guard, u"42"_s);
        QCOMPARE(crashed.id, qint64(42));
        QVERIFY(crashed.giveUp);
        leave("43"); // another one: once more
        QVERIFY(!takeCrashedReading(guard, u"42"_s).giveUp);
        leave("not a number");
        QCOMPARE(takeCrashedReading(guard, {}).id, qint64(0));
        QVERIFY(!QFile::exists(guard));
    }

    // The guard file holds the id of the picture being read, and only then.
    void recognizerHoldsItsGuardOnlyWhileReading()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString guard = dir.filePath(u"reading-picture"_s);
        const QString picture = dir.filePath(u"words.png"_s);
        QImage image(1600, 900, QImage::Format_RGB32); // rows of bars, like lines of words
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            for (int y = 40; y < 860; y += 60) {
                for (int x = 40; x < 1500; x += 90)
                    painter.fillRect(x, y, 30 + (x * 7 + y) % 50, 24, Qt::black);
            }
        }
        QVERIFY(image.save(picture));
        TextRecognizer recognizer;
        recognizer.setGuardFile(guard);
        QSignalSpy done(&recognizer, &TextRecognizer::done);
        recognizer.read(7, dir.filePath(u"not-there.png"_s)); // nothing to read: no guard
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2000);
        QVERIFY(!QFile::exists(guard));
        recognizer.read(8, picture);
        QByteArray held; // looked at every millisecond: a quick read is not missed
        QElapsedTimer waited;
        waited.start();
        while (held.isEmpty() && done.size() < 2 && waited.elapsed() < 60000) {
            if (QFile file(guard); file.open(QIODevice::ReadOnly))
                held = file.readAll();
            QThread::msleep(1);
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 60000);
        QCOMPARE(held, QByteArray("8")); // seen while it was read
        QCOMPARE(done.back().at(0).toLongLong(), 8);
        QVERIFY(!QFile::exists(guard));
        QTRY_VERIFY_WITH_TIMEOUT(!recognizer.running(), 5000);
    }

    // The whole way for a real picture, which loads the OCR models: only when
    // asked. WS_OCR_PICTURE names the file, WS_OCR_EXPECT what must be among
    // the words read in it (a QR code's content, GitHub #305). Skipped without.
    void recognizerReadsWhatAPictureSays()
    {
        const QString picture = qEnvironmentVariable("WS_OCR_PICTURE");
        if (picture.isEmpty())
            QSKIP("WS_OCR_PICTURE is not set");
        TextRecognizer recognizer;
        QSignalSpy done(&recognizer, &TextRecognizer::done);
        recognizer.read(1, picture);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 60000);
        QVERIFY(done.back().at(2).toBool());
        const QString words = done.back().at(1).toString();
        qInfo().noquote() << "engine" << recognizer.engine() << "read" << words.size() << "characters";
        QVERIFY2(words.contains(qEnvironmentVariable("WS_OCR_EXPECT")), qUtf8Printable(words));
        QTRY_VERIFY_WITH_TIMEOUT(!recognizer.running(), 5000);
    }

    void disabledWriterRetiresAfterItsRequest()
    {
        ClipboardWatcher watcher({}, {});
        std::atomic<int> result = -1;
        watcher.write({}, [&](bool ok) { result = ok ? 1 : 0; });
        // An empty write is rejected before OpenClipboard, leaving every format intact.
        QTRY_COMPARE_WITH_TIMEOUT(result.load(), 0, 2000);
        QVERIFY(!watcher.listening());
        QTRY_VERIFY_WITH_TIMEOUT(!watcher.running(), 3000);
        result = -1;
        watcher.write({}, [&](bool ok) { result = ok ? 1 : 0; });
        QTRY_COMPARE_WITH_TIMEOUT(result.load(), 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!watcher.running(), 3000);
    }
};

QTEST_GUILESS_MAIN(ClipLifecycleTest)
#include "tst_cliplifecycle.moc"
