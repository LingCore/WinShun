// The plugins that left Win顺's setup for the plugin library (GitHub #138,
// src/app/MovedPlugins): the screenshot's settings, [Capture] of
// WinShun.ini up to 0.16, moved to the plugin's capture.ini.

#include "MovedPlugins.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

class MovedPluginsTest : public QObject {
    Q_OBJECT

private slots:
    void captureSettingsMove()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ini = dir.filePath(u"WinShun.ini"_s);
        const QString dataRoot = dir.filePath(u"plugins"_s);
        const QString captureIni = dataRoot + u"/winshun.capture.data/capture.ini"_s;
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(u"Capture/Enabled"_s, true);
            s.setValue(u"Capture/Hotkey"_s, u"Ctrl+Alt+A"_s);
            s.setValue(u"Capture/PrintScreen"_s, true);
            s.setValue(u"Capture/PrintScreenBefore"_s, u"absent"_s);
            s.setValue(u"Capture/ToolbarHidden"_s, u"pin,redo"_s);
            s.setValue(u"Capture/Keyword"_s, u"sc"_s);
            s.setValue(u"Launcher/Hotkey"_s, u"Alt+Space"_s);
        }
        QVERIFY(ws::moved::moveCaptureSettings(ini, dataRoot));
        {
            QSettings moved(captureIni, QSettings::IniFormat);
            QCOMPARE(moved.value(u"Capture/Hotkey"_s).toString(), u"Ctrl+Alt+A"_s);
            QCOMPARE(moved.value(u"Capture/PrintScreen"_s).toBool(), true);
            QCOMPARE(moved.value(u"Capture/PrintScreenBefore"_s).toString(), u"absent"_s); // the plugin gives PrtScn back
            QCOMPARE(moved.value(u"Capture/ToolbarHidden"_s).toString(), u"pin,redo"_s);
            QVERIFY(!moved.contains(u"Capture/Enabled"_s)); // the plugin's switch is [Plugin.winshun.capture]
            QVERIFY(!moved.contains(u"Capture/Keyword"_s)); // Win顺's
        }
        {
            QSettings left(ini, QSettings::IniFormat);
            QCOMPARE(left.value(u"Capture/Keyword"_s).toString(), u"sc"_s);
            QVERIFY(!left.contains(u"Capture/Enabled"_s));
            QVERIFY(!left.contains(u"Capture/Hotkey"_s));
            QVERIFY(!left.contains(u"Capture/PrintScreenBefore"_s));
            QCOMPARE(left.value(u"Launcher/Hotkey"_s).toString(), u"Alt+Space"_s);
        }
        // Moved: not again.
        QVERIFY(!ws::moved::moveCaptureSettings(ini, dataRoot));

        // A capture.ini there already (the plugin installed by hand first) is kept as it is.
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(u"Capture/Enabled"_s, true);
            s.setValue(u"Capture/Hotkey"_s, u"Ctrl+Alt+Q"_s);
        }
        QVERIFY(ws::moved::moveCaptureSettings(ini, dataRoot));
        QCOMPARE(QSettings(captureIni, QSettings::IniFormat).value(u"Capture/Hotkey"_s).toString(), u"Ctrl+Alt+A"_s);
        QVERIFY(!QSettings(ini, QSettings::IniFormat).contains(u"Capture/Hotkey"_s));
    }

    void captureOffIsLeftAlone()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ini = dir.filePath(u"WinShun.ini"_s);
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(u"Capture/Enabled"_s, false);
            s.setValue(u"Capture/Hotkey"_s, u"Ctrl+Alt+A"_s);
        }
        QVERIFY(!ws::moved::moveCaptureSettings(ini, dir.filePath(u"plugins"_s)));
        QVERIFY(!QFile::exists(dir.filePath(u"plugins/winshun.capture.data/capture.ini"_s)));
        QCOMPARE(QSettings(ini, QSettings::IniFormat).value(u"Capture/Hotkey"_s).toString(), u"Ctrl+Alt+A"_s);
    }

    // The permissions said yes to for it are the plugin's own (package/plugin.json).
    void capturePermissionsAreTheManifests()
    {
        QFile manifest(QStringLiteral(WINSHUN_SOURCE_DIR) + u"/src/capture-plugin/package/plugin.json"_s);
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const QJsonArray permissions = QJsonDocument::fromJson(manifest.readAll()).object().value(u"permissions"_s).toArray();
        QStringList names;
        for (const QJsonValue& p : permissions)
            names.push_back(p.toString());
        QCOMPARE(ws::moved::capturePermissions(), names);
    }
};

QTEST_GUILESS_MAIN(MovedPluginsTest)
#include "tst_movedplugins.moc"
