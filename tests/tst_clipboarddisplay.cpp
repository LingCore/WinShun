#include "ClipboardDisplayProtocol.h"

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
};

QTEST_GUILESS_MAIN(ClipboardDisplayTest)
#include "tst_clipboarddisplay.moc"
