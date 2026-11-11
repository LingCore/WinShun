#include "ClipboardDisplayProtocol.h"
#include "DisplayProfile.h"

#include <QtTest>

using namespace ws::clipdisplay;
using namespace Qt::StringLiterals;

class ClipboardDisplayTest : public QObject {
    Q_OBJECT
private slots:
    void splitAtEveryBoundary()
    {
        const QJsonObject expected {{u"type"_s, u"state"_s}, {u"text"_s, u"图片\n完整文字\""_s}, {u"id"_s, 179}};
        const QByteArray bytes = frame(expected);
        for (int split = 0; split <= bytes.size(); ++split) {
            Decoder decoder;
            QJsonObject received;
            QVERIFY(decoder.append(QByteArrayView(bytes).first(split)));
            const bool complete = decoder.take(received);
            QCOMPARE(complete, split == bytes.size());
            QVERIFY(decoder.append(QByteArrayView(bytes).sliced(split)));
            if (!complete) QVERIFY(decoder.take(received));
            QCOMPARE(received, expected);
            QVERIFY(!decoder.take(received));
            QVERIFY(!decoder.failed());
        }
    }
    void coalescedFrames()
    {
        Decoder decoder;
        QVERIFY(decoder.append(frame({{u"sequence"_s, 1}}) + frame({{u"sequence"_s, 2}})));
        QJsonObject object;
        QVERIFY(decoder.take(object)); QCOMPARE(object.value(u"sequence"_s).toInt(), 1);
        QVERIFY(decoder.take(object)); QCOMPARE(object.value(u"sequence"_s).toInt(), 2);
        QVERIFY(!decoder.take(object)); QVERIFY(!decoder.failed());
    }
    void oversizedHeaderRejectedWithoutPayload()
    {
        QByteArray bytes(4, Qt::Uninitialized);
        qToLittleEndian<quint32>(kMaxFrame + 1, bytes.data());
        Decoder decoder;
        QVERIFY(!decoder.append(bytes));
        QVERIFY(decoder.failed());
        QVERIFY(!decoder.append(frame({{u"type"_s, u"ready"_s}})));
    }
    void malformedAndNonObjectRejected()
    {
        for (const QByteArray payload : {QByteArray("[1,2]"), QByteArray("{broken"), QByteArray()}) {
            QByteArray bytes(4, Qt::Uninitialized);
            qToLittleEndian<quint32>(quint32(payload.size()), bytes.data());
            bytes += payload;
            Decoder decoder;
            QVERIFY(decoder.append(bytes));
            QJsonObject object;
            QVERIFY(!decoder.take(object));
            QVERIFY(decoder.failed());
        }
    }
    void outgoingAndBufferedLimit()
    {
        QVERIFY(frame({{u"text"_s, QString(int(kMaxFrame), u'x')}}).isEmpty());
        Decoder decoder;
        QVERIFY(!decoder.append(QByteArray(3 * kMaxFrame + 1, 'x')));
        QVERIFY(decoder.failed());
    }
    // The display process is ended with its job and leaves its folder: the
    // next one removes it, and never the folder of one still running.
    void profileFoldersSwept()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QDir dir(root.path());
        const auto file = [&dir](const QString& name) {
            QFile made(dir.filePath(name));
            return made.open(QIODevice::WriteOnly) && made.write("x") == 1;
        };
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(dir.mkpath(u"WinShun-display-killed"_s) && file(u"WinShun-display-killed/state.ini"_s));
        QVERIFY(dir.mkpath(u"WinShun-AbCdEf"_s)); // the second folder of versions up to 0.23
        QVERIFY(dir.mkpath(u"WinShun-InUse1"_s) && file(u"WinShun-InUse1/import.db"_s));
        QVERIFY(dir.mkpath(u"WinShun-preview-12345678"_s) && dir.mkpath(u"Other-display-1"_s));
        ws::DisplayProfile running(root.path());
        QVERIFY(running.isValid());
        QVERIFY(QFileInfo(running.path()).fileName().startsWith(u"WinShun-display-"));

        // Just made, no lock file in it yet: another process is about to take it.
        QCOMPARE(ws::DisplayProfile::sweep(root.path(), now), 0);
        QCOMPARE(dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 6);

        // A day on: what was left goes, what is in use or another's stays.
        QVERIFY(file(dir.relativeFilePath(running.path() + u"/state.ini"_s)));
        QCOMPARE(ws::DisplayProfile::sweep(root.path(), now.addDays(2)), 2);
        QVERIFY(!dir.exists(u"WinShun-display-killed"_s));
        QVERIFY(!dir.exists(u"WinShun-AbCdEf"_s));
        QVERIFY(dir.exists(u"WinShun-InUse1/import.db"_s));
        QVERIFY(dir.exists(u"WinShun-preview-12345678"_s) && dir.exists(u"Other-display-1"_s));
        QVERIFY(QFileInfo::exists(running.path() + u"/state.ini"_s));
        QVERIFY(running.isValid());
    }
    void profileFolderGoesWithItsOwner()
    {
        QTemporaryDir root;
        QString path;
        {
            const ws::DisplayProfile profile(root.path());
            QVERIFY(profile.isValid());
            path = profile.path();
        }
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!ws::DisplayProfile(root.path() + u"/missing"_s).isValid());
    }
};

QTEST_GUILESS_MAIN(ClipboardDisplayTest)
#include "tst_clipboarddisplay.moc"
