// The plugins that left Win顺's setup for the plugin library (GitHub #138,
// src/app/MovedPlugins): the screenshot's settings, [Capture] of
// WinShun.ini up to 0.16, moved to the plugin's capture.ini; which ones a
// user is missing.

#include "MovedPlugins.h"

#include "Catalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

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
            QCOMPARE(left.value(u"Capture/PrintScreenBefore"_s).toString(), u"absent"_s);
            QCOMPARE(left.value(u"Plugin.winshun.capture/Enabled"_s).toBool(), true);
            QCOMPARE(left.value(u"Plugin.winshun.capture/Confirmed"_s).toStringList(), ws::moved::capturePermissions());
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

    void failedCopyKeepsOldSettings()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ini = dir.filePath(u"WinShun.ini"_s);
        {
            QSettings settings(ini, QSettings::IniFormat);
            settings.setValue(u"Capture/Enabled"_s, true);
            settings.setValue(u"Capture/Hotkey"_s, u"Ctrl+Alt+A"_s);
        }
        QFile obstruction(dir.filePath(u"plugins"_s));
        QVERIFY(obstruction.open(QIODevice::WriteOnly));
        obstruction.close(); // a file where the destination directory would be
        QVERIFY(!ws::moved::moveCaptureSettings(ini, obstruction.fileName()));
        QSettings original(ini, QSettings::IniFormat);
        QCOMPARE(original.value(u"Capture/Enabled"_s).toBool(), true);
        QCOMPARE(original.value(u"Capture/Hotkey"_s).toString(), u"Ctrl+Alt+A"_s);
        QVERIFY(!original.contains(u"Plugin.winshun.capture/Enabled"_s));
    }

    void failedRecoveryCanRetryWithoutLosingData()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(u"index.json"_s);
        const QByteArray before = qgetenv("WINSHUN_PLUGIN_INDEX");
        const bool wasSet = qEnvironmentVariableIsSet("WINSHUN_PLUGIN_INDEX");
        const auto reset = qScopeGuard([&] {
            if (wasSet)
                qputenv("WINSHUN_PLUGIN_INDEX", before);
            else
                qunsetenv("WINSHUN_PLUGIN_INDEX");
        });
        qputenv("WINSHUN_PLUGIN_INDEX", index.toUtf8());
        const QString userDir = dir.filePath(u"plugins"_s);
        QVERIFY(QDir().mkpath(userDir + u"/winshun.capture.data"_s));
        const QString settingsFile = userDir + u"/winshun.capture.data/capture.ini"_s;
        {
            QFile settings(settingsFile);
            QVERIFY(settings.open(QIODevice::WriteOnly));
            settings.write("[Capture]\nHotkey=Ctrl+Alt+A\n");
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt == 1) { // the source exists now, but has no such package
                QFile list(index);
                QVERIFY(list.open(QIODevice::WriteOnly));
                list.write("{\"schema\":1,\"plugins\":[]}");
            }
            auto result = std::make_shared<std::optional<QList<ws::moved::Outcome>>>();
            ws::moved::install({u"winshun.capture"_s}, userDir, u"zh"_s,
                [result](const auto& outcomes) { *result = outcomes; });
            QVERIFY(QTest::qWaitFor([&] { return result->has_value(); }, 5000));
            QCOMPARE(result->value().size(), 1);
            const auto& outcome = result->value().front();
            QCOMPARE(outcome.id, u"winshun.capture"_s);
            QCOMPARE(outcome.name, ws::moved::name(u"winshun.capture"));
            QVERIFY(!outcome.error.isEmpty());
            QVERIFY(!outcome.offline); // missing local list/package is not a network failure
            QFile settings(settingsFile);
            QVERIFY(settings.open(QIODevice::ReadOnly));
            QCOMPARE(settings.readAll(), QByteArray("[Capture]\nHotkey=Ctrl+Alt+A\n"));
            QVERIFY(!QFileInfo::exists(userDir + u"/winshun.capture/plugin.json"_s));
        }
    }

    // Who gets one fetched: on, and no copy of it anywhere. 休息提醒 never
    // came with Win顺, so it is nobody's to miss.
    void missingAreTheOnesTurnedOn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString userDir = dir.filePath(u"user"_s);
        const QString bundledDir = dir.filePath(u"bundled"_s);
        QVERIFY(QDir().mkpath(userDir));
        QVERIFY(QDir().mkpath(bundledDir + u"/winshun.bookmarks"_s));
        QFile old(bundledDir + u"/winshun.bookmarks/plugin.json"_s); // an old setup's folder, still there
        QVERIFY(old.open(QIODevice::WriteOnly));
        // (Built with QJsonDocument: moc loses the class over a raw string literal.)
        const QJsonObject command {{u"id"_s, u"bm"_s}, {u"keyword"_s, u"bm"_s}, {u"title"_s, u"B"_s},
            {u"run"_s, QJsonObject {{u"url"_s, u"https://example.com/?q=%s"_s}}}};
        old.write(QJsonDocument(QJsonObject {{u"manifest"_s, 1}, {u"id"_s, u"winshun.bookmarks"_s}, {u"name"_s, u"B"_s},
                                    {u"version"_s, u"1.1.0"_s}, {u"commands"_s, QJsonArray {command}}})
                      .toJson());
        old.close();
        ws::plugin::Catalog catalog(userDir, bundledDir);
        catalog.scan();
        QVERIFY(catalog.find(u"winshun.bookmarks"));
        const auto on = [](const QString& id, bool enabled) {
            ws::PluginConfig c;
            c.id = id;
            c.enabled = enabled;
            return c;
        };
        const QList<ws::PluginConfig> plugins {on(u"winshun.translate"_s, true), on(u"winshun.bookmarks"_s, true),
            on(u"winshun.localsend"_s, false), on(u"winshun.semantic"_s, true), on(u"winshun.break"_s, true), on(u"winshun.tools"_s, true)};
        QCOMPARE(ws::moved::missing(catalog, plugins), (QStringList {u"winshun.semantic"_s, u"winshun.translate"_s}));
        QFile::remove(bundledDir + u"/winshun.bookmarks/plugin.json"_s); // the setup took it out
        catalog.scan();
        QCOMPARE(ws::moved::missing(catalog, plugins), (QStringList {u"winshun.semantic"_s, u"winshun.translate"_s, u"winshun.bookmarks"_s}));
    }
};

QTEST_GUILESS_MAIN(MovedPluginsTest)
#include "tst_movedplugins.moc"
