#include "Settings.h"
#include "WebSearchConfig.h"
#include "PreviewShortcut.h"
#include "Query.h"
#include "SettingsCommand.h"

#include "ServiceVolumeAccess.h"
#include "platform/InputMode.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

const QString kSizeKeys[] {u"Content/MaxFileSizeMB"_s, u"Content/MaxCodeFileSizeMB"_s,
    u"Content/MaxDataFileSizeMB"_s, u"Content/MaxDocumentFileSizeMB"_s};

} // namespace

class SettingsTest : public QObject {
    Q_OBJECT

private slots:
    void keyboardHints()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(settings.shortcutHelp);
        QVERIFY(settings.footerHints);
        for (const bool help : {false, true}) {
            for (const bool footer : {false, true}) {
                settings.shortcutHelp = help;
                settings.footerHints = footer;
                settings.save();
                ws::Settings restored;
                restored.load();
                QCOMPARE(restored.shortcutHelp, help);
                QCOMPARE(restored.footerHints, footer);
            }
        }
    }

    void retiredExteriorLogoSetting_data()
    {
        QTest::addColumn<bool>("oldValue");
        QTest::newRow("previously-shown") << true;
        QTest::newRow("previously-hidden") << false;
    }

    void retiredExteriorLogoSetting()
    {
        QFETCH(bool, oldValue);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        {
            QSettings old(ws::Settings::filePath(), QSettings::IniFormat);
            old.setValue(u"Appearance/SearchLogo"_s, oldValue);
            old.setValue(u"Appearance/TaskbarIcon"_s, u"mono"_s);
        }
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.taskbarIcon, u"mono"_s);
        QVERIFY(settings.saveChecked());
        QSettings saved(ws::Settings::filePath(), QSettings::IniFormat);
        QVERIFY(!saved.contains(u"Appearance/SearchLogo"_s));
        QCOMPARE(saved.value(u"Appearance/TaskbarIcon"_s).toString(), u"mono"_s);
    }

    void webPluginMigration()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        {
            QSettings old(ws::Settings::filePath(), QSettings::IniFormat);
            old.setValue(u"WebSearch/Defaults"_s, ws::kWebDefaultsVersion);
            old.setValue(u"WebSearch/Suggestions"_s, false);
            old.beginWriteArray(u"WebSearch"_s, 1);
            old.setArrayIndex(0);
            old.setValue(u"Keyword"_s, u"my"_s);
            old.setValue(u"Name"_s, u"我的网站"_s);
            old.setValue(u"Url"_s, u"https://example.test/?q=%s"_s);
            old.setValue(u"Home"_s, u"https://example.test/home"_s);
            old.setValue(u"Icon"_s, u"0123456789abcdef.png"_s);
            old.setValue(u"Suggest"_s, QString());
            old.endArray();
            old.setValue(u"Plugin.winshun.web/Enabled"_s, true);
            old.setValue(u"Plugin.winshun.web/Keyword.gh"_s, u"code"_s);
            old.setValue(u"Plugin.winshun.web/Keyword.yt"_s, QString());
            old.setValue(u"Plugin.winshun.web/Keyword.bd"_s, u"MY"_s); // The built-in retains this word.
            old.sync();
        }
        ws::Settings migrated;
        migrated.load();
        QVERIFY(migrated.plugin(ws::webplugin::kId)->enabled);
        QCOMPARE(migrated.webShortcuts.first().keyword, u"my"_s);
        QCOMPARE(migrated.webShortcuts.first().icon, u"0123456789abcdef.png"_s);
        QCOMPARE(migrated.webShortcuts.first().home, u"https://example.test/home"_s);
        QVERIFY(migrated.webShortcuts.first().suggest.isEmpty());
        QVERIFY(!migrated.webSuggestions);
        QVERIFY(ws::typedKeyword(migrated.webShortcuts, u"code WinShun").has_value());
        QVERIFY(!ws::typedKeyword(migrated.webShortcuts, u"gh WinShun").has_value());
        QVERIFY(!ws::typedKeyword(migrated.webShortcuts, u"yt WinShun").has_value());
        QCOMPARE(std::count_if(migrated.webShortcuts.cbegin(), migrated.webShortcuts.cend(), [](const auto& s) {
            return s.keyword.compare(u"my", Qt::CaseInsensitive) == 0;
        }), 1);
        QVERIFY(migrated.saveChecked());
        ws::Settings again;
        again.load();
        QCOMPARE(again.webShortcuts, migrated.webShortcuts);
        QVERIFY(!again.webSuggestions);
        // Disabling and deleting every website are both intentional, persistent choices.
        again.pluginConfig(ws::webplugin::kId.toString()).enabled = false;
        again.webShortcuts.clear();
        QVERIFY(again.saveChecked());
        ws::Settings disabled;
        disabled.load();
        QVERIFY(!disabled.plugin(ws::webplugin::kId)->enabled);
        QVERIFY(disabled.webShortcuts.isEmpty());
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QVERIFY(ini.contains(u"WebSearch/size"_s)); // Rollback source was not destroyed or re-imported.
        QVERIFY(ini.value(u"Plugins/WebSearchMigrated"_s).toBool());
    }

    void webPluginFreshProfile()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(!fresh.plugin(ws::webplugin::kId)->enabled);
        QVERIFY(fresh.webShortcutsInUse().isEmpty());
        QVERIFY(!fresh.webShortcuts.isEmpty()); // Ready when installed/enabled.
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QVERIFY(!ini.contains(u"WebSearch/size"_s));
        ws::Settings reopened;
        reopened.load();
        QVERIFY(!reopened.plugin(ws::webplugin::kId)->enabled);
        reopened.pluginConfig(ws::webplugin::kId.toString()).enabled = true;
        QCOMPARE(reopened.webShortcutsInUse(), reopened.webShortcuts);
        reopened.pluginConfig(ws::webplugin::kId.toString()).enabled = false;
        QVERIFY(reopened.webShortcutsInUse().isEmpty());
    }

    void webPluginCustomManifestMigration()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        const QString folder = profile.filePath(u"plugins/winshun.web"_s);
        QVERIFY(QDir().mkpath(folder));
        QFile manifest(folder + u"/plugin.json"_s);
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        const auto command = QJsonObject {{u"id"_s, u"gh"_s}, {u"keyword"_s, u"old"_s}, {u"title"_s, u"自定义站点"_s},
            {u"run"_s, QJsonObject {{u"url"_s, u"https://private.test/search?q=%s"_s}, {u"home"_s, u"https://private.test/start"_s}}}};
        const QByteArray original = QJsonDocument(QJsonObject {{u"id"_s, u"winshun.web"_s}, {u"commands"_s, QJsonArray {command}}}).toJson();
        manifest.write(original);
        manifest.close();
        QSettings old(ws::Settings::filePath(), QSettings::IniFormat);
        old.setValue(u"Plugin.winshun.web/Enabled"_s, true);
        old.setValue(u"Plugin.winshun.web/Keyword.gh"_s, u"custom"_s);
        old.sync();
        ws::Settings migrated;
        migrated.load();
        const auto match = ws::typedKeyword(migrated.webShortcuts, u"custom 汉字");
        QVERIFY(match);
        const auto& shortcut = migrated.webShortcuts[match->index];
        QCOMPARE(shortcut.url, u"https://private.test/search?q=%s"_s);
        QCOMPARE(shortcut.home, u"https://private.test/start"_s);
        QCOMPARE(shortcut.name, u"自定义站点"_s);
        QVERIFY(!ws::typedKeyword(migrated.webShortcuts, u"gh 汉字"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QCOMPARE(manifest.readAll(), original);
    }

    void onboardingLifecycle()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(!fresh.onboardingComplete);
        // load() writes defaults, but an interrupted welcome is still pending.
        ws::Settings interrupted;
        interrupted.load();
        QVERIFY(!interrupted.onboardingComplete);
        interrupted.onboardingComplete = true;
        interrupted.onboardingCapturePending = true;
        QVERIFY(interrupted.saveChecked());
        ws::Settings completed;
        completed.load();
        QVERIFY(completed.onboardingComplete);
        QVERIFY(completed.onboardingCapturePending);
        completed.onboardingCapturePending = false;
        QVERIFY(completed.saveChecked());
        ws::Settings ready;
        ready.load();
        QVERIFY(!ready.onboardingCapturePending);
    }

    void onboardingExistingProfile()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        {
            QSettings legacy(ws::Settings::filePath(), QSettings::IniFormat);
            legacy.setValue(u"Clipboard/Enabled"_s, true);
            legacy.setValue(u"Launcher/Hotkey"_s, u"Ctrl+Alt+F9"_s);
            legacy.setValue(u"Content/Index"_s, true);
            legacy.sync();
        }
        ws::Settings upgraded;
        upgraded.load();
        QVERIFY(upgraded.onboardingComplete);
        QVERIFY(upgraded.clipboard);
        QVERIFY(upgraded.contentIndex);
        QCOMPARE(upgraded.hotkey, u"Ctrl+Alt+F9"_s);
        // Prepared profiles and previews deliberately skip the first-run UI.
        QVERIFY(ws::Settings::defaults().onboardingComplete);
    }

    void settingsSaveFailure()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        // A directory cannot be atomically replaced by a settings file.
        QVERIFY(QDir().mkpath(ws::Settings::filePath()));
        QVERIFY(!ws::Settings::defaults().saveChecked());
    }

    void systemSwitchesMigration()
    {
        const QString id = u"winshun.switches"_s;
        {
            QTemporaryDir profile;
            QVERIFY(profile.isValid());
            ws::Settings::useProfile(profile.path());
            ws::Settings settings;
            settings.load();
            QVERIFY(!settings.plugin(id) || !settings.plugin(id)->enabled);
            settings.save();
            settings.load();
            QVERIFY(!settings.plugin(id) || !settings.plugin(id)->enabled);
        }
        for (const bool oldSearchSystem : {true, false}) {
            QTemporaryDir profile;
            QVERIFY(profile.isValid());
            ws::Settings::useProfile(profile.path());
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Results/SearchSystem"_s, oldSearchSystem);
            ini.sync();
            ws::Settings settings;
            settings.load();
            QVERIFY(settings.plugin(id));
            QCOMPARE(settings.plugin(id)->enabled, oldSearchSystem);
            QVERIFY(settings.plugin(id)->confirmed.contains(u"system:settings"_s));
            for (ws::PluginConfig& config : settings.plugins)
                if (config.id == id)
                    config.enabled = false;
            settings.save();
            settings.load();
            QVERIFY(!settings.plugin(id)->enabled);
        }
        {
            QTemporaryDir profile;
            QVERIFY(profile.isValid());
            ws::Settings::useProfile(profile.path());
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Plugin.winshun.switches/Enabled"_s, false);
            ini.sync();
            ws::Settings settings;
            settings.load();
            QVERIFY(settings.plugin(id));
            QVERIFY(!settings.plugin(id)->enabled);
        }
    }

    void englishOnOpen()
    {
        // Off unless set: the input method as the user left it (GitHub #201).
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.englishOnOpen);
        settings.englishOnOpen = true;
        settings.save();
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QCOMPARE(ini.value(u"Launcher/EnglishOnOpen"_s).toBool(), true);
        ws::Settings restored;
        restored.load();
        QVERIFY(restored.englishOnOpen);
        ini.setValue(u"Launcher/EnglishOnOpen"_s, false);
        ini.sync();
        restored.load();
        QVERIFY(!restored.englishOnOpen);

        // What it sets, and when it is still the user's untouched state.
        using ws::win::InputMode;
        const InputMode chinese {.open = true, .conversion = IME_CMODE_NATIVE | IME_CMODE_SYMBOL, .sentence = 8};
        const InputMode letters = ws::win::lettersMode(chinese);
        QVERIFY(!letters.open);
        QCOMPARE(letters.conversion, DWORD {IME_CMODE_SYMBOL});
        QCOMPARE(letters.sentence, DWORD {8});
        QVERIFY(ws::win::sameLetters(letters, {.open = false, .conversion = IME_CMODE_ROMAN, .sentence = 0}));
        QVERIFY(!ws::win::sameLetters(letters, {.open = true, .conversion = IME_CMODE_SYMBOL, .sentence = 8}));
        QVERIFY(!ws::win::sameLetters(letters, {.open = false, .conversion = IME_CMODE_NATIVE, .sentence = 8}));
        QVERIFY(!ws::win::lettersMode({.open = true, .conversion = IME_CMODE_FULLSHAPE}).open);
        QCOMPARE(ws::win::lettersMode({.open = true, .conversion = IME_CMODE_FULLSHAPE}).conversion, DWORD {0});

        // WeChat's input method, as read in the box (the native bit does not
        // follow Shift): every opening switches, whatever it reads.
        const auto opening = [](const InputMode& before) {
            const InputMode after = ws::win::lettersMode(before); // what switchToLetters sets
            return std::pair {after, !ws::win::sameLetters(after, before)};
        };
        const auto [first, firstChanged] = opening({.open = true, .conversion = IME_CMODE_NATIVE});
        QVERIFY(firstChanged);
        QCOMPARE(first, (InputMode {.open = false, .conversion = 0}));
        const InputMode shifted {.open = true, .conversion = 0}; // Chinese again by Shift in the box
        QVERIFY(!ws::win::sameLetters(shifted, first)); // the user's: not put back on closing
        const auto [second, secondChanged] = opening(shifted); // the next opening
        QVERIFY(secondChanged);
        QCOMPARE(second, (InputMode {.open = false, .conversion = 0}));
        QVERIFY(!opening({.open = false, .conversion = 0}).second); // English already: nothing to put back
    }

    void startListOptions()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.recentItems, 5);
        QVERIFY(settings.favoritesOnStart);
        settings.recentItems = 20;
        settings.favoritesOnStart = false;
        settings.save();
        ws::Settings restored;
        restored.load();
        QCOMPARE(restored.recentItems, 20);
        QVERIFY(!restored.favoritesOnStart);
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        for (const int invalid : {-1, 0, 10000}) {
            ini.setValue(u"Results/RecentItems"_s, invalid);
            ini.sync();
            restored.load();
            QCOMPARE(restored.recentItems, std::clamp(invalid, 1, 200));
        }
    }

    void clipboardOpeningPosition()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.clipboardAtPointer);
        QVERIFY(!settings.clipboardPauseInFullScreen);
        for (const bool atPointer : {true, false}) {
            settings.clipboardAtPointer = atPointer;
            settings.clipboardPauseInFullScreen = atPointer;
            settings.save();
            ws::Settings restored;
            restored.load();
            QCOMPARE(restored.clipboardAtPointer, atPointer);
            QCOMPARE(restored.clipboardPauseInFullScreen, atPointer);
        }
    }

    // 剪贴板大小: its own step, kept as written; unknown names fall back to following 界面大小.
    void clipboardUiSize()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.clipboardUiSize, u"follow"_s);
        for (const QString size : {u"dense"_s, u"compact"_s, u"standard"_s, u"roomy"_s, u"follow"_s}) {
            settings.clipboardUiSize = size;
            settings.save();
            ws::Settings restored;
            restored.load();
            QCOMPARE(restored.clipboardUiSize, size);
        }
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Clipboard/UiSize"_s, u"huge"_s);
        }
        ws::Settings invalid;
        invalid.load();
        QCOMPARE(invalid.clipboardUiSize, u"follow"_s);
    }

    void previewHotkey()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.previewHotkey, u"Alt+P"_s);
        for (const QString& key : {u"Space"_s, u"Ctrl+Shift+P"_s, u"F8"_s, QString()}) {
            settings.previewHotkey = key;
            settings.save();
            ws::Settings restored;
            restored.load();
            QCOMPARE(restored.previewHotkey, key);
        }
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        for (const QString& key : {u"P"_s, u"Enter"_s, u"Ctrl+C"_s, u"Ctrl+P"_s, u"Ctrl+J"_s, u"Alt+F4"_s, u"Shift+F10"_s}) {
            ini.setValue(u"Results/PreviewHotkey"_s, key);
            ini.sync();
            settings.load();
            QCOMPARE(settings.previewHotkey, u"Alt+P"_s);
        }
        ini.setValue(u"Results/PreviewHotkey"_s, u" ctrl + shift + p "_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.previewHotkey, u"Ctrl+Shift+P"_s);
        ini.remove(u"Results/PreviewHotkey"_s);
        ini.sync();
        settings.load();
        QCOMPARE(settings.previewHotkey, u"Alt+P"_s);
    }

    void previewShortcutRules()
    {
        using namespace ws::previewkeys;
        QVERIFY(allowed(u"Space"_s));
        QVERIFY(allowed(u"Alt+P"_s));
        QVERIFY(allowed(u"Ctrl+Shift+P"_s));
        QVERIFY(allowed(u"F8"_s));
        for (const QString& key : {u"Shift+Space"_s, u"Alt+Space"_s, u"P"_s, u"Shift+P"_s,
                 u"Ctrl+1"_s, u"Alt+1"_s, u"Ctrl+C"_s, u"Ctrl+Shift+C"_s, u"Ctrl+R"_s,
                 u"Ctrl+J"_s, u"Ctrl+K"_s, u"Ctrl+N"_s, u"Ctrl+P"_s, u"Ctrl+U"_s, u"Ctrl+D"_s, u"Ctrl+,"_s,
                 u"Alt+F4"_s, u"Shift+F10"_s, u"Meta+P"_s, u"Ctrl+P, Ctrl+O"_s})
            QVERIFY2(!allowed(key), qPrintable(key));
        const QKeySequence space(u"Space"_s);
        QVERIFY(matches(space, Qt::Key_Space, Qt::NoModifier));
        QVERIFY(matches(space, Qt::Key_Space, Qt::KeypadModifier));
        QVERIFY(!matches(space, Qt::Key_Space, Qt::ShiftModifier));
        QVERIFY(!matches(space, Qt::Key_Space, Qt::ControlModifier));
        QVERIFY(!matches(QKeySequence(), Qt::Key_Space, Qt::NoModifier));
        QVERIFY(matches(QKeySequence(u"Alt+P"_s), Qt::Key_P, Qt::AltModifier));
        QVERIFY(!matches(QKeySequence(u"Alt+P"_s), Qt::Key_P, Qt::AltModifier | Qt::ShiftModifier));
    }

    void oneCommander()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.fileManager = u"onecommander"_s;
        settings.save();
        ws::Settings restored;
        restored.load();
        QCOMPARE(restored.fileManager, settings.fileManager);

        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Launcher/FileManager"_s, u" OneCommander "_s);
        ini.sync();
        restored.load();
        QCOMPARE(restored.fileManager, u"onecommander"_s);
    }

    void opusTyping()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.opusTyping);
        for (const bool explorer : {false, true}) {
            for (const bool opus : {true, false}) {
                settings.explorerTyping = explorer;
                settings.opusTyping = opus;
                settings.save();
                ws::Settings read;
                read.load();
                QCOMPARE(read.explorerTyping, explorer);
                QCOMPARE(read.opusTyping, opus);
            }
        }
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.remove(u"Launcher/OpusTyping"_s); // an older profile keeps Opus's own typing
        ini.sync();
        settings.load();
        QVERIFY(!settings.opusTyping);
        QVERIFY(settings.explorerTyping);
    }

    // GitHub #82: typed in Opus, that tab's folder (the default, for an older
    // profile and a value nobody knows too) or the whole PC.
    void opusTypingScope()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.opusTypingScope, u"folder"_s);
        settings.opusTypingScope = u"all"_s;
        settings.save();
        ws::Settings read;
        read.load();
        QCOMPARE(read.opusTypingScope, u"all"_s);

        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Launcher/OpusTypingScope"_s, u"elsewhere"_s);
        ini.sync();
        read.load();
        QCOMPARE(read.opusTypingScope, u"folder"_s);
    }

    // GitHub #110: typed in a folder's window, its rows first (the new
    // default, for a profile from before too) or that folder alone.
    void folderTyping()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.folderTyping, u"first"_s);
        settings.folderTyping = u"only"_s;
        settings.save();
        ws::Settings read;
        read.load();
        QCOMPARE(read.folderTyping, u"only"_s);

        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Launcher/FolderTyping"_s, u" Only "_s); // by hand
        ini.sync();
        read.load();
        QCOMPARE(read.folderTyping, u"only"_s);
        settings.folderTyping = u"level"_s; // this level only (GitHub #182)
        settings.save();
        read.load();
        QCOMPARE(read.folderTyping, u"level"_s);
        ini.setValue(u"Launcher/FolderTyping"_s, u" LEVEL "_s);
        ini.sync();
        read.load();
        QCOMPARE(read.folderTyping, u"level"_s);
        for (const QString& value : {u"everywhere"_s, QString()}) {
            ini.setValue(u"Launcher/FolderTyping"_s, value);
            ini.sync();
            read.load();
            QCOMPARE(read.folderTyping, u"first"_s);
        }
        ini.remove(u"Launcher/FolderTyping"_s); // a profile from before: the folder first
        ini.sync();
        read.load();
        QCOMPARE(read.folderTyping, u"first"_s);
    }

    void selectedDrivesRoundTrip()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ws::Settings::useProfile(temporary.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.selectedDrives);
        settings.selectedDrives = QStringList{u"c:"_s, u"D:\\"_s, u"C:"_s, u"invalid"_s};
        settings.save();
        settings.load();
        QCOMPARE(*settings.selectedDrives, (QStringList{u"C:"_s, u"D:"_s}));
        settings.selectedDrives = QStringList{};
        settings.save();
        settings.load();
        QVERIFY(settings.selectedDrives && settings.selectedDrives->isEmpty());
        settings.selectedDrives.reset();
        settings.save();
        settings.load();
        QVERIFY(!settings.selectedDrives);
    }

    // GitHub #132: skipped file names, Office's and WPS's lock files by
    // default; an emptied list stays empty, and one written by hand is read
    // as the settings window keeps it.
    void skippedFileNames()
    {
        QCOMPARE(ws::Settings::fileNamePatterns(u" ~$* ; Thumbs.db，*.TMP、新建 文本文档*\n~$*;THUMBS.DB"_s),
            (QStringList {u"~$*"_s, u"Thumbs.db"_s, u"*.TMP"_s, u"新建 文本文档*"_s}));
        QVERIFY(ws::Settings::fileNamePatterns(u"D:\\x\\~$*, a/b, c:d, ,"_s).isEmpty());
        QVERIFY(ws::Settings::fileNamePatterns(u"*; *.*; ?; **"_s).isEmpty()); // every file

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ws::Settings::useProfile(temporary.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.excludedFileNames, QStringList {u"~$*"_s});
        settings.excludedFileNames.clear();
        settings.save();
        settings.load();
        QVERIFY(settings.excludedFileNames.isEmpty());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/ExcludedFileNames"_s, u"~$*; *.tmp~"_s);
        }
        settings.load();
        QCOMPARE(settings.excludedFileNames, (QStringList {u"~$*"_s, u"*.tmp~"_s}));
    }

    void wslFolders()
    {
        const QString path = u"\\\\wsl.localhost\\Ubuntu\\home\\Dev"_s;
        QCOMPARE(ws::Settings::sharePath(u"\"\\\\wsl$\\Ubuntu\\home\\Dev\\\""_s), path);
        QCOMPARE(ws::Settings::sharePath(u"file://wsl.localhost/Ubuntu/home/Dev"_s), path);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.shares = {{path, u"项目"_s}, {u"\\\\wsl.localhost\\Ubuntu\\home\\dev"_s, {}}};
        settings.save();
        ws::Settings again;
        again.load();
        QCOMPARE(again.shares.size(), 2);
        QCOMPARE(again.shares[0].path, path);
        QCOMPARE(again.shareOptions().shares.size(), std::size_t {2});
    }

    void shareScanSettings()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.shares = {{u"\\\\nas\\docs"_s, {}, false, {true, 23 * 60, 6 * 60 + 30, 3}},
            {u"\\\\wsl.localhost\\Ubuntu\\home\\dev"_s, {}, true, {false, 9 * 60, 18 * 60, 1}}};
        settings.save();
        ws::Settings again;
        again.load();
        QCOMPARE(again.shares, settings.shares);
        QCOMPARE(again.shareOptions().shares[0].scan, settings.shares[0].scan);
        QCOMPARE(again.shareOptions().shares[1].scan, settings.shares[1].scan);
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Shares/1/ScanFrom"_s, -1);
            ini.setValue(u"Shares/1/ScanUntil"_s, 9999);
            ini.setValue(u"Shares/1/ScanDepth"_s, -5);
            for (const QString& key : {u"ScanScheduled"_s, u"ScanFrom"_s, u"ScanUntil"_s, u"ScanDepth"_s})
                ini.remove(u"Shares/2/"_s + key); // older settings: unchanged behaviour
        }
        again.load();
        QCOMPARE(again.shares[0].scan.fromMinute, 0);
        QCOMPARE(again.shares[0].scan.untilMinute, 1439);
        QCOMPARE(again.shares[0].scan.maxDepth, 0);
        QCOMPARE(again.shares[1].scan, ws::ShareScan {});
    }

    // GitHub #114: a WebDAV folder, as its web address or as Windows writes
    // it, is kept in one form: one added twice is seen (addShare), and two
    // spellings of one in the settings file become one.
    void webDavFolders()
    {
        const QString small = u"\\\\127.0.0.1@8714\\small"_s;
        QCOMPARE(ws::Settings::sharePath(u"http://127.0.0.1:8714/small/"_s), small);
        QCOMPARE(ws::Settings::sharePath(u"\\\\127.0.0.1@8714\\DavWWWRoot\\small"_s), small);
        QCOMPARE(ws::Settings::sharePath(u"\"\\\\127.0.0.1@8714\\davwwwroot\\small\\\""_s), small);
        QCOMPARE(ws::Settings::sharePath(u"//127.0.0.1@8714/small"_s), small);
        QCOMPARE(ws::Settings::sharePath(u"https://NAS/dav"_s), u"\\\\nas@SSL\\dav"_s);
        QCOMPARE(ws::Settings::sharePath(u"\\\\nas@SSL@443\\DavWWWRoot\\dav"_s), u"\\\\nas@SSL\\dav"_s);
        QCOMPARE(ws::Settings::sharePath(u"\\\\nas@SSL@5006"_s), u"\\\\nas@SSL@5006\\DavWWWRoot"_s);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.shares = {{u"\\\\127.0.0.1@8714\\DavWWWRoot\\small"_s, {}}, {small, {}},
            {u"http://127.0.0.1:8714/many"_s, {}}};
        settings.save();
        ws::Settings again;
        again.load();
        QCOMPARE(again.shares.size(), 2);
        QCOMPARE(again.shares[0].path, small);
        QCOMPARE(again.shares[1].path, u"\\\\127.0.0.1@8714\\many"_s);
    }

    void fileTypePrefixes()
    {
        auto types = ws::Settings::defaultFileTypes();
        QCOMPARE(types[0].keyword, u"documents"_s);
        const auto folder = ws::splitFileTypeQuery(u"  :FoLdEr 报告 2026 "_s, types, u"folder");
        QCOMPARE(folder.type, std::optional<int>(-2));
        QCOMPARE(folder.words, u"报告 2026"_s);
        const auto pictures = ws::splitFileTypeQuery(u":pictures"_s, types, u"folder");
        QCOMPARE(pictures.type, std::optional<int>(1));
        QVERIFY(pictures.words.isEmpty());
        types.append({u"设计图"_s, {u"dwg"_s, u"dxf"_s}, {}, u"cad"_s});
        QCOMPARE(ws::splitFileTypeQuery(u":CAD 泵 size:>1mb"_s, types, u"folder").words, u"泵 size:>1mb"_s);
        const auto suffixFolder = ws::splitFileTypeQuery(u"报告 :folder"_s, types, u"folder");
        QCOMPARE(suffixFolder.type, std::optional<int>(-2));
        QCOMPARE(suffixFolder.words, u"报告"_s);
        for (const QString text : {u":unknown 报告"_s, u"\":folder\" 报告"_s, u"C:\\folder\\报告"_s, u"报告 \":folder\""_s}) {
            const auto literal = ws::splitFileTypeQuery(text, types, u"folder");
            QVERIFY(!literal.type);
            QCOMPARE(literal.words, text);
        }
        QCOMPARE(ws::normalizedTypeKeyword(u" :CAD "), std::optional<QString>(u"cad"_s));
        QVERIFY(!ws::normalizedTypeKeyword(u"bad prefix"));
        QVERIFY(!ws::normalizedTypeKeyword(u"a:b"));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ws::Settings::useProfile(temporary.path());
        ws::Settings settings;
        settings.fileTypes = types;
        settings.folderKeyword = u"dir"_s;
        settings.showModifiedTime = true;
        settings.save();
        ws::Settings loaded;
        loaded.load();
        QCOMPARE(loaded.fileTypes, types);
        QCOMPARE(loaded.folderKeyword, u"dir"_s);
        QVERIFY(loaded.showModifiedTime);
    }

    // Limits still at the default of an older version take the new default;
    // ones the user set, and a list of extensions set by hand, stay.
    void contentDefaults_data()
    {
        QTest::addColumn<int>("version"); // 0: no DefaultsVersion
        QTest::addColumn<QList<double>>("written");
        QTest::addColumn<QString>("extensions");
        QTest::addColumn<QList<double>>("expected"); // -1: the new default
        QTest::addColumn<bool>("defaultExtensions");

        QTest::newRow("before 3, untouched") << 0 << QList<double> {64, 8, 16, 512} << u"txt"_s
                                             << QList<double> {-1, -1, -1, -1} << true;
        QTest::newRow("before 3, one set") << 2 << QList<double> {64, 3, 16, 512} << u"txt, md"_s
                                           << QList<double> {-1, 3, -1, -1} << false;
        QTest::newRow("3, untouched") << 3 << QList<double> {16, 2, 4, 100} << QString()
                                      << QList<double> {-1, -1, -1, -1} << true;
        QTest::newRow("3, text set to 1 KB") << 3 << QList<double> {1.0 / 1024, 2, 4, 100} << QString()
                                             << QList<double> {1.0 / 1024, -1, -1, -1} << true;
        QTest::newRow("3, txt set by hand") << 3 << QList<double> {16, 8, 4, 512} << u"txt"_s
                                            << QList<double> {-1, 8, -1, 512} << false;
        // The new defaults are what the user now sets, the old ones too.
        QTest::newRow("4, old values set") << 4 << QList<double> {16, 2, 4, 100} << QString()
                                           << QList<double> {16, 2, 4, 100} << true;
    }

    void contentDefaults()
    {
        QFETCH(int, version);
        QFETCH(QList<double>, written);
        QFETCH(QString, extensions);
        QFETCH(QList<double>, expected);
        QFETCH(bool, defaultExtensions);

        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            if (version)
                ini.setValue(u"Content/DefaultsVersion"_s, version);
            for (int k = 0; k < 4; ++k)
                ini.setValue(kSizeKeys[k], written[k]);
            if (!extensions.isEmpty())
                ini.setValue(u"Content/Extensions"_s, extensions.split(u", "_s));
        }
        const ws::Settings d = ws::Settings::defaults();
        ws::Settings settings;
        settings.load();
        for (std::size_t k = 0; k < 4; ++k) {
            const double want = expected[k] < 0 ? d.contentMaxSizeMB[k] : expected[k];
            QCOMPARE(settings.contentMaxSizeMB[k], want);
        }
        if (defaultExtensions)
            QCOMPARE(settings.contentExtensions, d.contentExtensions);
        else
            QCOMPARE(settings.contentExtensions, extensions.split(u", "_s));

        // Migrated once: loading again changes nothing, and the file holds it.
        ws::Settings again;
        again.load();
        QCOMPARE(again.contentMaxSizeMB, settings.contentMaxSizeMB);
        QCOMPARE(again.contentExtensions, settings.contentExtensions);
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QCOMPARE(ini.value(u"Content/DefaultsVersion"_s).toInt(), 4);
    }

    // The folder menu's groups (GitHub #51): all on by default, kept when
    // saved, and a file with none on reads back as all on.
    void folderMenuGroups()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(fresh.folderMenuFavorites && fresh.folderMenuRecent && fresh.folderMenuWindows);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuFavorites"_s, false);
            ini.setValue(u"Launcher/FolderMenuRecent"_s, true);
            ini.setValue(u"Launcher/FolderMenuWindows"_s, false);
        }
        ws::Settings some;
        some.load();
        QVERIFY(!some.folderMenuFavorites && some.folderMenuRecent && !some.folderMenuWindows);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuRecent"_s, false);
        }
        ws::Settings none;
        none.load();
        QVERIFY(none.folderMenuFavorites && none.folderMenuRecent && none.folderMenuWindows);
    }

    // The double click is a choice of its own (GitHub #196): on as it always
    // was, and off stays off with the middle button on.
    void folderMenuTriggers()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(fresh.folderMenuDouble && !fresh.folderMenuMiddle);

        fresh.folderMenuDouble = false;
        fresh.folderMenuMiddle = true;
        fresh.save();
        ws::Settings again;
        again.load();
        QVERIFY(!again.folderMenuDouble && again.folderMenuMiddle);

        // A hand-edited file with no trigger or no place gets them back.
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuDouble"_s, false);
            ini.setValue(u"Launcher/FolderMenuMiddle"_s, false);
            ini.setValue(u"Launcher/FolderMenuExplorer"_s, false);
            ini.setValue(u"Launcher/FolderMenuDesktop"_s, false);
        }
        ws::Settings none;
        none.load();
        QVERIFY(none.folderMenuDouble && none.folderMenuExplorer && none.folderMenuDesktop);
    }

    // The actions under the menu's lists (GitHub #200): all on as before, each
    // kept on its own.
    void folderMenuItems()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(fresh.folderMenuItemSearch && fresh.folderMenuItemCopyPath && fresh.folderMenuItemCmd && fresh.folderMenuItemPowerShell
            && fresh.folderMenuItemFavorite && fresh.folderMenuItemSettings);

        fresh.folderMenuItemCopyPath = false;
        fresh.folderMenuItemCmd = false;
        fresh.folderMenuItemSettings = false;
        fresh.save();
        ws::Settings again;
        again.load();
        QVERIFY(again.folderMenuItemSearch && !again.folderMenuItemCopyPath && !again.folderMenuItemCmd && again.folderMenuItemPowerShell
            && again.folderMenuItemFavorite && !again.folderMenuItemSettings);
    }

    // What a new install leaves off until wanted: the folder menu, the dialog
    // bar, 粘贴为文件. A file from before without their keys keeps the first
    // two on, as they were; a choice written stays.
    void offUntilWanted()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(!fresh.dialogBar);
        QCOMPARE(fresh.folderMenu, u"off"_s);
        QVERIFY(!fresh.pasteFileEnabled);
        QVERIFY(fresh.pasteFileCtrlV && fresh.pasteFileMenu); // its ways, once it is on
        fresh.save();
        ws::Settings again;
        again.load();
        QVERIFY(!again.dialogBar);
        QCOMPARE(again.folderMenu, u"off"_s);

        QTemporaryDir older;
        QVERIFY(older.isValid());
        ws::Settings::useProfile(older.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/Hotkey"_s, u"Alt+Space"_s); // a file from before the keys
        }
        ws::Settings upgraded;
        upgraded.load();
        QVERIFY(upgraded.dialogBar);
        QCOMPARE(upgraded.folderMenu, u"auto"_s);
        QVERIFY(!upgraded.pasteFileEnabled); // new: off, as 截图 is

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenu"_s, u"sideways"_s);
            ini.setValue(u"PasteFile/Enabled"_s, true);
            ini.setValue(u"PasteFile/CtrlV"_s, false);
        }
        ws::Settings written;
        written.load();
        QCOMPARE(written.folderMenu, u"auto"_s); // a stranger: as before
        QVERIFY(written.pasteFileEnabled && !written.pasteFileCtrlV && written.pasteFileMenu);
    }

    // GitHub #44: the other sorts and the double-click setting. A click opens
    // by default; every sort name is kept, and a stranger reads as the best match.
    // A number written by hand that is none ("abc", "", "1h") reads as the
    // default, not as the 0 a failed conversion gives ("每 0 小时", GitHub
    // #114); a 0 written as one stays 0 (the WebDAV folders: only at start).
    void numbersWrittenByHand()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        const ws::Settings d;
        const auto read = [](const QString& key, const QVariant& value) {
            {
                QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
                ini.setValue(key, value);
            }
            ws::Settings s;
            s.load();
            return s;
        };
        for (const QString& text : {u"abc"_s, QString(), u"1h"_s}) {
            QCOMPARE(read(u"Network/VerifyHoursUnwatched"_s, text).networkVerifyHoursUnwatched, d.networkVerifyHoursUnwatched);
            QCOMPARE(read(u"Network/Threads"_s, text).networkThreads, d.networkThreads);
        }
        QCOMPARE(read(u"Network/VerifyHoursUnwatched"_s, u"0"_s).networkVerifyHoursUnwatched, 0);
        QCOMPARE(read(u"Network/VerifyHoursUnwatched"_s, u"6"_s).networkVerifyHoursUnwatched, 6);
        QCOMPARE(read(u"Network/Threads"_s, u"8"_s).networkThreads, 8);
        QCOMPARE(read(u"Content/MaxFileSizeMB"_s, u"lots"_s).contentMaxSizeMB[0], d.contentMaxSizeMB[0]);
    }

    void sortAndDoubleClick()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QCOMPARE(fresh.sort, u"match"_s);
        QVERIFY(!fresh.doubleClickToOpen);

        for (const QStringView name : ws::kSortNames) {
            ws::Settings s;
            s.load();
            s.sort = name.toString();
            s.doubleClickToOpen = true;
            s.save();
            ws::Settings back;
            back.load();
            QCOMPARE(back.sort, name.toString());
            QVERIFY(back.doubleClickToOpen);
        }
        QCOMPARE(ws::sortIndex(u"newest"), 1);
        QCOMPARE(ws::sortIndex(u"smallest"), 7);
        QCOMPARE(ws::sortIndex(u"nonsense"), 0);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Results/Sort"_s, u"alphabetical"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.sort, u"match"_s);
    }

    // GitHub #52: a new install starts with auto and the known folders; an
    // older file, which indexed whole disks, goes on so (and says so in the
    // file). The folders stay as written: an emptied list is not filled again.
    void indexScope()
    {
        QTemporaryDir fresh;
        QVERIFY(fresh.isValid());
        ws::Settings::useProfile(fresh.path());
        ws::Settings installed;
        installed.load();
        QCOMPARE(installed.indexScope, u"auto"_s);
        QCOMPARE(installed.indexFolders, ws::Settings::defaultIndexFolders());
        QCOMPARE(installed.folderVerifyHours, 24);
        QCOMPARE(QSettings(ws::Settings::filePath(), QSettings::IniFormat).value(u"Index/Scope"_s).toString(), u"auto"_s);

        QTemporaryDir old;
        QVERIFY(old.isValid());
        ws::Settings::useProfile(old.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/RescanOnStartup"_s, true);
        }
        ws::Settings upgraded;
        upgraded.load();
        QCOMPARE(upgraded.indexScope, u"drives"_s);
        QCOMPARE(QSettings(ws::Settings::filePath(), QSettings::IniFormat).value(u"Index/Scope"_s).toString(), u"drives"_s);
        ws::Settings again;
        again.load();
        QCOMPARE(again.indexScope, u"drives"_s);

        // Kept as written, nested ones and variables too; an empty list stays empty.
        const QStringList written {u"D:\\Work"_s, u"D:\\Work\\Sub"_s, u"%USERPROFILE%\\Desktop"_s, u"E:\\"_s};
        again.indexScope = u"folders"_s;
        again.indexFolders = written;
        again.folderVerifyHours = 4;
        again.save();
        ws::Settings back;
        back.load();
        QCOMPARE(back.indexScope, u"folders"_s);
        QCOMPARE(back.indexFolders, written);
        QCOMPARE(back.folderVerifyHours, 4);
        back.indexFolders.clear();
        back.save();
        ws::Settings emptied;
        emptied.load();
        QVERIFY(emptied.indexFolders.isEmpty());

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/Scope"_s, u"Everything"_s);
            ini.setValue(u"Index/FolderVerifyHours"_s, 1000);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.indexScope, u"auto"_s);
        QCOMPARE(odd.folderVerifyHours, 24 * 7);
    }

    // GitHub #52 review: the known folders are not written into the file
    // when it is read. A list never set follows them (OneDrive taking the
    // documents, say) until only the chosen folders are first indexed, which
    // takes them as they are then, for good; one changed in any way (by the
    // AI too, which knows no flag) is written. An emptied list stays empty.
    void indexFoldersUnset()
    {
        if (ws::Settings::defaultIndexFolders().isEmpty())
            QSKIP("no known folders on a local disk here");
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        const auto written = [] { return QSettings(ws::Settings::filePath(), QSettings::IniFormat).contains(u"Index/Folders"_s); };
        ws::Settings fresh;
        fresh.load();
        QVERIFY(!fresh.indexFoldersChosen);
        QCOMPARE(fresh.indexFolders, ws::Settings::defaultIndexFolders());
        QVERIFY(!written());
        fresh.save();
        QVERIFY(!written());
        ws::Settings same;
        same.load();
        QVERIFY(!same.indexFoldersChosen);

        // The first time the chosen folders are in effect: taken, once.
        QVERIFY(fresh.chooseDefaultIndexFolders());
        QVERIFY(fresh.indexFoldersChosen);
        QCOMPARE(fresh.indexFolders, ws::Settings::defaultIndexFolders());
        QVERIFY(!fresh.chooseDefaultIndexFolders());
        fresh.save();
        QVERIFY(written());
        ws::Settings taken;
        taken.load();
        QVERIFY(taken.indexFoldersChosen);
        QCOMPARE(taken.indexFolders, ws::Settings::defaultIndexFolders());
        QVERIFY(!taken.chooseDefaultIndexFolders());

        // Emptied: stays so, and is not filled again.
        taken.indexFolders.clear();
        taken.save();
        ws::Settings emptied;
        emptied.load();
        QVERIFY(emptied.indexFoldersChosen);
        QVERIFY(!emptied.chooseDefaultIndexFolders());
        QVERIFY(emptied.indexFolders.isEmpty());

        // Changed without the flag (an edit that knows nothing of it): written all the same.
        QTemporaryDir other;
        QVERIFY(other.isValid());
        ws::Settings::useProfile(other.path());
        ws::Settings edited;
        edited.load();
        edited.indexFolders.removeLast();
        ws::Settings takenAsIs = edited; // the chosen folders come into effect after that edit
        QCOMPARE(takenAsIs.chooseDefaultIndexFolders(), !edited.indexFolders.isEmpty());
        QCOMPARE(takenAsIs.indexFolders, edited.indexFolders); // not put back to the known ones
        edited.save();
        QVERIFY(written());
        ws::Settings back;
        back.load();
        QVERIFY(back.indexFoldersChosen);
        QCOMPARE(back.indexFolders, edited.indexFolders);
    }

    // GitHub #52 review: whether only the chosen folders are indexed. Never
    // with the disks read whole, nor with drives; while the index service is
    // wanted and only on its way (stopped, starting, not answering), whole
    // disks for folders and auto alike: else every login that comes before
    // the service would drop the rest of each disk and read it again.
    void foldersInEffect()
    {
        using State = ws::ntfs::ServiceState;
        ws::Settings s;
        const auto in = [&](const char* scope, const char* access, bool walked, bool elevated, bool wanted, State state) {
            s.indexScope = QString::fromLatin1(scope);
            s.indexAccess = QString::fromLatin1(access);
            return s.foldersInEffect(walked, elevated, wanted, state);
        };
        // Read whole (through the service, or as administrator), or drives: never.
        QVERIFY(!in("folders", "auto", false, false, true, State::Running));
        QVERIFY(!in("folders", "direct", false, true, false, State::Unknown));
        QVERIFY(!in("drives", "walk", true, false, false, State::Unknown));
        QVERIFY(!in("drives", "auto", true, false, true, State::NotInstalled));
        // Walked for good: told to walk, or no service (not installed, disabled).
        QVERIFY(in("folders", "walk", true, false, false, State::Unknown));
        QVERIFY(in("auto", "walk", true, false, false, State::Unknown));
        QVERIFY(in("folders", "auto", true, false, true, State::NotInstalled));
        QVERIFY(in("folders", "auto", true, false, true, State::Disabled));
        QVERIFY(in("auto", "auto", true, false, true, State::NotInstalled));
        QVERIFY(in("auto", "service", true, false, true, State::Disabled));
        // As administrator, told to use the service that is not there: folders only when told so.
        QVERIFY(in("folders", "service", true, true, true, State::NotInstalled));
        QVERIFY(!in("auto", "service", true, true, true, State::NotInstalled));
        // The service on its way: whole disks, folders or auto alike.
        for (const State state : {State::Stopped, State::Starting, State::Running, State::Unknown}) {
            QVERIFY2(!in("folders", "auto", true, false, true, state), ws::ntfs::ServiceVolumeAccess::stateName(state));
            QVERIFY2(!in("folders", "service", true, true, true, state), ws::ntfs::ServiceVolumeAccess::stateName(state));
            QVERIFY2(!in("auto", "auto", true, false, true, state), ws::ntfs::ServiceVolumeAccess::stateName(state));
        }
    }

    // As IndexService takes them: full, the letter upper case, "D:" for a
    // drive; network places and what is no full path left out.
    void indexFolderPaths()
    {
        ws::Settings s;
        s.indexFolders = {u"d:\\Work\\"_s, u"E:\\"_s, u"e:"_s, u"\\\\nas\\docs"_s, u"Documents"_s, u"F:Work"_s, u"  "_s};
        const std::vector<std::wstring> expected {L"D:\\Work", L"E:", L"E:"};
        QCOMPARE(s.indexFolderPaths(), expected);
        QVERIFY(ws::Settings::isNetworkPath(u"\\\\nas\\docs"_s));
        QVERIFY(!ws::Settings::isNetworkPath(u"C:\\Windows"_s));

        // None inside another, none on a network, every one there.
        const QStringList defaults = ws::Settings::defaultIndexFolders();
        for (const QString& f : defaults) {
            QVERIFY2(QFileInfo(f).isDir(), qPrintable(f));
            QVERIFY(!ws::Settings::isNetworkPath(f));
            for (const QString& o : defaults)
                QVERIFY2(f == o || !f.startsWith(o + u'\\', Qt::CaseInsensitive), qPrintable(f + u" in "_s + o));
        }
    }

    // `WinShun.exe --settings --settings-at 9,0` to a copy that is running: the place travels
    // with the command (it was dropped, and the window opened on its first page).
    void settingsCommandCarriesThePlace()
    {
        namespace sc = ws::settingscommand;
        QCOMPARE(sc::make({}), u"settings"_s);
        QCOMPARE(sc::make(u"9,0"_s), u"settings:9,0"_s);
        QCOMPARE(sc::make(u"indexAccess"_s), u"settings:indexAccess"_s);
        QString place = u"left over"_s;
        QVERIFY(sc::parse(u"settings"_s, &place));
        QVERIFY(place.isEmpty());
        QVERIFY(sc::parse(u"settings:9,1"_s, &place));
        QCOMPARE(place, u"9,1"_s);
        QVERIFY(sc::parse(u"settings:indexAccess"_s, &place));
        QCOMPARE(place, u"indexAccess"_s);
        QVERIFY(sc::parse(u"settings:"_s, &place)); // no place: just open
        QVERIFY(place.isEmpty());
        QVERIFY(sc::parse(u"settings"_s, nullptr));
        // Other commands are not this one.
        for (const QString& other : {u"toggle"_s, u"quit"_s, u"show"_s, u"query:settings"_s, u"settingsx"_s, QString()})
            QVERIFY2(!sc::parse(other, &place), qPrintable(other));
        // What make() builds, parse() reads back.
        for (const QString& at : {u"0,0"_s, u"9,2"_s, u"indexAccess"_s}) {
            QVERIFY(sc::parse(sc::make(at), &place));
            QCOMPARE(place, at);
        }
    }
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "tst_settings.moc"
