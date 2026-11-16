#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

QJsonObject readMessage(QProcess& process)
{
    while (process.canReadLine() || process.waitForReadyRead(3000)) {
        const QByteArray line = process.readLine();
        if (!line.isEmpty()) {
            const QJsonDocument message = QJsonDocument::fromJson(line);
            if (!message.isObject())
                return {};
            return message.object();
        }
    }
    return {};
}

void send(QProcess& process, const QJsonObject& message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    QCOMPARE(process.write(line), qint64(line.size()));
    QVERIFY(process.waitForBytesWritten(3000));
}

struct InitializeResult {
    QJsonObject response;
    QJsonObject update;
};

InitializeResult initialize(QProcess& process)
{
    send(process, QJsonObject {
        {u"jsonrpc"_s, u"2.0"_s},
        {u"id"_s, 1},
        {u"method"_s, u"initialize"_s},
        {u"params"_s, QJsonObject {
            {u"settings"_s, QJsonObject {{u"initial"_s, true}}},
            {u"locale"_s, u"en"_s},
            {u"theme"_s, u"light"_s},
            {u"appearance"_s, QJsonObject {}}}}});
    return {readMessage(process), readMessage(process)};
}

void stop(QProcess& process, int id)
{
    send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"method"_s, u"shutdown"_s}});
    const QJsonObject response = readMessage(process);
    QCOMPARE(response.value(u"id"_s).toInt(), id);
    QCOMPARE(response.value(u"result"_s).toObject(), QJsonObject {});
    QVERIFY(process.waitForFinished(3000));
    QCOMPARE(process.exitCode(), 0);
}

class TestSdkSettings : public QObject {
    Q_OBJECT

private slots:
    void settingsApiRoundTrip()
    {
        QProcess process;
        process.start(QString::fromUtf8(WINSHUN_SDK_SETTINGS_DEMO));
        QVERIFY(process.waitForStarted(3000));

        const InitializeResult initialized = initialize(process);
        QCOMPARE(initialized.response.value(u"id"_s).toInt(), 1);
        QCOMPARE(initialized.response.value(u"result"_s).toObject().value(u"protocolVersion"_s).toString(), u"1"_s);
        const QJsonObject update = initialized.update;
        QCOMPARE(update.value(u"method"_s).toString(), u"winshun/settings/update"_s);
        const QJsonObject values = update.value(u"params"_s).toObject().value(u"settings"_s).toObject();
        QCOMPARE(values.value(u"text"_s).toString(), u"from SDK"_s);
        QCOMPARE(values.value(u"number"_s).toDouble(), 12.5);
        QVERIFY(values.value(u"enabled"_s).isBool());
        QVERIFY(values.value(u"enabled"_s).toBool());
        QCOMPARE(values.value(u"choice"_s).toString(), u"fast"_s);

        const QJsonObject changedParams {{u"settings"_s, QJsonObject {{u"changed"_s, 7}}}};
        send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"method"_s, u"settingsChanged"_s}, {u"params"_s, changedParams}});
        send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, 2}, {u"method"_s, u"query"_s},
            {u"params"_s, QJsonObject {{u"queryId"_s, 1}, {u"text"_s, u"test"_s}}}});
        const QJsonObject query = readMessage(process);
        const QJsonObject data = query.value(u"result"_s).toObject().value(u"items"_s).toArray().first().toObject().value(u"data"_s).toObject();
        QCOMPARE(data.value(u"settingsChangedCalls"_s).toInt(), 1);
        QCOMPARE(data.value(u"settings"_s).toObject().value(u"changed"_s).toInt(), 7);

        send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, 3}, {u"method"_s, u"showSettings"_s},
            {u"params"_s, QJsonObject {}}});
        const QJsonObject shown = readMessage(process);
        QCOMPARE(shown.value(u"id"_s).toInt(), 3);
        QCOMPARE(shown.value(u"result"_s).toObject(), QJsonObject {});

        send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, 4}, {u"method"_s, u"query"_s},
            {u"params"_s, QJsonObject {{u"queryId"_s, 2}, {u"text"_s, u"test"_s}}}});
        const QJsonObject afterShow = readMessage(process);
        const QJsonObject afterShowData = afterShow.value(u"result"_s).toObject().value(u"items"_s).toArray().first().toObject().value(u"data"_s).toObject();
        QCOMPARE(afterShowData.value(u"showSettingsCalls"_s).toInt(), 1);

        send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, 5}, {u"method"_s, u"unknown"_s},
            {u"params"_s, QJsonObject {}}});
        const QJsonObject unknown = readMessage(process);
        QCOMPARE(unknown.value(u"error"_s).toObject().value(u"code"_s).toInt(), -32601);
        stop(process, 6);
    }

    void defaultShowSettingsIsHandled()
    {
        QProcess process;
        process.start(QString::fromUtf8(WINSHUN_SDK_SETTINGS_DEMO), {u"--default"_s});
        QVERIFY(process.waitForStarted(3000));
        const InitializeResult initialized = initialize(process);
        QCOMPARE(initialized.response.value(u"id"_s).toInt(), 1);
        QCOMPARE(initialized.response.value(u"result"_s).toObject().value(u"protocolVersion"_s).toString(), u"1"_s);
        QVERIFY(initialized.update.value(u"method"_s).toString() == u"winshun/settings/update"_s);

        send(process, QJsonObject {{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, 2}, {u"method"_s, u"showSettings"_s},
            {u"params"_s, QJsonObject {}}});
        const QJsonObject shown = readMessage(process);
        QCOMPARE(shown.value(u"id"_s).toInt(), 2);
        QCOMPARE(shown.value(u"result"_s).toObject(), QJsonObject {});
        stop(process, 3);
    }
};

} // namespace

QTEST_GUILESS_MAIN(TestSdkSettings)

#include "tst_pluginsdk.moc"
