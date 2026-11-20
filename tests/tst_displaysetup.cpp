// How this PC's displays are set up, as an export's summary records it
// (DisplaySetup.h): read from the real system, so the checks are on shape
// and sense, not on one PC's values.
#include "DisplaySetup.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

using namespace Qt::StringLiterals;

class DisplaySetupTest : public QObject {
    Q_OBJECT

private slots:
    void eachActiveMonitorIsDescribed()
    {
        const QJsonArray monitors = ws::diagnostics::monitors();
        QVERIFY2(!monitors.isEmpty(), "no active monitor read");
        int primaries = 0;
        for (const QJsonValue& value : monitors) {
            const QJsonObject monitor = value.toObject();
            qInfo().noquote() << QJsonDocument(monitor).toJson(QJsonDocument::Compact);
            QVERIFY(!monitor[u"connection"_s].toString().isEmpty());
            const double hz = monitor[u"hz"_s].toDouble();
            QVERIFY2(hz > 20 && hz < 1000, qPrintable(u"refresh rate %1"_s.arg(hz)));
            QVERIFY(QRegularExpression(u"^\\d+x\\d+$"_s).match(monitor[u"size"_s].toString()).hasMatch());
            if (monitor.contains(u"edid"_s))
                QVERIFY(QRegularExpression(u"^[A-Z]{3} [0-9A-F]{4}$"_s).match(monitor[u"edid"_s].toString()).hasMatch());
            QVERIFY(monitor[u"hdr"_s].isBool());
            primaries += monitor[u"primary"_s].toBool() ? 1 : 0;
        }
        QCOMPARE(primaries, 1);
    }

    void graphicsSwitchesAreTextWhenSet()
    {
        const QJsonObject settings = ws::diagnostics::graphicsSettings();
        qInfo().noquote() << QJsonDocument(settings).toJson(QJsonDocument::Compact);
        for (auto it = settings.begin(); it != settings.end(); ++it)
            QVERIFY2(it->isString() && !it->toString().isEmpty(), qPrintable(it.key()));
    }

    void pacingSwitchesByName_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("pacing");
        // AMD's (its driver key, UMD and DAL subkeys)
        QTest::newRow("FreeSync") << u"KMD_EnableFreeSync"_s << true;
        QTest::newRow("Enhanced Sync") << u"TurboSync"_s << true;
        QTest::newRow("VSync") << u"VSyncControl"_s << true;
        QTest::newRow("Chill") << u"KMD_ChillEnabled"_s << true;
        QTest::newRow("Anti-Lag") << u"AntiLag"_s << true;
        QTest::newRow("Boost") << u"KMD_RadeonBoostEnabled"_s << true;
        QTest::newRow("HYPR-RX") << u"HyprRxEnabled"_s << true;
        QTest::newRow("AFMF") << u"AFMFEnabled"_s << true;
        QTest::newRow("flip queue") << u"FlipQueueSize"_s << true;
        QTest::newRow("ULPS") << u"EnableUlps"_s << true;
        QTest::newRow("stutter") << u"DalDisableStutter"_s << true;
        QTest::newRow("FRTC") << u"KMD_FRTEnabled_FRTC"_s << true;
        // Windows' and NVIDIA's
        QTest::newRow("MPO") << u"DisableMPO"_s << true;
        QTest::newRow("Mpo") << u"MpoEnabled"_s << true;
        QTest::newRow("TDR") << u"TdrDelay"_s << true;
        QTest::newRow("overlay") << u"OverlayTestMode"_s << true;
        QTest::newRow("G-SYNC") << u"GSyncState"_s << true;
        QTest::newRow("low latency") << u"LowLatencyMode"_s << true;
        QTest::newRow("PSR") << u"PSR_Disable"_s << true;
        // The letters inside other words
        QTest::newRow("RmPowerFeature") << u"RmPowerFeature"_s << false;
        QTest::newRow("DListDriverName") << u"UserModeDListDriverName"_s << false;
        QTest::newRow("AsyncCompute") << u"AsyncCompute"_s << false;
        QTest::newRow("DriverDesc") << u"DriverDesc"_s << false;
        QTest::newRow("HardwareInformation") << u"HardwareInformation.MemorySize"_s << false;
        QTest::newRow("Mpos") << u"Mposition"_s << false;
    }

    void pacingSwitchesByName()
    {
        QFETCH(QString, name);
        QFETCH(bool, pacing);
        QCOMPARE(ws::diagnostics::isPacingSwitch(name), pacing);
    }

    void eachDisplayAdapterHasItsPacingSwitches()
    {
        const QJsonObject drivers = ws::diagnostics::driverSettings();
        qInfo().noquote() << QJsonDocument(drivers).toJson(QJsonDocument::Compact).left(2000);
        QVERIFY2(!drivers.isEmpty(), "no display adapter read");
        for (auto adapter = drivers.begin(); adapter != drivers.end(); ++adapter) {
            QVERIFY(adapter->isObject());
            const QJsonObject values = adapter->toObject();
            QVERIFY(values.size() <= 120);
            for (auto it = values.begin(); it != values.end(); ++it) // only switches, named by their path in the key
                QVERIFY2(ws::diagnostics::isPacingSwitch(it.key().section(u'\\', -1)) && it->isString(), qPrintable(it.key()));
        }
    }

    void displayEventsAreTheGraphicsOnesNewestFirst()
    {
        const QJsonArray events = ws::diagnostics::displayEvents(365, 5);
        qInfo().noquote() << QJsonDocument(events).toJson(QJsonDocument::Compact);
        QVERIFY(events.size() <= 5);
        const QStringList providers {u"Display"_s, u"Microsoft-Windows-DxgKrnl"_s, u"amdkmdag"_s, u"amdwddmg"_s,
            u"nvlddmkm"_s, u"igfx"_s, u"igfxn"_s, u"igfxnd"_s};
        QDateTime previous;
        for (const QJsonValue& value : events) {
            const QJsonObject event = value.toObject();
            QVERIFY2(providers.contains(event[u"provider"_s].toString()), qPrintable(event[u"provider"_s].toString()));
            QVERIFY(event[u"id"_s].toInt(-1) >= 0);
            const QDateTime time = QDateTime::fromString(event[u"time"_s].toString(), Qt::ISODateWithMs);
            QVERIFY(time.isValid());
            QVERIFY(time.secsTo(QDateTime::currentDateTimeUtc()) < 366 * 86'400);
            if (previous.isValid())
                QVERIFY(time <= previous);
            previous = time;
        }
    }

    // In an export, on its own thread, but not to hold it up.
    void allOfItIsQuick()
    {
        QElapsedTimer timer;
        timer.start();
        ws::diagnostics::monitors();
        ws::diagnostics::graphicsSettings();
        ws::diagnostics::driverSettings();
        ws::diagnostics::displayEvents();
        qInfo() << "took" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() < 3000);
    }
};

QTEST_GUILESS_MAIN(DisplaySetupTest)
#include "tst_displaysetup.moc"
